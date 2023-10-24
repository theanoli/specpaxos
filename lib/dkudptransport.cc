// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * udptransport.cc:
 *   message-passing network interface that uses UDP message delivery
 *   and libasync
 *
 * Copyright 2013-2016 Dan R. K. Ports  <drkp@cs.washington.edu>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **********************************************************************/

#include "lib/assert.h"
#include "lib/configuration.h"
#include "lib/message.h"
#include "lib/dkudptransport.h"

#include <google/protobuf/message.h>
#include <event2/event.h>
#include <event2/thread.h>

#include <random>
#include <cinttypes>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <signal.h>

const size_t MAX_UDP_MESSAGE_SIZE = 9000; // XXX
const int SOCKET_BUF_SIZE = 10485760;

const uint64_t NONFRAG_MAGIC = 0x20050318;
const uint64_t FRAG_MAGIC = 0x20101010;

using std::pair;

UDPTransportAddress::UDPTransportAddress(const sockaddr_in &addr)
    : addr(addr)
{
    memset((void *)addr.sin_zero, 0, sizeof(addr.sin_zero));
}

UDPTransportAddress *
UDPTransportAddress::clone() const
{
    UDPTransportAddress *c = new UDPTransportAddress(*this);
    return c;    
}

bool operator==(const UDPTransportAddress &a, const UDPTransportAddress &b)
{
    return (memcmp(&a.addr, &b.addr, sizeof(a.addr)) == 0);
}

bool operator!=(const UDPTransportAddress &a, const UDPTransportAddress &b)
{
    return !(a == b);
}

bool operator<(const UDPTransportAddress &a, const UDPTransportAddress &b)
{
    return (memcmp(&a.addr, &b.addr, sizeof(a.addr)) < 0);
}

UDPTransportAddress
UDPTransport::LookupAddress(const specpaxos::ReplicaAddress &addr)
{
    int res;
    struct addrinfo hints;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = 0;
    hints.ai_flags    = 0;
    struct addrinfo *ai;
    if ((res = getaddrinfo(addr.host.c_str(), addr.port.c_str(), &hints, &ai))) {
        Panic("Failed to resolve %s:%s: %s",
              addr.host.c_str(), addr.port.c_str(), gai_strerror(res));
    }
    if (ai->ai_addr->sa_family != AF_INET) {
        Panic("getaddrinfo returned a non IPv4 address");
    }
    UDPTransportAddress out =
              UDPTransportAddress(*((sockaddr_in *)ai->ai_addr));
    freeaddrinfo(ai);
    return out;
}

UDPTransportAddress
UDPTransport::LookupAddress(const specpaxos::Configuration &config,
                            int idx)
{
    const specpaxos::ReplicaAddress &addr = config.replica(idx);
    return LookupAddress(addr);
}

const UDPTransportAddress *
UDPTransport::LookupMulticastAddress(const specpaxos::Configuration
                                     *config)
{
    if (!config->multicast()) {
        // Configuration has no multicast address
        return NULL;
    }

    if (multicastFds.find(config) != multicastFds.end()) {
        // We are listening on this multicast address. Some
        // implementations of MOM aren't OK with us both sending to
        // and receiving from the same address, so don't look up the
        // address.
        return NULL;
    }

    UDPTransportAddress *addr =
        new UDPTransportAddress(LookupAddress(*(config->multicast())));
    return addr;
}

static void
BindToPort(int qd, const string &host, const string &port)
{
    struct sockaddr_in sin;

    if ((host == "") && (port == "any")) {
        // Set up the sockaddr so we're OK with any UDP socket
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = 0;        
    } else {
        // Otherwise, look up its hostname and port number (which
        // might be a service name)
        struct addrinfo hints;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = 0;
        hints.ai_flags    = AI_PASSIVE;
        struct addrinfo *ai;
        int res;
        if ((res = getaddrinfo(host.c_str(), port.c_str(),
                               &hints, &ai))) {
            Panic("Failed to resolve host/port %s:%s: %s",
                  host.c_str(), port.c_str(), gai_strerror(res));
        }
        ASSERT(ai->ai_family == AF_INET);
        ASSERT(ai->ai_socktype == SOCK_DGRAM);
        if (ai->ai_addr->sa_family != AF_INET) {
            Panic("getaddrinfo returned a non IPv4 address");        
        }
        sin = *(sockaddr_in *)ai->ai_addr;
        
        freeaddrinfo(ai);
    }

    Notice("Binding to %s:%d", inet_ntoa(sin.sin_addr), htons(sin.sin_port));

    if (demi_bind(qd, (sockaddr *)&sin, sizeof(sin)) < 0) {
        PPanic("Failed to bind to socket");
    }
}

UDPTransport::UDPTransport(double dropRate, double reorderRate,
                           int dscp, event_base *evbase)
    : dropRate(dropRate), reorderRate(reorderRate),
      dscp(dscp)
{
    char *argv[] = {};
    demi_init(0, argv);

    lastTimerId = 0;
    lastFragMsgId = 0;

    uniformDist = std::uniform_real_distribution<double>(0.0,1.0);
    randomEngine.seed(time(NULL));
    reorderBuffer.valid = false;
    if (dropRate > 0) {
        Warning("Dropping packets with probability %g", dropRate);
    }
    if (reorderRate > 0) {
        Warning("Reordering packets with probability %g", reorderRate);
    }
    
    // Set up libevent
    event_set_log_callback(LogCallback);
    event_set_fatal_callback(FatalCallback);
    // XXX Hack for Naveen: allow the user to specify an existing
    // libevent base. This will probably not work exactly correctly
    // for error messages or signals, but that doesn't much matter...
    evthread_use_pthreads();
    libeventBase = event_base_new();
    evthread_make_base_notifiable(libeventBase);

    // Set up signal handler
    signalEvents.push_back(evsignal_new(libeventBase, SIGTERM,
                                        SignalCallback, this));
    signalEvents.push_back(evsignal_new(libeventBase, SIGINT,
                                        SignalCallback, this));
    for (event *x : signalEvents) {
        event_add(x, NULL);
    }

    // Add a timer to check the demikernel event loop 
    Timer(0, [=]() {
        shard[i]->Invoke(request_str,
                          bind(&Client::putCallback,
                          this, i,
                          placeholders::_1,
                          placeholders::_2));
    });
}

UDPTransport::~UDPTransport()
{
    // XXX Shut down libevent?

    // for (auto kv : timers) {
    //     delete kv.second;
    // }

}

void
UDPTransport::Register(TransportReceiver *receiver,
                       const specpaxos::Configuration &config,
                       int replicaIdx)
{
    ASSERT(replicaIdx < config.n);

    const specpaxos::Configuration *canonicalConfig =
        RegisterConfiguration(receiver, config, replicaIdx);

    this->replicaIdx = replicaIdx;

    // Create socket
    int qd;
    if ((qd = demi_socket(&qd, AF_INET, SOCK_DGRAM, 0)) < 0) {
        PPanic("Failed to create socket to listen");
    }

    /*
    // Put it in non-blocking mode
    if (fcntl(qd, F_SETFL, O_NONBLOCK, 1)) {
        PWarning("Failed to set O_NONBLOCK");
    }
    */

    // TODO how do we do this? 
    // Enable outgoing broadcast traffic
    int n = 1;
    if (setsockopt(qd, SOL_SOCKET,
                   SO_BROADCAST, (char *)&n, sizeof(n)) < 0) {
        PWarning("Failed to set SO_BROADCAST on socket");
    }

    // TODO check whether we should ignore this...
    /*
    if (dscp != 0) {
        n = dscp << 2;
        if (setsockopt(qd, IPPROTO_IP,
                       IP_TOS, (char *)&n, sizeof(n)) < 0) {
            PWarning("Failed to set DSCP on socket");
        }
    }
    */
    
    // Increase buffer size
    // TODO how? 
    /*
    n = SOCKET_BUF_SIZE;
    if (setsockopt(qd, SOL_SOCKET,
                   SO_RCVBUF, (char *)&n, sizeof(n)) < 0) {
        PWarning("Failed to set SO_RCVBUF on socket");
    }
    if (setsockopt(qd, SOL_SOCKET,
                   SO_SNDBUF, (char *)&n, sizeof(n)) < 0) {
        PWarning("Failed to set SO_SNDBUF on socket");
    }
    */
    
    if (replicaIdx != -1) {
        // Registering a replica. Bind socket to the designated
        // host/port
        const string &host = config.replica(replicaIdx).host;
        const string &port = config.replica(replicaIdx).port;
        BindToPort(qd, host, port);
	if (demi_listen(qd, 5) != 0) {
             PPanic("Failed to listen for Dk connections");
	}
    } else {
        // Registering a client. Bind to any available host/port
        BindToPort(qd, "", "any");        
    }

    /*
    // Set up a libevent callback
    event *ev = event_new(libeventBase, qd, EV_READ | EV_PERSIST,
                          SocketCallback, (void *)this);
    event_add(ev, NULL);
    listenerEvents.push_back(ev);
    */

    // Tell the receiver its address
    struct sockaddr_in sin;
    socklen_t sinsize = sizeof(sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons(config.replica(replicaIdx).port);
    if (assert(inet_pton(AF_INET, ip_str, &sin.sin_addr) != 1)) {
        PPanic("Failed to get socket name");
    }
    DKUDPTransportAddress *addr = new DKUDPTransportAddress(sin);
    receiver->SetAddress(addr);

    // Update mappings
    receivers[qd] = receiver;
    qds[receiver] = qd;

    Notice("Listening on UDP port %hu", ntohs(sin.sin_port));
}

static size_t
SerializeMessage(const ::google::protobuf::Message &m, char **out)
{
    string data = m.SerializeAsString();
    string type = m.GetTypeName();
    size_t typeLen = type.length();
    size_t dataLen = data.length();
    ssize_t totalLen = (sizeof(totalLen) + 
		        sizeof(uint32_t) +
                        typeLen + sizeof(typeLen) +
                        dataLen + sizeof(dataLen));

    char *buf = new char[totalLen];

    demi_sgarray_t sga; 
    sga.sga_numsegs = 1;
    sga.sga_segs[0].sgaseg_buf = p;
    sga.sga_segs[0].sgaseg_len = totalLen;
    char *ptr = buf;

    *(uint32_t *)ptr = NONFRAG_MAGIC;
    ptr += sizeof(uint32_t);
    ASSERT((size_t)(ptr-buf) < totalLen);

    *((size_t *) ptr) = totalLen;
    ptr += sizeof(size_t);
    ASSERT((size_t)(ptr-buf) < totalLen);

    *((size_t *) ptr) = typeLen;
    ptr += sizeof(size_t);
    ASSERT((size_t)(ptr-buf) < totalLen);

    ASSERT((size_t)(ptr+typeLen-buf) < totalLen);
    memcpy(ptr, type.c_str(), typeLen);
    ptr += typeLen;
    *((size_t *) ptr) = dataLen;
    ptr += sizeof(size_t);

    ASSERT((size_t)(ptr-buf) < totalLen);
    ASSERT((size_t)(ptr+dataLen-buf) == totalLen);
    memcpy(ptr, data.c_str(), dataLen);
    ptr += dataLen;
    
    *out = buf;
    return totalLen;
}

bool
UDPTransport::SendMessageInternal(TransportReceiver *src,
                                  const UDPTransportAddress &dst,
                                  const Message &m,
                                  bool multicast)
{
    sockaddr_in sin = dynamic_cast<const UDPTransportAddress &>(dst).addr;

    // Serialize message
    char *buf;
    size_t msgLen = SerializeMessage(m, &buf);

    int qd = qds[src];

    demi_qtoken_t t;
    int ret = demi_push(&t, qd, &sga);
    ASSERT(ret == 0);
    
    demi_qresult_t wait_out;
    ret = demi_wait(&wait_out, t, NULL);
    ASSERT(ret == 0);

    Debug("Sent %ld byte %s message to server over Dk",
          totalLen, type.c_str());
    
    /*
     * TODO I think we don't have to worry about deconstructing packets like this?
    // XXX All of this assumes that the socket is going to be
    // available for writing, which since it's a UDP socket it ought
    // to be.
    if (msgLen <= MAX_UDP_MESSAGE_SIZE) {
        if (sendto(qd, buf, msgLen, 0,
                   (sockaddr *)&sin, sizeof(sin)) < 0) {
            PWarning("Failed to send message");
            goto fail;
        }
    } else {
        msgLen -= sizeof(uint32_t);
        char *bodyStart = buf + sizeof(uint32_t);
        int numFrags = ((msgLen-1) / MAX_UDP_MESSAGE_SIZE) + 1;
        Notice("Sending large %s message in %d fragments",
               m.GetTypeName().c_str(), numFrags);
        uint64_t msgId = ++lastFragMsgId;
        for (size_t fragStart = 0; fragStart < msgLen;
             fragStart += MAX_UDP_MESSAGE_SIZE) {
            size_t fragLen = std::min(msgLen - fragStart,
                                      MAX_UDP_MESSAGE_SIZE);
            size_t fragHeaderLen = 2*sizeof(size_t) + sizeof(uint64_t) + sizeof(uint32_t);
            char fragBuf[fragLen + fragHeaderLen];
            char *ptr = fragBuf;
            *((uint32_t *)ptr) = FRAG_MAGIC;
            ptr += sizeof(uint32_t);
            *((uint64_t *)ptr) = msgId;
            ptr += sizeof(uint64_t);
            *((size_t *)ptr) = fragStart;
            ptr += sizeof(size_t);
            *((size_t *)ptr) = msgLen;
            ptr += sizeof(size_t);
            memcpy(ptr, &bodyStart[fragStart], fragLen);
            
            if (sendto(qd, fragBuf, fragLen + fragHeaderLen, 0,
                       (sockaddr *)&sin, sizeof(sin)) < 0) {
                PWarning("Failed to send message fragment %ld",
                         fragStart);
                goto fail;
            }
        }
    }    
    */

    delete [] buf;
    return true;
}

void
UDPTransport::Run()
{
    event_base_dispatch(libeventBase);
}

// DecodePacket(const char *buf, size_t sz, string &type, string &msg)
// This is called when a message is popped. 
static void
DecodePacket(demi_sgarray_t &sga)
{
    ASSERT(sga.sga_numsegs == 1);
    uint8_t *ptr = (uint8_t *)sga.sga_segs[0].sgaseg_buf;
    ASSERT(sga.sga_segs[0].sgaseg_len > 0);
    uint32_t magic = *(uint32_t *)ptr;
    Debug("[%x] MAGIC=%x", qd, magic);
    ptr += sizeof(magic);
    ASSERT(magic == MAGIC);
    size_t totalSize = *((size_t *)ptr);
    ptr += sizeof(totalSize);
    size_t typeLen = *((size_t *)ptr);
    ptr += sizeof(typeLen);
    string type((char *)ptr, typeLen);
    ptr += typeLen;
    size_t msgLen = *((size_t *)ptr);
    ptr += sizeof(msgLen);
    string data((char *)ptr, msgLen);;
    ptr += msgLen;
}

// TODO overhaul: call this when the message has been received in the event loop. 
// After it completes, we should demi_pop the same queue we got the message from 
// (maybe do outside this function though)
void
UDPTransport::OnReadable(int qd)
{
    const int BUFSIZE = 65536;
    
    do {
        ssize_t sz;
        char buf[BUFSIZE];
        sockaddr_in sender;
        socklen_t senderSize = sizeof(sender);
        
        sz = recvfrom(qd, buf, BUFSIZE, 0,
                      (struct sockaddr *) &sender, &senderSize);
        if (sz == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                PWarning("Failed to receive message from socket");
            }
        }
        
        UDPTransportAddress senderAddr(sender);
        string msgType, msg;

        // Take a peek at the first field. If it's all zeros, this is
        // a fragment. Otherwise, we can decode it directly.
        ASSERT(sizeof(uint32_t) - sz > 0);
        uint32_t magic = *(uint32_t*)buf;
        if (magic == NONFRAG_MAGIC) {
            // Not a fragment. Decode the packet
            DecodePacket(buf+sizeof(uint32_t), sz-sizeof(uint32_t),
                         msgType, msg);
        } else if (magic == FRAG_MAGIC) {
            // This is a fragment. Decode the header
            const char *ptr = buf;
            ptr += sizeof(uint32_t);
            ASSERT(ptr-buf < sz);
            uint64_t msgId = *((uint64_t *)ptr);
            ptr += sizeof(uint64_t);
            ASSERT(ptr-buf < sz);
            size_t fragStart = *((size_t *)ptr);
            ptr += sizeof(size_t);
            ASSERT(ptr-buf < sz);
            size_t msgLen = *((size_t *)ptr);
            ptr += sizeof(size_t);
            ASSERT(ptr-buf < sz);
            ASSERT(buf+sz-ptr == (ssize_t) std::min(msgLen-fragStart,
                                                    MAX_UDP_MESSAGE_SIZE));
            Notice("Received fragment of %zd byte packet %" PRIx64 " starting at %zd",
                   msgLen, msgId, fragStart);
            UDPTransportFragInfo &info = fragInfo[senderAddr];
            if (info.msgId == 0) {
                info.msgId = msgId;
                info.data.clear();
            }
            if (info.msgId != msgId) {
                ASSERT(msgId > info.msgId);
                Warning("Failed to reconstruct packet %" PRIx64 "", info.msgId);
                info.msgId = msgId;
                info.data.clear();
            }
            
            if (fragStart != info.data.size()) {
                Warning("Fragments out of order for packet %" PRIx64 "; "
                        "expected start %zd, got %zd",
                        msgId, info.data.size(), fragStart);
                continue;
            }
            
            info.data.append(string(ptr, buf+sz-ptr));
            if (info.data.size() == msgLen) {
                Debug("Completed packet reconstruction");
                DecodePacket(info.data.c_str(), info.data.size(),
                             msgType, msg);
                info.msgId = 0;
                info.data.clear();
            } else {
                continue;
            }
        } else {
            Warning("Received packet with bad magic number");
        }
        
        // Dispatch
        if (dropRate > 0.0) {
            double roll = uniformDist(randomEngine);
            if (roll < dropRate) {
                Debug("Simulating packet drop of message type %s",
                      msgType.c_str());
                continue;
            }
        }

        if (!reorderBuffer.valid && (reorderRate > 0.0)) {
            double roll = uniformDist(randomEngine);
            if (roll < reorderRate) {
                Debug("Simulating reorder of message type %s",
                      msgType.c_str());
                ASSERT(!reorderBuffer.valid);
                reorderBuffer.valid = true;
                reorderBuffer.addr = new UDPTransportAddress(senderAddr);
                reorderBuffer.message = msg;
                reorderBuffer.msgType = msgType;
                reorderBuffer.qd = qd;
                continue;
            }
        }

    deliver:
        // Was this received on a multicast qd?
        auto it = multicastConfigs.find(qd);
        if (it != multicastConfigs.end()) {
            // If so, deliver the message to all replicas for that
            // config, *except* if that replica was the sender of the
            // message.
            const specpaxos::Configuration *cfg = it->second;
            for (auto &kv : replicaReceivers[cfg]) {
                TransportReceiver *receiver = kv.second;
                const UDPTransportAddress &raddr = 
                    replicaAddresses[cfg].find(kv.first)->second;
                // Don't deliver a message to the sending replica
                if (raddr != senderAddr) {
                    receiver->ReceiveMessage(senderAddr, msgType, msg);
                }
            }
        } else {
            TransportReceiver *receiver = receivers[qd];
            receiver->ReceiveMessage(senderAddr, msgType, msg);
        }

        if (reorderBuffer.valid) {
            reorderBuffer.valid = false;
            msg = reorderBuffer.message;
            msgType = reorderBuffer.msgType;
            qd = reorderBuffer.qd;
            senderAddr = *(reorderBuffer.addr);
            delete reorderBuffer.addr;
            Debug("Delivering reordered packet of type %s",
                  msgType.c_str());
            goto deliver;       // XXX I am a bad person for this.
        }
    } while (0);
}

int
UDPTransport::Timer(uint64_t ms, timer_callback_t cb)
{
    UDPTransportTimerInfo *info = new UDPTransportTimerInfo();

    struct timeval tv;
    tv.tv_sec = ms/1000;
    tv.tv_usec = (ms % 1000) * 1000;
    
    ++lastTimerId;
    
    info->transport = this;
    info->id = lastTimerId;
    info->cb = cb;
    info->ev = event_new(libeventBase, -1, 0,
                         TimerCallback, info);

    timers[info->id] = info;
    
    event_add(info->ev, &tv);
    
    return info->id;
}

bool
UDPTransport::CancelTimer(int id)
{
    UDPTransportTimerInfo *info = timers[id];

    if (info == NULL) {
        return false;
    }

    timers.erase(info->id);
    event_del(info->ev);
    event_free(info->ev);
    delete info;
    
    return true;
}

void
UDPTransport::CancelAllTimers()
{
    while (!timers.empty()) {
        auto kv = timers.begin();
        CancelTimer(kv->first);
    }
}

void
UDPTransport::OnTimer(UDPTransportTimerInfo *info)
{
    timers.erase(info->id);
    event_del(info->ev);
    event_free(info->ev);
    
    info->cb();

    delete info;
}

void
UDPTransport::DemiCallback()
{
    demi_qtoken_t token;
    int status = 0;

    demi_qresult_t wait_out;
    int ready_idx;
    int status; 

    struct timespec ts; 
    ts.tv_sec = 0; 
    ts.tv_nsec = 5000;  // 5us; TODO set this properly
    
    status = demi_wait_any(&wait_out, &ready_idx, tokens.data(), tokens.size(), &ts);
    
    // if we got an EOK back from wait
    if (status == 0) {
        Debug("Found something: qd=%lx", wait_out.qr_qd);
	// process request
	demi_sgarray_t &sga = wait_out.qr_value.sga;
	assert(sga.sga_numsegs > 0);
	DkPopCallback(wait_out.qr_qd, receivers[wait_out.qr_qd], sga);
	status = demi_pop(&token, wait_out.qr_qd);
    } // else fall through; either timeout or connection closed
    
    // 
    if (status == 0) {
        tokens[ready_idx] = token;
    } else {
        //assert(status == ECONNRESET || status == ECONNABORTED);
        CloseConn(wait_out.qr_qd);
        tokens.erase(tokens.begin()+ready_idx);
        Warning("Exited loop");
    }
}

void
UDPTransport::SocketCallback(evutil_socket_t qd, short what, void *arg)
{
    UDPTransport *transport = (UDPTransport *)arg;
    if (what & EV_READ) {
        transport->OnReadable(qd);
    }
}

// Not really a callback...?
void
DkTransport::DkPopCallback(int qd,
                           TransportReceiver *receiver,
                           demi_sgarray_t &sga)
{
    Debug("Pop Callback");

    // Should call here OnReadable to put the packet(s) together if needed and dispatch the
    // message
    
    auto addr = dkIncoming.find(qd);
    
    ASSERT(sga.sga_numsegs == 1);
    uint8_t *ptr = (uint8_t *)sga.sga_segs[0].sgaseg_buf;
    ASSERT(sga.sga_segs[0].sgaseg_len > 0);
    uint32_t magic = *(uint32_t *)ptr;
    Debug("[%x] MAGIC=%x", qd, magic);
    ptr += sizeof(magic);
    ASSERT(magic == MAGIC);
    size_t totalSize = *((size_t *)ptr);
    ptr += sizeof(totalSize);
    size_t typeLen = *((size_t *)ptr);
    ptr += sizeof(typeLen);
    string type((char *)ptr, typeLen);
    ptr += typeLen;
    size_t msgLen = *((size_t *)ptr);
    ptr += sizeof(msgLen);
    string data((char *)ptr, msgLen);;
    ptr += msgLen;
    
    // Dispatch
    receiver->ReceiveMessage(addr->second,
                             type,
                             data);
    free(sga.sga_buf);

    Debug("Done processing large %s message", type.c_str());        
}

void
UDPTransport::TimerCallback(evutil_socket_t qd, short what, void *arg)
{
    UDPTransport::UDPTransportTimerInfo *info =
        (UDPTransport::UDPTransportTimerInfo *)arg;

    ASSERT(what & EV_TIMEOUT);

    info->transport->OnTimer(info);
}

void
UDPTransport::LogCallback(int severity, const char *msg)
{
    Message_Type msgType;
    switch (severity) {
    case _EVENT_LOG_DEBUG:
        msgType = MSG_DEBUG;
        break;
    case _EVENT_LOG_MSG:
        msgType = MSG_NOTICE;
        break;
    case _EVENT_LOG_WARN:
        msgType = MSG_WARNING;
        break;
    case _EVENT_LOG_ERR:
        msgType = MSG_WARNING;
        break;
    default:
        NOT_REACHABLE();
    }

    _Message(msgType, "libevent", 0, NULL, "%s", msg);
}

void
UDPTransport::FatalCallback(int err)
{
    Panic("Fatal libevent error: %d", err);
}

void
UDPTransport::SignalCallback(evutil_socket_t qd, short what, void *arg)
{
    Notice("Terminating on SIGTERM/SIGINT");
    UDPTransport *transport = (UDPTransport *)arg;
    event_base_loopbreak(transport->libeventBase);
}

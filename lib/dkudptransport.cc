// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * dkudptransport.cc:
 *   message-passing network interface that uses DKUDP message delivery
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

const size_t MAX_DKUDP_MESSAGE_SIZE = 9000; // XXX
const int SOCKET_BUF_SIZE = 10485760;

const uint64_t NONFRAG_MAGIC = 0x20050318;
const uint64_t FRAG_MAGIC = 0x20101010;

using std::pair;

DKUDPTransportAddress::DKUDPTransportAddress(const sockaddr_in &addr)
    : addr(addr)
{
    memset((void *)addr.sin_zero, 0, sizeof(addr.sin_zero));
}

DKUDPTransportAddress *
DKUDPTransportAddress::clone() const
{
    DKUDPTransportAddress *c = new DKUDPTransportAddress(*this);
    return c;    
}

bool operator==(const DKUDPTransportAddress &a, const DKUDPTransportAddress &b)
{
    return (memcmp(&a.addr, &b.addr, sizeof(a.addr)) == 0);
}

bool operator!=(const DKUDPTransportAddress &a, const DKUDPTransportAddress &b)
{
    return !(a == b);
}

bool operator<(const DKUDPTransportAddress &a, const DKUDPTransportAddress &b)
{
    return (memcmp(&a.addr, &b.addr, sizeof(a.addr)) < 0);
}

DKUDPTransportAddress
DKUDPTransport::LookupAddress(const specpaxos::ReplicaAddress &addr)
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
    DKUDPTransportAddress out =
              DKUDPTransportAddress(*((sockaddr_in *)ai->ai_addr));
    freeaddrinfo(ai);
    return out;
}

DKUDPTransportAddress
DKUDPTransport::LookupAddress(const specpaxos::Configuration &config,
                            int idx)
{
    const specpaxos::ReplicaAddress &addr = config.replica(idx);
    return LookupAddress(addr);
}

const DKUDPTransportAddress *
DKUDPTransport::LookupMulticastAddress(const specpaxos::Configuration
                                     *config)
{
    if (!config->multicast()) {
        // Configuration has no multicast address
        return NULL;
    }

    if (multicastQds.find(config) != multicastQds.end()) {
        // We are listening on this multicast address. Some
        // implementations of MOM aren't OK with us both sending to
        // and receiving from the same address, so don't look up the
        // address.
        return NULL;
    }

    DKUDPTransportAddress *addr =
        new DKUDPTransportAddress(LookupAddress(*(config->multicast())));
    return addr;
}

static void
BindToPort(int qd, const string &host, const string &port)
{
    Notice("Binding to socket at qd %d", qd);
    struct sockaddr_in sin = {0};
    int int_port = -1; 

    if ((host == "") && (port == "any")) {
        // Set up the sockaddr so we're OK with any DKUDP socket
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = 0;        
    } else {
	/*
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
	*/
	
	sscanf(port.c_str(), "%d", &int_port);
	sin.sin_family = AF_INET;
	sin.sin_port = htons(int_port);
    }
    if (inet_pton(AF_INET, host.c_str(), &(sin.sin_addr)) != 1) {
	Panic("Couldn't convert address to network format!");	    
    }

    if (demi_bind(qd, (const struct sockaddr *)&sin, sizeof(struct sockaddr_in)) != 0) {
        PPanic("Failed to bind to socket");
    }
    Notice("Bound to %s:%d (input: %s:%s)", 
		    inet_ntoa(sin.sin_addr), htons(sin.sin_port), 
		    host.c_str(), port.c_str());

}

DKUDPTransport::DKUDPTransport(double dropRate, double reorderRate,
                           int dscp, event_base *evbase)
    : dropRate(dropRate), reorderRate(reorderRate),
      dscp(dscp)
{
    char str[] = "catnip";
    char *argv[] = {str};
    if (demi_init(1, argv) != 0) {
        PPanic("Failed to initialize Demikernel");	    
    }

    lastTimerId = 0;
    lastFragMsgId = 0;

    if (dropRate > 0) {
        Panic("Drop rate unimplemented");
    }
    if (reorderRate > 0) {
        Panic("Reorder rate unimplemented");
    }
    
    // Set up libevent
    event_set_log_callback(LogCallback);
    event_set_fatal_callback(FatalCallback);

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
}

DKUDPTransport::~DKUDPTransport()
{
    // XXX Shut down libevent?

    // for (auto kv : timers) {
    //     delete kv.second;
    // }

}

void
DKUDPTransport::Register(TransportReceiver *receiver,
                       const specpaxos::Configuration &config,
                       int replicaIdx)
{
    ASSERT(replicaIdx < config.n);

    this->replicaIdx = replicaIdx;

    // Create socket
    int qd = -1;
    if ((demi_socket(&qd, AF_INET, SOCK_DGRAM, 0)) != 0) {
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
	/*
    int n = 1;
    if (setsockopt(qd, SOL_SOCKET,
                   SO_BROADCAST, (char *)&n, sizeof(n)) < 0) {
        PWarning("Failed to set SO_BROADCAST on socket");
    }
	*/

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
				 PPanic("Failed to listen for DKUDP connections");
		}
    } else {
        // Registering a client. Bind to any available host/port
        BindToPort(qd, "198.0.0.1", "9000");        
    }

    /*
	// TS note: we register a Timer for the Demikernel "event loop" in Run(). 
    // Set up a libevent callback
    event *ev = event_new(libeventBase, qd, EV_READ | EV_PERSIST,
                          SocketCallback, (void *)this);
    event_add(ev, NULL);
    listenerEvents.push_back(ev);
    */

    // Tell the receiver its address
    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_port = htons(9000);
    if (inet_pton(AF_INET, "198.0.0.1", &sin.sin_addr) != 1) {
        PPanic("Failed to get socket name");
    }
    /*
    if (inet_pton(AF_INET, config.replica(replicaIdx).host.c_str(), &sin.sin_addr) != 1) {
        PPanic("Failed to get socket name");
    }
    */
    DKUDPTransportAddress *addr = new DKUDPTransportAddress(sin);
    receiver->SetAddress(addr);

    // Update mappings
    receivers[qd] = receiver;
    qds[receiver] = qd;

    Notice("Listening on DKUDP port %hu", ntohs(sin.sin_port));
}

static size_t
SerializeMessage(const ::google::protobuf::Message &m, char **out)
{
    string data = m.SerializeAsString();
    string type = m.GetTypeName();
    size_t typeLen = type.length();
    size_t dataLen = data.length();
    ssize_t totalLen = (sizeof(uint32_t) +
                        typeLen + sizeof(typeLen) +
                        dataLen + sizeof(dataLen));

    char *buf = new char[totalLen];
    char *ptr = buf;

    *(uint32_t *)ptr = NONFRAG_MAGIC;
    ptr += sizeof(uint32_t);
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
DKUDPTransport::SendMessageInternal(TransportReceiver *src,
                                  const DKUDPTransportAddress &dst,
                                  const Message &m,
                                  bool multicast)
{
    sockaddr_in sin = dynamic_cast<const DKUDPTransportAddress &>(dst).addr;

    // Serialize message
    char *buf;
    size_t msgLen = SerializeMessage(m, &buf);

	// We should be able to send each message as a single SGA no matter the size...?
    demi_sgarray_t sga = demi_sgaalloc(msgLen);
    sga.sga_numsegs = 1;
	memcpy(sga.sga_segs[0].sgaseg_buf, buf, msgLen);

    int qd = qds[src];
    [[maybe_unused]] int ret; 
    demi_qtoken_t t;
    demi_qresult_t wait_out;
	ret = demi_pushto(&t, qd, &sga, (const struct sockaddr *)&sin, sizeof(sin));
	ASSERT(ret == 0);
	ret = demi_wait(&wait_out, t, NULL);  // Waits for push to complete
	ASSERT(ret == 0);
	ASSERT(wait_out.qr_opcode == DEMI_OPC_PUSH);

	demi_sgafree(&sga);

    delete [] buf;
    return true;
}

void
DKUDPTransport::Run()
{
	Notice("In the run loop.");
	// Pop an initial token and "register" the Demikernel check in libevent w/ timer. 
        demi_qtoken_t token = -1; 
	for (const auto &it : receivers) {
	    int status = demi_pop(&token, it.first);
	    tokens.push_back(token);
	    if (status != 0) {
		return;
	    }
	}

    Notice("Registering the Demikernel timer in libevent.");
    DemiTimer(1);

    Notice("Dispatching the libevent base.");
	// Timer callbacks will trigger here. 
    event_base_dispatch(libeventBase);
}

// Helper function for OnReadable; called after a packet is read in
static void
DecodePacket(const char *buf, size_t sz, string &type, string &msg)
{
    [[maybe_unused]] ssize_t ssz = sz;
    const char *ptr = buf;
    size_t typeLen = *((size_t *)ptr);
    ptr += sizeof(size_t);
    ASSERT(ptr-buf < ssz);
        
    ASSERT(ptr+typeLen-buf < ssz);
    type = string(ptr, typeLen);
    ptr += typeLen;

    size_t msgLen = *((size_t *)ptr);
    ptr += sizeof(size_t);
    ASSERT(ptr-buf < ssz);

    ASSERT(ptr+msgLen-buf <= ssz);
    msg = string(ptr, msgLen);
    ptr += msgLen;
}

void
DKUDPTransport::OnReadable(demi_qresult_t &qr, TransportReceiver *receiver)
{
	// int qd = qr.qr_qd;
	demi_sgarray_t *sga = &qr.qr_value.sga;
	ASSERT(sga->sga_numsegs > 0); 
    
	// There should only ever be one segment, apparently
	for (ssize_t i = 0; i < sga->sga_numsegs; i++) {
		demi_sgaseg_t *seg = &sga->sga_segs[i];
        ssize_t sz = seg->sgaseg_len;
        char *buf = (char *)seg->sgaseg_buf;
        
        DKUDPTransportAddress senderAddr(sga->sga_addr);
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
	    Panic("Weren't supposed to get here...");
		/*
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
                                                    MAX_DKUDP_MESSAGE_SIZE));
            Notice("Received fragment of %zd byte packet %" PRIx64 " starting at %zd",
                   msgLen, msgId, fragStart);
            DKUDPTransportFragInfo &info = fragInfo[senderAddr];
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
	    */
        } else {
            Warning("Received packet with bad magic number");
        }

        // Was this received on a multicast qd?
	/*
        auto it = multicastConfigs.find(qd);
        if (it != multicastConfigs.end()) {
            // If so, deliver the message to all replicas for that
            // config, *except* if that replica was the sender of the
            // message.
            const specpaxos::Configuration *cfg = it->second;
            for (auto &kv : replicaReceivers[cfg]) {
                TransportReceiver *receiver = kv.second;
                const DKUDPTransportAddress &raddr = 
                    replicaAddresses[cfg].find(kv.first)->second;
                // Don't deliver a message to the sending replica
                if (raddr != senderAddr) {
                    receiver->ReceiveMessage(senderAddr, msgType, msg);
                }
            }
        } else {
	*/
        receiver->ReceiveMessage(senderAddr, msgType, msg);
    }
    
    demi_sgafree(sga);
}

int
DKUDPTransport::DemiTimer(uint64_t ms)
{
    DKUDPTransportTimerInfo *info = new DKUDPTransportTimerInfo();

    struct timeval tv;
    tv.tv_sec = ms/1000;
    tv.tv_usec = (ms % 1000) * 1000;
    
    info->transport = this;
    info->id = 0;
    info->ev = event_new(libeventBase, -1, 0,
                         DemiTimerCallback, info);

    timers[info->id] = info;
    
    event_add(info->ev, &tv);
    
    return info->id;
}

int
DKUDPTransport::Timer(uint64_t ms, timer_callback_t cb)
{
    DKUDPTransportTimerInfo *info = new DKUDPTransportTimerInfo();

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
DKUDPTransport::CancelTimer(int id)
{
    DKUDPTransportTimerInfo *info = timers[id];

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
DKUDPTransport::CancelAllTimers()
{
    while (!timers.empty()) {
        auto kv = timers.begin();
        CancelTimer(kv->first);
    }
}

void
DKUDPTransport::OnTimer(DKUDPTransportTimerInfo *info)
{
    timers.erase(info->id);
    event_del(info->ev);
    event_free(info->ev);
    
    info->cb();

    delete info;
}

void
DKUDPTransport::OnDemiTimer(DKUDPTransportTimerInfo *info)
{
    timers.erase(info->id);
    event_del(info->ev);
    event_free(info->ev);
    
    CheckQdCallback(info->transport);

    DemiTimer(1);

    delete info;
}

// Check if there is anything waiting in the demikernel queue and process. 
// Timer will be re-set in the caller
void
DKUDPTransport::CheckQdCallback(DKUDPTransport *transport)
{
    struct timespec ts; 
    ts.tv_sec = 0; 
    ts.tv_nsec = 0;  // TODO set this properly? Is setting to 0 OK? 
    
	int status = -1;
    demi_qresult_t wait_out;
	int ready_idx;
    demi_qtoken_t token = -1;
    status = demi_wait_any(&wait_out, &ready_idx, 
		    transport->tokens.data(), transport->tokens.size(), &ts);
    
    // if we got an EOK back from wait
    if (status == 0) {
        Debug("Found something: qd=%d", wait_out.qr_qd);
	// process request
	demi_sgarray_t &sga = wait_out.qr_value.sga;
	assert(sga.sga_numsegs > 0);
	transport->OnReadable(wait_out, receivers[wait_out.qr_qd]);
	status = demi_pop(&token, wait_out.qr_qd);
    }
    
    if (status > 0 && status != ETIMEDOUT)  {
        Warning("Something went wrong---not resetting the timer");
		FatalCallback(status);  // Maybe not the right input to FatalCallback... 
    }

    if (status == 0) {
        tokens[ready_idx] = token;
    } 
}

void
DKUDPTransport::TimerCallback(evutil_socket_t qd, short what, void *arg)
{
    DKUDPTransport::DKUDPTransportTimerInfo *info =
        (DKUDPTransport::DKUDPTransportTimerInfo *)arg;

    ASSERT(what & EV_TIMEOUT);

    info->transport->OnTimer(info);
}

void
DKUDPTransport::DemiTimerCallback(evutil_socket_t qd, short what, void *arg)
{
    DKUDPTransport::DKUDPTransportTimerInfo *info =
        (DKUDPTransport::DKUDPTransportTimerInfo *)arg;

    ASSERT(what & EV_TIMEOUT);

    info->transport->OnDemiTimer(info);
}

void
DKUDPTransport::LogCallback(int severity, const char *msg)
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
DKUDPTransport::FatalCallback(int err)
{
    Panic("Fatal libevent error: %d", err);
}

void
DKUDPTransport::SignalCallback(evutil_socket_t qd, short what, void *arg)
{
    Notice("Terminating on SIGTERM/SIGINT");
    DKUDPTransport *transport = (DKUDPTransport *)arg;
    event_base_loopbreak(transport->libeventBase);
}

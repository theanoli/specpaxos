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

#include "lib/beehive_lib.h"

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

string
DKUDPTransport::get_host(const TransportAddress &addr)
{
    return inet_ntoa(((struct sockaddr_in *)&(dynamic_cast<const DKUDPTransportAddress *>(&addr)->addr))->sin_addr); 
}

string
DKUDPTransport::get_port(const TransportAddress &addr)
{
    return std::to_string(htons(((struct sockaddr_in *)&(dynamic_cast<const DKUDPTransportAddress *>(&addr))->addr)->sin_port)); 
}

DKUDPTransportAddress
DKUDPTransport::LookupAddress(const string &host, const string &port)
{
    int res;
    struct addrinfo hints;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = 0;
    hints.ai_flags    = 0;
    struct addrinfo *ai;
    if ((res = getaddrinfo(host.c_str(), port.c_str(), &hints, &ai))) {
        Panic("Failed to resolve %s:%s: %s",
              host.c_str(), port.c_str(), gai_strerror(res));
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
    struct sockaddr_in sin = {0};
    int int_port = -1; 

    sscanf(port.c_str(), "%d", &int_port);
    sin.sin_family = AF_INET;
    sin.sin_port = htons(int_port);

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
}
    
void
DKUDPTransport::SetupLibevent()
{
    // Set up libevent
    Notice("Grabbing lock to set up libevent");
    std::unique_lock<std::mutex> tlk(tm);

    event_set_log_callback(LogCallback);
    event_set_fatal_callback(FatalCallback);

    if (evthread_use_pthreads() != 0) {
        Panic("Couldn't set up libevent + pthreads");
    }
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

    setup_complete = true;
    Notice("Done setting up libevent");
    tlk.unlock();
    tcv.notify_all();
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
    Notice("Registering receiver with%s Beehive serialization layer", 
		    USE_BEEHIVE ? "" : "out");
    ASSERT(replicaIdx < config.n);

    this->replicaIdx = replicaIdx;

    RegisterConfiguration(receiver, config, replicaIdx);

    // Create socket
    int qd = -1;
    if ((demi_socket(&qd, AF_INET, SOCK_DGRAM, 0)) != 0) {
        PPanic("Failed to create socket to listen");
    }

    string host;
    string port;
    
    if (replicaIdx != -1) {
        // Registering a replica.
        // host/port
        host = config.replica(replicaIdx).host;
        port = config.replica(replicaIdx).port;
    } else {
        // Registering a client.
	host = config.client().host;
	port = config.client().port;
    }
    BindToPort(qd, host, port);

    // Tell the receiver its address
    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_port = htons(stoi(port));
    if (inet_pton(AF_INET, host.c_str(), &sin.sin_addr) != 1) {
        PPanic("Failed to get socket name");
    }

    DKUDPTransportAddress *addr = new DKUDPTransportAddress(sin);
    receiver->SetAddress(addr);

    // Update mappings
    receivers[qd] = receiver;
    qds[receiver] = qd;
}

static size_t BaseSerializeMessage(const ::google::protobuf::Message &m, char **out)
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

static size_t
SerializeMessage(const ::google::protobuf::Message &m, char **out)
{
    size_t result = 0;
    if (USE_BEEHIVE == 1) {
        //if (!CheckMessage(m)) {
        //    Panic("Serialization returned a different message");
        //}
        result = SerializeMessageBeehive(m, out);
        if (result == 0) { 
            Panic("Fatal Beehive serialization error");
        }
    }
    else {
        result = BaseSerializeMessage(m, out);
    }

    return result;
}

bool
DKUDPTransport::SendMessageInternal(TransportReceiver *src,
                                  const DKUDPTransportAddress &dst,
                                  const Message &m,
                                  bool multicast)
{
    sockaddr_in sin = dynamic_cast<const DKUDPTransportAddress &>(dst).addr;

    // Serialize message
    Notice("Preparing to seralize message to %s:%d", 
		    inet_ntoa(sin.sin_addr), htons(sin.sin_port));
    char *buf;
    size_t msgLen = SerializeMessage(m, &buf);
    Notice("Serialized");

	// We should be able to send each message as a single SGA no matter the size...?
    demi_sgarray_t sga = demi_sgaalloc(msgLen);
    memcpy(sga.sga_segs[0].sgaseg_buf, buf, msgLen);
    Notice("Allocated array and memcpied");

    int qd = qds[src];
    [[maybe_unused]] int ret; 
    demi_qtoken_t t = -1;
    demi_qresult_t wait_out = {}; 
    Notice("Pushing");
    ret = demi_pushto(&t, qd, &sga, (const struct sockaddr *)&sin, sizeof(sin));
    if (ret != 0) {
	Panic("Problem pushing to demiqueue");	    
    }
    Notice("Done pushing, waiting");
    ret = demi_wait(&wait_out, t, NULL);  // Waits for push to complete
    Notice("Done waiting, freeing sga");
    if (ret != 0) {
	Panic("Problem waiting for push to complete");	    
    }
    if (wait_out.qr_opcode != DEMI_OPC_PUSH) {
	Panic("Something weird---got wrong return opcode!");
    }
    
    demi_sgafree(&sga);

    delete [] buf;
    return true;
}

void
DKUDPTransport::Run()
{
    SetupLibevent();

    Debug("In the run loop.");
    // Pop an initial token and "register" the Demikernel check in libevent w/ timer. 
    demi_qtoken_t token = -1; 
    for (const auto &it : receivers) {
        int status = demi_pop(&token, it.first);
        tokens.push_back(token);
        if (status != 0) {
	    return;
        }
    }

    Debug("Registering the Demikernel timer in libevent; %ld tokens.", tokens.size());
    DemiTimer();

    Debug("Dispatching the libevent base.");
    event_base_dispatch(libeventBase);
    Notice("Exited the libevent loop!");
}

// Helper function for OnReadable; called after a packet is read in
static void BaseDecodePacket(const char *buf, size_t sz, string &type, string &msg)
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

static void
DecodePacket(const char *buf, size_t sz, string &type, string &msg)
{
    if (USE_BEEHIVE == 1) {
        DecodePacketBeehive(buf, sz, type, msg);
    }
    else {
        BaseDecodePacket(buf, sz, type, msg);
    }
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
	senderAddr.addr.sin_port = htons(senderAddr.addr.sin_port);
	Debug("Got something to read from %s:%d!",
		    inet_ntoa(senderAddr.addr.sin_addr), htons(senderAddr.addr.sin_port));
	// TODO how can we avoid copying the message body twice (once to create this string, 
	// once to hand it off to the receiver's thread? 
	// Does it matter? These messages are so small. Maybe just go with it and optimize 
	// later if the performance is really bad. We can do an easy comparison between 
	// the single-shard case without threading and the single-shard case with threading
	// to better understand the performnace hit we may get from this. 
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
        } else {
            Warning("Received packet with bad magic number");
        }

        receiver->ReceiveMessage(senderAddr, msgType, msg);
    }
    
    demi_sgafree(sga);
}

int
DKUDPTransport::DemiTimer()
{
    DKUDPTransportTimerInfo *info = new DKUDPTransportTimerInfo();

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    
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

    info->transport->DemiTimer();

    delete info;
}

// Check if there is anything waiting in the demikernel queue and process. 
// Timer will be re-set in the caller
void
DKUDPTransport::CheckQdCallback(DKUDPTransport *transport)
{
    struct timespec ts; 
    ts.tv_sec = 0; 
    ts.tv_nsec = 1000;
    
	int status = -1;
    demi_qresult_t wait_out = {};
	int ready_idx = -1;
    demi_qtoken_t token = -1;
    status = demi_wait_any(&wait_out, &ready_idx, 
		    transport->tokens.data(), transport->tokens.size(), &ts);
    
    // if we got an EOK back from wait
    // TODO how can we service multiple queue descriptors here? 
    if (status == 0) {
        Debug("Found something: qd=%d", wait_out.qr_qd);
	// process request
	demi_sgarray_t &sga = wait_out.qr_value.sga;
	assert(sga.sga_numsegs > 0);
	transport->OnReadable(wait_out, transport->receivers[wait_out.qr_qd]);
	status = demi_pop(&token, wait_out.qr_qd);
        if (status == 0) {
            transport->tokens[ready_idx] = token;
	    return;
        }
    }

    if (status != 0 && status != ETIMEDOUT)  {
        Warning("Something went wrong---not resetting the timer");
	FatalCallback(status);  // Maybe not the right input to FatalCallback... 
    }
}

void
DKUDPTransport::TimerCallback(evutil_socket_t qd, short what, void *arg)
{
	Debug("Timer went off!\n");
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

// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * dktransport.cc:
 *   message-passing network interface that uses DK message delivery
 *   and libasync
 *
 * Copyright 2013 Dan R. K. Ports  <drkp@cs.washington.edu>
 * Copyright 2018 Irene Zhang  <iyzhang@cs.washington.edu>
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
#include "lib/dktransport.h"
#include "demi/wait.h"
#include "lib/latency.h"

#include <google/protobuf/message.h>
#include <event2/event.h>
#include <event2/thread.h>

#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <netdb.h>
#include <functional>

const size_t MAX_Dk_SIZE = 100; // XXX
const uint32_t MAGIC = 0x06121983;
static bool stopLoop = false;

static void
DkSignalCallback(int signal)
{
    ASSERT(signal == SIGTERM || signal == SIGINT);
    Warning("Set stop loop from signal");
    Latency_DumpAll();
    exit(0);
}

using std::make_pair;

DEFINE_LATENCY(process_pop);
DEFINE_LATENCY(process_push);
DEFINE_LATENCY(push_msg);
DEFINE_LATENCY(protobuf_serialize);
DEFINE_LATENCY(run_app);

DkTransportAddress::DkTransportAddress(const sockaddr_in &addr)
    : addr(addr)
{
    memset((void *)addr.sin_zero, 0, sizeof(addr.sin_zero));
}

DkTransportAddress::DkTransportAddress()
{
    memset((void *)&addr, 0, sizeof(addr));
}

DkTransportAddress *
DkTransportAddress::clone() const
{
    DkTransportAddress *c = new DkTransportAddress(*this);
    return c;    
}

bool operator==(const DkTransportAddress &a, const DkTransportAddress &b)
{
    return (a.addr.sin_addr.s_addr == b.addr.sin_addr.s_addr);
}

bool operator!=(const DkTransportAddress &a, const DkTransportAddress &b)
{
    return !(a == b);
}

bool operator<(const DkTransportAddress &a, const DkTransportAddress &b)
{
    return (memcmp(&a.addr, &b.addr, sizeof(a.addr)) < 0);
}

DkTransportAddress
DkTransport::LookupAddress(const specpaxos::ReplicaAddress &addr)
{
        int res;
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = 0;
        hints.ai_flags    = 0;
        struct addrinfo *ai;
        if ((res = getaddrinfo(addr.host.c_str(),
                               addr.port.c_str(),
                               &hints, &ai))) {
            Panic("Failed to resolve %s:%s: %s",
                  addr.host.c_str(), addr.port.c_str(), gai_strerror(res));
        }
        if (ai->ai_addr->sa_family != AF_INET) {
            Panic("getaddrinfo returned a non IPv4 address");
        }
        DkTransportAddress out =
            DkTransportAddress(*((sockaddr_in *)ai->ai_addr));
        freeaddrinfo(ai);
        return out;
}

DkTransportAddress
DkTransport::LookupAddress(const specpaxos::Configuration &config,
                            int idx)
{
    const specpaxos::ReplicaAddress &addr = config.replica(idx);
    return LookupAddress(addr);
}

static void
BindToPort(int qd, const string &host, const string &port)
{
    struct sockaddr_in sin;

    // look up its hostname and port number (which
    // might be a service name)
    struct addrinfo hints;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = 0;
    hints.ai_flags    = AI_PASSIVE;
    struct addrinfo *ai;
    int res;
    if ((res = getaddrinfo(host.c_str(),
                           port.c_str(),
                           &hints, &ai))) {
        Panic("Failed to resolve host/port %s:%s: %s",
              host.c_str(), port.c_str(), gai_strerror(res));
    }
    ASSERT(ai->ai_family == AF_INET);
    ASSERT(ai->ai_socktype == SOCK_STREAM);
    if (ai->ai_addr->sa_family != AF_INET) {
        Panic("getaddrinfo returned a non IPv4 address");        
    }
    sin = *(sockaddr_in *)ai->ai_addr;
    freeaddrinfo(ai);
    Debug("Binding to %s %d Dk", inet_ntoa(sin.sin_addr), htons(sin.sin_port));

    if (demi_bind(qd, (sockaddr *)&sin, sizeof(sin)) != 0) {
        PPanic("Failed to bind to socket");
    }
}

// TODO This interface needs to be fixed in .h
// evbase should be NULL when called
DkTransport::DkTransport(double dropRate, double reorderRate,
			   int dscp, event_base *evbase)
{
    char *argv[] = {};
    demi_init(0, argv);

    lastTimerId = 0;
    
    /*
    demi_queue(&timerQD);
    ASSERT(timerQD != 0);
    */
    evthread_use_pthreads();
    libeventBase = event_base_new();
    evthread_make_base_notifiable(libeventBase);

    // Set up signal handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    // Setup the sighub handler
    sa.sa_handler = &DkSignalCallback;
    // Restart the system call, if at all possible
    sa.sa_flags = SA_RESTART;
    
    // Block every signal during the handler
    sigfillset(&sa.sa_mask);
    
    // Intercept SIGHUP and SIGINT
    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        Panic("Cannot handle SIGTERM");
    }
    
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        Panic("Error: cannot handle SIGINT"); // Should not happen
    }
    Debug("Using Dk transport");
}

DkTransport::~DkTransport()
{
    // XXX Shut down libevent?

    //stopLoop = true;
    // for (auto kv : timers) {
    //     delete kv.second;
    // }
}

void
DkTransport::ConnectDk(TransportReceiver *src, const DkTransportAddress &dst)
{
    // Create socket
    int qd;
    if (demi_socket(&qd, AF_INET, SOCK_STREAM, 0) != 0) {
        PPanic("Failed to create queue for outgoing Dk connection");
    }

    //this->receiver = src;
    int res;
    demi_qtoken_t t;
    demi_qresult_t wait_out;
    if ((res = demi_connect(&t, qd,
			    (struct sockaddr *)&(dst.addr),
			    sizeof(dst.addr))) != 0 ||
        (res = demi_wait(&wait_out, t, NULL)) != 0) {
        Panic("Failed to connect %s:%d: %s",
              inet_ntoa(dst.addr.sin_addr),
              htons(dst.addr.sin_port),
              strerror(res));
        return;
    }

    // Tell the receiver its address
    struct sockaddr_in sin;
    socklen_t sinsize = sizeof(sin);
	/*
	// TODO check this
    if (demi_getsockname(qd, (sockaddr *) &sin, &sinsize) < 0) {
        PPanic("Failed to get socket name");
    }
	*/
    DkTransportAddress *addr = new DkTransportAddress(sin);
    src->SetAddress(addr);

    dkOutgoing.insert(make_pair(dst, qd));
    dkIncoming.insert(make_pair(qd, dst));
    receivers[qd] = src;

    Debug("My sockname %s:%d",
	  inet_ntoa(sin.sin_addr), htons(sin.sin_port));

    Debug("Opened Dk connection to %s:%d",
	  inet_ntoa(dst.addr.sin_addr), htons(dst.addr.sin_port));

    demi_qtoken_t token;
    // add new queue to wait
    if (demi_pop(&token, qd) == 0)
        tokens.push_back(token);
    else
        CloseConn(qd);

}

void
DkTransport::Register(TransportReceiver *receiver,
                       const specpaxos::Configuration &config,
                       int replicaIdx)
{
    ASSERT(replicaIdx < config.n);
    struct sockaddr_in sin;

    //const specpaxos::Configuration *canonicalConfig =
    RegisterConfiguration(receiver, config, replicaIdx);
    this->replicaIdx = replicaIdx;
    // Clients don't need to accept Dk connections
    if (replicaIdx == -1) {
        return;
    }
    
    // Create socket
    int qd;
    if (demi_socket(&qd, AF_INET, SOCK_STREAM, 0) != 0) {
        PPanic("Failed to create socket to accept Dk connections");
    }
    // Registering a replica. Bind socket to the designated
    // host/port
    const string &host = config.replica(replicaIdx).host;
    const string &port = config.replica(replicaIdx).port;
    BindToPort(qd, host, port);
    
    // Listen for connections
    if (demi_listen(qd, 5) != 0) {
        PPanic("Failed to listen for Dk connections");
    }
        
    // Set up queue to receive connections
    this->acceptQD = qd;
    // Set up receiver to processes calls
    this->receiver = receiver;    
    
    // Tell the receiver its address
    socklen_t sinsize = sizeof(sin);
    
    if (demi_getsockname(qd, (sockaddr *) &sin, &sinsize) < 0) {
        PPanic("Failed to get socket name");
    }
    
    DkTransportAddress *addr = new DkTransportAddress(sin);
    receiver->SetAddress(addr);

    Debug("Accepting connections on Dk port %hu", ntohs(sin.sin_port));
}

bool
DkTransport::SendMessageInternal(TransportReceiver *src,
                                  const DkTransportAddress &dst,
                                  const Message &m,
                                  bool multicast)
{
    Latency_Start(&process_push);
    auto it = dkOutgoing.find(dst);
    // See if we have a connection open
    if (it == dkOutgoing.end()) {
        ConnectDk(src, dst);
        it = dkOutgoing.find(dst);
    }

    if (it == dkOutgoing.end()) {
        Debug("could not find connection");
        return false;
    }
    
    int qd = it->second;
    
    // Serialize message
    Latency_Start(&protobuf_serialize);
    string data = m.SerializeAsString();
    Latency_End(&protobuf_serialize);
    string type = m.GetTypeName();
    size_t typeLen = type.length();
    size_t dataLen = data.length();
    size_t totalLen = (typeLen + sizeof(typeLen) +
                       dataLen + sizeof(dataLen) +
                       sizeof(totalLen) +
                       sizeof(uint32_t));

    
    void *p = malloc(totalLen);
    assert(p != NULL);
    char *buf = reinterpret_cast<char *>(p);
    demi_sgarray_t sga;
    sga.sga_numsegs = 1;
    sga.sga_segs[0].sgaseg_buf = p;
    sga.sga_segs[0].sgaseg_len = totalLen;
    char *ptr = buf;

    *((uint32_t *) ptr) = MAGIC;
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

    ASSERT((size_t)(ptr-buf) <= totalLen);
    ASSERT((size_t)(ptr+dataLen-buf) == totalLen);
    memcpy(ptr, data.c_str(), dataLen);
    ptr += dataLen;

    Latency_Start(&push_msg);
    demi_qtoken_t t;
    int ret = demi_push(&t, qd, &sga);
    ASSERT(ret == 0);
    
    demi_qresult_t wait_out;
    ret = demi_wait(&wait_out, t, NULL);
    ASSERT(ret == 0);
    Latency_End(&push_msg);

    Debug("Sent %ld byte %s message to server over Dk",
          totalLen, type.c_str());
    free(buf);
    Latency_End(&process_push);
    return true;
}

void
DkTransport::CloseConn(int qd)
{
    int status = demi_close(qd);
    dkOutgoing.erase(dkIncoming[qd]);
    dkIncoming.erase(qd);
    receivers.erase(qd);
}
    


void
DkTransport::Run()
{
    demi_qtoken_t token;
    int status = 0;
    stopLoop = false;

	// Initial tokens to wait on; clients will not receive anything until they 
	// send, and it will block on receive, so we need a timer event to send things
    if (replicaIdx == -1) {
		// Check timer on clients; event_base_loop does single check for events 
		// event_base_loop(libeventBase);
		status = demi_pop(&token, timerQD);
    } else {
        // check accept on servers
        status = demi_accept(&token, acceptQD);
    }
	tokens.push_back(token);
    if (status != 0) {
        return;
    }
    while (!stopLoop) {
        demi_qresult_t wait_out;
        int ready_idx;

        int status = demi_wait_any(&wait_out, &ready_idx, tokens.data(), tokens.size());

        // if we got an EOK back from wait
        if (status == 0) {
            Debug("Found something: qd=%lx",
                  wait_out.qr_qd);
			/*
				// check timer on clients
				if (replicaIdx == -1 && wait_out.qr_qd == timerQD) {
			demi_sgarray_t &sga = wait_out.qr_value.sga;
					assert(sga.sga_numsegs == 1);
					OnTimer(reinterpret_cast<DkTransportTimerInfo *>(sga.sga_buf));
					status = demi_pop(&token, timerQD);
				} else if (wait_out.qr_qd == acceptQD) {
			// */
			if (wait_out.qr_qd == acceptQD) {
				// check accept on servers
				DkAcceptCallback(wait_out.qr_value.ares);
				// call accept again
				status = demi_accept(&token, acceptQD);
			} else {
				// process request
				demi_sgarray_t &sga = wait_out.qr_value.sga;
				assert(sga.sga_numsegs > 0);
				DkPopCallback(wait_out.qr_qd, receivers[wait_out.qr_qd], sga);
				status = demi_pop(&token, wait_out.qr_qd);
			}

			// ...otherwise check the client timer
			// This is wrong. It will only be checked if demi_wait_any is triggered and 
			// we apparently want to get here when the timeout elapses, which is independent
			// of the tokens we are waiting on in demi_wait_any.
			// if (replicaIdx == -1) {
			// 	event_base_loop(libeventBase);
			// }
        } // else fall through, typically connection closed
	
        if (status == 0)
            tokens[ready_idx] = token;
        else {
            if (wait_out.qr_qd == acceptQD)  // || wait_out.qr_qd == timerQD)
                break;
            //assert(status == ECONNRESET || status == ECONNABORTED);
            CloseConn(wait_out.qr_qd);
            tokens.erase(tokens.begin()+ready_idx);
        }
    }
    Warning("Exited loop");
    Latency_DumpAll();
}

void
DkTransport::Stop()
{
    for (auto &it : receivers) {
        int qd = it.first;
        CloseConn(qd);
    }
    Warning("Stop loop  was called");
    stopLoop = true;
}

int
DkTransport::Timer(uint64_t ms, timer_callback_t cb)
{
    DkTransportTimerInfo *info = new DkTransportTimerInfo();

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
DkTransport::CancelTimer(int id)
{
    DkTransportTimerInfo *info = timers[id];

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
DkTransport::CancelAllTimers()
{
    while (!timers.empty()) {
        auto kv = timers.begin();
        CancelTimer(kv->first);
    }
}

void
DkTransport::OnTimer(DkTransportTimerInfo *info)
{
    timers.erase(info->id);
    event_del(info->ev);
    event_free(info->ev);
    
    info->cb();

    delete info;
}

void
DkTransport::DkAcceptCallback(demi_accept_result ares)
{
    int newqd = ares.qd;
    demi_qtoken_t token;
    DkTransportAddress client = DkTransportAddress(ares.addr); 
    dkOutgoing[client] = newqd;
    dkIncoming.insert(std::pair<int, DkTransportAddress>(newqd,client));
    receivers[newqd] = receiver;
 
    // add new queue to wait
    if (demi_pop(&token, newqd) == 0) {
        tokens.push_back(token);
    
        Debug("Opened incoming Dk connection from %s:%d",
              inet_ntoa(ares.addr.sin_addr), htons(ares.addr.sin_port));
    } else {
        CloseConn(newqd);
    }
}

void
DkTransport::DkPopCallback(int qd,
                           TransportReceiver *receiver,
                           demi_sgarray_t &sga)
    
{
    Debug("Pop Callback");

    
    Latency_Start(&process_pop);
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
    Latency_Start(&run_app);
    receiver->ReceiveMessage(addr->second,
                             type,
                             data);
    Latency_End(&run_app);
    free(sga.sga_buf);
    Latency_End(&process_pop);

    Debug("Done processing large %s message", type.c_str());        
}

void
DkTransport::TimerCallback(evutil_socket_t fd, short what, void *arg)
{
    DkTransport::DkTransportTimerInfo *info =
        (DkTransport::DkTransportTimerInfo *)arg;

    ASSERT(what & EV_TIMEOUT);

    info->transport->OnTimer(info);
}

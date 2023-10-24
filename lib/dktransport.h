// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * dktransport.h:
 *   message-passing network interface that uses DK message delivery
 *   and libasync
 *
 * Copyright 2013 Dan R. K. Ports  <drkp@cs.washington.edu>
 * Copyright 2018 Irene Zhang <iyzhang@cs.washington.edu>
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

#ifndef _LIB_DKTRANSPORT_H_
#define _LIB_DKTRANSPORT_H_

#include "lib/configuration.h"
#include "lib/transport.h"
#include "lib/transportcommon.h"
#include "demi/libos.h"
#include <event2/event.h>

#include <map>
#include <list>
#include <random>
#include <mutex>
#include <netinet/in.h>
#include <signal.h>

#define MAX_CONNECTIONS 1000

class DkTransportAddress : public TransportAddress
{
public:
    DkTransportAddress * clone() const;
    DkTransportAddress();
private:
    DkTransportAddress(const sockaddr_in &addr);

    sockaddr_in addr;
    friend class DkTransport;
    friend bool operator==(const DkTransportAddress &a,
                           const DkTransportAddress &b);
    friend bool operator!=(const DkTransportAddress &a,
                           const DkTransportAddress &b);
    friend bool operator<(const DkTransportAddress &a,
                          const DkTransportAddress &b);
};

class DkTransport : public TransportCommon<DkTransportAddress>
{
public:
    DkTransport(double dropRate = 0.0, double reogrderRate = 0.0,
                    int dscp = 0, event_base *evbase = nullptr);
    virtual ~DkTransport();
    void Register(TransportReceiver *receiver,
                  const specpaxos::Configuration &config,
                  int replicaIdx);
    void Run();
    void Stop();
    int Timer(uint64_t ms, timer_callback_t cb);
    bool CancelTimer(int id);
    void CancelAllTimers();

private:
    int acceptQD;
    int replicaIdx;
    TransportReceiver *receiver;
     
    struct DkTransportTimerInfo
    {
        DkTransport *transport;
        timer_callback_t cb;
        event *ev;
        int id;
    };

    event_base *libeventBase;
    std::map<int, DkTransportTimerInfo *> timers;

    std::map<int, TransportReceiver*> receivers; // qd -> receiver
    //std::map<TransportReceiver*, int> qds; // receiver -> qd
    int lastTimerId;
    std::vector<demi_qtoken_t> tokens;
    std::map<DkTransportAddress, int> dkOutgoing;
    std::map<int, DkTransportAddress> dkIncoming;

    bool SendMessageInternal(TransportReceiver *src,
                             const DkTransportAddress &dst,
                             const Message &m, bool multicast = false);

    DkTransportAddress
    LookupAddress(const specpaxos::ReplicaAddress &addr);
    DkTransportAddress
    LookupAddress(const specpaxos::Configuration &cfg,
                  int replicaIdx);
    const DkTransportAddress *
    LookupMulticastAddress(const specpaxos::Configuration*config) { return NULL; };

    void ConnectDk(TransportReceiver *src, const DkTransportAddress &dst);
    void OnTimer(DkTransportTimerInfo *info);
    static void TimerCallback(evutil_socket_t fd, short what, void *arg);
    void DkAcceptCallback(demi_accept_result ares);
    void DkPopCallback(int qd, TransportReceiver *receiver, demi_sgarray_t &sga);
    void CloseConn(int qd);
};

#endif  // _LIB_DKTRANSPORT_H_

// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * dkudptransport.h:
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

#ifndef _LIB_DKUDPTRANSPORT_H_
#define _LIB_DKUDPTRANSPORT_H_

#include "lib/configuration.h"
#include "lib/transport.h"
#include "lib/transportcommon.h"

#include <event2/event.h>

#include <demi/libos.h>
#include <demi/sga.h>
#include <demi/wait.h>

#include <map>
#include <list>
#include <vector>
#include <unordered_map>
#include <random>
#include <netinet/in.h>

class DKUDPTransportAddress : public TransportAddress
{
public:
    DKUDPTransportAddress * clone() const;
private:
    DKUDPTransportAddress(const sockaddr_in &addr);
    sockaddr_in addr;
    friend class DKUDPTransport;
    friend bool operator==(const DKUDPTransportAddress &a,
                           const DKUDPTransportAddress &b);
    friend bool operator!=(const DKUDPTransportAddress &a,
                           const DKUDPTransportAddress &b);
    friend bool operator<(const DKUDPTransportAddress &a,
                          const DKUDPTransportAddress &b);
};

class DKUDPTransport : public TransportCommon<DKUDPTransportAddress>
{
public:
    DKUDPTransport(double dropRate = 0.0, double reorderRate = 0.0,
                 int dscp = 0, event_base *evbase = nullptr);
    virtual ~DKUDPTransport();
    void Register(TransportReceiver *receiver,
                  const specpaxos::Configuration &config,
                  int replicaIdx);
    void Run();
    int Timer(uint64_t ms, timer_callback_t cb);
    int DemiTimer(uint64_t ms);
    bool CancelTimer(int id);
    void CancelAllTimers();
    
private:
    int acceptQD;
    int replicaIdx; 

    struct DKUDPTransportTimerInfo
    {
        DKUDPTransport *transport;
        timer_callback_t cb;
        event *ev;
        int id;
    };

    double dropRate;
    double reorderRate;
    std::uniform_real_distribution<double> uniformDist;
    std::default_random_engine randomEngine;
    struct
    {
        bool valid;
        DKUDPTransportAddress *addr;
        string msgType;
        string message;
        int qd;
    } reorderBuffer;
    int dscp;

    event_base *libeventBase;
    // std::vector<event *> listenerEvents;
    std::vector<event *> signalEvents;
    std::vector<demi_qtoken_t> tokens;
    std::map<int, TransportReceiver*> receivers; // qd -> receiver
    std::map<TransportReceiver*, int> qds; // receiver -> qd
    std::map<const specpaxos::Configuration *, int> multicastQds;
    std::map<int, const specpaxos::Configuration *> multicastConfigs;
    int lastTimerId;
    std::map<int, DKUDPTransportTimerInfo *> timers;
    uint64_t lastFragMsgId;
    struct DKUDPTransportFragInfo
    {
        uint64_t msgId;
        string data;
    };
    std::map<DKUDPTransportAddress, DKUDPTransportFragInfo> fragInfo;

    bool SendMessageInternal(TransportReceiver *src,
                             const DKUDPTransportAddress &dst,
                             const Message &m, bool multicast = false);
    DKUDPTransportAddress
    LookupAddress(const specpaxos::ReplicaAddress &addr);
    DKUDPTransportAddress
    LookupAddress(const specpaxos::Configuration &cfg,
                  int replicaIdx);
    const DKUDPTransportAddress *
    LookupMulticastAddress(const specpaxos::Configuration *cfg);
    void OnTimer(DKUDPTransportTimerInfo *info);
    void OnDemiTimer(DKUDPTransportTimerInfo *info);
    void OnReadable(demi_qresult_t &qr, TransportReceiver *receiver);
    void CheckQdCallback(DKUDPTransport *transport); 
    static void TimerCallback(evutil_socket_t qd,
                              short what, void *arg);
    static void DemiTimerCallback(evutil_socket_t qd,
                              short what, void *arg);
    static void LogCallback(int severity, const char *msg);
    static void FatalCallback(int err);
    static void SignalCallback(evutil_socket_t qd,
                               short what, void *arg);
};

#endif  // _LIB_DKUDPTRANSPORT_H_

// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * udptransport.h:
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

#ifndef _LIB_UDPTRANSPORT_H_
#define _LIB_UDPTRANSPORT_H_

#include "lib/configuration.h"
#include "lib/transport.h"
#include "lib/transportcommon.h"

#include <event2/event.h>

#include <map>
#include <list>
#include <vector>
#include <unordered_map>
#include <random>
#include <netinet/in.h>

class UDPTransportAddress : public TransportAddress
{
public:
    UDPTransportAddress * clone() const;
private:
    UDPTransportAddress(const sockaddr_in &addr);
    sockaddr_in addr;
    friend class UDPTransport;
    friend bool operator==(const UDPTransportAddress &a,
                           const UDPTransportAddress &b);
    friend bool operator!=(const UDPTransportAddress &a,
                           const UDPTransportAddress &b);
    friend bool operator<(const UDPTransportAddress &a,
                          const UDPTransportAddress &b);
};

class UDPTransport : public TransportCommon<UDPTransportAddress>
{
public:
    UDPTransport(double dropRate = 0.0, double reorderRate = 0.0,
                 int dscp = 0, event_base *evbase = nullptr);
    virtual ~UDPTransport();
    void Register(TransportReceiver *receiver,
                  const specpaxos::Configuration &config,
                  int replicaIdx);
    void Run();
    int Timer(uint64_t ms, timer_callback_t cb);
    bool CancelTimer(int id);
    void CancelAllTimers();
    
private:
    int acceptQD;
    int replicaIdx; 

    struct UDPTransportTimerInfo
    {
        UDPTransport *transport;
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
        UDPTransportAddress *addr;
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
    std::map<int, UDPTransportTimerInfo *> timers;
    uint64_t lastFragMsgId;
    struct UDPTransportFragInfo
    {
        uint64_t msgId;
        string data;
    };
    std::map<UDPTransportAddress, UDPTransportFragInfo> fragInfo;

    bool SendMessageInternal(TransportReceiver *src,
                             const UDPTransportAddress &dst,
                             const Message &m, bool multicast = false);
    UDPTransportAddress
    LookupAddress(const specpaxos::ReplicaAddress &addr);
    UDPTransportAddress
    LookupAddress(const specpaxos::Configuration &cfg,
                  int replicaIdx);
    const UDPTransportAddress *
    LookupMulticastAddress(const specpaxos::Configuration *cfg);
    void ListenOnMulticastPort(const specpaxos::Configuration
                               *canonicalConfig);
    void OnReadable(int qd);
    void OnTimer(UDPTransportTimerInfo *info);
    static void SocketCallback(evutil_socket_t qd,
                               short what, void *arg);
    static void TimerCallback(evutil_socket_t qd,
                              short what, void *arg);
    static void LogCallback(int severity, const char *msg);
    static void FatalCallback(int err);
    static void SignalCallback(evutil_socket_t qd,
                               short what, void *arg);
};

#endif  // _LIB_UDPTRANSPORT_H_

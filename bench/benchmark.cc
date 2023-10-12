// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * benchmark.cpp:
 *   simple replication benchmark client
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

#include "bench/benchmark.h"
#include "common/client.h"
#include "lib/latency.h"
#include "lib/message.h"
#include "lib/transport.h"
#include "lib/timeval.h"

#include <sys/time.h>
#include <string>
#include <sstream>
#include <algorithm>
#include <string.h>

namespace specpaxos {
    
DEFINE_LATENCY(op);

BenchmarkClient::BenchmarkClient(Client &client, Transport &transport,
                                 int numRequests, uint64_t delay,
                                 int warmupSec,
                                 uint64_t payload_size,
                                 string latencyFilename
                                )
    : client(client), transport(transport),
      numRequests(numRequests), delay(delay),
      warmupSec(warmupSec), latencyFilename(latencyFilename), payload_size(payload_size)
{
    if (delay != 0) {
        Notice("Delay between requests: %" PRIu64 " ms", delay);        
    }
    started = false;
    done = false;
    cooldownDone = false;
    _Latency_Init(&latency, "op");
    latencies.reserve(numRequests);
}

void
BenchmarkClient::Start()
{
    n = 0;
    transport.Timer(warmupSec * 1000,
                    std::bind(&BenchmarkClient::WarmupDone,
                               this));
    SendNext();
}

void
BenchmarkClient::WarmupDone()
{
    started = true;
    Notice("Completed warmup period of %d seconds with %d requests",
           warmupSec, n);
    gettimeofday(&startTime, NULL);
    n = 0;
}

void
BenchmarkClient::CooldownDone()
{

    char buf[1024];
    cooldownDone = true;
    Notice("Finished cooldown period.");
}

void
BenchmarkClient::SendNext()
{
    char request_buf[this->payload_size];
    memset(request_buf, (n + 64)%32, this->payload_size);

//    std::ostringstream msg;
//    msg << "request" << n;
    string request(request_buf, payload_size);
//    Notice("request size is %lu", request.size());

    Latency_Start(&latency);
    client.Invoke(request, std::bind(&BenchmarkClient::OnReply,
                                       this,
                                       std::placeholders::_1,
                                       std::placeholders::_2));
}

void
BenchmarkClient::OnReply(const string &request, const string &reply)
{
    if (cooldownDone) {
        return;
    }
    
    if ((started) && (!done) && (n != 0)) {
        uint64_t ns = Latency_End(&latency);
        latencies.push_back(ns);
        if (n > numRequests) {
            Finish();
        }
    }
    
    n++;
    if (delay == 0) {
       SendNext();
    } else {
        uint64_t rdelay = rand() % delay*2;
        transport.Timer(rdelay,
                        std::bind(&BenchmarkClient::SendNext, this));
    }
}

void
BenchmarkClient::Finish()
{
    gettimeofday(&endTime, NULL);
    
    struct timeval diff = timeval_sub(endTime, startTime);

    Notice("Completed %d requests in " FMT_TIMEVAL_DIFF " seconds",
           numRequests, VA_TIMEVAL_DIFF(diff));
    done = true;

    transport.Timer(warmupSec * 1000,
                    std::bind(&BenchmarkClient::CooldownDone,
                              this));


    if (latencyFilename.size() > 0) {
        Latency_FlushTo(latencyFilename.c_str());
    }
}


} // namespace specpaxos

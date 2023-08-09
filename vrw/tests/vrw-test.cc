// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * vrw-test.cc:
 *   test cases for Viewstamped Replication protocol
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

#include "lib/configuration.h"
#include "lib/message.h"
#include "lib/transport.h"
#include "lib/simtransport.h"

#include "common/client.h"
#include "common/replica.h"
#include "vrw/client.h"
#include "vrw/replica.h"
#include "vrw/witness.h"

#include <stdlib.h>
#include <stdio.h>
#include <gtest/gtest.h>
#include <vector>
#include <sstream>

static string replicaLastOp;
static string clientLastOp;
static string clientLastReply;

using google::protobuf::Message;
using namespace specpaxos;
using namespace specpaxos::vrw;
using namespace specpaxos::vrw::proto;

class VRWTestApp : public AppReplica
{
public:
    VRWTestApp() { };
    virtual ~VRWTestApp() { };

    virtual void ReplicaUpcall(opnum_t opnum, const string &req, string &reply) {
        ops.push_back(req);
        reply = "reply: " + req;
    }
     
    virtual void UnloggedUpcall(const string &req, string &reply) {
        unloggedOps.push_back(req);
        reply = "unlreply: " + req;
    }

    std::vector<string> ops;
    std::vector<string> unloggedOps;

};

class VRWTest : public  ::testing::TestWithParam<std::pair<int, int>>
{
protected:
    std::vector<VRWTestApp *> apps;
    std::vector<Replica *> replicas;
    VRWClient *client;
    SimulatedTransport *transport;
    Configuration *config;
    int requestNum;
    
    virtual void SetUp() {
		int n = GetParam().first;
		int f = n/2;
		int batchSize = GetParam().second;

		int port = 12345;

		std::vector<ReplicaAddress> replicaAddrs; 
		for (int i = 0; i < n; i++) {
			std::string portStr = std::to_string(port + i);
			replicaAddrs.push_back({"localhost", portStr});
		}
        config = new Configuration(n, f, replicaAddrs);

        transport = new SimulatedTransport();
        
        for (int i = 0; i < config->n; i++) {
            apps.push_back(new VRWTestApp());
			if (IsWitness(i)) {
				replicas.push_back(new VRWWitness(*config, i, true, transport, batchSize, apps[i])); 
			} else {
				replicas.push_back(new VRWReplica(*config, i, true, transport, batchSize, apps[i]));
			}
        }

        client = new VRWClient(*config, transport);
        requestNum = -1;

        // Only let tests run for a simulated minute. This prevents
        // infinite retry loops, etc.
//        transport->Timer(60000, [&]() {
//                transport->CancelAllTimers();
//            });
    }

    virtual string RequestOp(int n) {
        std::ostringstream stream;
        stream << "test: " << n;
        return stream.str();
    }

    virtual string LastRequestOp() {
        return RequestOp(requestNum);
    }
    
    virtual void ClientSendNext(Client::continuation_t upcall) {
        requestNum++;
        client->Invoke(LastRequestOp(), upcall);
    }

    virtual void ClientSendNextUnlogged(int idx, Client::continuation_t upcall,
                                        Client::timeout_continuation_t timeoutContinuation = nullptr,
                                        uint32_t timeout = Client::DEFAULT_UNLOGGED_OP_TIMEOUT) {
        requestNum++;
        client->InvokeUnlogged(idx, LastRequestOp(), upcall, timeoutContinuation, timeout);
    }
    
    virtual void TearDown() {
        for (auto x : replicas) {
            delete x;
        }
        for (auto a : apps) {
            delete a;
        }
        apps.clear();
        replicas.clear();

        delete client;
        delete transport;
        delete config;
    }
};

TEST_P(VRWTest, OneOp)
{
    auto upcall = [this](const string &req, const string &reply) {
        EXPECT_EQ(req, LastRequestOp());
        EXPECT_EQ(reply, "reply: "+LastRequestOp());

        // Not guaranteed that any replicas except the leader have
        // executed this request.
        EXPECT_EQ(apps[0]->ops.back(), req);
        transport->CancelAllTimers();
    };
    
    ClientSendNext(upcall);
    transport->Run();

    // By now, they all should have executed the last request, except the 
	// witnesses.
    for (int i = 0; i < config->n; i++) {
		if (IsWitness(i)) {
			continue;
		}
		EXPECT_EQ(apps[i]->ops.size(), 1);
		EXPECT_EQ(apps[i]->ops.back(),  LastRequestOp());
    }
}

TEST_P(VRWTest, Unlogged)
{
	/*
	 * Sends an unlogged request to a specific replica. 
	 */
    auto upcall = [this](const string &req, const string &reply) {
        EXPECT_EQ(req, LastRequestOp());
        EXPECT_EQ(reply, "unlreply: "+LastRequestOp());

        EXPECT_EQ(apps[2]->unloggedOps.back(), req);
        transport->CancelAllTimers();
    };
    int timeouts = 0;
    auto timeout = [&](const string &req) {
        timeouts++;
    };
    
    ClientSendNextUnlogged(2, upcall, timeout);
    transport->Run();

    for (size_t i = 0; i < apps.size(); i++) {
		if (IsWitness(i)) {
			continue;
		}
        EXPECT_EQ(0, apps[i]->ops.size());
	}

    for (size_t i = 0; i < apps.size(); i++) {
        EXPECT_EQ((i == 2 ? 1 : 0), apps[i]->unloggedOps.size());
    }
    EXPECT_EQ(0, timeouts);
}

TEST_P(VRWTest, UnloggedTimeout)
{
    auto upcall = [this](const string &req, const string &reply) {
        FAIL();
        transport->CancelAllTimers();
    };
    int timeouts = 0;
    auto timeout = [&](const string &req) {
        timeouts++;
    };

    // Drop messages to or from replica 2 (i.e., non-witness)
    transport->AddFilter(10, [](TransportReceiver *src, int srcIdx,
                                TransportReceiver *dst, int dstIdx,
                                Message &m, uint64_t &delay) {
                             if ((srcIdx == 2) || (dstIdx == 2)) {
                                 return false;
                             }
                             return true;
                         });

    // Run for 10 seconds
    transport->Timer(10000, [&]() {
            transport->CancelAllTimers();
        });

    ClientSendNextUnlogged(2, upcall, timeout);
    transport->Run();

    for (size_t i = 0; i < apps.size(); i++) {
        EXPECT_EQ(0, apps[i]->ops.size());
        EXPECT_EQ(0, apps[i]->unloggedOps.size());
    }
    EXPECT_EQ(1, timeouts);
}


TEST_P(VRWTest, ManyOps)
{
    Client::continuation_t upcall = [&](const string &req, const string &reply) {
        EXPECT_EQ(req, LastRequestOp());
        EXPECT_EQ(reply, "reply: "+LastRequestOp());

        // Not guaranteed that any replicas except the leader have
        // executed this request.
        EXPECT_EQ(apps[0]->ops.back(), req);

        if (requestNum < 9) {
            ClientSendNext(upcall);
        } else {
            transport->CancelAllTimers();
        }
    };
    
    ClientSendNext(upcall);
    transport->Run();

    // By now, they all should have executed the last request.
    for (int i = 0; i < config->n; i+=2) {
        EXPECT_EQ(10, apps[i]->ops.size());
        for (int j = 0; j < 10; j++) {
            EXPECT_EQ(RequestOp(j), apps[i]->ops[j]);            
        }
    }

	// TODO check the log size. It should be quite short (TODO how short?)
}

TEST_P(VRWTest, FailedReplica)
{
    Client::continuation_t upcall = [&](const string &req, const string &reply) {
        EXPECT_EQ(req, LastRequestOp());
        EXPECT_EQ(reply, "reply: "+LastRequestOp());

        // Not guaranteed that any replicas except the leader have
        // executed this request.
        EXPECT_EQ(apps[0]->ops.back(), req);

        if (requestNum < 9) {
            ClientSendNext(upcall);
        } else {
            transport->CancelAllTimers();
        }
    };
    
    ClientSendNext(upcall);

    // Drop messages to or from replica 2
    transport->AddFilter(10, [](TransportReceiver *src, int srcIdx,
                                TransportReceiver *dst, int dstIdx,
                                Message &m, uint64_t &delay) {
                             if ((srcIdx == 2) || (dstIdx == 2)) {
                                 return false;
                             }
                             return true;
                         });
    
    transport->Run();

    // By now, they all should have executed the last request (except
	// the "failed" replica)
    for (int i = 0; i < config->n; i++) {
        if (i == 2) {
            EXPECT_EQ(0, apps[i]->ops.size());
			EXPECT_EQ(0, static_cast<VRWReplica *>(replicas[i])->GetLogSize());
        } else {
			if (!IsWitness(i)) {
				// Replicas should have executed these ops
				EXPECT_EQ(10, apps[i]->ops.size());
				for (int j = 0; j < 10; j++) {
					EXPECT_EQ(RequestOp(j), apps[i]->ops[j]);            
				}
				// Non-failed replicas should have full log
				EXPECT_EQ(10, static_cast<VRWReplica *>(replicas[i])->GetLogSize());
			} else {
				// Witnesses should have full log
				EXPECT_EQ(10, static_cast<VRWWitness *>(replicas[i])->GetLogSize());
			}
		}
    }
}

TEST_P(VRWTest, StateTransfer)
{
    Client::continuation_t upcall = [&](const string &req, const string &reply) {
        EXPECT_EQ(req, LastRequestOp());
        EXPECT_EQ(reply, "reply: "+LastRequestOp());

        // Not guaranteed that any replicas except the leader have
        // executed this request.
        EXPECT_EQ(apps[0]->ops.back(), req);

        if (requestNum == 5) {
            // Restore replica 2
            transport->RemoveFilter(10);
        }

        if (requestNum < 9) {
            ClientSendNext(upcall);
        } else {
            transport->CancelAllTimers();
        }
    };
    
    ClientSendNext(upcall);

    // Drop messages to or from replica 2 (this kicks in before any messages are 
	// sent/received to any nodes)
    transport->AddFilter(10, [](TransportReceiver *src, int srcIdx,
                                TransportReceiver *dst, int dstIdx,
                                Message &m, uint64_t &delay) {
                             if ((srcIdx == 2) || (dstIdx == 2)) {
                                 return false;
                             }
                             return true;
                         });
    
    transport->Run();

    // By now, they all should have executed the last request.
    for (int i = 0; i < config->n; i++) {
		if (!IsWitness(i)) {
			EXPECT_EQ(10, apps[i]->ops.size());
			for (int j = 0; j < 10; j++) {
				EXPECT_EQ(RequestOp(j), apps[i]->ops[j]);            
			}
			EXPECT_EQ(2, static_cast<VRWReplica *>(replicas[i])->GetLogSize());
		} else {
			EXPECT_EQ(2, static_cast<VRWWitness *>(replicas[i])->GetLogSize());
		}
    }
}


TEST_P(VRWTest, FailedLeader)
{
    Client::continuation_t upcall = [&](const string &req, const string &reply) {
        EXPECT_EQ(req, LastRequestOp());
        EXPECT_EQ(reply, "reply: "+LastRequestOp());

        if (requestNum == 5) {
            // Drop messages to or from replica 0
            transport->AddFilter(10, [](TransportReceiver *src, int srcIdx,
                                        TransportReceiver *dst, int dstIdx,
                                        Message &m, uint64_t &delay) {
                                     if ((srcIdx == 0) || (dstIdx == 0)) {
                                         return false;
                                     }
                                     return true;
                                 });
        }
        if (requestNum < 9) {
            ClientSendNext(upcall);
        } else {
            transport->CancelAllTimers();
        }
    };
    
    ClientSendNext(upcall);
    
    transport->Run();

    // By now, they all should have executed the last request.
    for (int i = 0; i < config->n; i++) {
		if (IsWitness(i)) {
			continue;
		}
        if (i == 0) {
            continue;
        }
        EXPECT_EQ(10, apps[i]->ops.size());
        for (int j = 0; j < 10; j++) {
            EXPECT_EQ(RequestOp(j), apps[i]->ops[j]);            
        }
    }
}

TEST_P(VRWTest, DroppedReply)
{
    bool received = false;
    Client::continuation_t upcall = [&](const string &req, const string &reply) {
        EXPECT_EQ(req, LastRequestOp());
        EXPECT_EQ(reply, "reply: "+LastRequestOp());
        transport->CancelAllTimers();
        received = true;
    };

    // Drop the first ReplyMessage
    bool dropped = false;
    transport->AddFilter(10, [&dropped](TransportReceiver *src, int srcIdx,
                                        TransportReceiver *dst, int dstIdx,
                                        Message &m, uint64_t &delay) {
                             ReplyMessage r;
                             if (m.GetTypeName() == r.GetTypeName()) {
                                 if (!dropped) {
                                     dropped = true;
                                     return false;
                                 }
                             }
                             return true;
                         });
    ClientSendNext(upcall);
    
    transport->Run();

    EXPECT_TRUE(received);
    
    // Each replica should have executed only one request
    for (int i = 0; i < config->n; i++) {
		if (IsWitness(i)) {
			continue;
		}
        EXPECT_EQ(1, apps[i]->ops.size());
   }
}

TEST_P(VRWTest, DroppedReplyThenFailedLeader)
{
    bool received = false;
    Client::continuation_t upcall = [&](const string &req, const string &reply) {
        EXPECT_EQ(req, LastRequestOp());
        EXPECT_EQ(reply, "reply: "+LastRequestOp());
        transport->CancelAllTimers();
        received = true;
    };

    // Drop the first ReplyMessage
    bool dropped = false;
    transport->AddFilter(10, [&dropped](TransportReceiver *src, int srcIdx,
                                        TransportReceiver *dst, int dstIdx,
                                        Message &m, uint64_t &delay) {
                             ReplyMessage r;
                             if (m.GetTypeName() == r.GetTypeName()) {
                                 if (!dropped) {
                                     dropped = true;
                                     return false;
                                 }
                             }
                             return true;
                         });

    // ...and after we've done that, fail the leader altogether
    transport->AddFilter(20, [&dropped](TransportReceiver *src, int srcIdx,
                                        TransportReceiver *dst, int dstIdx,
                                        Message &m, uint64_t &delay) {
                             if ((srcIdx == 0) || (dstIdx == 0)) {
                                 return !dropped;
                             }
                             return true;
                         });
    
    ClientSendNext(upcall);
    
    transport->Run();

    EXPECT_TRUE(received);
    
    // Each replica should have executed only one request
    // (and actually the faulty one should too, but don't check that)
    for (int i = 0; i < config->n; i++) {
		if (IsWitness(i)) {
			continue;
		}
        if (i != 0) {
            EXPECT_EQ(1, apps[i]->ops.size());            
        }
    }
}

TEST_P(VRWTest, ManyClients)
{
    const int NUM_CLIENTS = 10;
    const int MAX_REQS = 100;
    
    std::vector<VRWClient *> clients;
    std::vector<int> lastReq;
    std::vector<Client::continuation_t> upcalls;
    for (int i = 0; i < NUM_CLIENTS; i++) {
        clients.push_back(new VRWClient(*config, transport));
        lastReq.push_back(0);
        upcalls.push_back([&, i](const string &req, const string &reply) {
                EXPECT_EQ("reply: "+RequestOp(lastReq[i]), reply);
                lastReq[i] += 1;
                if (lastReq[i] < MAX_REQS) {
                    clients[i]->Invoke(RequestOp(lastReq[i]), upcalls[i]);
                }
            });
        clients[i]->Invoke(RequestOp(lastReq[i]), upcalls[i]);
    }

    // This could take a while; simulate two hours
    transport->Timer(7200000, [&]() {
            transport->CancelAllTimers();
        });

    transport->Run();

    for (int i = 0; i < config->n; i++) {
		if (IsWitness(i)) {
			continue;
		}
        ASSERT_EQ(NUM_CLIENTS * MAX_REQS, apps[i]->ops.size());
    }

    for (int i = 0; i < NUM_CLIENTS*MAX_REQS; i++) {
        for (int j = 0; j < config->n; j++) {
			if (IsWitness(j)) {
				continue;
			}
            ASSERT_EQ(apps[0]->ops[i], apps[j]->ops[i]);
        }
    }

    for (VRWClient *c : clients) {
        delete c;
    }
}

/*
 * TODO: this may not work, since we are truncating the logs. The recovering
 * node will not have the state from the earliest operations. May need to 
 * update recovery code on the replicas? Or update this test to keep "persisted" 
 * state?
TEST_P(VRWTest, Recovery)
{
    Client::continuation_t upcall = [&](const string &req, const string &reply) {
        EXPECT_EQ(req, LastRequestOp());
        EXPECT_EQ(reply, "reply: "+LastRequestOp());

        if (requestNum == 5) {
            // Drop messages to or from replica 0
            transport->AddFilter(10, [](TransportReceiver *src, int srcIdx,
                                        TransportReceiver *dst, int dstIdx,
                                        Message &m, uint64_t &delay) {
                                     if ((srcIdx == 0) || (dstIdx == 0)) {
                                         return false;
                                     }
                                     return true;
                                 });
        }
        if (requestNum == 7) {
            // Destroy and recover replica 0
            delete apps[0];
            delete replicas[0];
            transport->RemoveFilter(10);
            apps[0] = new VRWTestApp();
            replicas[0] = new VRWReplica(*config, 0, false,
                                        transport, GetParam(), apps[0]);
        }
        if (requestNum < 9) {
            transport->Timer(10000, [&]() {
                    ClientSendNext(upcall);
                });
        } else {
            transport->CancelAllTimers();
        }
    };
    
    ClientSendNext(upcall);
    
    transport->Run();

    // By now, they all should have executed the last request,
    // including the recovered replica 0
    for (int i = 0; i < config->n; i++) {
        EXPECT_EQ(10, apps[i]->ops.size());
        for (int j = 0; j < 10; j++) {
            EXPECT_EQ(RequestOp(j), apps[i]->ops[j]);            
        }
    }
}
*/

TEST_P(VRWTest, StressDropClientReqs)
{
    const int NUM_CLIENTS = 10;
    const int MAX_REQS = 1000;
    const int MAX_DELAY = 1;
    const int DROP_PROBABILITY = 10; // 1/x
    
    std::vector<VRWClient *> clients;
    std::vector<int> lastReq;
    std::vector<Client::continuation_t> upcalls;
    for (int i = 0; i < NUM_CLIENTS; i++) {
        clients.push_back(new VRWClient(*config, transport));
        lastReq.push_back(0);
        upcalls.push_back([&, i](const string &req, const string &reply) {
                EXPECT_EQ("reply: "+RequestOp(lastReq[i]), reply);
                lastReq[i] += 1;
                if (lastReq[i] < MAX_REQS) {
                    clients[i]->Invoke(RequestOp(lastReq[i]), upcalls[i]);
                }
            });
        clients[i]->Invoke(RequestOp(lastReq[i]), upcalls[i]);
    }

    srand(time(NULL));
    
    // Delay messages from clients by a random amount, and drop some
    // of them
    transport->AddFilter(10, [=](TransportReceiver *src, int srcIdx,
                                TransportReceiver *dst, int dstIdx,
                                Message &m, uint64_t &delay) {
                             if (srcIdx == -1) {
                                 delay = rand() % MAX_DELAY;
                             }
                             return ((rand() % DROP_PROBABILITY) != 0);
                         });
    
    // This could take a while; simulate two hours
    transport->Timer(7200000, [&]() {
            transport->CancelAllTimers();
        });

    transport->Run();

    for (int i = 0; i < config->n; i++) {
		if (IsWitness(i)) {
			continue;
		}
        ASSERT_EQ(NUM_CLIENTS * MAX_REQS, apps[i]->ops.size());
    }

    for (int i = 0; i < NUM_CLIENTS*MAX_REQS; i++) {
        for (int j = 0; j < config->n; j++) {
			if (IsWitness(j)) {
				continue;
			}
            ASSERT_EQ(apps[0]->ops[i], apps[j]->ops[i]);
        }
    }

    for (VRWClient *c : clients) {
        delete c;
    }
}

TEST_P(VRWTest, StressDropNodeReqs)
{
    const int NUM_CLIENTS = 10;
    const int MAX_REQS = 1000;
    const int MAX_DELAY = 1;
    const int DROP_PROBABILITY = 10; // 1/x
    
    std::vector<VRWClient *> clients;
    std::vector<int> lastReq;
    std::vector<Client::continuation_t> upcalls;
    for (int i = 0; i < NUM_CLIENTS; i++) {
        clients.push_back(new VRWClient(*config, transport));
        lastReq.push_back(0);
        upcalls.push_back([&, i](const string &req, const string &reply) {
                EXPECT_EQ("reply: "+RequestOp(lastReq[i]), reply);
                lastReq[i] += 1;
                if (lastReq[i] < MAX_REQS) {
                    clients[i]->Invoke(RequestOp(lastReq[i]), upcalls[i]);
                }
            });
        clients[i]->Invoke(RequestOp(lastReq[i]), upcalls[i]);
    }

    srand(time(NULL));
    
    // Delay messages from nodes by a random amount, and drop some
    // of them
    transport->AddFilter(10, [=](TransportReceiver *src, int srcIdx,
                                TransportReceiver *dst, int dstIdx,
                                Message &m, uint64_t &delay) {
                             if (srcIdx > -1) {
                                 delay = rand() % MAX_DELAY;
                             }
                             return ((rand() % DROP_PROBABILITY) != 0);
                         });
    
    // This could take a while; simulate two hours
    transport->Timer(7200000, [&]() {
            transport->CancelAllTimers();
        });

    transport->Run();

    for (int i = 0; i < config->n; i++) {
		if (IsWitness(i)) {
			continue;
		}
        ASSERT_EQ(NUM_CLIENTS * MAX_REQS, apps[i]->ops.size());
    }

    for (int i = 0; i < NUM_CLIENTS*MAX_REQS; i++) {
        for (int j = 0; j < config->n; j++) {
			if (IsWitness(j)) {
				continue;
			}
            ASSERT_EQ(apps[0]->ops[i], apps[j]->ops[i]);
        }
    }

    for (VRWClient *c : clients) {
        delete c;
    }
}

TEST_P(VRWTest, StressDropAnyReqs)
{
    const int NUM_CLIENTS = 10;
    const int MAX_REQS = 1000;
    const int DROP_PROBABILITY = 3; // 1/x
    
    std::vector<VRWClient *> clients;
    std::vector<int> lastReq;
    std::vector<Client::continuation_t> upcalls;
    for (int i = 0; i < NUM_CLIENTS; i++) {
        clients.push_back(new VRWClient(*config, transport, i));
        lastReq.push_back(0);
        upcalls.push_back([&, i](const string &req, const string &reply) {
                EXPECT_EQ("reply: "+RequestOp(lastReq[i]), reply);
                lastReq[i] += 1;
                if (lastReq[i] < MAX_REQS) {
                    clients[i]->Invoke(RequestOp(lastReq[i]), upcalls[i]);
                }
            });
        clients[i]->Invoke(RequestOp(lastReq[i]), upcalls[i]);
    }
	int dropIdx = std::numeric_limits<int>::max();  // Invalid dropIdx means drop nothing
	auto t = time(NULL);
    srand(t);
	Notice("Seed: %lu", t); 
    
    // Delay messages from clients by a random amount, and drop some
    // of them
    transport->AddFilter(10, [&dropIdx](TransportReceiver *src, int srcIdx,
                                TransportReceiver *dst, int dstIdx,
                                Message &m, uint64_t &delay) {
							 auto p = rand() % DROP_PROBABILITY;
							 if (dropIdx == std::numeric_limits<int>::max() && p == 0) {
							 	// Maybe drop this src's packets for a bit 
								dropIdx = srcIdx; 
							 } else if (dropIdx == srcIdx && p > 0) {
							 	// Stop dropping src's packets
								dropIdx = std::numeric_limits<int>::max(); 
							 }
							 return dropIdx != srcIdx;
                         });
    
    // This could take a while; simulate two hours
    transport->Timer(7200000, [&]() {
            transport->CancelAllTimers();
        });

    transport->Run();

    for (int i = 0; i < config->n; i++) {
		if (IsWitness(i)) {
			continue;
		}
        ASSERT_EQ(NUM_CLIENTS * MAX_REQS, apps[i]->ops.size());
    }

    for (int i = 0; i < NUM_CLIENTS*MAX_REQS; i++) {
        for (int j = 0; j < config->n; j++) {
			if (IsWitness(j)) {
				continue;
			}
            ASSERT_EQ(apps[0]->ops[i], apps[j]->ops[i]);
        }
    }

    for (VRWClient *c : clients) {
        delete c;
    }
}


// Parameter here is the batch size
INSTANTIATE_TEST_CASE_P(Batching,
                        VRWTest,
                        ::testing::Values(std::pair<int, int>(3, 1), 
							std::pair<int, int>(3, 8), 
							std::pair<int, int>(5, 1),
							std::pair<int, int>(5, 8)));

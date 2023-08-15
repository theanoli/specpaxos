// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
// vim: set ts=4 sw=4:
/***********************************************************************
 *
 * kvstore/client.h:
 *   KVStore client-side logic and APIs
 *
 **********************************************************************/

#ifndef _KV_CLIENT_H_
#define _KV_CLIENT_H_

#include "lib/assert.h"
#include "lib/message.h"
#include "lib/udptransport.h"
#include "common/client.h"
#include "lib/configuration.h"
#include "spec/client.h"
#include "vr/client.h"
#include "vrw/client.h"
#include "fastpaxos/client.h"
#include "kvstore/request.pb.h"

#include <iostream>
#include <cstdlib>
#include <string>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <set>

namespace kvstore {

using namespace std;

enum Proto {
    PROTO_UNKNOWN,
    PROTO_VR,
    PROTO_VRW,
    PROTO_SPEC,
    PROTO_FAST
};

class Client
{
public:
    /* Constructor needs path to shard configs and number of shards. */
    Client(Proto mode, string configPath, int nshards);
    ~Client();

    /* API Calls for KVStore. */
    bool Get(const string &key, string &value);
    void Put(const string &key, const string &value);

private:
    long client_id; // Unique ID for this client.
    long nshards; // Number of shards in niStore

    UDPTransport transport; // Transport used by paxos client proxies.
    thread *clientTransport; // Thread running the transport event loop.

    vector<specpaxos::Client *> shard; // List of shard client proxies.

    mutex cv_m; // Synchronize access to all state in this class and cv.
    condition_variable cv; // To block api calls till a replica reply.

    /* Transaction specific variables. */
    bool status; // Whether to commit transaction & reply status.
    string replica_reply; // Reply back from a shard.

    /* Private helper functions. */
    void run_client(); // Runs the transport event loop.

    /* Callbacks for hearing back from a shard for an operation. */
    void getCallback(const int, const string &, const string &);
    void putCallback(const int, const string &, const string &);

    // Sharding logic: Given key, generates a number b/w 0 to nshards-1
    long key_to_shard(const string &key);
};

} // namespace kvstore

#endif /* _KV_CLIENT_H_ */

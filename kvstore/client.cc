// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
// vim: set ts=4 sw=4:
/***********************************************************************
 *
 * kvstore/client.cc:
 *   Single KVStore client. Implements the API functionalities.
 *
 **********************************************************************/

#include "kvstore/client.h"

namespace kvstore {

Client::Client(Proto mode, string configPath, int nShards, int threadIdx, 
		string host, string port)
    : transport(0.0, 0.0, 0)
{
    // Initialize all state here;
    struct timeval t1;
    gettimeofday(&t1, NULL);
    srand(t1.tv_sec + t1.tv_usec);
    client_id = rand();

    nshards = nShards;
    shard.reserve(nshards);

    Notice("Initializing KVStore client with id [%lu]", client_id);

    /* Start a client for each shard. */
    for (int i = 0; i < nShards; i++) {
        string shardConfigPath = configPath + to_string(i) + ".config";
        ifstream shardConfigStream(shardConfigPath);
        if (shardConfigStream.fail()) {
            fprintf(stderr, "unable to read configuration file: %s\n",
                    shardConfigPath.c_str());
            exit(0);
        }
        specpaxos::Configuration shardConfig(shardConfigStream);

	shardConfig.setClientAddress(host, port, threadIdx);

        if (mode != PROTO_VRW) {
	    NOT_REACHABLE();
	} else {
	    shard[i] = new specpaxos::vrw::VRWClient(shardConfig, &transport);
        }
    }

    /* Run the transport in a new thread. */
    clientTransport = new thread(&Client::run_client, this);

    Notice("KVStore client [%lu] created!", client_id);
}

Client::~Client()
{
    // TODO: Consider killing transport and associated thread.
}

/* Runs the transport event loop. */
void
Client::run_client()
{
    transport.Run();
    Notice("Client %lu has finished.", client_id);
}

/* Returns the value corresponding to the supplied key. */
bool
Client::Get(const string &key, string &value)
{
    // Contact the appropriate shard to get the value.
    unique_lock<mutex> lk(cv_m);

    int i = key_to_shard(key);

    // Send the GET operation to appropriate shard.
    Notice("[shard %d] Sending GET [%s]", i, key.c_str());
    string request_str;
    Request request;
    request.set_op(Request::GET);
    request.set_txnid(client_id);
    request.set_arg0(key);
    request.SerializeToString(&request_str);

    // Choose a client proxy to send the request to the VRW group
    // This is probably where we'd have another level of client proxies 
    // available to handle requests concurrently. But we would need to 
    // deal with the locks; we can possibly have an array of locks, one
    // for each client proxy. 
    // Does this work if we only have a single thread doing the client's
    // work? What would we need to do to handle more threads? Is transport
    // thread-safe?
    transport.Timer(0, [=]() {
        shard[i]->Invoke(request_str,
                          bind(&Client::getCallback,
                          this, i,
                          placeholders::_1,
                          placeholders::_2));
    });

    // Wait for reply from shard.
    Notice("[shard %d] Waiting for GET reply", i);
    cv.wait(lk);
    Notice("[shard %d] GET reply received", i);

    // Reply from shard should be available in "replica_reply".
    // This is a class variable. We can have an array of these, one for each
    // client proxy. 
    value = replica_reply;
    return status;
}

/* Sets the value corresponding to the supplied key. */
void
Client::Put(const string &key, const string &value)
{
    // Contact the appropriate shard to set the value.
    unique_lock<mutex> lk(cv_m);

    int i = key_to_shard(key);

    Notice("[shard %d] Sending PUT [%s]", i, key.c_str());
    string request_str;
    Request request;
    request.set_op(Request::PUT);
    request.set_txnid(client_id);
    request.set_arg0(key);
    request.set_arg1(value);
    request.SerializeToString(&request_str);

    transport.Timer(0, [=]() {
        shard[i]->Invoke(request_str,
                          bind(&Client::putCallback,
                          this, i,
                          placeholders::_1,
                          placeholders::_2));
    });

    // Wait for reply from shard.
    Notice("[shard %d] Waiting for PUT reply", i);
    cv.wait(lk);
    Notice("[shard %d] PUT reply received", i);

    // PUT operation should have suceeded. Return.
}


/* Callback from a shard replica on get operation completion. */
void
Client::getCallback(const int index, const string &request_str, const string &reply_str)
{
    lock_guard<mutex> lock(cv_m);

    // Copy reply to "replica_reply".
    Reply reply;
    reply.ParseFromString(reply_str);
    Notice("[shard %d] GET callback [%d]", index, reply.status());

    if (reply.status() >= 0) {
        status = true;
        replica_reply = reply.value();
    } else {
        status = false;
    }
    
    // Wake up thread waiting for the reply.
    cv.notify_all();
}

/* Callback from a shard replica on put operation completion. */
void
Client::putCallback(const int index, const string &request_str, const string &reply_str)
{
    lock_guard<mutex> lock(cv_m);

    // PUTs always returns success, so no need to check reply.
    Reply reply;
    reply.ParseFromString(reply_str);
    Notice("[shard %d] PUT callback [%d]", index, reply.status());

    // Wake up thread waiting for the reply.
    cv.notify_all();
}


/* Takes a key and returns which shard the key is stored in. */
long
Client::key_to_shard(const string &key)
{
    unsigned long hash = 0;
    const char* str = key.c_str();
    for (unsigned int i = 0; i < key.length(); i++) {
        hash = hash << 1 ^ str[i];
    }

    return (hash % nshards);
}

} // namespace kvstore

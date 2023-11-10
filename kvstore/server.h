// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
// vim: set ts=4 sw=4:
/***********************************************************************
 *
 * kvstore/server.h:
 *   KVStore application server logic
 *
 **********************************************************************/

#ifndef _KV_SERVER_H_
#define _KV_SERVER_H_

#include "lib/configuration.h"
#include "common/replica.h"
#include "lib/udptransport.h"
#include "lib/dkudptransport.h"
#include "vrw/replica.h"
#include "vrw/witness.h"
#include "kvstore/kvstore.h"

#include <functional>
#include <vector>

namespace kvstore {

using namespace std;

class Server : public specpaxos::AppReplica
{
public:
    // set up the store
    Server() {store = KVStore();};
    ~Server() {};
    void ReplicaUpcall(opnum_t opnum, const string &str1, string &str2);
	void LeaderUpcall(opnum_t opnum, const string &op, bool &replicate, string &res);
	void SetReadValidation(bool validate_reads);

private:
    // data store
    KVStore store;
	bool doReadValidation = false;  // Consistent but unlogged reads

    struct Operation
    {
        long id;  // client ID
        string op; // requested operation
        std::vector<string> args; // arguments
    };

    Operation parse(string str);
    vector<string> split(string str);
};

} // namespace kvstore

#endif /* _KV_SERVER_H_ */

// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
// vim: set ts=4 sw=4:
/***********************************************************************
 *
 * kvstore/kvstore.cc:
 *   Simple versioned key-value store
 *
 **********************************************************************/

#include "kvstore/kvstore.h"
#include "lib/assert.h"
#include "lib/message.h"

namespace kvstore {
using namespace std;

KVStore::KVStore() { }
    
KVStore::~KVStore() { }
    

bool
KVStore::get(const string &key, string &value)
{
    // check for existence of key in store
    if (store.find(key) == store.end() || store[key].empty()) {
        return false;
    } else {
        value = store[key].back();
	return true;
    }
}
    

bool
KVStore::put(const string &key, const string &value)
{
    store[key].push_back(value);
    return true;
}

} // namespace kvstore

// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
// vim: set ts=4 sw=4:
/***********************************************************************
 *
 * kvstore/kvstore.h:
 *   Simple historied key-value store
 *
 **********************************************************************/

#ifndef _KVSTORE_H_
#define _KVSTORE_H_

#include <unistd.h>
#include <stdlib.h>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <iostream>
#include <list>

namespace kvstore {
using namespace std;

class KVStore
{

public:
    KVStore();
    ~KVStore();

    bool get(const string &key, string &value);
    bool put(const string &key, const string &value);

private:
    /* Global store which keep key -> (timestamp, value) list. */
    unordered_map<string, list<string>> store;
};

} // namespace kvstore

#endif  /* _KVSTORE_H_ */

// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * vrw/replica.h:
 *   Viewstamped Replication protocol
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

#ifndef _VRW_REPLICA_H_
#define _VRW_REPLICA_H_

#include "lib/configuration.h"
#include "lib/latency.h"
#include "common/log.h"
#include "common/replica.h"
#include "common/quorumset.h"
#include "vrw/vrw-proto.pb.h"

#include <limits>
#include <map>
#include <memory>
#include <list>

namespace specpaxos {
namespace vrw {

// Want a custom function here, since we can end validation on receiving a NACK from 
// a replica (i.e., saying the leader is not the current leader)
class ValidateQuorumSet : public QuorumSet<std::pair<uint64_t, uint64_t>, proto::ValidateReplyMessage>
{
public: 
	ValidateQuorumSet(int numRequired) : QuorumSet<std::pair<uint64_t, uint64_t>, proto::ValidateReplyMessage>(numRequired) {}

    const std::map<int, proto::ValidateReplyMessage> *
    AddAndCheckForQuorumOrNack(std::pair<uint64_t, uint64_t> vs, int replicaIdx, const proto::ValidateReplyMessage &msg)
    {
        std::map<int, proto::ValidateReplyMessage> &vsmessages = messages[vs];
        if (vsmessages.find(replicaIdx) != vsmessages.end()) {
            // This is a duplicate message

            // But we'll ignore that, replace the old message from
            // this replica, and proceed.
            //
            // XXX Is this the right thing to do? It is for
            // speculative replies in SpecPaxos...
        }

        vsmessages[replicaIdx] = msg;
        
		if (!msg.isvalid()) {
			return &vsmessages;
		}

        return CheckForQuorum(vs);
    }
};

class VRWReplica : public Replica
{
public:
    VRWReplica(Configuration config, int myIdx, bool initialize,
              Transport *transport, int batchSize,
              AppReplica *app);
    ~VRWReplica();
    
    void AddToReceiveQueue(const TransportAddress &remote,
                        const string &type, const string &data);
	size_t GetLogSize();  // For testing

private:
    std::queue<std::tuple<TrasportAddress *, const string, const string>> receiveQueue; 
    view_t view;
    opnum_t lastCommitted;
    opnum_t lastOp;
    view_t lastRequestStateTransferView;
    opnum_t lastRequestStateTransferOpnum;
    uint64_t recoveryNonce;
    std::list<std::pair<TransportAddress *,
                        proto::PrepareMessage> > pendingPrepares;
    proto::PrepareMessage lastPrepare;
	proto::ValidateRequestMessage lastValidate;
    int batchSize;
    opnum_t lastBatchEnd;
    bool batchComplete;

	opnum_t cleanUpTo;
	std::vector<opnum_t> lastCommitteds; 
    
    Log log;

    std::map<uint64_t, std::unique_ptr<TransportAddress> > clientAddresses;
    struct ClientTableEntry
    {
		view_t needsReadValidation;  // only needed for validated reads
        uint64_t lastReqId;
        bool replied;
        proto::ReplyMessage reply;
    };
    std::map<uint64_t, ClientTableEntry> clientTable;
    
    QuorumSet<viewstamp_t, proto::PrepareOKMessage> prepareOKQuorum;
    QuorumSet<view_t, proto::StartViewChangeMessage> startViewChangeQuorum;
    QuorumSet<view_t, proto::DoViewChangeMessage> doViewChangeQuorum;
    QuorumSet<uint64_t, proto::RecoveryResponseMessage> recoveryResponseQuorum;
	
	// Key is std::pair<clientid, clientreqid>
    ValidateQuorumSet validateReadQuorum;

    Timeout *viewChangeTimeout;
    Timeout *nullCommitTimeout;
    Timeout *stateTransferTimeout;
    Timeout *resendPrepareTimeout;
    Timeout *resendValidateTimeout;
    Timeout *closeBatchTimeout;
    Timeout *recoveryTimeout;

    Latency_t requestLatency;
    Latency_t executeAndReplyLatency;

    uint64_t GenerateNonce() const;
    bool AmLeader() const;
    void CommitUpTo(opnum_t upto);
    void SendPrepareOKs(opnum_t oldLastOp);
    void SendRecoveryMessages();
    void RequestStateTransfer();
    void EnterView(view_t newview);
    void StartViewChange(view_t newview);
    void SendNullCommit();
    void UpdateClientTable(const Request &req);
    void ResendPrepare();
    void ResendValidate();
    void CloseBatch();
	opnum_t GetLowestReplicaCommit();
	void CleanLog();
    
    void HandleRequest(const TransportAddress &remote,
                       const proto::RequestMessage &msg);
    void HandleUnloggedRequest(const TransportAddress &remote,
                               const proto::UnloggedRequestMessage &msg);

    void HandleValidateRequest(const TransportAddress &remote,
                       const proto::ValidateRequestMessage &msg);
    void HandleValidateReply(const TransportAddress &remote,
                         const proto::ValidateReplyMessage &msg);
    
    void HandlePrepare(const TransportAddress &remote,
                       const proto::PrepareMessage &msg);
    void HandlePrepareOK(const TransportAddress &remote,
                         const proto::PrepareOKMessage &msg);
    void HandleCommit(const TransportAddress &remote,
                      const proto::CommitMessage &msg);
    void HandleRequestStateTransfer(const TransportAddress &remote,
                                    const proto::RequestStateTransferMessage &msg);
    void HandleStateTransfer(const TransportAddress &remote,
                             const proto::StateTransferMessage &msg);
    void HandleStartViewChange(const TransportAddress &remote,
                               const proto::StartViewChangeMessage &msg);
    void HandleDoViewChange(const TransportAddress &remote,
                            const proto::DoViewChangeMessage &msg);
    void HandleStartView(const TransportAddress &remote,
                         const proto::StartViewMessage &msg);
    void HandleRecovery(const TransportAddress &remote,
                        const proto::RecoveryMessage &msg);
    void HandleRecoveryResponse(const TransportAddress &remote,
                                const proto::RecoveryResponseMessage &msg);
    void ReceiveLoop(); 
    void ReceiveMessage(const TransportAddress &remote,
                        const string &type, const string &data);
};

} // namespace specpaxos::vrw
} // namespace specpaxos

#endif  /* _VRW_REPLICA_H_ */

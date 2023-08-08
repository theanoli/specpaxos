// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
/***********************************************************************
 *
 * vrw/replica.cc:
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

#include "common/replica.h"
#include "vrw/witness.h"
#include "vrw/vrw-proto.pb.h"

#include "lib/assert.h"
#include "lib/configuration.h"
#include "lib/latency.h"
#include "lib/message.h"
#include "lib/transport.h"

#include <algorithm>
#include <random>

#define RDebug(fmt, ...) Debug("[%d] " fmt, myIdx, ##__VA_ARGS__)
#define RNotice(fmt, ...) Notice("[%d] " fmt, myIdx, ##__VA_ARGS__)
#define RWarning(fmt, ...) Warning("[%d] " fmt, myIdx, ##__VA_ARGS__)
#define RPanic(fmt, ...) Panic("[%d] " fmt, myIdx, ##__VA_ARGS__)

namespace specpaxos {
namespace vrw {

using namespace proto;
    
VRWWitness::VRWWitness(Configuration config, int myIdx,
                     bool initialize,
                     Transport *transport, int batchSize,
                     AppReplica *app)
    : Replica(config, myIdx, initialize, transport, app),
      batchSize(batchSize),
	  lastCommitteds(config.n, 0),
      log(false),
      startViewChangeQuorum(config.QuorumSize()-1),
      recoveryResponseQuorum(config.QuorumSize())
{
    this->status = STATUS_NORMAL;
    this->view = 0;
    this->lastOp = 0;
    this->lastCommitted = 0;
    this->lastRequestStateTransferView = 0;
    this->lastRequestStateTransferOpnum = 0;

	this->cleanUpTo = 0;

    if (batchSize > 1) {
        Notice("Batching enabled; batch size %d", batchSize);
    }

    this->viewChangeTimeout = new Timeout(transport, 5000, [this,myIdx]() {
            RWarning("Have not heard from leader; starting view change");
			view_t step = 1;
			while (specpaxos::IsWitness(configuration.GetLeaderIndex(view + step))) {
				step++; 
			}
            StartViewChange(view + step);
        });
    this->stateTransferTimeout = new Timeout(transport, 1000, [this]() {
            this->lastRequestStateTransferView = 0;
            this->lastRequestStateTransferOpnum = 0;            
        });
    this->stateTransferTimeout->Start();
    this->recoveryTimeout = new Timeout(transport, 5000, [this]() {
            SendRecoveryMessages();
        });

    _Latency_Init(&requestLatency, "request");
    _Latency_Init(&executeAndReplyLatency, "executeAndReply");

    if (initialize) {
		viewChangeTimeout->Start();
    } else {
        this->status = STATUS_RECOVERING;
        this->recoveryNonce = GenerateNonce();
        SendRecoveryMessages();
        recoveryTimeout->Start();
    }
}

VRWWitness::~VRWWitness()
{
    Latency_Dump(&requestLatency);
    Latency_Dump(&executeAndReplyLatency);

    delete viewChangeTimeout;
    delete stateTransferTimeout;
    delete recoveryTimeout;
    
    for (auto &kv : pendingPrepares) {
        delete kv.first;
    }
}

uint64_t
VRWWitness::GenerateNonce() const
{
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    return dis(gen);    
}

void
VRWWitness::CommitUpTo(opnum_t upto)
{
    while (lastCommitted < upto) {
        Latency_Start(&executeAndReplyLatency);
        
        lastCommitted++;

        /* Find operation in log */
        const LogEntry *entry = log.Find(lastCommitted);
        if (!entry) {
            RPanic("Did not find operation " FMT_OPNUM " in log", lastCommitted);
        }

        /* Mark it as committed */
        log.SetStatus(lastCommitted, LOG_STATE_COMMITTED);

        Latency_End(&executeAndReplyLatency);
    }
}

void
VRWWitness::SendPrepareOKs(opnum_t oldLastOp)
{
    /* Send PREPAREOKs for new uncommitted operations */
    for (opnum_t i = oldLastOp; i <= lastOp; i++) {
        /* It has to be new *and* uncommitted */
        if (i <= lastCommitted) {
            continue;
        }

        const LogEntry *entry = log.Find(i);
        if (!entry) {
            RPanic("Did not find operation " FMT_OPNUM " in log", i);
        }
        ASSERT(entry->state == LOG_STATE_PREPARED);

        PrepareOKMessage reply;
        reply.set_view(view);
        reply.set_opnum(i);
        reply.set_replicaidx(myIdx);
		reply.set_lastcommitted(lastCommitted);

        RDebug("Sending PREPAREOK " FMT_VIEWSTAMP " for new uncommitted operation",
               reply.view(), reply.opnum());
    
        if (!(transport->SendMessageToReplica(this,
                                              configuration.GetLeaderIndex(view),
                                              reply))) {
            RWarning("Failed to send PrepareOK message to leader");
        }
    }
}

void
VRWWitness::SendRecoveryMessages()
{
    RecoveryMessage m;
    m.set_replicaidx(myIdx);
    m.set_nonce(recoveryNonce);
    
    RNotice("Requesting recovery");
    if (!transport->SendMessageToAll(this, m)) {
        RWarning("Failed to send Recovery message to all replicas");
    }
}

void
VRWWitness::RequestStateTransfer()
{
    RequestStateTransferMessage m;
    m.set_view(view);
    m.set_opnum(lastCommitted);

    if ((lastRequestStateTransferOpnum != 0) &&
        (lastRequestStateTransferView == view) &&
        (lastRequestStateTransferOpnum == lastCommitted)) {
        RDebug("Skipping state transfer request " FMT_VIEWSTAMP
               " because we already requested it", view, lastCommitted);
        return;
    }
    
    RNotice("Requesting state transfer: " FMT_VIEWSTAMP, view, lastCommitted);

    this->lastRequestStateTransferView = view;
    this->lastRequestStateTransferOpnum = lastCommitted;

    if (!transport->SendMessageToAll(this, m)) {
        RWarning("Failed to send RequestStateTransfer message to all replicas");
    }
}

void
VRWWitness::EnterView(view_t newview)
{
    RNotice("Entering new view " FMT_VIEW, newview);

    view = newview;
    status = STATUS_NORMAL;

    recoveryTimeout->Stop();

	viewChangeTimeout->Start();

    startViewChangeQuorum.Clear();
    recoveryResponseQuorum.Clear();
}

void
VRWWitness::StartViewChange(view_t newview)
{
    RNotice("Starting view change for view " FMT_VIEW ", lastCommitted " FMT_OPNUM, newview, lastCommitted);
	ASSERT(!specpaxos::IsWitness(configuration.GetLeaderIndex(newview)));

    view = newview;
    status = STATUS_VIEW_CHANGE;

    viewChangeTimeout->Reset();

    StartViewChangeMessage m;
    m.set_view(newview);
    m.set_replicaidx(myIdx);
    m.set_lastcommitted(lastCommitted);

    if (!transport->SendMessageToAll(this, m)) {
        RWarning("Failed to send StartViewChange message to all replicas");
    }
}

void
VRWWitness::ReceiveMessage(const TransportAddress &remote,
                          const string &type, const string &data)
{
    static RequestMessage request;
    static UnloggedRequestMessage unloggedRequest;
    static PrepareMessage prepare;
    static PrepareOKMessage prepareOK;
    static CommitMessage commit;
    static RequestStateTransferMessage requestStateTransfer;
    static StateTransferMessage stateTransfer;
    static StartViewChangeMessage startViewChange;
    static DoViewChangeMessage doViewChange;
    static StartViewMessage startView;
    static RecoveryMessage recovery;
    static RecoveryResponseMessage recoveryResponse;
    
    if (type == request.GetTypeName()) {
		Latency_Start(&requestLatency);
		RDebug("Ignoring logged request because I am a witness");
		Latency_EndType(&requestLatency, 'i');
    } else if (type == unloggedRequest.GetTypeName()) {
		Latency_Start(&requestLatency);
		RDebug("Ignoring unlogged request because I am a witness");
		Latency_EndType(&requestLatency, 'i');
        unloggedRequest.ParseFromString(data);
    } else if (type == prepare.GetTypeName()) {
        prepare.ParseFromString(data);
        HandlePrepare(remote, prepare);
    } else if (type == prepareOK.GetTypeName()) {
        prepareOK.ParseFromString(data);
		RDebug("Received PREPAREOK <" FMT_VIEW ", "
			   FMT_OPNUM  "> from replica %d",
			   prepareOK.view(), prepareOK.opnum(), prepareOK.replicaidx());
		RWarning("Ignoring PrepareOK because I am a witness");
    } else if (type == commit.GetTypeName()) {
        commit.ParseFromString(data);
        HandleCommit(remote, commit);
    } else if (type == requestStateTransfer.GetTypeName()) {
        requestStateTransfer.ParseFromString(data);
        HandleRequestStateTransfer(remote, requestStateTransfer);
    } else if (type == stateTransfer.GetTypeName()) {
        stateTransfer.ParseFromString(data);
        HandleStateTransfer(remote, stateTransfer);
    } else if (type == startViewChange.GetTypeName()) {
        startViewChange.ParseFromString(data);
        HandleStartViewChange(remote, startViewChange);
    } else if (type == doViewChange.GetTypeName()) {
        doViewChange.ParseFromString(data);
		RDebug("Received DOVIEWCHANGE " FMT_VIEW " from replica %d, "
			   "lastnormalview=" FMT_VIEW " op=" FMT_OPNUM " committed=" FMT_OPNUM,
			   doViewChange.view(), doViewChange.replicaidx(),
			   doViewChange.lastnormalview(), doViewChange.lastop(), doViewChange.lastcommitted());
    } else if (type == startView.GetTypeName()) {
        startView.ParseFromString(data);
        HandleStartView(remote, startView);
    } else if (type == recovery.GetTypeName()) {
        recovery.ParseFromString(data);
        HandleRecovery(remote, recovery);
    } else if (type == recoveryResponse.GetTypeName()) {
        recoveryResponse.ParseFromString(data);
        HandleRecoveryResponse(remote, recoveryResponse);
    } else {
        RPanic("Received unexpected message type in VRW proto: %s",
              type.c_str());
    }
}

void
VRWWitness::HandlePrepare(const TransportAddress &remote,
                         const PrepareMessage &msg)
{
    RDebug("Received PREPARE <" FMT_VIEW "," FMT_OPNUM "-" FMT_OPNUM ">",
           msg.view(), msg.batchstart(), msg.opnum());

    if (this->status != STATUS_NORMAL) {
        RDebug("Ignoring PREPARE due to abnormal status");
        return;
    }
    
    if (msg.view() < this->view) {
        RDebug("Ignoring PREPARE due to stale view");
        return;
    }

    if (msg.view() > this->view) {
        RequestStateTransfer();
        pendingPrepares.push_back(std::pair<TransportAddress *, PrepareMessage>(remote.clone(), msg));
        return;
    }

	if (msg.cleanupto() > lastCommitted) {
		RPanic("Asking me to clean an entry after my lastCommitted!");
	}

	if (msg.cleanupto() > cleanUpTo) {
		// Clean log up to the lowest committed entry by any replica
		cleanUpTo = msg.cleanupto();
		CleanLog(); 
	} else if (msg.cleanupto() < cleanUpTo) {
		RWarning("cleanUpTo decreased! Got " FMT_OPNUM ", had " FMT_OPNUM, 
				msg.cleanupto(), cleanUpTo);
	}

    ASSERT(msg.batchstart() <= msg.opnum());
    ASSERT_EQ(msg.opnum()-msg.batchstart()+1, (unsigned)msg.request_size());
              
    viewChangeTimeout->Reset();
    
    if (msg.opnum() <= this->lastOp) {
        RDebug("Ignoring PREPARE; already prepared that operation");
        // Resend the prepareOK message
        PrepareOKMessage reply;
        reply.set_view(msg.view());
        reply.set_opnum(msg.opnum());
        reply.set_replicaidx(myIdx);
		reply.set_lastcommitted(lastCommitted);
        if (!(transport->SendMessageToReplica(this,
                                              configuration.GetLeaderIndex(view),
                                              reply))) {
            RWarning("Failed to send PrepareOK message to leader");
        }
        return;
    }

    if (msg.batchstart() > this->lastOp+1) {
		RNotice("Requesting state transfer in HandlePrepare");
        RequestStateTransfer();
        pendingPrepares.push_back(std::pair<TransportAddress *, PrepareMessage>(remote.clone(), msg));
        return;
    }
    
    /* Add operations to the log */
    opnum_t op = msg.batchstart()-1;
    for (auto &req : msg.request()) {
        op++;
        if (op <= lastOp) {
            continue;
        }
        this->lastOp++;
        log.Append(viewstamp_t(msg.view(), op),
                   req, LOG_STATE_PREPARED);
    }
    ASSERT(op == msg.opnum());
    
    /* Build reply and send it to the leader */
    PrepareOKMessage reply;
    reply.set_view(msg.view());
    reply.set_opnum(msg.opnum());
    reply.set_replicaidx(myIdx);
	reply.set_lastcommitted(lastCommitted);
    
    if (!(transport->SendMessageToReplica(this,
                                          configuration.GetLeaderIndex(view),
                                          reply))) {
        RWarning("Failed to send PrepareOK message to leader");
    }
}

void
VRWWitness::HandleCommit(const TransportAddress &remote,
                        const CommitMessage &msg)
{
    RDebug("Received COMMIT " FMT_VIEWSTAMP, msg.view(), msg.opnum());

    if (this->status != STATUS_NORMAL) {
        RDebug("Ignoring COMMIT due to abnormal status");
        return;
    }
    
    if (msg.view() < this->view) {
        RDebug("Ignoring COMMIT due to stale view");
        return;
    }

    if (msg.view() > this->view) {
        RequestStateTransfer();
        return;
    }

    viewChangeTimeout->Reset();
    
    if (msg.opnum() <= this->lastCommitted) {
        RDebug("Ignoring COMMIT; already committed that operation");
        return;
    }

    if (msg.opnum() > this->lastOp) {
		RNotice("Requesting state transfer in HandleCommit");
        RequestStateTransfer();
        return;
    }

    CommitUpTo(msg.opnum());
}


void
VRWWitness::HandleRequestStateTransfer(const TransportAddress &remote,
                                      const RequestStateTransferMessage &msg)
{    
    RDebug("Received REQUESTSTATETRANSFER " FMT_VIEWSTAMP,
           msg.view(), msg.opnum());

    if (status != STATUS_NORMAL) {
        RDebug("Ignoring REQUESTSTATETRANSFER due to abnormal status");
        return;
    }

    if (msg.view() > view) {
        RequestStateTransfer();
        return;
    }

    RNotice("Sending state transfer from " FMT_VIEWSTAMP " to "
            FMT_VIEWSTAMP,
            msg.view(), msg.opnum(), view, lastCommitted);

    StateTransferMessage reply;
    reply.set_view(view);
    reply.set_opnum(lastCommitted);
    
    log.Dump(msg.opnum()+1, reply.mutable_entries());

    transport->SendMessage(this, remote, reply);
}

void
VRWWitness::HandleStateTransfer(const TransportAddress &remote,
                               const StateTransferMessage &msg)
{
    RDebug("Received STATETRANSFER " FMT_VIEWSTAMP, msg.view(), msg.opnum());
    
    if (msg.view() < view) {
        RWarning("Ignoring state transfer for older view");
        return;
    }
    
    opnum_t oldLastOp = lastOp;
    
    /* Install the new log entries */
    for (auto newEntry : msg.entries()) {
        if (newEntry.opnum() <= lastCommitted) {
            // Already committed this operation; nothing to be done.
#if PARANOID
            const LogEntry *entry = log.Find(newEntry.opnum());
            ASSERT(entry->viewstamp.opnum == newEntry.opnum());
            ASSERT(entry->viewstamp.view == newEntry.view());
//          ASSERT(entry->request == newEntry.request());
#endif
        } else if (newEntry.opnum() <= lastOp) {
            // We already have an entry with this opnum, but maybe
            // it's from an older view?
            const LogEntry *entry = log.Find(newEntry.opnum());
            ASSERT(entry->viewstamp.opnum == newEntry.opnum());
            ASSERT(entry->viewstamp.view <= newEntry.view());
            
            if (entry->viewstamp.view == newEntry.view()) {
                // We already have this operation in our log.
                ASSERT(entry->state == LOG_STATE_PREPARED);
#if PARANOID
//              ASSERT(entry->request == newEntry.request());                
#endif
            } else {
                // Our operation was from an older view, so obviously
                // it didn't survive a view change. Throw out any
                // later log entries and replace with this one.
                ASSERT(entry->state != LOG_STATE_COMMITTED);
                log.RemoveAfter(newEntry.opnum());
                lastOp = newEntry.opnum();
                oldLastOp = lastOp;

                viewstamp_t vs = { newEntry.view(), newEntry.opnum() };
                log.Append(vs, newEntry.request(), LOG_STATE_PREPARED);
            }
        } else {
            // This is a new operation to us. Add it to the log.
            ASSERT(newEntry.opnum() == lastOp+1);
            
            lastOp++;
            viewstamp_t vs = { newEntry.view(), newEntry.opnum() };
            log.Append(vs, newEntry.request(), LOG_STATE_PREPARED);
        }
    }
    

    if (msg.view() > view) {
        EnterView(msg.view());
    }

    /* Execute committed operations */
    ASSERT(msg.opnum() <= lastOp);
    CommitUpTo(msg.opnum());
    SendPrepareOKs(oldLastOp);

    // Process pending prepares
    std::list<std::pair<TransportAddress *, PrepareMessage> >pending = pendingPrepares;
    pendingPrepares.clear();
    for (auto & msgpair : pendingPrepares) {
        RDebug("Processing pending prepare message");
        HandlePrepare(*msgpair.first, msgpair.second);
        delete msgpair.first;
    }
}

void
VRWWitness::HandleStartViewChange(const TransportAddress &remote,
                                 const StartViewChangeMessage &msg)
{
    RDebug("Received STARTVIEWCHANGE " FMT_VIEW " from replica %d",
           msg.view(), msg.replicaidx());

    if (msg.view() < view) {
        RDebug("Ignoring STARTVIEWCHANGE for older view");
        return;
    }

    if ((msg.view() == view) && (status != STATUS_VIEW_CHANGE)) {
        RDebug("Ignoring STARTVIEWCHANGE for current view");
        return;
    }

    if ((status != STATUS_VIEW_CHANGE) || (msg.view() > view)) {
        RWarning("Received StartViewChange for view " FMT_VIEW
                 " from replica %d", msg.view(), msg.replicaidx());
        StartViewChange(msg.view());
    }

    ASSERT(msg.view() == view);
    
    if (auto msgs =
        startViewChangeQuorum.AddAndCheckForQuorum(msg.view(),
                                                   msg.replicaidx(),
                                                   msg)) {
        int leader = configuration.GetLeaderIndex(view);
        // Don't try to send a DoViewChange message to ourselves
		ASSERT(leader != myIdx);
		DoViewChangeMessage dvc;
		dvc.set_view(view);
		dvc.set_lastnormalview(log.LastViewstamp().view);
		dvc.set_lastop(lastOp);
		dvc.set_lastcommitted(lastCommitted);
		dvc.set_replicaidx(myIdx);

		// Figure out how much of the log to include
		opnum_t minCommitted = std::min_element(
			msgs->begin(), msgs->end(),
			[](decltype(*msgs->begin()) a,
			   decltype(*msgs->begin()) b) {
				return a.second.lastcommitted() < b.second.lastcommitted();
			})->second.lastcommitted();
		minCommitted = std::min(minCommitted, lastCommitted);
		minCommitted = std::min(minCommitted, GetLowestReplicaCommit()); 
		
		log.Dump(minCommitted,
				 dvc.mutable_entries());

		if (!(transport->SendMessageToReplica(this, leader, dvc))) {
			RWarning("Failed to send DoViewChange message to leader of new view");
		}
    }
}

void
VRWWitness::HandleStartView(const TransportAddress &remote,
                           const StartViewMessage &msg)
{
    RDebug("Received STARTVIEW " FMT_VIEW 
          " op=" FMT_OPNUM " committed=" FMT_OPNUM " entries=%d",
          msg.view(), msg.lastop(), msg.lastcommitted(), msg.entries_size());
    RDebug("Currently in view " FMT_VIEW " op " FMT_OPNUM " committed " FMT_OPNUM,
          view, lastOp, lastCommitted);

    if (msg.view() < view) {
        RWarning("Ignoring STARTVIEW for older view");
        return;
    }

    if ((msg.view() == view) && (status != STATUS_VIEW_CHANGE)) {
        RWarning("Ignoring STARTVIEW for current view");
        return;
    }

    ASSERT(configuration.GetLeaderIndex(msg.view()) != myIdx);

    if (msg.entries_size() == 0) {
        ASSERT(msg.lastcommitted() == lastCommitted);
        ASSERT(msg.lastop() == msg.lastcommitted());
    } else {
        if (msg.entries(0).opnum() > lastCommitted+1) {
            RPanic("Not enough entries in STARTVIEW message to install new log");
        }
        
        // Install the new log
		// TODO fix: The last opnum in the log is larger than the opnum of one (or more) of 
		// the entries we are trying to append on installation. 
		// msg.lastop() = 4087
		//
		// On Append: LastOpnum() = 4087, vs.opnum = 4083
        log.RemoveAfter(msg.lastop()+1);
        log.Install(msg.entries().begin(),
                    msg.entries().end());        
    }


    EnterView(msg.view());
    opnum_t oldLastOp = lastOp;
    lastOp = msg.lastop();

    CommitUpTo(msg.lastcommitted());
    SendPrepareOKs(oldLastOp);
}

void
VRWWitness::HandleRecovery(const TransportAddress &remote,
                          const RecoveryMessage &msg)
{
    RDebug("Received RECOVERY from replica %d", msg.replicaidx());

    if (status != STATUS_NORMAL) {
        RDebug("Ignoring RECOVERY due to abnormal status");
        return;
    }

    RecoveryResponseMessage reply;
    reply.set_replicaidx(myIdx);
    reply.set_view(view);
    reply.set_nonce(msg.nonce());

    if (!(transport->SendMessage(this, remote, reply))) {
        RWarning("Failed to send recovery response");
    }
    return;
}

void
VRWWitness::HandleRecoveryResponse(const TransportAddress &remote,
                                  const RecoveryResponseMessage &msg)
{
    RDebug("Received RECOVERYRESPONSE from replica %d",
           msg.replicaidx());

    if (status != STATUS_RECOVERING) {
        RDebug("Ignoring RECOVERYRESPONSE because we're not recovering");
        return;
    }

    if (msg.nonce() != recoveryNonce) {
        RNotice("Ignoring recovery response because nonce didn't match");
        return;
    }

    auto msgs = recoveryResponseQuorum.AddAndCheckForQuorum(msg.nonce(),
                                                            msg.replicaidx(),
                                                            msg);
    if (msgs != NULL) {
        view_t highestView = 0;
        for (const auto &kv : *msgs) {
            if (kv.second.view() > highestView) {
                highestView = kv.second.view();
            }
        }
        
        int leader = configuration.GetLeaderIndex(highestView);
        ASSERT(leader != myIdx);
        auto leaderResponse = msgs->find(leader);
        if ((leaderResponse == msgs->end()) ||
            (leaderResponse->second.view() != highestView)) {
            RDebug("Have quorum of RECOVERYRESPONSE messages, "
                   "but still need to wait for one from the leader");
            return;
        }

        Notice("Recovery completed");
        
        log.Install(leaderResponse->second.entries().begin(),
                    leaderResponse->second.entries().end());        
        EnterView(leaderResponse->second.view());
        lastOp = leaderResponse->second.lastop();
        CommitUpTo(leaderResponse->second.lastcommitted());
    }
}

opnum_t
VRWWitness::GetLowestReplicaCommit()
{
	opnum_t lowest = *std::min_element(lastCommitteds.begin(), lastCommitteds.end()); 
	return lowest;
}

void
VRWWitness::CleanLog()
{
	/* 
	 * Truncate the log up to the current cleanUpTo value.
	 */
	RNotice("Cleaning up to " FMT_OPNUM, cleanUpTo);
	log.RemoveUpTo(cleanUpTo);
}

size_t
VRWWitness::GetLogSize()
{
	return log.Size();
}

} // namespace specpaxos::vrw
} // namespace specpaxos

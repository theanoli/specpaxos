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

/* TODO
 * May need to move the read validations to a different data structure instead 
 * of the client table.
 */

#include "common/replica.h"
#include "vrw/replica.h"
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
    
VRWReplica::VRWReplica(Configuration config, int myIdx,
                     bool initialize,
                     Transport *transport, int batchSize,
                     AppReplica *app)
    : Replica(config, myIdx, initialize, transport, app),
      batchSize(batchSize),
	  lastCommitteds(config.n, 0),
      log(false),
      prepareOKQuorum(config.QuorumSize()-1),
      startViewChangeQuorum(config.QuorumSize()-1),
      doViewChangeQuorum(config.QuorumSize()-1),
      recoveryResponseQuorum(config.QuorumSize()),
      validateReadQuorum(config.QuorumSize()-1)
{
    this->status = STATUS_NORMAL;
    this->view = 0;
    this->lastOp = 0;
    this->lastCommitted = 0;
    this->lastRequestStateTransferView = 0;
    this->lastRequestStateTransferOpnum = 0;
    lastBatchEnd = 0;
    batchComplete = true;

	this->cleanUpTo = 0;

    if (batchSize > 1) {
        Notice("Batching enabled; batch size %d", batchSize);
    }

    this->viewChangeTimeout = new Timeout(transport, 5000, [this,myIdx]() {
            RWarning("Have not heard from leader; starting view change");
			view_t step = 1;
			while (configuration.IsWitness(configuration.GetLeaderIndex(view + step))) {
				step++; 
			}
            StartViewChange(view + step);
        });
    this->nullCommitTimeout = new Timeout(transport, 1000, [this]() {
            SendNullCommit();
        });
    this->stateTransferTimeout = new Timeout(transport, 1000, [this]() {
            this->lastRequestStateTransferView = 0;
            this->lastRequestStateTransferOpnum = 0;            
        });
    this->stateTransferTimeout->Start();
    this->resendPrepareTimeout = new Timeout(transport, 500, [this]() {
            ResendPrepare();
        });
	/*
    this->resendValidateTimeout = new Timeout(transport, 500, [this]() {
            ResendValidate();
        });
		*/
    this->closeBatchTimeout = new Timeout(transport, 300, [this]() {
            CloseBatch();
        });
    this->recoveryTimeout = new Timeout(transport, 5000, [this]() {
            SendRecoveryMessages();
        });

    _Latency_Init(&requestLatency, "request");
    _Latency_Init(&executeAndReplyLatency, "executeAndReply");

    if (initialize) {
        if (AmLeader()) {
            nullCommitTimeout->Start();
        } else {
            viewChangeTimeout->Start();
        }        
    } else {
        this->status = STATUS_RECOVERING;
        this->recoveryNonce = GenerateNonce();
        SendRecoveryMessages();
        recoveryTimeout->Start();
    }
}

VRWReplica::~VRWReplica()
{
    Latency_Dump(&requestLatency);
    Latency_Dump(&executeAndReplyLatency);

    delete viewChangeTimeout;
    delete nullCommitTimeout;
    delete stateTransferTimeout;
    delete resendPrepareTimeout;
	// delete resendValidateTimeout;
    delete closeBatchTimeout;
    delete recoveryTimeout;
    
    for (auto &kv : pendingPrepares) {
        delete kv.first;
    }
}

uint64_t
VRWReplica::GenerateNonce() const
{
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    return dis(gen);    
}

bool
VRWReplica::AmLeader() const
{
    return (configuration.GetLeaderIndex(view) == myIdx);
}

void
VRWReplica::CommitUpTo(opnum_t upto)
{
    while (lastCommitted < upto) {
        Latency_Start(&executeAndReplyLatency);
        
        lastCommitted++;

        /* Find operation in log */
        const LogEntry *entry = log.Find(lastCommitted);
        if (!entry) {
            RPanic("Did not find operation " FMT_OPNUM " in log", lastCommitted);
        }

        /* Execute it */
        RDebug("Executing request " FMT_OPNUM " from client %lu (" FMT_OPNUM ")\n\t\t\tLast request from this client: %lu", 
				lastCommitted, entry->request.clientid(), 
				entry->request.clientreqid(),
				clientTable[entry->request.clientid()].lastReqId);
        ReplyMessage reply;
		Execute(lastCommitted, entry->request, reply);

		reply.set_view(entry->viewstamp.view);
		reply.set_opnum(entry->viewstamp.opnum);
		reply.set_clientreqid(entry->request.clientreqid());

        /* Mark it as committed */
        log.SetStatus(lastCommitted, LOG_STATE_COMMITTED);

		// Store reply in the client table
		ClientTableEntry &cte =
			clientTable[entry->request.clientid()];
		
		if (cte.lastReqId <= entry->request.clientreqid()) {
			cte.lastReqId = entry->request.clientreqid();
			cte.replied = true;
			cte.reply = reply;            
		} else {
			// We've subsequently prepared another operation from the
			// same client. So this request must have been completed
			// at the client, and there's no need to record the
			// result.
		}
		RDebug("Reply size: %ld", reply.ByteSizeLong());
		RDebug("Sending reply: Client table entry's last reqID for the client %lu: " FMT_OPNUM ", %s", entry->request.clientid(), cte.lastReqId, reply.DebugString().c_str());
		
		/* Send reply */
		auto iter = clientAddresses.find(entry->request.clientid());
		if (iter != clientAddresses.end()) {
			transport->SendMessage(this, *iter->second, reply);
		}

        Latency_End(&executeAndReplyLatency);
    }
}

void
VRWReplica::SendPrepareOKs(opnum_t oldLastOp)
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
        UpdateClientTable(entry->request);

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
VRWReplica::SendRecoveryMessages()
{
    RecoveryMessage m;
    m.set_replicaidx(myIdx);
    m.set_nonce(recoveryNonce);
    
    RNotice("Sending recovery request");
    if (!transport->SendMessageToAll(this, m)) {
        RWarning("Failed to send Recovery message to all replicas");
    }
}

void
VRWReplica::RequestStateTransfer()
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
VRWReplica::EnterView(view_t newview)
{
    RNotice("Entering new view " FMT_VIEW, newview);
    RNotice("Leader is %u", configuration.GetLeaderIndex(newview));

    view = newview;
    status = STATUS_NORMAL;
    lastBatchEnd = lastOp;
    batchComplete = true;

    recoveryTimeout->Stop();

    if (AmLeader()) {
        viewChangeTimeout->Stop();
        nullCommitTimeout->Start();
    } else {
        viewChangeTimeout->Start();
        nullCommitTimeout->Stop();
        resendPrepareTimeout->Stop();
        // resendValidateTimeout->Stop();
        closeBatchTimeout->Stop();
    }

    prepareOKQuorum.Clear();
    startViewChangeQuorum.Clear();
    doViewChangeQuorum.Clear();
    recoveryResponseQuorum.Clear();
	validateReadQuorum.Clear();
}

void
VRWReplica::StartViewChange(view_t newview)
{
    RNotice("Starting view change for view " FMT_VIEW ", lastCommitted " FMT_OPNUM, newview, lastCommitted);
	ASSERT(!configuration.IsWitness(configuration.GetLeaderIndex(newview)));

    view = newview;
    status = STATUS_VIEW_CHANGE;

    viewChangeTimeout->Reset();
    nullCommitTimeout->Stop();
    resendPrepareTimeout->Stop();
	// resendValidateTimeout->Stop();
    closeBatchTimeout->Stop();

    StartViewChangeMessage m;
    m.set_view(newview);
    m.set_replicaidx(myIdx);
    m.set_lastcommitted(lastCommitted);

	RDebug("Sending StartViewChange");
    if (!transport->SendMessageToAll(this, m)) {
        RWarning("Failed to send StartViewChange message to all replicas");
    }
}

void
VRWReplica::SendNullCommit()
{
    CommitMessage cm;
    cm.set_view(this->view);
    cm.set_opnum(this->lastCommitted);

    ASSERT(AmLeader());

	RDebug("Sending NullCommit");
    if (!(transport->SendMessageToAll(this, cm))) {
        RWarning("Failed to send null COMMIT message to all replicas");
    }
}

void
VRWReplica::UpdateClientTable(const Request &req)
{
    ClientTableEntry &entry = clientTable[req.clientid()];
	RDebug("Updating client %lu: to " FMT_OPNUM " from " FMT_OPNUM, req.clientid(), req.clientreqid(), entry.lastReqId);

    // ASSERT(entry.lastReqId <= req.clientreqid());

    if (entry.lastReqId >= req.clientreqid()) {
        return;
    }

    entry.lastReqId = req.clientreqid();
    entry.replied = false;
    entry.reply.Clear();
	entry.needsReadValidation = false;
}

void
VRWReplica::ResendPrepare()
{
    ASSERT(AmLeader());
    if (lastOp == lastCommitted) {
        return;
    }
    RNotice("Resending prepare");
    if (!(transport->SendMessageToAll(this, lastPrepare))) {
        RWarning("Failed to ressend prepare message to all replicas");
    }
}

void
VRWReplica::CloseBatch()
{
    ASSERT(AmLeader());
    ASSERT(lastBatchEnd < lastOp);

    opnum_t batchStart = lastBatchEnd+1;
    
    RDebug("Sending batched prepare from " FMT_OPNUM
           " to " FMT_OPNUM,
           batchStart, lastOp);
    /* Send prepare messages */
    PrepareMessage p;
    p.set_view(view);
    p.set_opnum(lastOp);
    p.set_batchstart(batchStart);
	p.set_cleanupto(GetLowestReplicaCommit()); 
	
    for (opnum_t i = batchStart; i <= lastOp; i++) {
        Request *r = p.add_request();
        const LogEntry *entry = log.Find(i);
        ASSERT(entry != NULL);
        ASSERT(entry->viewstamp.view == view);
        ASSERT(entry->viewstamp.opnum == i);
        *r = entry->request;
    }
    lastPrepare = p;

    if (!(transport->SendMessageToAll(this, p))) {
        RWarning("Failed to send prepare message to all replicas");
    }
    lastBatchEnd = lastOp;
    batchComplete = false;
    
    resendPrepareTimeout->Reset();
    closeBatchTimeout->Stop();

	if (p.cleanupto() > cleanUpTo) {
		// Clean log up to the lowest committed entry by any replica
		cleanUpTo = p.cleanupto();
		CleanLog(); 
	} else if (p.cleanupto() < cleanUpTo) {
		RPanic("cleanUpTo decreased! Got " FMT_OPNUM ", had " FMT_OPNUM, 
				p.cleanupto(), cleanUpTo);
	}
}

void
VRWReplica::ReceiveMessage(const TransportAddress &remote,
                          const string &type, const string &data)
{
    static RequestMessage request;
    static UnloggedRequestMessage unloggedRequest;
    static PrepareMessage prepare;
    static PrepareOKMessage prepareOK;
	static ValidateRequestMessage validateRequest;
	static ValidateReplyMessage validateReply;
    static CommitMessage commit;
    static RequestStateTransferMessage requestStateTransfer;
    static StateTransferMessage stateTransfer;
    static StartViewChangeMessage startViewChange;
    static DoViewChangeMessage doViewChange;
    static StartViewMessage startView;
    static RecoveryMessage recovery;
    static RecoveryResponseMessage recoveryResponse;
    
    if (type == request.GetTypeName()) {
        request.ParseFromString(data);
        HandleRequest(remote, request);
	} else if (type == validateRequest.GetTypeName()) {
		validateRequest.ParseFromString(data); 
		HandleValidateRequest(remote, validateRequest);
	} else if (type == validateReply.GetTypeName()) {
		validateReply.ParseFromString(data); 
		HandleValidateReply(remote, validateReply);
    } else if (type == unloggedRequest.GetTypeName()) {
        unloggedRequest.ParseFromString(data);
        HandleUnloggedRequest(remote, unloggedRequest);
    } else if (type == prepare.GetTypeName()) {
        prepare.ParseFromString(data);
        HandlePrepare(remote, prepare);
    } else if (type == prepareOK.GetTypeName()) {
        prepareOK.ParseFromString(data);
        HandlePrepareOK(remote, prepareOK);
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
        HandleDoViewChange(remote, doViewChange);
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
VRWReplica::HandleRequest(const TransportAddress &remote,
                         const RequestMessage &msg)
{
    viewstamp_t v;
    Latency_Start(&requestLatency);
    
    if (status != STATUS_NORMAL) {
        RNotice("Ignoring request due to abnormal status");
        Latency_EndType(&requestLatency, 'i');
        return;
    }

    if (!AmLeader()) {
        RDebug("Ignoring request because I'm not the leader");
        Latency_EndType(&requestLatency, 'i');
        return;        
    }

    // Save the client's address
    clientAddresses.erase(msg.req().clientid());
    clientAddresses.insert(
        std::pair<uint64_t, std::unique_ptr<TransportAddress> >(
            msg.req().clientid(),
            std::unique_ptr<TransportAddress>(remote.clone())));

    // Check the client table to see if this is a duplicate request
    auto kv = clientTable.find(msg.req().clientid());
    if (kv != clientTable.end()) {
        const ClientTableEntry &entry = kv->second;
        if (msg.req().clientreqid() < entry.lastReqId) {
            RNotice("Ignoring stale request");
            Latency_EndType(&requestLatency, 's');
            return;
        }
        if (msg.req().clientreqid() == entry.lastReqId) {
            // This is a duplicate request. Resend the reply if we
            // have one. We might not have a reply to resend if we're
            // waiting for the other replicas; in that case, just
            // discard the request.
            if (entry.replied) {
                RNotice("Received duplicate request; resending reply");
                if (!(transport->SendMessage(this, remote,
                                             entry.reply))) {
                    RWarning("Failed to resend reply to client");
                }
                Latency_EndType(&requestLatency, 'r');
                return;
            } else {
				if (!entry.needsReadValidation) {
					RNotice("Received duplicate request %lu:" FMT_OPNUM " but no reply available; ignoring", 
							msg.req().clientid(), msg.req().clientreqid());
					Latency_EndType(&requestLatency, 'd');
					return;
				}
            }
        }
		RDebug("Last reqID for client %lu: " FMT_OPNUM, msg.req().clientid(), entry.lastReqId);
    } else {
		RNotice("New client %lu!", msg.req().clientid());
	}

    // Update the client table
    UpdateClientTable(msg.req());

    // Leader Upcall
    bool replicate = false;
    string res;
	// TS: LeaderUpcall is basically a no-op for all example ops: it checks if the op should 
	// be replicated and copies the op into res. 
    LeaderUpcall(lastCommitted, msg.req().op(), replicate, res);
    ClientTableEntry &cte =
        clientTable[msg.req().clientid()];

	// Check whether this is a stealth read. This is NOT the same as an unlogged operation.
	// The current leader must prove leadership at the time the read was done: this proves
	// the replica had the most up-to-date version of the database at read time. 
	Request request;
	request.set_op(res);
	request.set_clientid(msg.req().clientid());
	request.set_clientreqid(msg.req().clientreqid());

    if (!replicate) {
		ReplyMessage reply; 
		Execute(lastCommitted, request, reply);
		reply.set_view(view);
		reply.set_opnum(0);
		reply.set_clientreqid(msg.req().clientreqid());

        cte.replied = false;
        cte.reply = reply;
		cte.needsReadValidation = true;
		
		// Send the ValidateRequestMessage here. 
		RDebug("Sending request to confirm leader-ship to other replicas");
		/* Send validate messages */
		ValidateRequestMessage p;
		p.set_view(this->view);  // How to tell leader was leader when read was done
		p.set_clientid(msg.req().clientid());  // Use client_id and client_req_id as labels? 
		p.set_clientreqid(msg.req().clientreqid());

		lastValidate = p; 
		
		if (!(transport->SendMessageToAll(this, p))) {
			RWarning("Failed to send validate message to all replicas");
		}
    } else {
        /* Assign it an opnum */
        ++this->lastOp;
        v.view = this->view;
        v.opnum = this->lastOp;

        RDebug("Received REQUEST %zu:" FMT_OPNUM", assigning " FMT_VIEWSTAMP, 
				msg.req().clientid(), msg.req().clientreqid(), VA_VIEWSTAMP(v));

        /* Add the request to my log */
        log.Append(v, request, LOG_STATE_PREPARED);

        if (batchComplete ||
            (lastOp - lastBatchEnd+1 > (unsigned int)batchSize)) {
            CloseBatch();
        } else {
            RDebug("Keeping in batch");
            if (!closeBatchTimeout->Active()) {
                closeBatchTimeout->Start();
            }
        }
		nullCommitTimeout->Reset();
    }
	Latency_End(&requestLatency);
}

void
VRWReplica::HandleValidateRequest(const TransportAddress &remote,
									const ValidateRequestMessage &msg)
{
    if (status != STATUS_NORMAL) {
		// Cannot do this in the middle of a view change
        RNotice("Ignoring validate request due to abnormal status");
        return;
    }

	bool isvalid = false; 
	if (this->view > msg.view()) {
		RNotice("We went through a view change after validation was sent! Refusing to validate"); 
	} else if (this->view < msg.view()) {
		// OK to drop here; others will validate instead, since a quorum accepted
		// the viewchange already
		RNotice("We are lagging in viewchanges; dropping validation request");
		return;
	} else {
		isvalid = true;
	}
	
    ValidateReplyMessage reply;
	reply.set_replicaidx(myIdx); 
	reply.set_isvalid(isvalid);
    reply.set_clientid(msg.clientid());
    reply.set_clientreqid(msg.clientreqid());

	RDebug("Sending validate response to leader");
	if (!(transport->SendMessage(this, remote, reply))) {
		RWarning("Failed to send validate message to leader");
	}
}

void
VRWReplica::HandleValidateReply(const TransportAddress &remote,
									const ValidateReplyMessage &msg)
{
	// I think we can still validate if the status is not normal.
	// The read was stored while the replica was the leader; if it was valid 
	// then, it is valid now. 
    auto msgs = validateReadQuorum.AddAndCheckForQuorumOrNack(
			std::pair<uint64_t, uint64_t>(msg.clientid(), msg.clientreqid()),
                                                            msg.replicaidx(),
                                                            msg);
	// When we can validate: 
	// - A quorum of nodes responds with ACKs
	// When we cannot validate: 
	// - ANYONE responds with a NACK. Responding with a NACK means they are in the normal state
	// 		and their view number has advanced, i.e., they went through a round of leader
	// 		election and it was successful. 
	//
    if (msgs != NULL) {
		// Extra validation response we do not need to bother with. If we have reached a 
		// quorum already, any nodes that respond will not respond with a NACK even if
		// they advance to a new view (unless a bunch of time has passed and the whole system
		// has moved on to a new view since the validation-seeker got responses from the
		// quorum, but anyway those validations are still... valid)
        if (msgs->size() >= (unsigned)configuration.QuorumSize()) {
            return;
        }

        for (const auto &kv : *msgs) {
			if (!kv.second.isvalid()) {
				RDebug("Another replica NACKed the validate request! Not servicing read");
				return;
			}
        }
		
		// TODO respond to the client and do whatever bookkeeping is needed
		ClientTableEntry &cte = clientTable[msg.clientid()];
		ASSERT(cte.reply.clientreqid() == msg.clientreqid());
		cte.replied = true;

		/* THIS IS THE WRONG THING TO DO
        ReplyMessage reply;
		const LogEntry *entry = log.Find(lastCommitted);
		Execute(lastCommitted, entry->request, reply);

		reply.set_view(cte.reply.view());
		reply.set_opnum(msg.clientreqid());
		reply.set_clientreqid(msg.clientreqid());
		*/

		RDebug("Reply size: %ld", cte.reply.ByteSizeLong());
		RDebug("Sending response for validated read for client %lu, reqid %lu, view %lu: %s",
				msg.clientid(), cte.reply.clientreqid(), cte.reply.view(), cte.reply.DebugString().c_str());

		/* Send reply */
		auto iter = clientAddresses.find(msg.clientid());
		if (iter != clientAddresses.end()) {
			transport->SendMessage(this, *iter->second, cte.reply);
		}
    }
}

void
VRWReplica::ResendValidate()
{
	if ((!AmLeader()) || (lastValidate.view() != this->view)) {
		// Drop if we are not leader anymore; someone else should handle
		// this request. 
		// If the view has changed a few times and we're still the leader, the
		// old read is still valid. But it is easiest to have the client re-send
		// the read and for this leader to re-do it with an up-to-date value. 
		// We could handle responses for the same view for multiple outstanding 
		// validations, but this is complicated and doing it the naive way 
		// incurs at most as many messages as normal-case processing. 
        return;
    }
    RNotice("Resending last validate message");
    if (!(transport->SendMessageToAll(this, lastValidate))) {
        RWarning("Failed to ressend prepare message to all replicas");
    }
}

void
VRWReplica::HandleUnloggedRequest(const TransportAddress &remote,
                                 const UnloggedRequestMessage &msg)
{
    if (status != STATUS_NORMAL) {
        // Not clear if we should ignore this or just let the request
        // go ahead, but this seems reasonable.
        RNotice("Ignoring unlogged request due to abnormal status");
        return;
    }

    UnloggedReplyMessage reply;
    
    Debug("Received unlogged request %s", (char *)msg.req().op().c_str());

    ExecuteUnlogged(msg.req(), reply);
    
    if (!(transport->SendMessage(this, remote, reply)))
        Warning("Failed to send reply message");
}

void
VRWReplica::HandlePrepare(const TransportAddress &remote,
                         const PrepareMessage &msg)
{
    RDebug("Received PREPARE <" FMT_VIEW "," FMT_OPNUM "-" FMT_OPNUM ">",
           msg.view(), msg.batchstart(), msg.opnum());

    if (msg.view() == this->view && this->status == STATUS_VIEW_CHANGE) {
		if (AmLeader()) {
			RPanic("Unexpected PREPARE: I'm the leader of this view");
		}
        RequestStateTransfer();
        pendingPrepares.push_back(std::pair<TransportAddress *, PrepareMessage>(remote.clone(), msg));
        return;
    }

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

    if (AmLeader()) {
        RPanic("Unexpected PREPARE: I'm the leader of this view");
    }

	if (msg.cleanupto() > lastCommitted) {
		RPanic("Asking me to clean an entry after my lastCommitted!");
	}

	if (msg.cleanupto() > cleanUpTo) {
		// Clean log up to the lowest committed entry by any replica
		cleanUpTo = msg.cleanupto();
		CleanLog(); 
	} else if (msg.cleanupto() < cleanUpTo) {
		// A node can see a lower cleanUpTo if the leader fell behind: when it reconstructs
		// state, it will use its own cleanUpTo as a "safe" value, and will update it 
		// later once it hears from all the other replicas. 
		RWarning("cleanUpTo decreased! Got " FMT_OPNUM ", had " FMT_OPNUM, 
				msg.cleanupto(), cleanUpTo);
	}

    ASSERT(msg.batchstart() <= msg.opnum());
    ASSERT_EQ(msg.opnum()-msg.batchstart()+1, (unsigned)msg.request_size());
              
    viewChangeTimeout->Reset();
    
    if (msg.opnum() <= this->lastOp) {
        RDebug("Ignoring PREPARE; already prepared that operation. Resending PREPAREOK");
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
        UpdateClientTable(req);
    }
    ASSERT(op == msg.opnum());
    
    /* Build reply and send it to the leader */
    PrepareOKMessage reply;
    reply.set_view(msg.view());
    reply.set_opnum(msg.opnum());
    reply.set_replicaidx(myIdx);
	reply.set_lastcommitted(lastCommitted);
    
	RDebug("Sending PREPAREOK");
    if (!(transport->SendMessageToReplica(this,
                                          configuration.GetLeaderIndex(view),
                                          reply))) {
        RWarning("Failed to send PrepareOK message to leader");
    }
}

void
VRWReplica::HandlePrepareOK(const TransportAddress &remote,
                           const PrepareOKMessage &msg)
{

    RDebug("Received PREPAREOK <" FMT_VIEW ", "
           FMT_OPNUM  "> from replica %d",
           msg.view(), msg.opnum(), msg.replicaidx());

    if (this->status != STATUS_NORMAL) {
        RDebug("Ignoring PREPAREOK due to abnormal status");
        return;
    }

    if (msg.view() < this->view) {
        RDebug("Ignoring PREPAREOK due to stale view");
        return;
    }

    if (msg.view() > this->view) {
        RequestStateTransfer();
        return;
    }

    if (!AmLeader()) {
        RWarning("Ignoring PREPAREOK because I'm not the leader");
        return;        
    }

	/* 
	 * The leader needs to figure out the lowest commit number among replicas. 
	 * It should maintain a vector of the current lastCommitteds of the replicas, 
	 * and then update as replicas make progress.
	 */
	try {
		opnum_t replicaLastRecordedCommit = lastCommitteds.at(msg.replicaidx());
		ASSERT(replicaLastRecordedCommit <= msg.lastcommitted());
		lastCommitteds.at(msg.replicaidx()) = msg.lastcommitted();
	} catch (std::out_of_range const& exc) {
		RPanic("Tried to access an element that was out of range in lastCommitteds!");
	}
    
    viewstamp_t vs = { msg.view(), msg.opnum() };
    if (auto msgs =
        (prepareOKQuorum.AddAndCheckForQuorum(vs, msg.replicaidx(), msg))) {
        /*
         * We have a quorum of PrepareOK messages for this
         * opnumber. Execute it and all previous operations.
         *
         * (Note that we might have already executed it. That's fine,
         * we just won't do anything.)
         *
         * This also notifies the client of the result.
         */
        CommitUpTo(msg.opnum());
		lastCommitteds.at(myIdx) = lastCommitted;

        if (msgs->size() >= (unsigned)configuration.QuorumSize()) {
            return;
        }
        
        /*
         * Send COMMIT message to the other replicas.
         *
         * This can be done asynchronously, so it really ought to be
         * piggybacked on the next PREPARE or something.
         */
        CommitMessage cm;
        cm.set_view(this->view);
        cm.set_opnum(this->lastCommitted);
        RDebug("Sending commit %s", cm.ShortDebugString().c_str());

		RDebug("Sending COMMIT");
        if (!(transport->SendMessageToAll(this, cm))) {
            RWarning("Failed to send COMMIT message to all replicas");
        }

        nullCommitTimeout->Reset();

        // XXX Adaptive batching -- make this configurable
        if (lastBatchEnd == msg.opnum()) {
            batchComplete = true;
            if  (lastOp > lastBatchEnd) {
                CloseBatch();
            }
        }
    }
}

void
VRWReplica::HandleCommit(const TransportAddress &remote,
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

    if (AmLeader()) {
        RPanic("Unexpected COMMIT: I'm the leader of this view");
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
VRWReplica::HandleRequestStateTransfer(const TransportAddress &remote,
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
VRWReplica::HandleStateTransfer(const TransportAddress &remote,
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
		UpdateClientTable(newEntry.request());
    }
    

    if (msg.view() > view || (msg.view() == view && status == STATUS_VIEW_CHANGE)) {
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
VRWReplica::HandleStartViewChange(const TransportAddress &remote,
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
        if (leader != myIdx) {            
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
			RNotice("Sending DoViewChange: %d minCommitted: " FMT_OPNUM ", lastCommitted: " FMT_OPNUM, 
					msg.replicaidx(), minCommitted, lastCommitted);

            if (!(transport->SendMessageToReplica(this, leader, dvc))) {
                RWarning("Failed to send DoViewChange message to leader of new view");
            }
        }
    }
}


void
VRWReplica::HandleDoViewChange(const TransportAddress &remote,
                              const DoViewChangeMessage &msg)
{
    RDebug("Received DOVIEWCHANGE " FMT_VIEW " from replica %d, "
           "lastnormalview=" FMT_VIEW " op=" FMT_OPNUM " committed=" FMT_OPNUM,
           msg.view(), msg.replicaidx(),
           msg.lastnormalview(), msg.lastop(), msg.lastcommitted());

    if (msg.view() < view) {
        RDebug("Ignoring DOVIEWCHANGE for older view");
        return;
    }

    if ((msg.view() == view) && (status != STATUS_VIEW_CHANGE)) {
        RDebug("Ignoring DOVIEWCHANGE for current view");
        return;
    }

    if ((status != STATUS_VIEW_CHANGE) || (msg.view() > view)) {
        // It's superfluous to send the StartViewChange messages here,
        // but harmless...
        RWarning("Received DoViewChange for view " FMT_VIEW
                 " from replica %d", msg.view(), msg.replicaidx());
        StartViewChange(msg.view());
    }

    ASSERT(configuration.GetLeaderIndex(msg.view()) == myIdx);
    
    auto msgs = doViewChangeQuorum.AddAndCheckForQuorum(msg.view(),
                                                        msg.replicaidx(),
                                                        msg);

    if (msgs != NULL) {
        // Find the response with the most up to date log, i.e. the
        // one with the latest viewstamp
        view_t latestView = log.LastViewstamp().view;
        opnum_t latestOp = log.LastViewstamp().opnum;
		opnum_t highestCommitted = lastCommitted; 
        DoViewChangeMessage latestMsgObj;
        DoViewChangeMessage *latestMsg = NULL;

        for (auto kv : *msgs) {
            DoViewChangeMessage &x = kv.second;
			highestCommitted = std::max(x.lastcommitted(), highestCommitted); 
            if ((x.lastnormalview() > latestView) ||
                (((x.lastnormalview() == latestView) &&
                  (x.lastop() > latestOp)))) {
                latestView = x.lastnormalview();
                latestOp = x.lastop();
				latestMsgObj = kv.second;
                latestMsg = &latestMsgObj;
            }
        }


        // Install the new log. We might not need to do this, if our
        // log was the most current one.
        if (latestMsg != NULL) {
            RNotice("Selected log from replica %d with lastop=" FMT_OPNUM ", entries size %d",
                   latestMsg->replicaidx(), latestMsg->lastop(), latestMsg->entries_size());
            if (latestMsg->entries_size() == 0) {
                // There weren't actually any entries in the
                // log. That should only happen in the corner case
                // that everyone already had the entire log, maybe
                // because it actually is empty.
				RDebug("Log is empty; continuing with view change"); 
                ASSERT(lastCommitted == msg.lastcommitted());
                ASSERT(msg.lastop() == msg.lastcommitted());
            } else {
                if (latestMsg->entries(0).opnum() > lastCommitted+1) {
                    RPanic("Received log that didn't include enough entries to install it");
                }
                
                log.RemoveAfter(latestMsg->lastop()+1);
                log.Install(latestMsg->entries().begin(),
                            latestMsg->entries().end());
				for (auto entry : latestMsg->entries()) {
					UpdateClientTable(entry.request()); 
				}
            }
        } else {
            RDebug("My log is most current, lastnormalview=" FMT_VIEW " lastop=" FMT_OPNUM,
                   log.LastViewstamp().view, lastOp);
        }

        // How much of the log should we include when we send the
        // STARTVIEW message? Start from the lowest committed opnum of
        // any of the STARTVIEWCHANGE or DOVIEWCHANGE messages we got.
        //
        // We need to compute this before we enter the new view
        // because the saved messages will go away.
        auto svcs = startViewChangeQuorum.GetMessages(view);
			
        opnum_t minCommittedSVC = std::min_element(
            svcs.begin(), svcs.end(),
            [](decltype(*svcs.begin()) a,
               decltype(*svcs.begin()) b) {
                return a.second.lastcommitted() < b.second.lastcommitted();
            })->second.lastcommitted();
        opnum_t minCommittedDVC = std::min_element(
            msgs->begin(), msgs->end(),
            [](decltype(*msgs->begin()) a,
               decltype(*msgs->begin()) b) {
                return a.second.lastcommitted() < b.second.lastcommitted();
            })->second.lastcommitted();

        opnum_t minCommitted = std::min(minCommittedSVC, minCommittedDVC);
        minCommitted = std::min(minCommitted, lastCommitted);
        minCommitted = std::min(minCommitted, GetLowestReplicaCommit());

        lastOp = latestOp;
        EnterView(msg.view());

        ASSERT(AmLeader());
        
		CommitUpTo(highestCommitted);

		// When a new leader comes up, it will start sending around cleanUpTo messages.
		// It is SAFE to send around a cleanUpTo of 0, since it will be a no-op on replicas.
		// It is also safe to send around the new leader's cleanUpTo, since cleaning 
		// up to that value was safe before it became the leader. We can also use this 
		// value as the lastCommitted for each node in the lastCommitteds list; the true
		// lastCommitted of each node is not less than this value, because otherwise we 
		// could not have chosen this value as the cleanUpTo. If the lastCommitted of a node
		// is equal, no-op. If it is greater, then we will update it on the next PrepareOK round.
		for (size_t i = 0; i < lastCommitteds.size(); i++) {
			lastCommitteds[i] = cleanUpTo;
		}

        // Send a STARTVIEW message with the new log
        StartViewMessage sv;
        sv.set_view(view);
        sv.set_lastop(lastOp);
        sv.set_lastcommitted(lastCommitted);
        
        log.Dump(minCommitted, sv.mutable_entries());

		RDebug("Sending StartView");
        if (!(transport->SendMessageToAll(this, sv))) {
            RWarning("Failed to send StartView message to all replicas");
        }
    }    
}

void
VRWReplica::HandleStartView(const TransportAddress &remote,
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
		// TS We want to get rid of anything after the last operation seen by the replica
		// giving us the log. Then we will reconcile what's left with the log given to us
		// by that replica, possibly adding new entries and/or replacing existing entries. 
        log.RemoveAfter(msg.lastop()+1);
        log.Install(msg.entries().begin(),
                    msg.entries().end());        
		
		for (auto entry : msg.entries()) {
			UpdateClientTable(entry.request());
		}
    }


    EnterView(msg.view());
    opnum_t oldLastOp = lastOp;
    lastOp = msg.lastop();

    ASSERT(!AmLeader());

    CommitUpTo(msg.lastcommitted());
    SendPrepareOKs(oldLastOp);
}

void
VRWReplica::HandleRecovery(const TransportAddress &remote,
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
    if (AmLeader()) {
        reply.set_lastcommitted(lastCommitted);
        reply.set_lastop(lastOp);
        log.Dump(0, reply.mutable_entries());
    }

	RDebug("Sending RECOVERY");
    if (!(transport->SendMessage(this, remote, reply))) {
        RWarning("Failed to send recovery response");
    }
    return;
}

void
VRWReplica::HandleRecoveryResponse(const TransportAddress &remote,
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
VRWReplica::GetLowestReplicaCommit()
{
	opnum_t lowest = *std::min_element(lastCommitteds.begin(), lastCommitteds.end()); 
	return lowest;
}

void
VRWReplica::CleanLog()
{
	/* 
	 * Truncate the log up to the current cleanUpTo value.
	 */
	RDebug("Cleaning up to " FMT_OPNUM, cleanUpTo);
	log.RemoveUpTo(cleanUpTo);
}

size_t
VRWReplica::GetLogSize()
{
	return log.Size();
}

} // namespace specpaxos::vrw
} // namespace specpaxos

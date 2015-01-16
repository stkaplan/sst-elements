// Copyright 2009-2014 Sandia Corporation. Under the terms
// of Contract DE-AC04-94AL85000 with Sandia Corporation, the U.S.
// Government retains certain rights in this software.
// 
// Copyright (c) 2009-2014, Sandia Corporation
// All rights reserved.
// 
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

/*
 * File:   MESIBottomCoherenceController.cc
 * Author: Caesar De la Paz III
 * Email:  caesar.sst@gmail.com
 */

#include <sst_config.h>
#include <vector>
#include "MESIBottomCoherenceController.h"
using namespace SST;
using namespace SST::MemHierarchy;

/*----------------------------------------------------------------------------------------------------------------------
 * Bottom Coherence Controller Implementation
 *---------------------------------------------------------------------------------------------------------------------*/
	    
void MESIBottomCC::handleEviction(CacheLine* _wbCacheLine, uint32_t _groupId, string _origRqstr){
    State state = _wbCacheLine->getState();
    setGroupId(_groupId);

    switch(state){
    case S:
        inc_EvictionPUTSReqSent();
        sendWriteback(PutS, _wbCacheLine, _origRqstr);
        _wbCacheLine->setState(I);
	break;
    case E:
        inc_EvictionPUTEReqSent();
        sendWriteback(PutE, _wbCacheLine, _origRqstr);
	_wbCacheLine->setState(I);
        break;
    case M:
        inc_EvictionPUTMReqSent();
	sendWriteback(PutM, _wbCacheLine, _origRqstr);
        _wbCacheLine->setState(I);
	break;
    default:
	d_->fatal(CALL_INFO,-1,"BCC is in an invalid state during eviction: %s\n", BccLineString[state]);
    }
}


/**
 *  Handle request at bottomCC
 *  Obtain block if a cache miss
 *  Obtain needed coherence permission from lower level cache/memory if coherence miss
 */
void MESIBottomCC::handleRequest(MemEvent* _event, CacheLine* _cacheLine, Command _cmd, bool _mshrHit){
    bool upgrade;
    setGroupId(_event->getGroupId());
    d_->debug(_L6_,"BottomCC State = %s\n", BccLineString[_cacheLine->getState()]);
    
    switch(_cmd){
    case GetS:
        handleGetSRequest(_event, _cacheLine);
        break;
    case GetX:
    case GetSEx:
        upgrade = isUpgradeToModifiedNeeded(_event, _cacheLine);
        if(upgrade){
            forwardMessage(_event, _cacheLine, &_event->getPayload());
            return;
        }
        handleGetXRequest(_event, _cacheLine, _mshrHit);
        break;
    case PutS:
        inc_PUTSReqsReceived();
        break;
    case PutM:
        handlePutMRequest(_event, _cacheLine);
        break;
    case PutX:
    case PutXE:                             //TODO:  Case PutX:  PUTXReqsReceived_++;
        handlePutXRequest(_event, _cacheLine);
        break;
    case PutE:
        handlePutERequest(_cacheLine);
        break;
    default:
	d_->fatal(CALL_INFO,-1,"BCC received an unrecognized request: %s\n", CommandString[_cmd]);
    }
}



void MESIBottomCC::handleInvalidate(MemEvent* _event, CacheLine* _cacheLine, Command _cmd){
    /* Important note: 
       If cache line is in transition, ignore INV request (or send ack in needed) but keep state "in transition".
       Since this cache line is in transition, any other requests will be blocked in the MSHR. This makes the cache line
       behave as if it was in Invalid state.  Coherency is maintained because the request in transition wont proceed
       until AFTER the request that ignited the INV finishes.
       
       For SM transitions, this cache eventually gets M state since the requests is actually saved in the MSHR
       of the lwlvl cache
       
       Because of this, the lower level cache can proceed even though this cache line
       is not actually invalidated.  No need for an ack sent to lower level cache since only possible 
       transitional state is (SM);  lower level cache knows this state is in S state so it proceeds without the PutS(weak consistency).
       Alternatively, if it the request is marked with 'acksNeeded' (a lock request is in progress in the hgLvl cache),
       then a PutS is actually sent to maintain the locking mechanism
       
    */
    
    _cacheLine->atomicEnd();
    setGroupId(_event->getGroupId());
    
    if(_cacheLine->inTransition()){
        d_->debug(_L6_,"Cache line in transition.\n");
        if(_event->getAckNeeded() && _cacheLine->getState() == SM){  //no need to send ACK if state = IM
            inc_InvalidatePUTSReqSent();
            _cacheLine->setState(IM); 
            sendWriteback(PutS, _cacheLine, _event->getRqstr());
        }
        return;
    }
    
    switch(_cmd){
        case Inv:
			processInvRequest(_event, _cacheLine);
            break;
        case InvX:
			processInvXRequest(_event, _cacheLine);
            break;
	    default:
	    d_->fatal(CALL_INFO,-1,"BCC received an unrecognized invalidation request: %s\n", CommandString[_cmd]);
	}

}



void MESIBottomCC::handleResponse(MemEvent* _responseEvent, CacheLine* _cacheLine, MemEvent* _origRequest){
   
    updateCoherenceState(_cacheLine,_responseEvent->getGrantedState());

    Command origCmd = _origRequest->getCmd();
    if(!MemEvent::isDataRequest(origCmd)){
	d_->fatal(CALL_INFO,-1,"BCC received a response to an invalid command type. Invalid command: %s\n", CommandString[origCmd]);
    }

    _cacheLine->setData(_responseEvent->getPayload(), _responseEvent);
}


void MESIBottomCC::updateCoherenceState(CacheLine* _cacheLine, State _grantedState) {
    State state = _cacheLine->getState();
    switch (state) {
        case I:
            break;
        case S:
        case IS:
            if (_grantedState == E) _cacheLine->setState(E);
            else _cacheLine->setState(S);
            break;
        case E:
            break;
        case M:
        case IM:
        case SM:
            _cacheLine->setState(M);
            break;
        default:
	    d_->fatal(CALL_INFO,-1,"BCC is in an unrecognized state: %s\n", BccLineString[state]);
    }
}


void MESIBottomCC::handleFetchInvalidate(MemEvent* _event, CacheLine* _cacheLine, int _parentId, bool _mshrHit){
    setGroupId(_event->getGroupId());
    if(_cacheLine->inTransition() && !_event->getAckNeeded()) return;
    
    Command cmd = _event->getCmd();
    sendResponse(_event, _cacheLine, _parentId, _mshrHit);
    
    switch(cmd){
        case FetchInv:
            _cacheLine->setState(I);
            inc_FetchInvReqSent();
            break;
        case FetchInvX:
            _cacheLine->setState(S);
            inc_FetchInvXReqSent();
            break;
	    default:
	    d_->fatal(CALL_INFO,-1,"BCC received an unrecognized fetch invalidate request: %s\n", CommandString[cmd]);
	}
    
}


/*---------------------------------------------------------------------------------------------------
 * Helper Functions
 *--------------------------------------------------------------------------------------------------*/

bool MESIBottomCC::isCoherenceMiss(MemEvent* _event, CacheLine* _cacheLine) {
    Command cmd = _event->getCmd();
    State state = _cacheLine->getState();
    if (cmd == GetS) {
        if (state != I) { return false; }
        else { return true; }
    } else if (cmd == GetX || cmd == GetSEx) {
        if (state == S || state == I) { return true; }
        else { return false; }
    }
    d_->fatal(CALL_INFO,-1,"BCC received an unrecognized request: %s\n", CommandString[cmd]);
    return true;
}

bool MESIBottomCC::isUpgradeToModifiedNeeded(MemEvent* _event, CacheLine* _cacheLine){
    State state = _cacheLine->getState();

    if(state == S || state == I){
        if(state == S){
            inc_GETXMissSM(_event);
            _cacheLine->setState(SM);
        }
        else{
            inc_GETXMissIM(_event);
            _cacheLine->setState(IM);
        }
        return true;
    }
    return false;
}



void MESIBottomCC::handleGetXRequest(MemEvent* _event, CacheLine* _cacheLine, bool _mshrHit){
    State state = _cacheLine->getState();
    Command cmd = _event->getCmd();

    if(state == E) _cacheLine->setState(M);    /* set block to dirty */
    
    if(cmd == GetX){
        if(L1_ && (!_event->isStoreConditional() || _cacheLine->isAtomic()))
            _cacheLine->setData(_event->getPayload(), _event);
        if(L1_ && _event->queryFlag(MemEvent::F_LOCKED)){
            assert(_cacheLine->isLocked());
            _cacheLine->decLock();
        }
    }
    else{
        inc_GetSExReqsReceived(_mshrHit);
        if(L1_) _cacheLine->incLock();
    }
    inc_GETXHit(_event);
}



void MESIBottomCC::processInvRequest(MemEvent* _event, CacheLine* _cacheLine){
    State state = _cacheLine->getState();
    
    if(state == M || state == E){
        if(state == M){
            inc_InvalidatePUTMReqSent();
            sendWriteback(PutM, _cacheLine, _event->getRqstr());
        }
        else{
            inc_InvalidatePUTEReqSent();
            sendWriteback(PutE, _cacheLine, _event->getRqstr());
        }
        _cacheLine->setState(I);
    }
    else if(state == S){
        if(_event->getAckNeeded()) sendWriteback(PutS, _cacheLine, _event->getRqstr());
        _cacheLine->setState(I);
    }
    else d_->fatal(CALL_INFO,-1,"BCC received an invalidation but is not in a valid stable state: %s\n", BccLineString[state]);
}


/*
 *  Give up exclusive ownership of block and send writeback if dirty
 */
void MESIBottomCC::processInvXRequest(MemEvent* _event, CacheLine* _cacheLine){
    State state = _cacheLine->getState();
    
    if(state == M || state == E){
        _cacheLine->setState(S);
        inc_InvalidatePUTXReqSent();
        if(state == E) sendWriteback(PutXE, _cacheLine, _event->getRqstr());
        else           sendWriteback(PutX, _cacheLine, _event->getRqstr());
    }
    else if(state == S){
        if(_event->getAckNeeded()) sendWriteback(PutS, _cacheLine, _event->getRqstr());
        _cacheLine->setState(I);
    }
    else d_->fatal(CALL_INFO,-1,"BCC received an invalidation but is not in a valid stable state: %s\n", BccLineString[state]);
}



void MESIBottomCC::handleGetSRequest(MemEvent* _event, CacheLine* _cacheLine){
    State state = _cacheLine->getState();

    if(_event->isLoadLink()) _cacheLine->atomicStart();
    
    if(state != I) inc_GETSHit(_event);
    else{
        _cacheLine->setState(IS);
        forwardMessage(_event, _cacheLine, NULL);
        inc_GETSMissIS(_event);
    }
}



void MESIBottomCC::handlePutMRequest(MemEvent* _event, CacheLine* _cacheLine){
    updateCacheLineRxWriteback(_event, _cacheLine);
    inc_PUTMReqsReceived();
}



void MESIBottomCC::handlePutXRequest(MemEvent* _event, CacheLine* _cacheLine){
    updateCacheLineRxWriteback(_event, _cacheLine);
    inc_PUTXReqsReceived();
}



void MESIBottomCC::updateCacheLineRxWriteback(MemEvent* _event, CacheLine* _cacheLine){
    State state = _cacheLine->getState();
    assert(state == M || state == E);
    if(state == E && _event->getCmd() != PutXE) _cacheLine->setState(M);    // Update state if line was written
    if(_event->getCmd() != PutXE){
        _cacheLine->setData(_event->getPayload(), _event);                  //Only PutM/PutX write data in the cache line
        d_->debug(_L6_,"Data written to cache line\n");
    }
}



void MESIBottomCC::handlePutERequest(CacheLine* _cacheLine){
    State state = _cacheLine->getState();
    assert(state == E || state == M);
    inc_PUTEReqsReceived();
    
}


/*********************************************
 *  Methods for sending & receiving messages
 *********************************************/

/* 
 *  Handles: forwarding of requests
 */
void MESIBottomCC::forwardMessage(MemEvent* _event, CacheLine* _cacheLine, vector<uint8_t>* _data){
    Addr baseAddr = _cacheLine->getBaseAddr();
    unsigned int lineSize = _cacheLine->getLineSize();
    forwardMessage(_event, baseAddr, lineSize, _data);
}


/*
 *  Handles: forwarding of requests
 *  Latency:
 *      Cached access: tag access to determine that access needs to be forwarded (also record in MSHR) 
 *      Un-cached access: MSHR access to record in MSHR
 */
void MESIBottomCC::forwardMessage(MemEvent* _event, Addr _baseAddr, unsigned int _lineSize, vector<uint8_t>* _data){
    /* Create event to be forwarded */
    MemEvent* forwardEvent;
    forwardEvent = new MemEvent(*_event);
    forwardEvent->setSrc(((Component*)owner_)->getName());
    forwardEvent->setDst(getDestination(_baseAddr));
    forwardEvent->setSize(_lineSize);
    
    /* Determine latency in cycles */
    uint64 deliveryTime;
    if(_event->queryFlag(MemEvent::F_NONCACHEABLE)){
        forwardEvent->setFlag(MemEvent::F_NONCACHEABLE);
        deliveryTime = timestamp_ + mshrLatency_;
    }
    else deliveryTime = timestamp_ + tagLatency_; 
    
    Response fwdReq = {forwardEvent, deliveryTime, false};
    addToOutgoingQueue(fwdReq);
    d_->debug(_L3_,"BCC - Forwarding request at cycle = %"PRIu64"\n", deliveryTime);
}


/*
 *  Handles: resending NACKed events
 *  Latency: MSHR access to find NACKed event
 */
void MESIBottomCC::resendEvent(MemEvent* _event){
    uint64 deliveryTime =  timestamp_ + mshrLatency_;
    Response resp = {_event, deliveryTime, false};
    addToOutgoingQueue(resp);
    
    d_->debug(_L3_,"BCC - Sending request: Addr = %"PRIx64", BaseAddr = %"PRIx64", Cmd = %s\n",
             _event->getAddr(), _event->getBaseAddr(), CommandString[_event->getCmd()]);
}


/*
 *  Handles: responses to fetch invalidates
 *  Latency: cache access to read data for payload  
 */
void MESIBottomCC::sendResponse(MemEvent* _event, CacheLine* _cacheLine, int _parentId, bool _mshrHit){
    MemEvent *responseEvent = _event->makeResponse();
    responseEvent->setPayload(*_cacheLine->getData());

    uint64 deliveryTime = _mshrHit ? timestamp_ + mshrLatency_ : timestamp_ + accessLatency_;
    Response resp  = {responseEvent, deliveryTime, true};
    addToOutgoingQueue(resp);
    
    d_->debug(_L3_,"BCC - Sending Response at cycle = %"PRIu64", Cmd = %s, Src = %s\n", deliveryTime, CommandString[responseEvent->getCmd()], responseEvent->getSrc().c_str());
}


/*
 *  Handles: sending writebacks
 *  Latency: cache access + tag to read data that is being written back and update coherence state
 */
void MESIBottomCC::sendWriteback(Command _cmd, CacheLine* _cacheLine, string _origRqstr){
    MemEvent* newCommandEvent = new MemEvent((SST::Component*)owner_, _cacheLine->getBaseAddr(), _cacheLine->getBaseAddr(), _cmd);
    newCommandEvent->setDst(getDestination(_cacheLine->getBaseAddr()));
    if(_cmd == PutM || _cmd == PutX){
        newCommandEvent->setSize(_cacheLine->getLineSize());
        newCommandEvent->setPayload(*_cacheLine->getData());
    }
    newCommandEvent->setRqstr(_origRqstr);
    
    uint64 deliveryTime = timestamp_ + accessLatency_;
    Response resp = {newCommandEvent, deliveryTime, false};
    addToOutgoingQueue(resp);
    d_->debug(_L3_,"BCC - Sending Writeback at cycle = %"PRIu64", Cmd = %s\n", deliveryTime, CommandString[_cmd]);
}


/*
 *  Handles: sending NACKs
 *  Latency: tag/state access to determine whether event needs to be NACKed
 */
void MESIBottomCC::sendNACK(MemEvent* _event){
    setGroupId(_event->getGroupId());
    MemEvent *NACKevent = _event->makeNACKResponse((Component*)owner_, _event);
    uint64 deliveryTime      = timestamp_ + tagLatency_;
    
    Response resp = {NACKevent, deliveryTime, true};
    inc_NACKsSent();
    addToOutgoingQueue(resp);
    d_->debug(_L3_,"BCC - Sending NACK at cycle = %"PRIu64"\n", deliveryTime);
}

/********************
 * Helper functions
 ********************/

/*
 *  Set list of next lower level caches
 */
void MESIBottomCC::setNextLevelCache(vector<string>* _nlc) {
    nextLevelCacheNames_ = _nlc; 
}

/*
 *  Find destination for a request to a lower level cache
 *  For distributed shared cache, identify target cache slice
 *  using round-robin distribution of cache blocks
 */
string MESIBottomCC::getDestination(Addr baseAddr) {
    if (nextLevelCacheNames_->size() == 1) {
        return nextLevelCacheNames_->front();
    } else if (nextLevelCacheNames_->size() > 1) {
        // round robin for now
        int index = (baseAddr/lineSize_) % nextLevelCacheNames_->size();
        return (*nextLevelCacheNames_)[index];
    } else {
        return "";
    }
}

/*
 *  Print stats
 */
void MESIBottomCC::printStats(int _stats, vector<int> _groupIds, map<int, CtrlStats> _ctrlStats, uint64_t _updgradeLatency){
    Output* dbg = new Output();
    dbg->init("", 0, 0, (Output::output_location_t)_stats);
    dbg->output(CALL_INFO,"\n------------------------------------------------------------------------\n");
    dbg->output(CALL_INFO,"--- Cache Stats\n");
    dbg->output(CALL_INFO,"--- Name: %s\n", ownerName_.c_str());
    dbg->output(CALL_INFO,"--- Overall Statistics\n");
    dbg->output(CALL_INFO,"------------------------------------------------------------------------\n");

    for(unsigned int i = 0; i < _groupIds.size(); i++){
        uint64_t totalMisses = stats_[_groupIds[i]].GETXMissIM_      + stats_[_groupIds[i]].GETXMissSM_ + stats_[_groupIds[i]].GETSMissIS_ +
                               stats_[_groupIds[i]].GETSMissBlocked_ + stats_[_groupIds[i]].GETXMissBlocked_;
        uint64_t totalHits = stats_[_groupIds[i]].GETSHit_ + stats_[_groupIds[i]].GETXHit_;
        uint64_t totalRequests = totalHits + totalMisses;
        double hitRatio = ((double)totalHits / ( totalHits + totalMisses)) * 100;
        
        if(i != 0){
            dbg->output(CALL_INFO,"------------------------------------------------------------------------\n");
            dbg->output(CALL_INFO,"--- Cache Stats\n");
            dbg->output(CALL_INFO,"--- Name: %s\n", ownerName_.c_str());
            dbg->output(CALL_INFO,"--- Group Statistics, Group ID = %i\n", _groupIds[i]);
            dbg->output(CALL_INFO,"------------------------------------------------------------------------\n");
        }
        dbg->output(CALL_INFO,"- Total data requests:                            %"PRIu64"\n", totalRequests);
        dbg->output(CALL_INFO,"- Total misses:                                   %"PRIu64"\n", totalMisses);
        dbg->output(CALL_INFO,"- Total hits:                                     %"PRIu64"\n", totalHits);
        dbg->output(CALL_INFO,"- Hit ratio:                                      %.3f%%\n", hitRatio);
        dbg->output(CALL_INFO,"- Miss ratio:                                     %.3f%%\n", 100 - hitRatio);
        dbg->output(CALL_INFO,"- Read misses:                                    %"PRIu64"\n", stats_[_groupIds[i]].GETSMissIS_);
        dbg->output(CALL_INFO,"- Write misses:                                   %"PRIu64"\n", stats_[_groupIds[i]].GETXMissSM_ + stats_[_groupIds[i]].GETXMissIM_);
        dbg->output(CALL_INFO,"- GetS received:                                  %"PRIu64"\n", stats_[_groupIds[i]].GETSMissIS_ + stats_[_groupIds[i]].GETSHit_);
        dbg->output(CALL_INFO,"- GetX received:                                  %"PRIu64"\n", stats_[_groupIds[i]].GETXMissSM_ + stats_[_groupIds[i]].GETXMissIM_ + stats_[_groupIds[i]].GETXHit_);
        dbg->output(CALL_INFO,"- GetSEx received:                                %"PRIu64"\n", stats_[_groupIds[i]].GetSExReqsReceived_);
        dbg->output(CALL_INFO,"- GetS-IS misses:                                 %"PRIu64"\n", stats_[_groupIds[i]].GETSMissIS_);
        dbg->output(CALL_INFO,"- GetS-Blocked misses:                            %"PRIu64"\n", stats_[_groupIds[i]].GETSMissBlocked_);
        dbg->output(CALL_INFO,"- GetX-SM misses:                                 %"PRIu64"\n", stats_[_groupIds[i]].GETXMissSM_);
        dbg->output(CALL_INFO,"- GetX-IM misses:                                 %"PRIu64"\n", stats_[_groupIds[i]].GETXMissIM_);
        dbg->output(CALL_INFO,"- GetX-Blocked misses:                            %"PRIu64"\n", stats_[_groupIds[i]].GETXMissBlocked_);
        dbg->output(CALL_INFO,"- GetS hits:                                      %"PRIu64"\n", stats_[_groupIds[i]].GETSHit_);
        dbg->output(CALL_INFO,"- GetX hits:                                      %"PRIu64"\n", stats_[_groupIds[i]].GETXHit_);
        dbg->output(CALL_INFO,"- Avg Updgrade Latency (cyc):                     %"PRIu64"\n", _updgradeLatency);
        if(!L1_){
            dbg->output(CALL_INFO,"- PutS received:                                  %"PRIu64"\n", stats_[_groupIds[i]].PUTSReqsReceived_);
            dbg->output(CALL_INFO,"- PutM received:                                  %"PRIu64"\n", stats_[_groupIds[i]].PUTMReqsReceived_);
            dbg->output(CALL_INFO,"- PutX received:                                  %"PRIu64"\n", stats_[_groupIds[i]].PUTXReqsReceived_);
        }
        else{
            assert(stats_[_groupIds[i]].PUTSReqsReceived_ == 0);
            assert(stats_[_groupIds[i]].PUTMReqsReceived_ == 0);
            assert(stats_[_groupIds[i]].PUTXReqsReceived_ == 0);
        }
        dbg->output(CALL_INFO,"- PUTM sent due to [inv, evictions]:              [%"PRIu64", %"PRIu64"]\n", stats_[_groupIds[i]].InvalidatePUTMReqSent_, stats_[_groupIds[i]].EvictionPUTSReqSent_);
        dbg->output(CALL_INFO,"- PUTE sent due to [inv, evictions]:              [%"PRIu64", %"PRIu64"]\n", stats_[_groupIds[i]].InvalidatePUTEReqSent_, stats_[_groupIds[i]].EvictionPUTMReqSent_);
        dbg->output(CALL_INFO,"- PUTX sent due to [inv, evictions]:              [%"PRIu64", %"PRIu64"]\n", stats_[_groupIds[i]].InvalidatePUTXReqSent_, stats_[_groupIds[i]].EvictionPUTEReqSent_);
        dbg->output(CALL_INFO,"- Inv stalled because LOCK held:                  %"PRIu64"\n", _ctrlStats[_groupIds[i]].InvWaitingForUserLock_);
        dbg->output(CALL_INFO,"- Requests received (incl coherence traffic):     %"PRIu64"\n", _ctrlStats[_groupIds[i]].TotalRequestsReceived_);
        dbg->output(CALL_INFO,"- Requests handled by MSHR (MSHR hits):           %"PRIu64"\n", _ctrlStats[_groupIds[i]].TotalMSHRHits_);
        if(!L1_) dbg->output(CALL_INFO,"- NACKs sent (MSHR Full, BottomCC):               %"PRIu64"\n", stats_[_groupIds[i]].NACKsSent_);
        else    assert(stats_[_groupIds[i]].NACKsSent_ == 0);
    }

}


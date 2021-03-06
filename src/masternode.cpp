// Copyright (c) 2014-2017 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activemasternode.h"
#include "init.h"
#include "netbase.h"
#include "masternode.h"
#include "masternode-payments.h"
#include "masternode-sync.h"
#include "masternodeman.h"
#include "messagesigner.h"
#include "util.h"
#include "masternode-list.h"

#include <boost/lexical_cast.hpp>


CMasternode::CMasternode() :
    masternode_info_t{ MASTERNODE_ENABLED, PROTOCOL_VERSION, GetAdjustedTime()}
{}

CMasternode::CMasternode(CService addr, CPubKey pubKeyMasternode,
                         CBitcoinAddress payee, int nProtocolVersionIn) :
    masternode_info_t{MASTERNODE_ENABLED, nProtocolVersionIn,
                      GetAdjustedTime(), addr, pubKeyMasternode,
                      payee}
{}

CMasternode::CMasternode(const CMasternode& other) :
    masternode_info_t{other},
    lastPing(other.lastPing),
    vchSig(other.vchSig),
    nCollateralMinConfBlockHash(other.nCollateralMinConfBlockHash),
    nBlockLastPaid(other.nBlockLastPaid),
    nPoSeBanScore(other.nPoSeBanScore),
    nPoSeBanHeight(other.nPoSeBanHeight),
    fUnitTest(other.fUnitTest)
{}

CMasternode::CMasternode(const CMasternodeBroadcast& mnb) :
    masternode_info_t{ mnb.nActiveState, mnb.nProtocolVersion, mnb.sigTime,
                       mnb.addr, mnb.pubKeyMasternode, mnb.payee,
                       mnb.sigTime},
    lastPing(mnb.lastPing),
    vchSig(mnb.vchSig)
{}

//
// When a new masternode broadcast is sent, update our information
//
bool CMasternode::UpdateFromNewBroadcast(CMasternodeBroadcast& mnb, CConnman& connman)
{
    if(mnb.sigTime <= sigTime && !mnb.fRecovery) return false;

    pubKeyMasternode = mnb.pubKeyMasternode;
    sigTime = mnb.sigTime;
    vchSig = mnb.vchSig;
    nProtocolVersion = mnb.nProtocolVersion;
    addr = mnb.addr;
    nPoSeBanScore = 0;
    nPoSeBanHeight = 0;
    nTimeLastChecked = 0;
    int nDos = 0;
    if (mnb.lastPing == CMasternodePing() ||
        (mnb.lastPing != CMasternodePing() &&
         mnb.lastPing.CheckAndUpdate(this, true, nDos, connman))) {
        lastPing = mnb.lastPing;
        mnodeman.mapSeenMasternodePing.insert(std::make_pair(lastPing.GetHash(), lastPing));
    }
    // if it matches our Masternode privkey...
    if (fMasterNode && pubKeyMasternode == activeMasternode.pubKeyMasternode) {
        nPoSeBanScore = -MASTERNODE_POSE_BAN_MAX_SCORE;
        if (nProtocolVersion == PROTOCOL_VERSION) {
            // ... and PROTOCOL_VERSION, then we've been remotely activated ...
            activeMasternode.ManageState(connman);
        } else {
            // ... otherwise we need to reactivate our node, do not add it to the list and do not relay
            // but also do not ban the node we get this message from
            LogPrintf("CMasternode::UpdateFromNewBroadcast -- wrong PROTOCOL_VERSION, re-activate your MN: message nProtocolVersion=%d  PROTOCOL_VERSION=%d\n", nProtocolVersion, PROTOCOL_VERSION);
            return false;
        }
    }
    return true;
}

//
// Deterministically calculate a given "score" for a Masternode depending on how close it's hash is to
// the proof of work for that block. The further away they are the better, the furthest will win the election
// and get paid this block
//
arith_uint256 CMasternode::CalculateScore(const uint256& blockHash)
{
    if (fDIP0001WasLockedIn) {
        // Deterministically calculate a "score" for a Masternode based on any given (block)hash
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << pubKeyMasternode << nCollateralMinConfBlockHash << blockHash;
        return UintToArith256(ss.GetHash());
    }

    // TODO: remove calculations below after migration to 12.2

    uint256 aux = ArithToUint256(UintToArith256(pubKeyMasternode.GetHash()));

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << blockHash;
    arith_uint256 hash2 = UintToArith256(ss.GetHash());

    CHashWriter ss2(SER_GETHASH, PROTOCOL_VERSION);
    ss2 << blockHash;
    ss2 << aux;
    arith_uint256 hash3 = UintToArith256(ss2.GetHash());

    return (hash3 > hash2 ? hash3 - hash2 : hash2 - hash3);
}

void CMasternode::Check(bool fForce)
{
    LOCK(cs);

    if(ShutdownRequested()) return;

    if(!fForce && (GetTime() - nTimeLastChecked < MASTERNODE_CHECK_SECONDS)) return;
    nTimeLastChecked = GetTime();

    LogPrint("masternode", "CMasternode::Check -- Masternode %s is in %s state\n",
             pubKeyMasternode.GetID().ToString() , GetStateString());

    /* ToDo: it is a good place to check whether the masternode is still on
     * the list propagated with spork mechanism.
     */
    if (!masternodeListManager.IsMNActive(pubKeyMasternode)) {
        LogPrint("masternode", "CMasternode::Check -- Masternode %s is not on the list\n",
                 HexStr(pubKeyMasternode));
        return;
    }

    int nHeight = 0;

    nHeight = chainActive.Height();

    if (IsPoSeBanned()) {
        if (nHeight < nPoSeBanHeight) return; // too early?
        // Otherwise give it a chance to proceed further to do all the usual checks and to change its state.
        // Masternode still will be on the edge and can be banned back easily if it keeps ignoring mnverify
        // or connect attempts. Will require few mnverify messages to strengthen its position in mn list.
        LogPrintf("CMasternode::Check -- Masternode %s is unbanned and back in list now\n",
                  pubKeyMasternode.GetID().ToString());
        DecreasePoSeBanScore();
    } else if (nPoSeBanScore >= MASTERNODE_POSE_BAN_MAX_SCORE) {
        nActiveState = MASTERNODE_POSE_BAN;
        // ban for the whole payment cycle
        nPoSeBanHeight = nHeight + mnodeman.size();
        LogPrintf("CMasternode::Check -- Masternode %s is banned till block %d now\n",
                  pubKeyMasternode.GetID().ToString(), nPoSeBanHeight);
        return;
    }

    int nActiveStatePrev = nActiveState;
    bool fOurMasternode = fMasterNode && activeMasternode.pubKeyMasternode == pubKeyMasternode;

                   // masternode doesn't meet payment protocol requirements ...
    bool fRequireUpdate = nProtocolVersion < mnpayments.GetMinMasternodePaymentsProto() ||
                   // or it's our own node and we just updated it to the new protocol but we are still waiting for activation ...
                   (fOurMasternode && nProtocolVersion < PROTOCOL_VERSION);

    if (fRequireUpdate) {
        nActiveState = MASTERNODE_UPDATE_REQUIRED;
        if(nActiveStatePrev != nActiveState) {
            LogPrint("masternode", "CMasternode::Check -- Masternode %s is in %s state now\n",
            pubKeyMasternode.GetID().ToString(), GetStateString());
        }
        return;
    }

    // keep old masternodes on start, give them a chance to receive updates...
    bool fWaitForPing = !masternodeSync.IsMasternodeListSynced() && !IsPingedWithin(MASTERNODE_MIN_MNP_SECONDS);

    if (fWaitForPing && !fOurMasternode) {
        // ...but if it was already expired before the initial check - return right away
        if (IsExpired() || IsNewStartRequired()) {
            LogPrint("masternode", "CMasternode::Check -- Masternode %s is in %s state, waiting for ping\n",
                     pubKeyMasternode.GetID().ToString(), GetStateString());
            return;
        }
    }

    // don't expire if we are still in "waiting for ping" mode unless it's our own masternode
    if (!fWaitForPing || fOurMasternode) {

        if (!IsPingedWithin(MASTERNODE_NEW_START_REQUIRED_SECONDS)) {
            nActiveState = MASTERNODE_NEW_START_REQUIRED;
            if(nActiveStatePrev != nActiveState) {
                LogPrint("masternode", "CMasternode::Check -- Masternode %s is in %s state now\n",
                         pubKeyMasternode.GetID().ToString(), GetStateString());
            }
            return;
        }

        LogPrint("masternode", "CMasternode::Check -- outpoint=%s, GetAdjustedTime()=%d\n",
                 pubKeyMasternode.GetID().ToString(), GetAdjustedTime());

        if (!IsPingedWithin(MASTERNODE_EXPIRATION_SECONDS)) {
            nActiveState = MASTERNODE_EXPIRED;
            if(nActiveStatePrev != nActiveState) {
                LogPrint("masternode", "CMasternode::Check -- Masternode %s is in %s state now\n",
                         pubKeyMasternode.GetID().ToString(), GetStateString());
            }
            return;
        }
    }

    if (lastPing.sigTime - sigTime < MASTERNODE_MIN_MNP_SECONDS) {
        nActiveState = MASTERNODE_PRE_ENABLED;
        if (nActiveStatePrev != nActiveState) {
            LogPrint("masternode", "CMasternode::Check -- Masternode %s is in %s state now\n",
                     pubKeyMasternode.GetID().ToString(), GetStateString());
        }
        return;
    }

    nActiveState = MASTERNODE_ENABLED; // OK
    if (nActiveStatePrev != nActiveState) {
        LogPrint("masternode", "CMasternode::Check -- Masternode %s is in %s state now\n",
                 pubKeyMasternode.GetID().ToString(), GetStateString());
    }
}

bool CMasternode::IsValidNetAddr()
{
    return IsValidNetAddr(addr);
}

bool CMasternode::IsValidNetAddr(CService addrIn)
{
    // TODO: regtest is fine with any addresses for now,
    // should probably be a bit smarter if one day we start to implement tests for this
    return Params().NetworkIDString() == CBaseChainParams::REGTEST ||
            (addrIn.IsIPv4() && IsReachable(addrIn) && addrIn.IsRoutable());
}

masternode_info_t CMasternode::GetInfo()
{
    masternode_info_t info{*this};
    info.nTimeLastPing = lastPing.sigTime;
    info.fInfoValid = true;
    return info;
}

std::string CMasternode::StateToString(int nStateIn)
{
    switch(nStateIn) {
        case MASTERNODE_PRE_ENABLED:            return "PRE_ENABLED";
        case MASTERNODE_ENABLED:                return "ENABLED";
        case MASTERNODE_EXPIRED:                return "EXPIRED";
        case MASTERNODE_UPDATE_REQUIRED:        return "UPDATE_REQUIRED";
        case MASTERNODE_NEW_START_REQUIRED:     return "NEW_START_REQUIRED";
        case MASTERNODE_POSE_BAN:               return "POSE_BAN";
        default:                                return "UNKNOWN";
    }
}

std::string CMasternode::GetStateString() const
{
    return StateToString(nActiveState);
}

std::string CMasternode::GetStatus() const
{
    // TODO: return smth a bit more human readable here
    return GetStateString();
}

void CMasternode::UpdateLastPaid(const CBlockIndex *pindex, int nMaxBlocksToScanBack)
{
    if(!pindex) return;

    const CBlockIndex *BlockReading = pindex;

    // ToDo: FIX IT
    CScript mnpayee = GetScriptForDestination(payee.Get());

    LOCK(cs_mapMasternodeBlocks);

    for (int i = 0; BlockReading && BlockReading->nHeight > nBlockLastPaid && i < nMaxBlocksToScanBack; i++) {
        if(mnpayments.mapMasternodeBlocks.count(BlockReading->nHeight) &&
            mnpayments.mapMasternodeBlocks[BlockReading->nHeight].HasPayeeWithVotes(mnpayee, 2))
        {
            CBlock block;
            if(!ReadBlockFromDisk(block, BlockReading, Params().GetConsensus())) // shouldn't really happen
                continue;

            CAmount nMasternodePayment = GetMasternodePayment(BlockReading->nHeight, block.vtx[0].GetValueOut());

            BOOST_FOREACH(CTxOut txout, block.vtx[0].vout)
                if (mnpayee == txout.scriptPubKey && nMasternodePayment == txout.nValue) {
                    nBlockLastPaid = BlockReading->nHeight;
                    nTimeLastPaid = BlockReading->nTime;
                    LogPrint("masternode", "CMasternode::UpdateLastPaidBlock -- "
                             "searching for block with payment to %s -- found new %d\n",
                             payee.ToString(), nBlockLastPaid);
                    return;
                }
        }

        if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
        BlockReading = BlockReading->pprev;
    }

    // Last payment for this masternode wasn't found in latest mnpayments blocks
    // or it was found in mnpayments blocks but wasn't found in the blockchain.
    // LogPrint("masternode", "CMasternode::UpdateLastPaidBlock -- searching for block with payment to %s -- keeping old %d\n", vin.prevout.ToStringShort(), nBlockLastPaid);
}

bool CMasternodeBroadcast::Create(std::string strService, std::string strKeyMasternode,
                                  std::string strPayee, std::string& strErrorRet,
                                  CMasternodeBroadcast &mnbRet, bool fOffline)
{
    CBitcoinAddress payeeNew(strPayee);
    CPubKey pubKeyMasternodeNew;
    CKey keyMasternodeNew;

    auto Log = [&strErrorRet](std::string sErr)->bool
    {
        strErrorRet = sErr;
        LogPrintf("CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    };

    //need correct blocks to send ping
    if (!fOffline && !masternodeSync.IsBlockchainSynced())
        return Log("Sync in progress. Must wait until sync is complete to start Masternode");

    if (!CMessageSigner::GetKeysFromSecret(strKeyMasternode, keyMasternodeNew,
                                           pubKeyMasternodeNew)) {
            return Log(strprintf("Invalid masternode key %s", strKeyMasternode));
    }

    CService service;

    if (!Lookup(strService.c_str(), service, 0, false)) {
        return Log(strprintf("Invalid address %s for masternode.", strService));
    }

    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();

    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (service.GetPort() != mainnetDefaultPort) {
            return Log(strprintf("Invalid port %u for masternode %s, only %d is supported on mainnet.",
                                 service.GetPort(), strService, mainnetDefaultPort));
        }
    } else if (service.GetPort() == mainnetDefaultPort) {
        return Log(strprintf("Invalid port %u for masternode %s, %d is the only supported on mainnet.",
                             service.GetPort(), strService, mainnetDefaultPort));
    }

    return Create(service, keyMasternodeNew, pubKeyMasternodeNew, payeeNew,
                  strErrorRet, mnbRet);
}

bool CMasternodeBroadcast::Create(const CService& service, const CKey& keyMasternodeNew,
                                  const CPubKey& pubKeyMasternodeNew, const CBitcoinAddress& payeeNew,
                                  std::string &strErrorRet, CMasternodeBroadcast &mnbRet)
{
    // wait for reindex and/or import to finish
    if (fImporting || fReindex) return false;

    LogPrint("masternode", "CMasternodeBroadcast::Create -- pubKeyMasternodeNew.GetID() = %s\n",
             pubKeyMasternodeNew.GetID().ToString());

    auto Log = [&strErrorRet,&mnbRet](std::string sErr)->bool
    {
        strErrorRet = sErr;
        LogPrintf("CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CMasternodeBroadcast();
        return false;
    };

    CMasternodePing mnp(pubKeyMasternodeNew);

    if (!mnp.Sign(keyMasternodeNew, pubKeyMasternodeNew))
        return Log(strprintf("Failed to sign ping, masternode=%s",
                             pubKeyMasternodeNew.GetID().ToString()));

    mnbRet = CMasternodeBroadcast(service, pubKeyMasternodeNew,
                                  payeeNew, PROTOCOL_VERSION);

    if (!mnbRet.IsValidNetAddr())
        return Log(strprintf("Invalid IP address, masternode=%s",
                             pubKeyMasternodeNew.GetID().ToString()));

    mnbRet.lastPing = mnp;

    if (!mnbRet.Sign(keyMasternodeNew))
        return Log(strprintf("Failed to sign broadcast, masternode=%s",
                             pubKeyMasternodeNew.GetID().ToString()));

    return true;
}

bool CMasternodeBroadcast::SimpleCheck(int& nDos)
{
    nDos = 0;

    // make sure addr is valid
    if (!IsValidNetAddr()) {
        LogPrintf("CMasternodeBroadcast::SimpleCheck -- Invalid addr, rejected: masternode=%s  addr=%s\n",
                  pubKeyMasternode.GetID().ToString(), addr.ToString());
        return false;
    }

    // make sure signature isn't in the future (past is OK)
    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrintf("CMasternodeBroadcast::SimpleCheck -- Signature rejected, too far into the future: masternode=%s\n",
                  pubKeyMasternode.GetID().ToString());
        nDos = 1;
        return false;
    }

    // empty ping or incorrect sigTime/unknown blockhash
    if (lastPing == CMasternodePing() || !lastPing.SimpleCheck(nDos)) {
        // one of us is probably forked or smth, just mark it as expired and check the rest of the rules
        nActiveState = MASTERNODE_EXPIRED;
    }

    if (nProtocolVersion < mnpayments.GetMinMasternodePaymentsProto()) {
        LogPrintf("CMasternodeBroadcast::SimpleCheck -- ignoring outdated Masternode: masternode=%s  nProtocolVersion=%d\n",
                  pubKeyMasternode.GetID().ToString(), nProtocolVersion);
        return false;
    }

    CScript pubkeyScript;
    pubkeyScript = GetScriptForDestination(payee.Get());

    if (pubkeyScript.size() != 25) {
        LogPrintf("CMasternodeBroadcast::SimpleCheck -- pubKeyReward has the wrong size\n");
        nDos = 100;
        return false;
    }

    CScript pubkeyScript2;
    pubkeyScript2 = GetScriptForDestination(pubKeyMasternode.GetID());

    if (pubkeyScript2.size() != 25) {
        LogPrintf("CMasternodeBroadcast::SimpleCheck -- pubKeyMasternode has the wrong size\n");
        nDos = 100;
        return false;
    }

    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (addr.GetPort() != mainnetDefaultPort) {
            return false;
        }
    } else if (addr.GetPort() == mainnetDefaultPort) {
        return false;
    }

    return true;
}

bool CMasternodeBroadcast::Update(CMasternode* pmn, int& nDos, CConnman& connman)
{
    nDos = 0;

    if(pmn->sigTime == sigTime && !fRecovery) {
        // mapSeenMasternodeBroadcast in CMasternodeMan::CheckMnbAndUpdateMasternodeList should filter legit duplicates
        // but this still can happen if we just started, which is ok, just do nothing here.
        return false;
    }

    // this broadcast is older than the one that we already have - it's bad and should never happen
    // unless someone is doing something fishy
    if(pmn->sigTime > sigTime) {
        LogPrintf("CMasternodeBroadcast::Update -- Bad sigTime %d (existing broadcast is at %d) for Masternode %s %s\n",
                      sigTime, pmn->sigTime, pubKeyMasternode.GetID().ToString(), addr.ToString());
        return false;
    }

    pmn->Check();

    // masternode is banned by PoSe
    if (pmn->IsPoSeBanned()) {
        LogPrintf("CMasternodeBroadcast::Update -- Banned by PoSe, masternode=%s\n", pubKeyMasternode.GetID().ToString());
        return false;
    }

    if (pmn->payee.CompareTo(payee) != 0) {
        LogPrintf("CMasternodeBroadcast::Update -- Got mismatched payee, %s vs %s\n",
        		pmn->payee.ToString(), payee.ToString());
        nDos = 33;
        return false;
    }

    if (!CheckSignature(nDos)) {
        LogPrintf("CMasternodeBroadcast::Update -- CheckSignature() failed, masternode=%s\n", pubKeyMasternode.GetID().ToString());
        return false;
    }

    // if ther was no masternode broadcast recently or if it matches our Masternode privkey...
    if ((!pmn->IsBroadcastedWithin(MASTERNODE_MIN_MNB_SECONDS)) ||
        (fMasterNode && pubKeyMasternode == activeMasternode.pubKeyMasternode)) {
        // take the newest entry
        LogPrintf("CMasternodeBroadcast::Update -- Got UPDATED Masternode entry: addr=%s\n", addr.ToString());

        if (pmn->UpdateFromNewBroadcast(*this, connman)) {
            pmn->Check();
            Relay(connman);
        }

        masternodeSync.BumpAssetLastTime("CMasternodeBroadcast::Update");
    }

    return true;
}

bool CMasternodeBroadcast::CheckMasternode(int& nDos)
{
    // we are a masternode with the same vin (i.e. already activated) and this mnb is ours (matches our Masternode privkey)
    // so nothing to do here for us
    if (fMasterNode && pubKeyMasternode == activeMasternode.pubKeyMasternode) {
        if (mnodeman.Has(pubKeyMasternode)) {
            LogPrintf("CMasternodeBroadcast::CheckMasternode -- Masternode already added\n");
            return false;
        } else {
            LogPrintf("CMasternodeBroadcast::CheckMasternode -- Target masternode reached\n");
            return true;
        }
    }

    if (!CheckSignature(nDos)) {
        LogPrintf("CMasternodeBroadcast::CheckMasternode -- CheckSignature() failed, masternode=%s\n",
                  pubKeyMasternode.GetID().ToString());
        return false;
    }

    {
        TRY_LOCK(cs_main, lockMain);
        if(!lockMain) {
            // not mnb fault, let it to be checked again later
            LogPrint("masternode", "CMasternodeBroadcast::CheckMasternode -- Failed to acquire lock, addr=%s", addr.ToString());
            mnodeman.mapSeenMasternodeBroadcast.erase(GetHash());
            return false;
        }
    }

    LogPrint("masternode", "CMasternodeBroadcast::CheckMasternode -- Masternode verified\n");

    return true;
}

bool CMasternodeBroadcast::Sign(const CKey& keyMasternode)
{
    std::string strError;
    std::string strMessage;

    sigTime = GetAdjustedTime();

    strMessage = addr.ToString(false) + boost::lexical_cast<std::string>(sigTime) +
                    pubKeyMasternode.GetID().ToString() +
                    payee.ToString() +
                    boost::lexical_cast<std::string>(nProtocolVersion);

    if (!CMessageSigner::SignMessage(strMessage, vchSig, keyMasternode)) {
        LogPrintf("CMasternodeBroadcast::Sign -- SignMessage() failed\n");
        return false;
    }

    if (!CMessageSigner::VerifyMessage(pubKeyMasternode, vchSig, strMessage, strError)) {
        LogPrintf("CMasternodeBroadcast::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CMasternodeBroadcast::CheckSignature(int& nDos)
{
    std::string strMessage;
    std::string strError = "";
    nDos = 0;

    strMessage = addr.ToString(false) + boost::lexical_cast<std::string>(sigTime) +
                    pubKeyMasternode.GetID().ToString() +
                    payee.ToString() +
                    boost::lexical_cast<std::string>(nProtocolVersion);

    LogPrint("masternode", "CMasternodeBroadcast::CheckSignature -- strMessage: %s  pubKeyMasternode: %s  sig: %s\n",
             strMessage, pubKeyMasternode.GetID().ToString(),
             EncodeBase64(&vchSig[0], vchSig.size()));

    if(!CMessageSigner::VerifyMessage(pubKeyMasternode, vchSig, strMessage, strError)){
        LogPrintf("CMasternodeBroadcast::CheckSignature -- "
                  "Got bad Masternode announce signature, error: %s\n", strError);
        nDos = 100;
        return false;
    }

    return true;
}

void CMasternodeBroadcast::Relay(CConnman& connman)
{
    CInv inv(MSG_MASTERNODE_ANNOUNCE, GetHash());
    connman.RelayInv(inv);
}

CMasternodePing::CMasternodePing(const CPubKey& pubKey)
{
    LOCK(cs_main);
    if (!chainActive.Tip() || chainActive.Height() < 12) return;

    pubKeyMasternode = pubKey;
    blockHash = chainActive[chainActive.Height() - 12]->GetBlockHash();
    sigTime = GetAdjustedTime();
}

bool CMasternodePing::Sign(const CKey& keyMasternode, const CPubKey& pubKeyMasternode)
{
    std::string strError;
    std::string strMasterNodeSignMessage;

    sigTime = GetAdjustedTime();
    std::string strMessage = pubKeyMasternode.GetID().ToString() + blockHash.ToString() +
                             boost::lexical_cast<std::string>(sigTime);

    if(!CMessageSigner::SignMessage(strMessage, vchSig, keyMasternode)) {
        LogPrintf("CMasternodePing::Sign -- SignMessage() failed\n");
        return false;
    }

    if(!CMessageSigner::VerifyMessage(pubKeyMasternode, vchSig, strMessage, strError)) {
        LogPrintf("CMasternodePing::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CMasternodePing::CheckSignature(CPubKey& pubKeyMasternode, int &nDos)
{
    std::string strMessage = pubKeyMasternode.GetID().ToString() + blockHash.ToString() +
                             boost::lexical_cast<std::string>(sigTime);
    std::string strError = "";
    nDos = 0;

    if (!CMessageSigner::VerifyMessage(pubKeyMasternode, vchSig, strMessage, strError)) {
        LogPrintf("CMasternodePing::CheckSignature -- Got bad Masternode ping signature, masternode=%s, error: %s\n",
                  pubKeyMasternode.GetID().ToString(), strError);
        nDos = 33;
        return false;
    }
    return true;
}

bool CMasternodePing::SimpleCheck(int& nDos)
{
    // don't ban by default
    nDos = 0;

    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrintf("CMasternodePing::SimpleCheck -- Signature rejected, too far into the future, masternode=%s\n",
                  pubKeyMasternode.GetID().ToString());
        nDos = 1;
        return false;
    }

    {
        AssertLockHeld(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(blockHash);
        if (mi == mapBlockIndex.end()) {
            LogPrint("masternode", "CMasternodePing::SimpleCheck -- Masternode ping is invalid, unknown block hash: masternode=%s blockHash=%s\n",
                     pubKeyMasternode.GetID().ToString(), blockHash.ToString());
            // maybe we stuck or forked so we shouldn't ban this node, just fail to accept this ping
            // TODO: or should we also request this block?
            return false;
        }
    }
    LogPrint("masternode", "CMasternodePing::SimpleCheck -- Masternode ping verified: masternode=%s  blockHash=%s  sigTime=%d\n",
             pubKeyMasternode.GetID().ToString(), blockHash.ToString(), sigTime);
    return true;
}

bool CMasternodePing::CheckAndUpdate(CMasternode* pmn, bool fFromNewBroadcast, int& nDos, CConnman& connman)
{
    // don't ban by default
    nDos = 0;

    if (!SimpleCheck(nDos)) {
        return false;
    }

    if (pmn == NULL) {
        LogPrint("masternode", "CMasternodePing::CheckAndUpdate -- Couldn't find Masternode entry, masternode=%s\n",
                 pubKeyMasternode.GetID().ToString());
        return false;
    }

    if (!fFromNewBroadcast) {
        if (pmn->IsUpdateRequired()) {
            LogPrint("masternode", "CMasternodePing::CheckAndUpdate -- masternode protocol is outdated, masternode=%s\n",
                     pubKeyMasternode.GetID().ToString());
            return false;
        }

        if (pmn->IsNewStartRequired()) {
            LogPrint("masternode", "CMasternodePing::CheckAndUpdate -- masternode is completely expired, new start is required, masternode=%s\n",
                     pubKeyMasternode.GetID().ToString());
            return false;
        }
    }

    {
        LOCK(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(blockHash);
        if ((*mi).second && (*mi).second->nHeight < chainActive.Height() - 24) {
            LogPrintf("CMasternodePing::CheckAndUpdate -- Masternode ping is invalid, block hash is too old: masternode=%s  blockHash=%s\n",
                      pubKeyMasternode.GetID().ToString(), blockHash.ToString());
            // nDos = 1;
            return false;
        }
    }

    LogPrint("masternode", "CMasternodePing::CheckAndUpdate -- New ping: masternode=%s  blockHash=%s  sigTime=%d\n",
             pubKeyMasternode.GetID().ToString(), blockHash.ToString(), sigTime);

    // LogPrintf("mnping - Found corresponding mn for vin: %s\n", vin.prevout.ToStringShort());
    // update only if there is no known ping for this masternode or
    // last ping was more then MASTERNODE_MIN_MNP_SECONDS-60 ago comparing to this one
    if (pmn->IsPingedWithin(MASTERNODE_MIN_MNP_SECONDS - 60, sigTime)) {
        LogPrint("masternode", "CMasternodePing::CheckAndUpdate -- Masternode ping arrived too early, masternode=%s\n",
                 pubKeyMasternode.GetID().ToString());
        //nDos = 1; //disable, this is happening frequently and causing banned peers
        return false;
    }

    if (!CheckSignature(pmn->pubKeyMasternode, nDos)) return false;

    // so, ping seems to be ok

    // if we are still syncing and there was no known ping for this mn for quite a while
    // (NOTE: assuming that MASTERNODE_EXPIRATION_SECONDS/2 should be enough to finish mn list sync)
    if(!masternodeSync.IsMasternodeListSynced() && !pmn->IsPingedWithin(MASTERNODE_EXPIRATION_SECONDS/2)) {
        // let's bump sync timeout
        LogPrint("masternode", "CMasternodePing::CheckAndUpdate -- bumping sync timeout, masternode=%s\n",
                 pubKeyMasternode.GetID().ToString());
        masternodeSync.BumpAssetLastTime("CMasternodePing::CheckAndUpdate");
    }

    // let's store this ping as the last one
    LogPrint("masternode", "CMasternodePing::CheckAndUpdate -- Masternode ping accepted, masternode=%s\n",
             pubKeyMasternode.GetID().ToString());
    pmn->lastPing = *this;

    // and update mnodeman.mapSeenMasternodeBroadcast.lastPing which is probably outdated
    CMasternodeBroadcast mnb(*pmn);
    uint256 hash = mnb.GetHash();
    if (mnodeman.mapSeenMasternodeBroadcast.count(hash)) {
        mnodeman.mapSeenMasternodeBroadcast[hash].second.lastPing = *this;
    }

    pmn->Check(true); // force update, ignoring cache
    if (!pmn->IsEnabled()) return false;

    LogPrint("masternode", "CMasternodePing::CheckAndUpdate -- Masternode ping acceepted and relayed, masternode=%s\n",
             pubKeyMasternode.GetID().ToString());
    Relay(connman);

    return true;
}

void CMasternodePing::Relay(CConnman& connman)
{
    CInv inv(MSG_MASTERNODE_PING, GetHash());
    connman.RelayInv(inv);
}

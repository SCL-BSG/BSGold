// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2018 Profit Hunters Coin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "rpcserver.h"


#include "base58.h"
#include "init.h"
#include "util.h"
#include "sync.h"
#include "base58.h"
//#include "db.h"
#include "ui_interface.h"
//#include <filesystem>
#ifdef ENABLE_WALLET
#include "wallet.h"
#endif

//#include "random.h"

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ip/v6_only.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/bind.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/foreach.hpp>
#include <boost/iostreams/concepts.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/shared_ptr.hpp>
#include <list>

/* RGP */
#define DEFAULT_RPC_SERVER_THREADS      20

using namespace std;
using namespace boost;
using namespace boost::asio;
using namespace json_spirit;

static std::string strRPCUserColonPass;

// These are created by StartRPCThreads, destroyed in StopRPCThreads
static asio::io_service* rpc_io_service = NULL;
static map<string, boost::shared_ptr<deadline_timer> > deadlineTimers;
static ssl::context* rpc_ssl_context = NULL;
static boost::thread_group* rpc_worker_group = NULL;


//printf("*** RGP DEBUG getblockchaininfo needs to be implemented for mining!!! \n");
//UniValue getblockchaininfo(const JSONRPCRequest& request);




void RPCTypeCheck(const Array& params,
                  const list<Value_type>& typesExpected,
                  bool fAllowNull)
{
    unsigned int i = 0;

    BOOST_FOREACH(Value_type t, typesExpected)
    {
        if (params.size() <= i)
            break;

        const Value& v = params[i];
        if (!((v.type() == t) || (fAllowNull && (v.type() == null_type))))
        {
            string err = strprintf("Expected type %s, got %s",
                                   Value_type_name[t], Value_type_name[v.type()]);
            throw JSONRPCError(RPC_TYPE_ERROR, err);
        }
        i++;
    }

}

void RPCTypeCheck(const Object& o,
                  const map<string, Value_type>& typesExpected,
                  bool fAllowNull)
{

    BOOST_FOREACH(const PAIRTYPE(string, Value_type)& t, typesExpected)
    {
        const Value& v = find_value(o, t.first);
        if (!fAllowNull && v.type() == null_type)
            throw JSONRPCError(RPC_TYPE_ERROR, strprintf("Missing %s", t.first));

        if (!((v.type() == t.second) || (fAllowNull && (v.type() == null_type))))
        {
            string err = strprintf("Expected type %s for %s, got %s",
                                   Value_type_name[t.second], t.first, Value_type_name[v.type()]);
            throw JSONRPCError(RPC_TYPE_ERROR, err);
        }
    }

}

int64_t AmountFromValue(const Value& value)
{

    double dAmount = value.get_real();
    if (dAmount <= 0.0 || dAmount > MAX_MONEY)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");
    CAmount nAmount = roundint64(dAmount * COIN);
    if (!MoneyRange(nAmount))
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");

    return nAmount;
}

Value ValueFromAmount(int64_t amount)
{

    return (double)amount / (double)COIN;
}


//
// Utilities: convert hex-encoded Values
// (throws error if not hex).
//
uint256 ParseHashV(const Value& v, string strName)
{
uint256 result;

    string strHex;
    if (v.type() == str_type)
        strHex = v.get_str();
    if (!IsHex(strHex)) // Note: IsHex("") is false
        throw JSONRPCError(RPC_INVALID_PARAMETER, strName+" must be hexadecimal string (not '"+strHex+"')");


    result.SetHex(strHex);

    return result;
}

uint256 ParseHashO(const Object& o, string strKey)
{
    return ParseHashV(find_value(o, strKey), strKey);
}

vector<unsigned char> ParseHexV(const Value& v, string strName)
{
    string strHex;
    if (v.type() == str_type)
        strHex = v.get_str();
    if (!IsHex(strHex))
        throw JSONRPCError(RPC_INVALID_PARAMETER, strName+" must be hexadecimal string (not '"+strHex+"')");
    return ParseHex(strHex);
}

vector<unsigned char> ParseHexO(const Object& o, string strKey)
{
    return ParseHexV(find_value(o, strKey), strKey);
}


///
/// Note: This interface may still be subject to change.
///

string CRPCTable::help(string strCommand) const
{
    string strRet;
    set<rpcfn_type> setDone;
    for (map<string, const CRPCCommand*>::const_iterator mi = mapCommands.begin(); mi != mapCommands.end(); ++mi)
    {
        const CRPCCommand *pcmd = mi->second;
        string strMethod = mi->first;
        // We already filter duplicates, but these deprecated screw up the sort order
        if (strMethod.find("label") != string::npos)
            continue;
        if (strCommand != "" && strMethod != strCommand)
            continue;
#ifdef ENABLE_WALLET
        if (pcmd->reqWallet && !pwalletMain)
            continue;
#endif

        try
        {
            Array params;
            rpcfn_type pfn = pcmd->actor;
            if (setDone.insert(pfn).second)
                (*pfn)(params, true);
        }
        catch (std::exception& e)
        {
            // Help text is returned in an exception
            string strHelp = string(e.what());
            if (strCommand == "")
                if (strHelp.find('\n') != string::npos)
                    strHelp = strHelp.substr(0, strHelp.find('\n'));
            strRet += strHelp + "\n";
        }
    }
    if (strRet == "")
        strRet = strprintf("help: unknown command: %s\n", strCommand);
    strRet = strRet.substr(0,strRet.size()-1);
    return strRet;
}

Value help(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "help [command]\n"
            "List commands, or get help for a command.");

    string strCommand;
    if (params.size() > 0)
        strCommand = params[0].get_str();

    return tableRPC.help(strCommand);
}


Value stop(const Array& params, bool fHelp)
{

    // Accept the deprecated and ignored 'detach' boolean argument
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "stop\n"
            "Stop SocietyG server.");
    // Shutdown will take long enough that the response should get back
    StartShutdown();
    return "SocietyG server stopping";
}


/* RGP new mining routine
UniValue getblockchaininfo(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw runtime_error(
            "getblockchaininfo\n"
            "Returns an object containing various state info regarding blockchain processing.\n"
            "\nResult:\n"
            "{\n"
            "  \"chain\": \"xxxx\",        (string) current network name as defined in BIP70 (main, test, regtest)\n"
            "  \"blocks\": xxxxxx,         (numeric) the current number of blocks processed in the server\n"
            "  \"headers\": xxxxxx,        (numeric) the current number of headers we have validated\n"
            "  \"bestblockhash\": \"...\", (string) the hash of the currently best block\n"
            "  \"difficulty\": xxxxxx,     (numeric) the current difficulty\n"
            "  \"mediantime\": xxxxxx,     (numeric) median time for the current best block\n"
            "  \"verificationprogress\": xxxx, (numeric) estimate of verification progress [0..1]\n"
            "  \"initialblockdownload\": xxxx, (bool) (debug information) estimate of whether this node is in Initial Block Download mode.\n"
            "  \"chainwork\": \"xxxx\"     (string) total amount of work in active chain, in hexadecimal\n"
            "  \"size_on_disk\": xxxxxx,   (numeric) the estimated size of the block and undo files on disk\n"
            "  \"pruned\": xx,             (boolean) if the blocks are subject to pruning\n"
            "  \"pruneheight\": xxxxxx,    (numeric) lowest-height complete block stored (only present if pruning is enabled)\n"
            "  \"automatic_pruning\": xx,  (boolean) whether automatic pruning is enabled (only present if pruning is enabled)\n"
            "  \"prune_target_size\": xxxxxx,  (numeric) the target size used by pruning (only present if automatic pruning is enabled)\n"
            "  \"softforks\": [            (array) status of softforks in progress\n"
            "     {\n"
            "        \"id\": \"xxxx\",        (string) name of softfork\n"
            "        \"version\": xx,         (numeric) block version\n"
            "        \"reject\": {            (object) progress toward rejecting pre-softfork blocks\n"
            "           \"status\": xx,       (boolean) true if threshold reached\n"
            "        },\n"
            "     }, ...\n"
            "  ],\n"
            "  \"bip9_softforks\": {          (object) status of BIP9 softforks in progress\n"
            "     \"xxxx\" : {                (string) name of the softfork\n"
            "        \"status\": \"xxxx\",    (string) one of \"defined\", \"started\", \"locked_in\", \"active\", \"failed\"\n"
            "        \"bit\": xx,             (numeric) the bit (0-28) in the block version field used to signal this softfork (only for \"started\" status)\n"
            "        \"startTime\": xx,       (numeric) the minimum median time past of a block at which the bit gains its meaning\n"
            "        \"timeout\": xx,         (numeric) the median time past of a block at which the deployment is considered failed if not yet locked in\n"
            "        \"since\": xx            (numeric) height of the first block to which the status applies\n"
            "     }\n"
            "  }\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getblockchaininfo", "")
            + HelpExampleRpc("getblockchaininfo", "")
        );

    LOCK(cs_main);

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("chain",                 Params().NetworkIDString());
    obj.pushKV("blocks",                (int)chainActive.Height());
    obj.pushKV("headers",               pindexBestHeader ? pindexBestHeader->nHeight : -1);
    obj.pushKV("bestblockhash",         chainActive.Tip()->GetBlockHash().GetHex());
    obj.pushKV("difficulty",            (double)GetDifficulty());
    obj.pushKV("mediantime",            (int64_t)chainActive.Tip()->GetMedianTimePast());
    obj.pushKV("verificationprogress",  GuessVerificationProgress(Params().TxData(), chainActive.Tip()));
    obj.pushKV("initialblockdownload",  IsInitialBlockDownload());
    obj.pushKV("chainwork",             chainActive.Tip()->nChainWork.GetHex());
    obj.pushKV("size_on_disk",          CalculateCurrentUsage());
    obj.pushKV("pruned",                fPruneMode);
    if (fPruneMode) {
        CBlockIndex* block = chainActive.Tip();
        assert(block);
        while (block->pprev && (block->pprev->nStatus & BLOCK_HAVE_DATA)) {
            block = block->pprev;
        }

        obj.pushKV("pruneheight",        block->nHeight);

        // if 0, execution bypasses the whole if block.
        bool automatic_pruning = (GetArg("-prune", 0) != 1);
        obj.pushKV("automatic_pruning",  automatic_pruning);
        if (automatic_pruning) {
            obj.pushKV("prune_target_size",  nPruneTarget);
        }
    }

    const Consensus::Params& consensusParams = Params().GetConsensus(0);
    CBlockIndex* tip = chainActive.Tip();
    UniValue softforks(UniValue::VARR);
    UniValue bip9_softforks(UniValue::VOBJ);
    softforks.push_back(SoftForkDesc("bip34", 2, tip, consensusParams));
    softforks.push_back(SoftForkDesc("bip66", 3, tip, consensusParams));
    softforks.push_back(SoftForkDesc("bip65", 4, tip, consensusParams));
    BIP9SoftForkDescPushBack(bip9_softforks, "csv", consensusParams, Consensus::DEPLOYMENT_CSV);
    BIP9SoftForkDescPushBack(bip9_softforks, "segwit", consensusParams, Consensus::DEPLOYMENT_SEGWIT);
    obj.pushKV("softforks",             softforks);
    obj.pushKV("bip9_softforks", bip9_softforks);
    obj.pushKV("warnings", GetWarnings("statusbar"));
    return obj;
}
*/

/*
static bool rest_chaininfo(HTTPRequest* req, const std::string& strURIPart)
{
    if (!CheckWarmup(req))
        return false;
    std::string param;
    const RetFormat rf = ParseDataFormat(param, strURIPart);

    switch (rf) {
    case RF_JSON: {
        JSONRPCRequest jsonRequest;
        jsonRequest.params = UniValue(UniValue::VARR);
        UniValue chainInfoObject = getblockchaininfo(jsonRequest);
        std::string strJSON = chainInfoObject.write() + "\n";
        req->WriteHeader("Content-Type", "application/json");
        req->WriteReply(HTTP_OK, strJSON);
        return true;
    }
    default: {
        return RESTERR(req, HTTP_NOT_FOUND, "output format not found (available: json)");
    }
    }

    // not reached
    return true; // continue to process further HTTP reqs on this cxn
}
*/
//
// Call Table
//


static const CRPCCommand vRPCCommands[] =
{ //  name                      actor (function)         okSafeMode threadSafe reqWallet
  //  ------------------------  -----------------------  ---------- ---------- ---------
    { "help",                   &help,                   true,      true,      false },
    { "stop",                   &stop,                   true,      true,      false },
    { "getbestblockhash",       &getbestblockhash,       true,      false,     false },
    { "getblockcount",          &getblockcount,          true,      false,     false },
    { "getconnectioncount",     &getconnectioncount,     true,      false,     false },
    { "getpeerinfo",            &getpeerinfo,            true,      false,     false },
    { "addnode",                &addnode,                true,      true,      false },
    { "getaddednodeinfo",       &getaddednodeinfo,       true,      true,      false },
    { "ping",                   &ping,                   true,      false,     false },
    { "setban",                 &setban,                 true,      false,     false },
    { "listbanned",             &listbanned,             true,      false,     false },
    { "clearbanned",            &clearbanned,            true,      false,     false },
    { "getnettotals",           &getnettotals,           true,      true,      false },
    { "getdifficulty",          &getdifficulty,          true,      false,     false },
    { "getinfo",                &getinfo,                true,      false,     false },
    { "getrawmempool",          &getrawmempool,          true,      false,     false },
    { "getblock",               &getblock,               false,     false,     false },
    { "getblockbynumber",       &getblockbynumber,       false,     false,     false },
    { "getblockhash",           &getblockhash,           false,     false,     false },
    { "getrawtransaction",      &getrawtransaction,      false,     false,     false },
    { "createrawtransaction",   &createrawtransaction,   false,     false,     false },
    { "decoderawtransaction",   &decoderawtransaction,   false,     false,     false },
    { "decodescript",           &decodescript,           false,     false,     false },
    { "signrawtransaction",     &signrawtransaction,     false,     false,     false },
    { "sendrawtransaction",     &sendrawtransaction,     false,     false,     false },
    { "getcheckpoint",          &getcheckpoint,          true,      false,     false },
    { "sendalert",              &sendalert,              false,     false,     false },
    { "validateaddress",        &validateaddress,        true,      false,     false },
    { "validatepubkey",         &validatepubkey,         true,      false,     false },
    { "verifymessage",          &verifymessage,          false,     false,     false },
    { "searchrawtransactions",  &searchrawtransactions,  false,     false,     false },

    /* ---------------------
       -- RGP JIRA BSG-51 --
       -----------------------------------------------------
       -- added getnetworkinfo to list of server commands --
       ----------------------------------------------------- */

    { "getnetworkinfo",         &getnetworkinfo,         true,      false,     false },

    /* Firewall General Session Settings */
    { "firewallstatus",                                &firewallstatus,                             false,      false,    false },
    { "firewallenabled",                               &firewallenabled,                             false,      false,    false },
    { "firewallclearblacklist",                        &firewallclearblacklist,                      false,      false,    false },
    { "firewallclearbanlist",                          &firewallclearbanlist,                        false,      false,    false },
    { "firewalltraffictolerance",                      &firewalltraffictolerance,                    false,      false,    false },
    { "firewalltrafficzone",                           &firewalltrafficzone,                         false,      false,    false },
    { "firewalladdtowhitelist",                        &firewalladdtowhitelist,                      false,      false,    false },
    { "firewalladdtoblacklist",                        &firewalladdtoblacklist,                      false,      false,    false },

    /* Firewall Firewall Debug (Live Output) */
    { "firewalldebug",                                 &firewalldebug,                               false,      false,    false },
    { "firewalldebugexam",                             &firewalldebugexam,                           false,      false,    false },
    { "firewalldebugbans",                             &firewallclearbanlist,                        false,      false,    false },
    { "firewalldebugblacklist",                        &firewalldebugblacklist,                      false,      false,    false },
    { "firewalldebugdisconnect",                       &firewalldebugdisconnect,                     false,      false,    false },
    { "firewalldebugbandwidthabuse",                   &firewalldebugbandwidthabuse,                 false,      false,    false },
    { "firewalldebugnofalsepositivebandwidthabuse",    &firewalldebugnofalsepositivebandwidthabuse,  false,      false,    false },
    { "firewalldebuginvalidwallet",                    &firewalldebuginvalidwallet,                  false,      false,    false },
    { "firewalldebugforkedwallet",                     &firewalldebugforkedwallet,                   false,      false,    false },
    { "firewalldebugfloodingwallet",                   &firewalldebugfloodingwallet,                 false,      false,    false },

    /* Firewall BandwidthAbuse Session Settings */
    { "firewalldetectbandwidthabuse",                  &firewalldetectbandwidthabuse,                false,      false,    false },
    { "firewallblacklistbandwidthabuse",               &firewallblacklistbandwidthabuse,             false,      false,    false },
    { "firewallbanbandwidthabuse",                     &firewallbanbandwidthabuse,                   false,      false,    false },
    { "firewallnofalsepositivebandwidthabuse",         &firewallnofalsepositivebandwidthabuse,       false,      false,    false },
    { "firewallbantimebandwidthabuse",                 &firewallbantimebandwidthabuse,               false,      false,    false },
    { "firewallbandwidthabusemaxcheck",                &firewallbandwidthabusemaxcheck,              false,      false,    false },
    { "firewallbandwidthabuseminattack",               &firewallbandwidthabuseminattack,             false,      false,    false },
    { "firewallbandwidthabuseminattack",               &firewallbandwidthabuseminattack,             false,      false,    false },

    /* Firewall Invalid Wallet Session Settings */
    { "firewalldetectinvalidwallet",                   &firewalldetectinvalidwallet,                 false,      false,    false },
    { "firewallblacklistinvalidwallet",                &firewallblacklistinvalidwallet,              false,      false,    false },
    { "firewallbaninvalidwallet",                      &firewallbaninvalidwallet,                    false,      false,    false },
    { "firewallbantimeinvalidwallet",                  &firewallbantimeinvalidwallet,                false,      false,    false },
    { "firewallinvalidwalletminprotocol",              &firewallinvalidwalletminprotocol,            false,      false,    false },
    { "firewallinvalidwalletmaxcheck",                 &firewallinvalidwalletmaxcheck,               false,      false,    false },

    /* Firewall Forked Wallet Session Settings */
    { "firewalldetectforkedwallet",                    &firewalldetectforkedwallet,                  false,      false,    false },
    { "firewallblacklistforkedwallet",                 &firewallblacklistforkedwallet,               false,      false,    false },
    { "firewallbanforkedwallet",                       &firewallbanforkedwallet,                     false,      false,    false },
    { "firewallbantimeforkedwallet",                   &firewallbantimeforkedwallet,                 false,      false,    false },
    { "firewallforkedwalletnodeheight",                &firewallforkedwalletnodeheight,              false,      false,    false },

    /* Firewall Flooding Wallet Session Settings */
    { "firewalldetectfloodingwallet",                  &firewalldetectfloodingwallet,                false,      false,    false },
    { "firewallblacklistfloodingwallet",               &firewallblacklistfloodingwallet,             false,      false,    false },
    { "firewallbanfloodingwallet",                     &firewallbanfloodingwallet,                   false,      false,    false },
    { "firewallbantimefloodingwallet",                 &firewallbantimefloodingwallet,               false,      false,    false },
    { "firewallfloodingwalletminbytes",                &firewallfloodingwalletminbytes,              false,      false,    false },
    { "firewallfloodingwalletmaxbytes",                &firewallfloodingwalletmaxbytes,              false,      false,    false },
    { "firewallfloodingwalletattackpatternadd",        &firewallfloodingwalletattackpatternadd,      false,      false,    false },
    { "firewallfloodingwalletattackpatternremove",     &firewallfloodingwalletattackpatternremove,   false,      false,    false },
    { "firewallfloodingwalletmintrafficavg",           &firewallfloodingwalletmintrafficavg,         false,      false,    false },
    { "firewallfloodingwalletmaxtrafficavg",           &firewallfloodingwalletmaxtrafficavg,         false,      false,    false },
    { "firewallfloodingwalletmincheck",                &firewallfloodingwalletmincheck,              false,      false,    false },
    { "firewallfloodingwalletmaxcheck",                &firewallfloodingwalletmaxcheck,              false,      false,    false },

/* Dark features */
    { "spork",                  &spork,                  true,      false,      false },
    { "masternode",             &masternode,             true,      false,      true },
    { "masternodelist",         &masternodelist,         true,      false,      false },
    
#ifdef ENABLE_WALLET
    { "darksend",               &darksend,               false,     false,      true },
    { "getmininginfo",          &getmininginfo,          true,      false,     false },
    { "getstakinginfo",         &getstakinginfo,         true,      false,     false },
    { "getnewaddress",          &getnewaddress,          true,      false,     true },
    { "getnewpubkey",           &getnewpubkey,           true,      false,     true },
    { "getaccountaddress",      &getaccountaddress,      true,      false,     true },
    { "setaccount",             &setaccount,             true,      false,     true },
    { "getaccount",             &getaccount,             false,     false,     true },
    { "getaddressesbyaccount",  &getaddressesbyaccount,  true,      false,     true },
    { "sendtoaddress",          &sendtoaddress,          false,     false,     true },
    { "getreceivedbyaddress",   &getreceivedbyaddress,   false,     false,     true },
    { "getreceivedbyaccount",   &getreceivedbyaccount,   false,     false,     true },
    { "listreceivedbyaddress",  &listreceivedbyaddress,  false,     false,     true },
    { "listreceivedbyaccount",  &listreceivedbyaccount,  false,     false,     true },
    { "backupwallet",           &backupwallet,           true,      false,     true },
    { "keypoolrefill",          &keypoolrefill,          true,      false,     true },
    { "walletpassphrase",       &walletpassphrase,       true,      false,     true },
    { "walletpassphrasechange", &walletpassphrasechange, false,     false,     true },
    { "walletlock",             &walletlock,             true,      false,     true },
    { "encryptwallet",          &encryptwallet,          false,     false,     true },
    { "getbalance",             &getbalance,             false,     false,     true },
    { "move",                   &movecmd,                false,     false,     true },
    { "sendfrom",               &sendfrom,               false,     false,     true },
    { "sendmany",               &sendmany,               false,     false,     true },
    { "addmultisigaddress",     &addmultisigaddress,     false,     false,     true },
    { "addredeemscript",        &addredeemscript,        false,     false,     true },
    { "gettransaction",         &gettransaction,         false,     false,     true },
    { "listtransactions",       &listtransactions,       false,     false,     true },
    { "listaddressgroupings",   &listaddressgroupings,   false,     false,     true },
    { "signmessage",            &signmessage,            false,     false,     true },
    { "getwork",                &getwork,                true,      false,     true },
    { "getworkex",              &getworkex,              true,      false,     true },
    { "listaccounts",           &listaccounts,           false,     false,     true },
    { "getblocktemplate",       &getblocktemplate,       true,      false,     false },
    { "submitblock",            &submitblock,            false,     false,     false },
    { "listsinceblock",         &listsinceblock,         false,     false,     true },
    { "dumpprivkey",            &dumpprivkey,            false,     false,     true },
    { "dumpwallet",             &dumpwallet,             true,      false,     true },
    { "importprivkey",          &importprivkey,          false,     false,     true },
    { "importwallet",           &importwallet,           false,     false,     true },
    { "importaddress",          &importaddress,          false,     false,     true },
    { "listunspent",            &listunspent,            false,     false,     true },
    { "settxfee",               &settxfee,               false,     false,     true },
    { "getsubsidy",             &getsubsidy,             true,      true,      false },
    { "getstakesubsidy",        &getstakesubsidy,        true,      true,      false },
    { "reservebalance",         &reservebalance,         false,     true,      true },
    { "createmultisig",         &createmultisig,         true,      true,      false },
    { "checkwallet",            &checkwallet,            false,     true,      true },
    { "repairwallet",           &repairwallet,           false,     true,      true },
    { "resendtx",               &resendtx,               false,     true,      true },
    { "makekeypair",            &makekeypair,            false,     true,      false },
    { "checkkernel",            &checkkernel,            true,      false,     true },
    { "getnewstealthaddress",   &getnewstealthaddress,   false,     false,     true },
    { "liststealthaddresses",   &liststealthaddresses,   false,     false,     true },
    { "scanforalltxns",         &scanforalltxns,         false,     false,     false },
    { "scanforstealthtxns",     &scanforstealthtxns,     false,     false,     false },
    { "importstealthaddress",   &importstealthaddress,   false,     false,     true },
    { "sendtostealthaddress",   &sendtostealthaddress,   false,     false,     true },
    { "smsgenable",             &smsgenable,             false,     false,     false },
    { "smsgdisable",            &smsgdisable,            false,     false,     false },
    { "smsglocalkeys",          &smsglocalkeys,          false,     false,     false },
    { "smsgoptions",            &smsgoptions,            false,     false,     false },
    { "smsgscanchain",          &smsgscanchain,          false,     false,     false },
    { "smsgscanbuckets",        &smsgscanbuckets,        false,     false,     false },
    { "smsgaddkey",             &smsgaddkey,             false,     false,     false },
    { "smsggetpubkey",          &smsggetpubkey,          false,     false,     false },
    { "smsgsend",               &smsgsend,               false,     false,     false },
    { "smsgsendanon",           &smsgsendanon,           false,     false,     false },
    { "smsginbox",              &smsginbox,              false,     false,     false },
    { "smsgoutbox",             &smsgoutbox,             false,     false,     false },
    { "smsgbuckets",            &smsgbuckets,            false,     false,     false },
#endif
};

CRPCTable::CRPCTable()
{
    unsigned int vcidx;
    for (vcidx = 0; vcidx < (sizeof(vRPCCommands) / sizeof(vRPCCommands[0])); vcidx++)
    {
        const CRPCCommand *pcmd;

        pcmd = &vRPCCommands[vcidx];
        mapCommands[pcmd->name] = pcmd;
    }
}

const CRPCCommand *CRPCTable::operator[](string name) const
{

    map<string, const CRPCCommand*>::const_iterator it = mapCommands.find(name);
    if (it == mapCommands.end())
    {
        LogPrintf("RGP Operator returning null \n");
        return NULL;
    }

    return (*it).second;
}


bool HTTPAuthorized(map<string, string>& mapHeaders)
{
    string strAuth = mapHeaders["authorization"];
    if (strAuth.substr(0,6) != "Basic ")
        return false;
    string strUserPass64 = strAuth.substr(6); boost::trim(strUserPass64);
    string strUserPass = DecodeBase64(strUserPass64);

    return TimingResistantEqual(strUserPass, strRPCUserColonPass);
}

void ErrorReply(std::ostream& stream, const Object& objError, const Value& id)
{

    // Send error reply from json-rpc error object
    int nStatus = HTTP_INTERNAL_SERVER_ERROR;
    int code = find_value(objError, "code").get_int();
    if (code == RPC_INVALID_REQUEST) nStatus = HTTP_BAD_REQUEST;
    else if (code == RPC_METHOD_NOT_FOUND) nStatus = HTTP_NOT_FOUND;
    string strReply = JSONRPCReply(Value::null, objError, id);
    stream << HTTPReply(nStatus, strReply, false) << std::flush;
}

bool ClientAllowed(const boost::asio::ip::address& address)
{

    // Make sure that IPv4-compatible and IPv4-mapped IPv6 addresses are treated as IPv4 addresses
    if (address.is_v6()
     && (address.to_v6().is_v4_compatible()
      || address.to_v6().is_v4_mapped()))
        return ClientAllowed(address.to_v6().to_v4());

    if (address == asio::ip::address_v4::loopback()
     || address == asio::ip::address_v6::loopback()
     || (address.is_v4()
         // Check whether IPv4 addresses match 127.0.0.0/8 (loopback subnet)
      && (address.to_v4().to_ulong() & 0xff000000) == 0x7f000000))
        return true;

    const string strAddress = address.to_string();
    const vector<string>& vAllow = mapMultiArgs["-rpcallowip"];
    BOOST_FOREACH(string strAllow, vAllow)
        if (WildcardMatch(strAddress, strAllow))
            return true;
    return false;
}

class AcceptedConnection
{
public:
    virtual ~AcceptedConnection() {}

    virtual std::iostream& stream() = 0;
    virtual std::string peer_address_to_string() const = 0;
    virtual void close() = 0;
};

template <typename Protocol>
class AcceptedConnectionImpl : public AcceptedConnection
{
public:
    AcceptedConnectionImpl(
            asio::io_service& io_service,
            ssl::context &context,
            bool fUseSSL) :
        sslStream(io_service, context),
        _d(sslStream, fUseSSL),
        _stream(_d)
    {
    }

    virtual std::iostream& stream()
    {
        return _stream;
    }

    virtual std::string peer_address_to_string() const
    {
        return peer.address().to_string();
    }

    virtual void close()
    {
        _stream.close();
    }

    typename Protocol::endpoint peer;
    asio::ssl::stream<typename Protocol::socket> sslStream;

private:
    SSLIOStreamDevice<Protocol> _d;
    iostreams::stream< SSLIOStreamDevice<Protocol> > _stream;
};

#if BOOST_VERSION >= 107000
#define GET_IO_SERVICE(s) ((boost::asio::io_context&)(s).get_executor().context())
#else
#define GET_IO_SERVICE(s) ((s).get_io_service())
#endif

void ServiceConnection(AcceptedConnection *conn);

// Forward declaration required for RPCListen
template <typename Protocol, typename SocketAcceptorService>
static void RPCAcceptHandler(boost::shared_ptr< basic_socket_acceptor<Protocol, SocketAcceptorService> > acceptor,
                             ssl::context& context,
                             bool fUseSSL,
                             AcceptedConnection* conn,
                             const boost::system::error_code& error);

/**
 * Sets up I/O resources to accept and handle a new connection.
 */
template <typename Protocol, typename SocketAcceptorService>
static void RPCListen(boost::shared_ptr< basic_socket_acceptor<Protocol, SocketAcceptorService> > acceptor,
                   ssl::context& context,
                   const bool fUseSSL)
{

    // Accept connection
/*    AcceptedConnectionImpl<Protocol>* conn = new AcceptedConnectionImpl<Protocol>get_io_service(), context, fUseSSL);

    acceptor->async_accept(
            conn->sslStream.lowest_layer(),
            conn->peer,
            boost::bind(&RPCAcceptHandler<Protocol, SocketAcceptorService>,
                acceptor,
                boost::ref(context),
                fUseSSL,
                conn,
                boost::asio::placeholders::error));
*/
}


/**
 * Accept and handle incoming connection.
 */
template <typename Protocol, typename SocketAcceptorService>
static void RPCAcceptHandler(boost::shared_ptr< basic_socket_acceptor<Protocol, SocketAcceptorService> > acceptor,
                             ssl::context& context,
                             const bool fUseSSL,
                             AcceptedConnection* conn,
                             const boost::system::error_code& error)
{
    // Immediately start accepting new connections, except when we're cancelled or our socket is closed.

    LogPrintf("*** RGP RPC Server RPCAcceptHandler() start from %s \n", conn->peer_address_to_string() );

    if (error != asio::error::operation_aborted && acceptor->is_open())
        RPCListen(acceptor, context, fUseSSL);

    AcceptedConnectionImpl<ip::tcp>* tcp_conn = dynamic_cast< AcceptedConnectionImpl<ip::tcp>* >(conn);

    // TODO: Actually handle errors
    if (error)
    {
        LogPrintf("*** RGP RPC Server RPCAcceptHandler() error condition \n");

        delete conn;
    }

    // Restrict callers by IP.  It is important to
    // do this before starting client thread, to filter out
    // certain DoS and misbehaving clients.
    else if (tcp_conn && !ClientAllowed(tcp_conn->peer.address()))
    {
        // Only send a 403 if we're not using SSL to prevent a DoS during the SSL handshake.
        if (!fUseSSL)
            conn->stream() << HTTPReply(HTTP_FORBIDDEN, "", false) << std::flush;
        delete conn;
    }
    else
    {
        LogPrintf("*** RGP RPC Server ServiceConnect() start \n");

        ServiceConnection(conn);
        conn->close();
        delete conn;
    }
}

void StartRPCThreads()
{

    strRPCUserColonPass = mapArgs["-rpcuser"] + ":" + mapArgs["-rpcpassword"];
    if (((mapArgs["-rpcpassword"] == "") ||
         (mapArgs["-rpcuser"] == mapArgs["-rpcpassword"])) && Params().RequireRPCPassword())
    {
        unsigned char rand_pwd[32];
        GetRandBytes(rand_pwd, 32);
        string strWhatAmI = "To use societyGd";
        if (mapArgs.count("-server"))
            strWhatAmI = strprintf(_("To use the %s option"), "\"-server\"");
        else if (mapArgs.count("-daemon"))
            strWhatAmI = strprintf(_("To use the %s option"), "\"-daemon\"");
        uiInterface.ThreadSafeMessageBox(strprintf(
            _("%s, you must set a rpcpassword in the configuration file:\n"
              "%s\n"
              "It is recommended you use the following random password:\n"
              "rpcuser=SocietyGrpc\n"
              "rpcpassword=%s\n"
              "(you do not need to remember this password)\n"
              "The username and password MUST NOT be the same.\n"
              "If the file does not exist, create it with owner-readable-only file permissions.\n"
              "It is also recommended to set alertnotify so you are notified of problems;\n"
              "for example: alertnotify=echo %%s | mail -s \"SocietyG Alert\" admin@foo.com\n"),
                strWhatAmI,
                GetConfigFile().string(),
                EncodeBase58(&rand_pwd[0],&rand_pwd[0]+32)),
                "", CClientUIInterface::MSG_ERROR);
        StartShutdown();
        return;
    }

    assert(rpc_io_service == NULL);
    rpc_io_service = new asio::io_service;
    //rpc_ssl_context = new boost::asio::ssl::context   (*rpc_io_service, boost::asio::ssl::context::sslv23 );

    const SSL_METHOD* raw_method = SSLv23_server_method();
    SSL_CTX*          raw_ctx    = SSL_CTX_new(raw_method);


    const bool fUseSSL = GetBoolArg("-rpcssl", false);
    printf("*** RGP DEBUG Ignoroing rpcserver.cpp line 790 for FUseSSL \n");
    /*
    if (fUseSSL)
    {
        rpc_ssl_context->set_options(ssl::context::no_sslv2 | ssl::context::no_sslv3);

        boost::filesystem::path::pathCertFile(GetArg("-rpcsslcertificatechainfile", "server.cert"));
        if (!pathCertFile.is_complete()) pathCertFile = filesystem::path(GetDataDir()) / pathCertFile;
        if (filesystem::exists(pathCertFile)) rpc_ssl_context->use_certificate_chain_file(pathCertFile.string());
        else LogPrintf("ThreadRPCServer ERROR: missing server certificate file %s\n", pathCertFile.string());

        filesystem::path pathPKFile(GetArg("-rpcsslprivatekeyfile", "server.pem"));
        if (!pathPKFile.is_complete()) pathPKFile = filesystem::path(GetDataDir()) / pathPKFile;
        if (filesystem::exists(pathPKFile)) rpc_ssl_context->use_private_key_file(pathPKFile.string(), ssl::context::pem);
        else LogPrintf("ThreadRPCServer ERROR: missing server private key file %s\n", pathPKFile.string());

        string strCiphers = GetArg("-rpcsslciphers", "TLSv1.2+HIGH:TLSv1+HIGH:!SSLv3:!SSLv2:!aNULL:!eNULL:!3DES:@STRENGTH");
        SSL_CTX_set_cipher_list(rpc_ssl_context->impl(), strCiphers.c_str());
    }
*/
    // Try a dual IPv6/IPv4 socket, falling back to separate IPv4 and IPv6 sockets
    const bool loopback = !mapArgs.count("-rpcallowip");
    asio::ip::address bindAddress = loopback ? asio::ip::address_v6::loopback() : asio::ip::address_v6::any();
    ip::tcp::endpoint endpoint(bindAddress, GetArg("-rpcport", Params().RPCPort()));
    boost::system::error_code v6_only_error;
    boost::shared_ptr<ip::tcp::acceptor> acceptor(new ip::tcp::acceptor(*rpc_io_service));

    bool fListening = false;
    std::string strerr;
    try
    {
        acceptor->open(endpoint.protocol());
        acceptor->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));

        // Try making the socket dual IPv6/IPv4 (if listening on the "any" address)
        acceptor->set_option(boost::asio::ip::v6_only(loopback), v6_only_error);

        acceptor->bind(endpoint);
        acceptor->listen(socket_base::max_connections);

        RPCListen(acceptor, *rpc_ssl_context, fUseSSL);

        fListening = true;
    }
    catch(boost::system::system_error &e)
    {
        strerr = strprintf(_("An error occurred while setting up the RPC port %u for listening on IPv6, falling back to IPv4: %s"), endpoint.port(), e.what());
    }

    try {
        // If dual IPv6/IPv4 failed (or we're opening loopback interfaces only), open IPv4 separately
        if (!fListening || loopback || v6_only_error)
        {
            bindAddress = loopback ? asio::ip::address_v4::loopback() : asio::ip::address_v4::any();
            endpoint.address(bindAddress);

            acceptor.reset(new ip::tcp::acceptor(*rpc_io_service));
            acceptor->open(endpoint.protocol());
            acceptor->set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
            acceptor->bind(endpoint);
            acceptor->listen(socket_base::max_connections);

            RPCListen(acceptor, *rpc_ssl_context, fUseSSL);

            fListening = true;
        }
    }
    catch(boost::system::system_error &e)
    {
        strerr = strprintf(_("An error occurred while setting up the RPC port %u for listening on IPv4: %s"), endpoint.port(), e.what());
    }

    if (!fListening) {
        uiInterface.ThreadSafeMessageBox(strerr, "", CClientUIInterface::MSG_ERROR);
        StartShutdown();
        return;
    }

    rpc_worker_group = new boost::thread_group();
    for (int i = 0; i < GetArg("-rpcthreads", DEFAULT_RPC_SERVER_THREADS ); i++)
        rpc_worker_group->create_thread(boost::bind(&asio::io_service::run, rpc_io_service));
}

void StopRPCThreads()
{
    if (rpc_io_service == NULL) return;

    deadlineTimers.clear();
    rpc_io_service->stop();
    if (rpc_worker_group != NULL)
        rpc_worker_group->join_all();
    delete rpc_worker_group; rpc_worker_group = NULL;
    delete rpc_ssl_context; rpc_ssl_context = NULL;
    delete rpc_io_service; rpc_io_service = NULL;
}

void RPCRunHandler(const boost::system::error_code& err, boost::function<void(void)> func)
{

    if (!err)
        func();
}

void RPCRunLater(const std::string& name, boost::function<void(void)> func, int64_t nSeconds)
{
    assert(rpc_io_service != NULL);

    if (deadlineTimers.count(name) == 0)
    {
        deadlineTimers.insert(make_pair(name,
                                        boost::shared_ptr<deadline_timer>(new deadline_timer(*rpc_io_service))));
    }
    deadlineTimers[name]->expires_from_now(posix_time::seconds(nSeconds));
    deadlineTimers[name]->async_wait(boost::bind(RPCRunHandler, _1, func));
}

class JSONRequest
{
public:
    Value id;
    string strMethod;
    Array params;


    JSONRequest() { id = Value::null; }
    void parse(const Value& valRequest);
};

void JSONRequest::parse(const Value& valRequest)
{

    // Parse request
    if (valRequest.type() != obj_type)
        throw JSONRPCError(RPC_INVALID_REQUEST, "Invalid Request object");
    const Object& request = valRequest.get_obj();

    // Parse id now so errors from here on will have the id
    id = find_value(request, "id");

    // Parse method
    Value valMethod = find_value(request, "method");
    if (valMethod.type() == null_type)
        throw JSONRPCError(RPC_INVALID_REQUEST, "Missing method");
    if (valMethod.type() != str_type)
        throw JSONRPCError(RPC_INVALID_REQUEST, "Method must be a string");
    strMethod = valMethod.get_str();
    if (strMethod != "getwork" && strMethod != "getblocktemplate")
        LogPrint("rpc", "ThreadRPCServer method=%s\n", strMethod);

    // Parse params
    Value valParams = find_value(request, "params");
    if (valParams.type() == array_type)
        params = valParams.get_array();
    else if (valParams.type() == null_type)
        params = Array();
    else
        throw JSONRPCError(RPC_INVALID_REQUEST, "Params must be an array");

}


static Object JSONRPCExecOne(const Value& req)
{
    Object rpc_result;

    LogPrintf("RGP JSONRPCExecOne Start \n");

    JSONRequest jreq;
    try {
        jreq.parse(req);


        Value result = tableRPC.execute(jreq.strMethod, jreq.params);
        rpc_result = JSONRPCReplyObj(result, Value::null, jreq.id);
    }
    catch (Object& objError)
    {
        rpc_result = JSONRPCReplyObj(Value::null, objError, jreq.id);
    }
    catch (std::exception& e)
    {
        rpc_result = JSONRPCReplyObj(Value::null,
                                     JSONRPCError(RPC_PARSE_ERROR, e.what()), jreq.id);
    }

    return rpc_result;
}

static string JSONRPCExecBatch(const Array& vReq)
{
    Array ret;
    for (unsigned int reqIdx = 0; reqIdx < vReq.size(); reqIdx++)
        ret.push_back(JSONRPCExecOne(vReq[reqIdx]));

    return write_string(Value(ret), false) + "\n";
}

void ServiceConnection(AcceptedConnection *conn)
{
bool fRun = true;
bool httpRet; false;
int nProto = 0;

    LogPrintf("*** RGP ServiceConnection start connection with %s \n", conn->peer_address_to_string() );

    fRun = true;
    while (fRun)
    {
        LogPrintf("*** RGP ServiceConnection Main Loop \n");

        nProto = 0;
        map<string, string> mapHeaders;
        string strRequest, strMethod, strURI;

        LogPrintf("*** RGP ServiceConnection before ReadHTTPRequestLine  \n" );

        // Read HTTP request line
        if (!ReadHTTPRequestLine(conn->stream(), nProto, strMethod, strURI))
        {
            LogPrintf("*** RGP ServiceConnection ReadHTTPMessage request FAILED! \n");
            break;
        }

        LogPrintf("*** RGP ServiceConnection ReadHTTPMessage strRequest %s mapHeaders %s Protocol %d \n", strRequest, strURI, nProto );


        // Read HTTP message headers and body
        // RGP added bool checks
        httpRet = false;
        httpRet = ReadHTTPMessage(conn->stream(), mapHeaders, strRequest, nProto, MAX_SIZE);

        if ( httpRet == false )
        {
            LogPrintf("*** RGP ServiceConnection Error, ReadHTTP, no stream contents \n" );
            break;
        }


        if (strURI != "/")
        {
            conn->stream() << HTTPReply(HTTP_NOT_FOUND, "", false) << std::flush;

            LogPrintf("*** RGP ServiceConnection Error with strURI %s \n", strURI );

            break;
        }

        LogPrintf("*** RGP ServiceConnection Debug 1b \n" );

        // Check authorization
        if (mapHeaders.count("authorization") == 0)
        {
            conn->stream() << HTTPReply(HTTP_UNAUTHORIZED, "", false) << std::flush;

            LogPrintf("*** RGP ServiceConnection Error with mapheaders count\n" );
            break;                       
        }

        LogPrintf("*** RGP ServiceConnection Debug 1c \n" );

        if (!HTTPAuthorized(mapHeaders))
        {
            LogPrintf("ThreadRPCServer incorrect password attempt from %s\n", conn->peer_address_to_string());

            /* Deter brute-forcing short passwords.
               If this results in a DoS the user really
               shouldn't have their RPC port exposed. */
            if (mapArgs["-rpcpassword"].size() < 20)
                MilliSleep(250);

            conn->stream() << HTTPReply(HTTP_UNAUTHORIZED, "", false) << std::flush;
            break;
        }

        LogPrintf("*** RGP ServiceConnection Debug 1d \n" );

        if (mapHeaders["connection"] == "close")
            fRun = false;

        JSONRequest jreq;
        try
        {            

            LogPrintf("*** RGP ServiceConnection JSONRequest Phase Debug 1e \n" );

            // Parse request
            Value valRequest;            

            if (!read_string(strRequest, valRequest))
            {

                LogPrintf("*** RGP ServiceConnection JSONRequest Phase read_string failed Debug 1f \n" );

                LogPrintf("RGP ServiceConnection value request invalid \n");
                throw JSONRPCError(RPC_PARSE_ERROR, "Parse error");
            }

            LogPrintf("*** RGP ServiceConnection string requested %s \n", strRequest );

            string strReply;

            LogPrintf("*** RGP ServiceConnection JSONRequest Phase Debug 1g \n" );

            // singleton request
            if (valRequest.type() == obj_type) {
                jreq.parse(valRequest);

                LogPrintf("*** RGP ServiceConnection JSONRequest before execute Debug 1ga Method %s \n",  jreq.strMethod );

                Value result = tableRPC.execute(jreq.strMethod, jreq.params);

                // Send reply
                strReply = JSONRPCReply(result, Value::null, jreq.id);

            // array of requests
            } else if (valRequest.type() == array_type)
                strReply = JSONRPCExecBatch(valRequest.get_array());
            else
            {

                LogPrintf("*** RGP ServiceConnection JSONRequest Phase Debug 1h \n" );

                LogPrintf("RGP ServiceConnection type request invalid \n");
                throw JSONRPCError(RPC_PARSE_ERROR, "Top-level object parse error");
            }

            LogPrintf("*** RGP ServiceConnection JSONRequest Phase Debug 1i \n" );

            conn->stream() << HTTPReply(HTTP_OK, strReply, fRun) << std::flush;
        }
        catch (Object& objError)
        {
            ErrorReply(conn->stream(), objError, jreq.id);
            LogPrintf("RGP ServiceConnection objerror exception caught \n");
            //break;
        }
        catch (std::exception& e)
        {
            ErrorReply(conn->stream(), JSONRPCError(RPC_PARSE_ERROR, e.what()), jreq.id);
            LogPrintf("RGP ServiceConnection std exception exception caught \n");
            //break;
        }
    }
}

json_spirit::Value CRPCTable::execute(const std::string &strMethod, const json_spirit::Array &params) const
{
Value result;
    // Find method

    const CRPCCommand *pcmd = tableRPC[strMethod];
    if (!pcmd)
    {
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Method not found");
    }

#ifdef ENABLE_WALLET
    if (pcmd->reqWallet && !pwalletMain)
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Method not found (disabled)");
#endif

    // Observe safe mode
    string strWarning = GetWarnings("rpc");
    if (strWarning != "" && !GetBoolArg("-disablesafemode", false) &&
        !pcmd->okSafeMode)
        throw JSONRPCError(RPC_FORBIDDEN_BY_SAFE_MODE, string("Safe mode: ") + strWarning);

    try
    {
        // Execute

        {

            if (pcmd->threadSafe)
            {
                result = pcmd->actor(params, false);
            }
#ifdef ENABLE_WALLET
            else if (!pwalletMain)
            {
                LOCK(cs_main);
                result = pcmd->actor(params, false);
            }
            else
            {
                LOCK2(cs_main, pwalletMain->cs_wallet);


                try
                    {
                        // Execute
                        return pcmd->actor(params, false);
                    }
                    catch (const std::exception& e)
                    {
                        throw JSONRPCError(RPC_MISC_ERROR, e.what());
                    }



                result = pcmd->actor(params, false);
            }
#else // ENABLE_WALLET
            else
            {
                LOCK(cs_main);
                result = pcmd->actor(params, false);
            }
#endif // !ENABLE_WALLET
        }

        return result;
    }
    catch (std::exception& e)
    {
        throw JSONRPCError(RPC_MISC_ERROR, e.what());
    }
}

std::string HelpExampleCli(string methodname, string args){
    return "> societyGd " + methodname + " " + args + "\n";
}

std::string HelpExampleRpc(string methodname, string args){
    return "> curl --user myusername --data-binary '{\"jsonrpc\": \"1.0\", \"id\":\"curltest\", "
        "\"method\": \"" + methodname + "\", \"params\": [" + args + "] }' -H 'content-type: text/plain;' http://127.0.0.1:9998/\n";
}

const CRPCTable tableRPC;

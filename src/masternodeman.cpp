

#include "masternodeman.h"
#include "masternode.h"
#include "activemasternode.h"
#include "darksend.h"
#include "core.h"
#include "util.h"
#include "addrman.h"

#include "net.h"

#include <boost/lexical_cast.hpp>
#include <boost/filesystem.hpp>


/** Masternode manager */
CMasternodeMan mnodeman;
CCriticalSection cs_process_message;

struct CompareValueOnly
{
    bool operator()(const pair<int64_t, CTxIn>& t1,
                    const pair<int64_t, CTxIn>& t2) const
    {
        return t1.first < t2.first;
    }
};
struct CompareValueOnlyMN
{
    bool operator()(const pair<int64_t, CMasternode>& t1,
                    const pair<int64_t, CMasternode>& t2) const
    {
        return t1.first < t2.first;
    }
};


//
// CMasternodeDB
//

CMasternodeDB::CMasternodeDB()
{
    pathMN = GetDataDir() / "mncache.dat";
    strMagicMessage = "MasternodeCache";
}

bool CMasternodeDB::Write(const CMasternodeMan& mnodemanToSave)
{
    //LogPrintf("*** RGP CMasternodeDB::Write Start \n");

    int64_t nStart = GetTimeMillis();

    // serialize addresses, checksum data up to that point, then append csum
    CDataStream ssMasternodes(SER_DISK, CLIENT_VERSION);
    ssMasternodes << strMagicMessage; // masternode cache file specific magic message
    ssMasternodes << FLATDATA(Params().MessageStart()); // network specific magic number
    ssMasternodes << mnodemanToSave;
    uint256 hash = Hash(ssMasternodes.begin(), ssMasternodes.end());
    ssMasternodes << hash;

    // open output file, and associate with CAutoFile
    FILE *file = fopen(pathMN.string().c_str(), "wb");
    CAutoFile fileout = CAutoFile(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s : Failed to open file %s", __func__, pathMN.string());

    // Write and commit header, data
    try {
        fileout << ssMasternodes;
    }
    catch (std::exception &e) {
        return error("%s : Serialize or I/O error - %s", __func__, e.what());
    }
    FileCommit(fileout.Get());
    fileout.fclose();

    //LogPrintf("Written info to mncache.dat  %dms\n", GetTimeMillis() - nStart);
    //LogPrintf("  %s\n", mnodemanToSave.ToString());

    return true;
}

CMasternodeDB::ReadResult CMasternodeDB::Read(CMasternodeMan& mnodemanToLoad)
{

    //LogPrintf("*** RGP CMasternodeDB::Read Start \n");

    int64_t nStart = GetTimeMillis();
    // open input file, and associate with CAutoFile
    FILE *file = fopen(pathMN.string().c_str(), "rb");
    CAutoFile filein = CAutoFile(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
    {
        error("%s : Failed to open file %s", __func__, pathMN.string());
        return FileError;
    }

    // use file size to size memory buffer
    int fileSize = boost::filesystem::file_size(pathMN);
    int dataSize = fileSize - sizeof(uint256);
    // Don't try to resize to a negative number if file is small
    if (dataSize < 0)
        dataSize = 0;
    vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // read data and checksum from file
    try {
        filein.read((char *)&vchData[0], dataSize);
        filein >> hashIn;
    }
    catch (std::exception &e) {
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return HashReadError;
    }
    filein.fclose();

    CDataStream ssMasternodes(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssMasternodes.begin(), ssMasternodes.end());
    if (hashIn != hashTmp)
    {
        error("%s : Checksum mismatch, data corrupted", __func__);
        return IncorrectHash;
    }

    unsigned char pchMsgTmp[4];
    std::string strMagicMessageTmp;
    try {
        // de-serialize file header (masternode cache file specific magic message) and ..

        ssMasternodes >> strMagicMessageTmp;

        // ... verify the message matches predefined one
        if (strMagicMessage != strMagicMessageTmp)
        {
            error("%s : Invalid masternode cache magic message", __func__);
            return IncorrectMagicMessage;
        }

        // de-serialize file header (network specific magic number) and ..
        ssMasternodes >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp)))
        {
            error("%s : Invalid network magic number", __func__);
            return IncorrectMagicNumber;
        }

        // de-serialize address data into one CMnList object
        ssMasternodes >> mnodemanToLoad;
    }
    catch (std::exception &e) {
        mnodemanToLoad.Clear();
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return IncorrectFormat;
    }

    mnodemanToLoad.CheckAndRemove(); // clean out expired
    //LogPrintf("Loaded info from mncache.dat  %dms\n", GetTimeMillis() - nStart);
    //LogPrintf("  %s\n", mnodemanToLoad.ToString());

    return Ok;
}

void DumpMasternodes()
{
    int64_t nStart = GetTimeMillis();

    LogPrintf("*** RGP DumpMasternodes Start \n");

    CMasternodeDB mndb;
    CMasternodeMan tempMnodeman;

    LogPrintf("Verifying mncache.dat format...\n");
    CMasternodeDB::ReadResult readResult = mndb.Read(tempMnodeman);
    // there was an error and it was not an error on file openning => do not proceed
    if (readResult == CMasternodeDB::FileError)
        LogPrintf("Missing masternode list file - mncache.dat, will try to recreate\n");
    else if (readResult != CMasternodeDB::Ok)
    {
        LogPrintf("Error reading mncache.dat: ");
        if(readResult == CMasternodeDB::IncorrectFormat)
            LogPrintf("magic is ok but data has invalid format, will try to recreate\n");
        else
        {
            LogPrintf("file format is unknown or invalid, please fix it manually\n");
            return;
        }
    }
    LogPrintf("Writting info to mncache.dat...\n");
    mndb.Write(mnodeman);

    LogPrintf("Masternode dump finished  %dms\n", GetTimeMillis() - nStart);
}

CMasternodeMan::CMasternodeMan() {
    nDsqCount = 0;
}

bool CMasternodeMan::Add(CMasternode &mn)
{
    LOCK(cs);

    //CMasternode testmn;

    //LogPrintf("*** RGP CMasternodeMan::Add Debug MN node %s 001\n", mn.addr.ToString() );

    //CMasternode testmn = Find( mn.vin );

    //LogPrintf("*** RGP CMasternodeMan::Add Debug after Find \n");

    /* -------------------------------------------------------------------
       -- RGP, if pwn is NULL, it's a new Masternode                    --
       --      changed to a bool, so if not found, add a new MasterNode --
       ------------------------------------------------------------------- */

    if ( !Find( mn.vin ) )
    {        
        //LogPrintf("*** RGP CMasternodeMan::Add Debug MN node  001\n" );

        vMasternodes.push_back(mn);
        return true;
    }

    //LogPrintf("*** RGP CMasternodeMan::Add Debug END \n" );

    return false;
}

void CMasternodeMan::AskForMN(CNode* pnode, CTxIn &vin)
{

    //LogPrintf("*** RGP CMasternodeMan::AskForMN Start \n");

    std::map<COutPoint, int64_t>::iterator i = mWeAskedForMasternodeListEntry.find(vin.prevout);
    if (i != mWeAskedForMasternodeListEntry.end())
    {
        int64_t t = (*i).second;
        if (GetTime() < t) return; // we've asked recently
    }

    // ask for the mnb info once from the node that sent mnp

    //LogPrintf("CMasternodeMan::AskForMN - Asking node for missing entry, vin: %s\n", vin.ToString());
    pnode->PushMessage("dseg", vin);
    int64_t askAgain = GetTime() + MASTERNODE_MIN_DSEEP_SECONDS;
    mWeAskedForMasternodeListEntry[vin.prevout] = askAgain;
}

void CMasternodeMan::Check()
{

    LOCK(cs);

    BOOST_FOREACH(CMasternode& mn, vMasternodes)
    {
        mn.Check();

    }


}

void CMasternodeMan::CheckAndRemove()
{
    LOCK(cs);

    //LogPrintf("*** RGP CMasternodeMan::ChckAndRemove Start \n");

    Check();

    //remove inactive
    vector<CMasternode>::iterator it = vMasternodes.begin();
    while(it != vMasternodes.end())
    {
        if((*it).activeState == CMasternode::MASTERNODE_REMOVE || (*it).activeState == CMasternode::MASTERNODE_VIN_SPENT || (*it).protocolVersion < nMasternodeMinProtocol)
        {

            LogPrint("masternode", "CMasternodeMan: Removing inactive masternode %s - %i now\n", (*it).addr.ToString().c_str(), size() - 1);

            it = vMasternodes.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // check who's asked for the masternode list
    map<CNetAddr, int64_t>::iterator it1 = mAskedUsForMasternodeList.begin();
    while(it1 != mAskedUsForMasternodeList.end()){
        if((*it1).second < GetTime()) {
            mAskedUsForMasternodeList.erase(it1++);
        } else {
            ++it1;
        }
    }

    // check who we asked for the masternode list
    it1 = mWeAskedForMasternodeList.begin();
    while(it1 != mWeAskedForMasternodeList.end()){
        if((*it1).second < GetTime()){
            mWeAskedForMasternodeList.erase(it1++);
        } else {
            ++it1;
        }
    }

    // check which masternodes we've asked for
    map<COutPoint, int64_t>::iterator it2 = mWeAskedForMasternodeListEntry.begin();
    while(it2 != mWeAskedForMasternodeListEntry.end()){
        if((*it2).second < GetTime()){
            mWeAskedForMasternodeListEntry.erase(it2++);
        } else {
            ++it2;
        }
    }

}

void CMasternodeMan::Clear()
{
    LOCK(cs);

    //LogPrintf("*** RGP CMasternodeMan::Clear Start \n");

    vMasternodes.clear();
    mAskedUsForMasternodeList.clear();
    mWeAskedForMasternodeList.clear();
    mWeAskedForMasternodeListEntry.clear();
    nDsqCount = 0;
}

int CMasternodeMan::CountEnabled(int protocolVersion)
{
    int i = 0;
    protocolVersion = protocolVersion == -1 ? masternodePayments.GetMinMasternodePaymentsProto() : protocolVersion;    

    BOOST_FOREACH(CMasternode& mn, vMasternodes)
    {
        mn.Check();
        if ( mn.protocolVersion < protocolVersion || !mn.IsEnabled() )
        {            
            continue;
        }

        i++;
    }

    return i;
}

int CMasternodeMan::CountMasternodesAboveProtocol(int protocolVersion)
{
    int i = 0;

   // LogPrintf("*** RGP CMasternodeMan::CountMasternodesAboveProtocol Start \n");

    BOOST_FOREACH(CMasternode& mn, vMasternodes)
    {
        mn.Check();
        if(mn.protocolVersion < protocolVersion || !mn.IsEnabled()) continue;
        i++;
    }

    return i;
}

void CMasternodeMan::DsegUpdate(CNode* pnode)
{
    LOCK(cs);

   // LogPrintf("*** RGP CMasternodeMan::DsegUpdate Start \n");

    std::map<CNetAddr, int64_t>::iterator it = mWeAskedForMasternodeList.find(pnode->addr);
    if (it != mWeAskedForMasternodeList.end())
    {
        if (GetTime() < (*it).second)
        {
            //LogPrintf("dseg - we already asked for the list; skipping...\n");
            return;
        }
    }
    pnode->PushMessage("dseg", CTxIn());
    int64_t askAgain = GetTime() + MASTERNODES_DSEG_SECONDS;
    mWeAskedForMasternodeList[pnode->addr] = askAgain;
}

bool CMasternodeMan::FindNull(const CTxIn &vin)
{
    LOCK(cs);

    BOOST_FOREACH(CMasternode& mn, vMasternodes)
    {

        if(mn.vin.prevout == vin.prevout)
            return true;
    }

   // LogPrintf("*** RGP CMasternodeMan::Find Debug 2 \n");

    return false;
}


CMasternode *CMasternodeMan::Find(const CTxIn &vin)
{

    LOCK(cs);

    BOOST_FOREACH(CMasternode& mn, vMasternodes)
    {
        if(mn.vin.prevout == vin.prevout)
            return &mn;
    }

    return NULL;
}

CMasternode* CMasternodeMan::FindOldestNotInVec(const std::vector<CTxIn> &vVins, int nMinimumAge)
{
    LOCK(cs);

    CMasternode *pOldestMasternode = NULL;

    BOOST_FOREACH(CMasternode &mn, vMasternodes)
    {   
        mn.Check();
        if(!mn.IsEnabled()) continue;

        if(mn.GetMasternodeInputAge() < nMinimumAge) continue;

        bool found = false;
        BOOST_FOREACH(const CTxIn& vin, vVins)
        {

            if(mn.vin.prevout == vin.prevout)
            {   
                found = true;
                break;
            }
        }
        if(found) continue;

        if(pOldestMasternode == NULL || pOldestMasternode->SecondsSincePayment() < mn.SecondsSincePayment())
        {
            pOldestMasternode = &mn;
        }
    }

    return pOldestMasternode;
}

CMasternode *CMasternodeMan::FindRandom()
{
    LOCK(cs);

    //LogPrintf("*** RGP CMasternodeMan::FindRandom Start \n");

    if(size() == 0) return NULL;

    return &vMasternodes[GetRandInt(vMasternodes.size())];
}

CMasternode *CMasternodeMan::Find(const CPubKey &pubKeyMasternode)
{
    LOCK(cs);

    //LogPrintf("*** RGP CMasternodeMan::Find Start \n");

    BOOST_FOREACH(CMasternode& mn, vMasternodes)
    {
        if(mn.pubkey2 == pubKeyMasternode)
            return &mn;
    }
    return NULL;
}

CMasternode *CMasternodeMan::FindRandomNotInVec(std::vector<CTxIn> &vecToExclude, int protocolVersion)
{
    LOCK(cs);

    protocolVersion = protocolVersion == -1 ? masternodePayments.GetMinMasternodePaymentsProto() : protocolVersion;

     //LogPrintf("*** RGP CMasternodeMan::FindRandomNotInVec Start \n");

    int nCountEnabled = CountEnabled(protocolVersion);
    //LogPrintf("CMasternodeMan::FindRandomNotInVec - nCountEnabled - vecToExclude.size() %d\n", nCountEnabled - vecToExclude.size());
    if(nCountEnabled - vecToExclude.size() < 1) return NULL;

    int rand = GetRandInt(nCountEnabled - vecToExclude.size());
    //LogPrintf("CMasternodeMan::FindRandomNotInVec - rand %d\n", rand);
    bool found;

    BOOST_FOREACH(CMasternode &mn, vMasternodes) {
        if(mn.protocolVersion < protocolVersion || !mn.IsEnabled()) continue;
        found = false;
        BOOST_FOREACH(CTxIn &usedVin, vecToExclude) {
            if(mn.vin.prevout == usedVin.prevout) {
                found = true;
                break;
            }
        }
        if(found) continue;
        if(--rand < 1) {
            return &mn;
        }
    }

    return NULL;
}

CMasternode* CMasternodeMan::GetCurrentMasterNode(int mod, int64_t nBlockHeight, int minProtocol)
{
    unsigned int score = 0;
    CMasternode* winner = NULL;

    //LogPrintf("*** RGP GetCurrentMasterNode start \n");

    /* RGP : 13th April 2021 update this line to build vMasternodes */
    std::vector<CMasternode> vMasternodes = mnodeman.GetFullMasternodeVector();

    if ( vMasternodes.empty() )
    {

        //LogPrintf("*** RGP vMasternodes empty returning null winner \n");
        return winner;
    }

    // scan for winner
    BOOST_FOREACH(CMasternode& mn, vMasternodes)
    {

        /* -------------------------------------------------------------------
           -- RGP, Patch for BSG-182, where rewards are updated, but old MN --
           -- are not replying. Using GHaydons MN at the moment for the     --
           -- rewards fix.                                                  --
           ------------------------------------------------------------------- */
        //LogPrintf("*** RGP GetCurrentMasterNode loop MN addr %s \n", mn.addr.ToString() );

        //if ( mn.addr.ToString() == "143.198.227.129:23980" )
        //{
        //    /* RGP, this is GHayden's MN */
        //    // calculate the score for each masternode
        //    uint256 n = mn.CalculateScore(mod, nBlockHeight);
        //    unsigned int n2 = 0;
        //    memcpy(&n2, &n, sizeof(n2));
        //    winner = &mn;
        //    break;
        //}

        mn.Check();
        //LogPrintf("*** RGP RGP GetCurrentMasterNode %s \n",  mn.addr.ToString() );
        if(mn.protocolVersion < minProtocol || !mn.IsEnabled())
        {
            //LogPrintf("*** RGP GetCurrentMasterNode Protocol failure %d \n", mn.protocolVersion  );
            continue;

        }

        // calculate the score for each masternode
        uint256 n = mn.CalculateScore(mod, nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        // determine the winner
        if(n2 > score)
        {
            score = n2;
            winner = &mn;

            //LogPrintf("*** RGP MasterNode Winner of Stake %s \n", winner->addr.ToString() );

        }

    }

    //LogPrintf("*** RGP GetCurrentMasterNode end winner node %s \n", winner->addr.ToString() );

    return winner;
}

int CMasternodeMan::GetMasternodeRank(const CTxIn& vin, int64_t nBlockHeight, int minProtocol, bool fOnlyActive)
{
    std::vector<pair<unsigned int, CTxIn> > vecMasternodeScores;

     //LogPrintf("*** RGP CMasternodeMan::GetMasternodeRank Start \n");

    //make sure we know about this block
    uint256 hash = 0;
    if(!GetBlockHash(hash, nBlockHeight)) return -1;

    // scan for winner
    BOOST_FOREACH(CMasternode& mn, vMasternodes) {

        if(mn.protocolVersion < minProtocol) continue;
        if(fOnlyActive) {
            mn.Check();
            if(!mn.IsEnabled()) continue;
        }

        uint256 n = mn.CalculateScore(1, nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        vecMasternodeScores.push_back(make_pair(n2, mn.vin));
    }

    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareValueOnly());

    int rank = 0;
    BOOST_FOREACH (PAIRTYPE(unsigned int, CTxIn)& s, vecMasternodeScores){
        rank++;
        if(s.second == vin) {
            return rank;
        }
    }

    return -1;
}

std::vector<pair<int, CMasternode> > CMasternodeMan::GetMasternodeRanks(int64_t nBlockHeight, int minProtocol)
{
    std::vector<pair<unsigned int, CMasternode> > vecMasternodeScores;
    std::vector<pair<int, CMasternode> > vecMasternodeRanks;

    //make sure we know about this block
    uint256 hash = 0;
    if(!GetBlockHash(hash, nBlockHeight)) return vecMasternodeRanks;

    // scan for winner
    BOOST_FOREACH(CMasternode& mn, vMasternodes) {

        mn.Check();

        if(mn.protocolVersion < minProtocol) continue;
        if(!mn.IsEnabled()) {
            continue;
        }

        uint256 n = mn.CalculateScore(1, nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        vecMasternodeScores.push_back(make_pair(n2, mn));
    }

    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareValueOnlyMN());

    int rank = 0;
    BOOST_FOREACH (PAIRTYPE(unsigned int, CMasternode)& s, vecMasternodeScores){
        rank++;
        vecMasternodeRanks.push_back(make_pair(rank, s.second));
    }

    return vecMasternodeRanks;
}

CMasternode* CMasternodeMan::GetMasternodeByRank(int nRank, int64_t nBlockHeight, int minProtocol, bool fOnlyActive)
{
    std::vector<pair<unsigned int, CTxIn> > vecMasternodeScores;

    // scan for winner
    BOOST_FOREACH(CMasternode& mn, vMasternodes) {

        if(mn.protocolVersion < minProtocol) continue;
        if(fOnlyActive) {
            mn.Check();
            if(!mn.IsEnabled()) continue;
        }

        uint256 n = mn.CalculateScore(1, nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        vecMasternodeScores.push_back(make_pair(n2, mn.vin));
    }

    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareValueOnly());

    int rank = 0;
    BOOST_FOREACH (PAIRTYPE(unsigned int, CTxIn)& s, vecMasternodeScores){
        rank++;
        if(rank == nRank) {
            return Find(s.second);
        }
    }

    return NULL;
}

void CMasternodeMan::ProcessMasternodeConnections()
{
    LOCK(cs_vNodes);

    //LogPrintf("*** RGP CMasternodeMan::ProcessMasternodeConnections Start \n");

    if(!darkSendPool.pSubmittedToMasternode)
    {
        //LogPrintf("*** RGP CMasternodeMan::ProcessMasternodeConnections Darksend submit failed \n");
        return;
    }
    
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        //LogPrintf("*** RGP CMasternodeMan::ProcessMasternodeConnections Masternodes %s \n", pnode->addr.ToString() );

        if(darkSendPool.pSubmittedToMasternode->addr == pnode->addr) continue;

        if(pnode->fDarkSendMaster){
            //LogPrintf("Closing masternode connection %s \n", pnode->addr.ToString().c_str());
            pnode->CloseSocketDisconnect();
        }
    }
}

void CMasternodeMan::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{

    //Normally would disable functionality, NEED this enabled for staking.
    if(fLiteMode) return;

    if(!darkSendPool.IsBlockchainSynced() )
    {

        return;
    }

    LOCK(cs_process_message);

    if (strCommand == "dsee")
    {
        //DarkSend Election Entry

        CTxIn vin;
        CService addr;
        CPubKey pubkey;
        CPubKey pubkey2;
        vector<unsigned char> vchSig;
        int64_t sigTime;
        int count;
        int current;
        int64_t lastUpdated;
        int protocolVersion;
        std::string strMessage;
        CScript rewardAddress = CScript();
        int rewardPercentage = 0;
        std::string strKeyMasternode;

        //LogPrintf("*** RGP CMasternodeMan::ProcessMessage Debug 1 \n");

        // 70047 and greater
        vRecv >> vin >> addr >> vchSig >> sigTime >> pubkey >> pubkey2 >> count >> current >> lastUpdated >> protocolVersion;

        //LogPrintf("*** RGP CMasternodeMan::ProcessMessage Debug 2 \n");

        //Invalid nodes check
        if (sigTime < 1511159400) {
            //LogPrintf("dsee - Bad packet\n");
            return;
        }
        
        //LogPrintf("*** RGP CMasternodeMan::ProcessMessage Debug 3 \n");


        if (sigTime > lastUpdated) {
            //LogPrintf("dsee - Bad node entry\n");
            return;
        }
        
        if (addr.GetPort() == 0) {
            //LogPrintf("dsee - Bad port\n");
            return;
        }
        
        // make sure signature isn't in the future (past is OK)
        if (sigTime > GetAdjustedTime() + 60 * 60) {
            //LogPrintf("dsee - Signature rejected, too far into the future %s\n", vin.ToString().c_str());
            return;
        }

        //LogPrintf("*** RGP CMasternodeMan::ProcessMessage Debug 4 \n");

        bool isLocal = addr.IsRFC1918() || addr.IsLocal();
        //if(RegTest()) isLocal = false;

        std::string vchPubKey(pubkey.begin(), pubkey.end());
        std::string vchPubKey2(pubkey2.begin(), pubkey2.end());

        strMessage = addr.ToString() + boost::lexical_cast<std::string>(sigTime) + vchPubKey + vchPubKey2 + boost::lexical_cast<std::string>(protocolVersion);    

        //LogPrintf("*** RGP, CMasternodeMan::ProcessMessage strMessage %s \n", strMessage );

        if(protocolVersion < MIN_POOL_PEER_PROTO_VERSION)
        {
            //LogPrintf("dsee - ignoring outdated masternode %s protocol version %d\n", vin.ToString().c_str(), protocolVersion);
            return;
        }

        CScript pubkeyScript;
        pubkeyScript.SetDestination(pubkey.GetID());

        if(pubkeyScript.size() != 25) {
            //LogPrintf("dsee - pubkey the wrong size\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        //LogPrintf("*** RGP CMasternodeMan::ProcessMessage Debug 4a \n");

        CScript pubkeyScript2;
        pubkeyScript2.SetDestination(pubkey2.GetID());

        if(pubkeyScript2.size() != 25) {
            //LogPrintf("dsee - pubkey2 the wrong size\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        //LogPrintf("*** RGP CMasternodeMan::ProcessMessage Debug 5 \n");

        if(!vin.scriptSig.empty()) {
            //LogPrintf("dsee - Ignore Not Empty ScriptSig %s\n",vin.ToString().c_str());
            return;
        }

        //LogPrintf("*** RGP CMasternodeMan::ProcessMessage Debug 6 \n");

        std::string errorMessage = "";
        if(!darkSendSigner.VerifyMessage(pubkey, vchSig, strMessage, errorMessage)){
            LogPrintf("dsee - Got bad masternode address signature\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

       // LogPrintf("*** RGP CMasternodeMan::ProcessMessage Debug 7 \n");

        //search existing masternode list, this is where we update existing masternodes with new dsee broadcasts
        CMasternode* pmn = this->Find(vin);
        // if we are a masternode but with undefined vin and this dsee is ours (matches our Masternode privkey) then just skip this part
        if(pmn != NULL && !(fMasterNode && activeMasternode.vin == CTxIn() && pubkey2 == activeMasternode.pubKeyMasternode))
        {
            // count == -1 when it's a new entry
            //   e.g. We don't want the entry relayed/time updated when we're syncing the list
            // mn.pubkey = pubkey, IsVinAssociatedWithPubkey is validated once below,
            //   after that they just need to match
            if(count == -1 && pmn->pubkey == pubkey && !pmn->UpdatedWithin(MASTERNODE_MIN_DSEE_SECONDS)){
                pmn->UpdateLastSeen();
                if(pmn->sigTime < sigTime){ //take the newest entry
                    if (!CheckNode((CAddress)addr)){
                        pmn->isPortOpen = false;
                    } else {
                        pmn->isPortOpen = true;
                        addrman.Add(CAddress(addr), pfrom->addr, 2*60*60); // use this as a peer
                    }
                   // LogPrintf("dsee - Got updated entry for %s\n", addr.ToString().c_str());
                    pmn->pubkey2 = pubkey2;
                    pmn->sigTime = sigTime;
                    pmn->sig = vchSig;
                    pmn->protocolVersion = protocolVersion;
                    pmn->addr = addr;
                    pmn->Check();
                    pmn->isOldNode = true;
                    if(pmn->IsEnabled())
                        mnodeman.RelayOldMasternodeEntry(vin, addr, vchSig, sigTime, pubkey, pubkey2, count, current, lastUpdated, protocolVersion);
                }
            }

            return;
        }

        //LogPrintf("*** RGP CMasternodeMan::ProcessMessage Debug 8 \n");

        // make sure the vout that was signed is related to the transaction that spawned the masternode
        //  - this is expensive, so it's only done once per masternode
        if(!darkSendSigner.IsVinAssociatedWithPubkey(vin, pubkey)) {
            LogPrintf("dsee - Got mismatched pubkey and vin\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

       // LogPrintf("masternode, dsee - Got NEW masternode entry %s\n", addr.ToString().c_str());

        // make sure it's still unspent
        //  - this is checked later by .check() in many places and by ThreadCheckDarkSendPool()

        CValidationState state;
        CTransaction tx = CTransaction();
        CTxOut vout = CTxOut((GetMNCollateral(pindexBest->nHeight)-1)*COIN, darkSendPool.collateralPubKey);
        tx.vin.push_back(vin);
        tx.vout.push_back(vout);
        bool fAcceptable = false;
        {
            TRY_LOCK(cs_main, lockMain);
            if(!lockMain) return;
            fAcceptable = AcceptableInputs(mempool, tx, false, NULL);
        }
        if(fAcceptable){
            LogPrintf("masternode, dsee - Accepted masternode entry %i %i\n", count, current);

            if(GetInputAge(vin) < MASTERNODE_MIN_CONFIRMATIONS){
                LogPrintf("dsee - Input must have least %d confirmations\n", MASTERNODE_MIN_CONFIRMATIONS);
                Misbehaving(pfrom->GetId(), 20);
                return;
            }

            // verify that sig time is legit in past
            // should be at least not earlier than block when 10000 SocietyG tx got MASTERNODE_MIN_CONFIRMATIONS
            uint256 hashBlock = 0;
            GetTransaction(vin.prevout.hash, tx, hashBlock);
            map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
            if (mi != mapBlockIndex.end() && (*mi).second)
            {
                CBlockIndex* pMNIndex = (*mi).second; // block for 10000 SocietyG tx -> 1 confirmation
                CBlockIndex* pConfIndex = FindBlockByHeight((pMNIndex->nHeight + MASTERNODE_MIN_CONFIRMATIONS - 1)); // block where tx got MASTERNODE_MIN_CONFIRMATIONS
                if(pConfIndex->GetBlockTime() > sigTime)
                {
                    LogPrintf("dsee - Bad sigTime %d for masternode %20s %105s (%i conf block is at %d)\n",
                              sigTime, addr.ToString(), vin.ToString(), MASTERNODE_MIN_CONFIRMATIONS, pConfIndex->GetBlockTime());
                    return;
                }
            }

            // add our masternode
            CMasternode mn(addr, vin, pubkey, vchSig, sigTime, pubkey2, protocolVersion, rewardAddress, rewardPercentage);
            mn.UpdateLastSeen(lastUpdated);

            if (!CheckNode((CAddress)addr)){
                mn.ChangePortStatus(false);
            } else {
                addrman.Add(CAddress(addr), pfrom->addr, 2*60*60); // use this as a peer
            }
            
            mn.ChangeNodeStatus(true);
            this->Add(mn);

            // if it matches our masternodeprivkey, then we've been remotely activated
            if(pubkey2 == activeMasternode.pubKeyMasternode && protocolVersion == PROTOCOL_VERSION){
                activeMasternode.EnableHotColdMasterNode(vin, addr);
                }

            if(count == -1 && !isLocal)
                mnodeman.RelayOldMasternodeEntry(vin, addr, vchSig, sigTime, pubkey, pubkey2, count, current, lastUpdated, protocolVersion);
            
        } else {
            LogPrintf("dsee - Rejected masternode entry %s\n", addr.ToString().c_str());

            int nDoS = 0;
            if (state.IsInvalid(nDoS))
            {
                LogPrintf("dsee - %s from %s %s was not accepted into the memory pool\n", tx.GetHash().ToString().c_str(),
                    pfrom->addr.ToString().c_str(), pfrom->cleanSubVer.c_str());
                if (nDoS > 0)
                    Misbehaving(pfrom->GetId(), nDoS);
            }
        }
    }

    else if (strCommand == "dsee+") { //DarkSend Election Entry+

        CTxIn vin;
        CService addr;
        CPubKey pubkey;
        CPubKey pubkey2;
        vector<unsigned char> vchSig;
        int64_t sigTime;
        int count;
        int current;
        int64_t lastUpdated;
        int protocolVersion;
        CScript rewardAddress;
        int rewardPercentage;
        std::string strMessage;
        std::string strKeyMasternode;
        int vsigcheck;

        //LogPrintf("*** RGP CMasternodeMan::ProcessMessage dsee+ read from another node \n") ;

        // 70047 and greater
        vRecv >> vin >> addr >> vchSig >> sigTime >> pubkey >> pubkey2 >> count >> current >> lastUpdated >> protocolVersion >> rewardAddress >> rewardPercentage;        


        std::string vchPubKeyX1(pubkey.begin(), pubkey.end());
        std::string vchPubKeyX2(pubkey2.begin(), pubkey2.end());
        std::string vchsigtestX1( vchSig.begin(), vchSig.end() );

        //LogPrintf(" \n");
        //for ( vsigcheck = 0; vsigcheck < vchSig.size(); vsigcheck++ )
        //{
        //    LogPrintf("%x", vchSig[ vsigcheck ] );
        //}
        //LogPrintf("*** RGP vchSig end \n");

        //LogPrintf("*** RGP pubkey begin \n");
        //for ( vsigcheck = 0; vsigcheck < pubkey.size(); vsigcheck++ )
        //{
            //LogPrintf("%x", pubkey.operator [] ( vsigcheck  ) );
        //}
        //const unsigned char &operator[](unsigned int pos) const { return vch[pos]; }
        //LogPrintf("*** RGP pubkey end \n");

        //LogPrintf("*** RGP pubkey2 begin \n");
        //for ( vsigcheck = 0; vsigcheck < pubkey2.size(); vsigcheck++ )
        //{
            //LogPrintf("%x", pubkey2.operator [] ( vsigcheck  ) );
        //}
        //LogPrintf("*** RGP pubkey2 end \n");


        //LogPrintf("*** RGP CTxin VIN scriptsig %s \n", vin.scriptSig.ToString() );
        //LogPrintf("*** RGP CTxin VIN prevpubkey %s \n", vin.prevPubKey.ToString() );
        //LogPrintf("*** RGP Node Addr %s \n",  addr.ToString() );
        //LogPrintf("*** RGP vchSig %s \n",   vchsigtestX1 );
        //LogPrintf("*** RGP Time NOW %d \n",  lastUpdated );
        //LogPrintf("*** RGP pubkey1 %s \n",   vchPubKeyX1 );
        //LogPrintf("*** RGP pubkey2 %s \n",   vchPubKeyX2 );

        //ExtractDestination(mn.rewardAddress, address1);

        //LogPrintf("*** RGP Rewardaddress %s \n",   rewardAddress );
        //LogPrintf("*** RGP Reeward Percent %d \n",   rewardPercentage );


        //Invalid nodes check
        if (sigTime < 1511159400) {
            //LogPrintf("dsee+ - Bad packet\n");
            return;
        }
        
        if (sigTime > lastUpdated) {
            //LogPrintf("dsee+ - Bad node entry\n");
            return;
        }
        
        if (addr.GetPort() == 0) {
            //LogPrintf("dsee+ - Bad port\n");
            return;
        }
        
        // make sure signature isn't in the future (past is OK)
        if (sigTime > GetAdjustedTime() + 60 * 60) {
            LogPrintf("dsee+ - Signature rejected, too far into the future %s\n", vin.ToString().c_str());
            return;
        }

        bool isLocal = addr.IsRFC1918() || addr.IsLocal();
        //if(RegTest()) isLocal = false;

        std::string vchPubKey(pubkey.begin(), pubkey.end());
        std::string vchPubKey2(pubkey2.begin(), pubkey2.end());

        strMessage = addr.ToString() + boost::lexical_cast<std::string>(sigTime) + vchPubKey + vchPubKey2 + boost::lexical_cast<std::string>(protocolVersion)  + rewardAddress.ToString() + boost::lexical_cast<std::string>(rewardPercentage);
        
        if(rewardPercentage < 0 || rewardPercentage > 100){
            LogPrintf("dsee+ - reward percentage out of range %d\n", rewardPercentage);
            return;
        }
        if(protocolVersion < MIN_POOL_PEER_PROTO_VERSION) {
            LogPrintf("dsee+ - ignoring outdated masternode %s protocol version %d\n", vin.ToString().c_str(), protocolVersion);
            return;
        }

        CScript pubkeyScript;
        pubkeyScript.SetDestination(pubkey.GetID());

        if(pubkeyScript.size() != 25) {
            LogPrintf("dsee+ - pubkey the wrong size\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        CScript pubkeyScript2;
        pubkeyScript2.SetDestination(pubkey2.GetID());

        if(pubkeyScript2.size() != 25) {
            LogPrintf("dsee+ - pubkey2 the wrong size\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        if(!vin.scriptSig.empty()) {
            LogPrintf("dsee+ - Ignore Not Empty ScriptSig %s\n",vin.ToString().c_str());
            return;
        }

        std::string errorMessage = "";
        if(!darkSendSigner.VerifyMessage(pubkey, vchSig, strMessage, errorMessage))
        {
            LogPrintf("dsee+ - Got bad masternode address signature\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        //search existing masternode list, this is where we update existing masternodes with new dsee broadcasts
        CMasternode* pmn = this->Find(vin);
        // if we are a masternode but with undefined vin and this dsee is ours (matches our Masternode privkey) then just skip this part
        if(pmn != NULL && !(fMasterNode && activeMasternode.vin == CTxIn() && pubkey2 == activeMasternode.pubKeyMasternode))
        {
            // count == -1 when it's a new entry
            //   e.g. We don't want the entry relayed/time updated when we're syncing the list
            // mn.pubkey = pubkey, IsVinAssociatedWithPubkey is validated once below,
            //   after that they just need to match
            if(count == -1 && pmn->pubkey == pubkey && !pmn->UpdatedWithin(MASTERNODE_MIN_DSEE_SECONDS)){
                pmn->UpdateLastSeen();

                if(pmn->sigTime < sigTime){ //take the newest entry
                    if (!CheckNode((CAddress)addr)){
                        pmn->isPortOpen = false;
                    } else {
                        pmn->isPortOpen = true;
                        addrman.Add(CAddress(addr), pfrom->addr, 2*60*60); // use this as a peer
                    }
                    LogPrintf("dsee+ - Got updated entry for %s\n", addr.ToString().c_str());
                    pmn->pubkey2 = pubkey2;
                    pmn->sigTime = sigTime;
                    pmn->sig = vchSig;
                    pmn->protocolVersion = protocolVersion;
                    pmn->addr = addr;
                    pmn->rewardAddress = rewardAddress;
                    pmn->rewardPercentage = rewardPercentage;                    
                    pmn->Check();
                    pmn->isOldNode = false;

                    if(pmn->IsEnabled())
                    {
                        LogPrintf("dsee+ - pmn is  enabled, RelayMasternodeEntry  \n");
                        mnodeman.RelayMasternodeEntry(vin, addr, vchSig, sigTime, pubkey, pubkey2, count, current, lastUpdated, protocolVersion, rewardAddress, rewardPercentage );
                    }
                    else
                         LogPrintf("dsee+ - pmn is not enabled \n");
                }
            }

            return;
        }

        // make sure the vout that was signed is related to the transaction that spawned the masternode
        //  - this is expensive, so it's only done once per masternode
        if(!darkSendSigner.IsVinAssociatedWithPubkey(vin, pubkey)) {
            LogPrintf("dsee+ - Got mismatched pubkey and vin\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        //LogPrintf("masternode, dsee+ - Got NEW masternode entry %s\n", addr.ToString().c_str());

        // make sure it's still unspent
        //  - this is checked later by .check() in many places and by ThreadCheckDarkSendPool()

        CValidationState state;
        CTransaction tx = CTransaction();
        CTxOut vout = CTxOut((GetMNCollateral(pindexBest->nHeight)-1)*COIN, darkSendPool.collateralPubKey);
        tx.vin.push_back(vin);
        tx.vout.push_back(vout);
        bool fAcceptable = false;
        {
            TRY_LOCK(cs_main, lockMain);
            if(!lockMain) return;
            fAcceptable = AcceptableInputs(mempool, tx, false, NULL);
        }
        if( fAcceptable )
        {
            //LogPrintf("masternode, dsee+ - Accepted masternode entry %i %i\n", count, current);

            if( GetInputAge( vin ) < MASTERNODE_MIN_CONFIRMATIONS)
            {
                LogPrintf("dsee+ - Input must have least %d confirmations\n", MASTERNODE_MIN_CONFIRMATIONS);

                Misbehaving(pfrom->GetId(), 20);

                return;
            }

           // LogPrintf("masternode, dsee+ DEBUG 001 %s \n", pfrom->addr.ToString() );


            // verify that sig time is legit in past
            // should be at least not earlier than block when 10000 SocietyG tx got MASTERNODE_MIN_CONFIRMATIONS
            uint256 hashBlock = 0;
            GetTransaction(vin.prevout.hash, tx, hashBlock);
            map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
            if (mi != mapBlockIndex.end() && (*mi).second)
            {

                //LogPrintf("masternode, dsee+ DEBUG 002 %s \n", pfrom->addr.ToString() );

                CBlockIndex* pMNIndex = (*mi).second; // block for 10000 SocietyG tx -> 1 confirmation
                CBlockIndex* pConfIndex = FindBlockByHeight((pMNIndex->nHeight + MASTERNODE_MIN_CONFIRMATIONS - 1)); // block where tx got MASTERNODE_MIN_CONFIRMATIONS
                if(pConfIndex->GetBlockTime() > sigTime)
                {
                    LogPrintf("dsee+ - Bad sigTime %d for masternode %20s %105s (%i conf block is at %d)\n",
                              sigTime, addr.ToString(), vin.ToString(), MASTERNODE_MIN_CONFIRMATIONS, pConfIndex->GetBlockTime());
                    return;
                }
            }

           // LogPrintf("masternode, dsee+ DEBUG 003 %s \n", pfrom->addr.ToString() );

            //doesn't support multisig addresses
            if(rewardAddress.IsPayToScriptHash()){
                rewardAddress = CScript();
                rewardPercentage = 0;
            }

            //LogPrintf("masternode, dsee+ DEBUG 004 %s \n", pfrom->addr.ToString() );

            // add our masternode
            CMasternode mn(addr, vin, pubkey, vchSig, sigTime, pubkey2, protocolVersion, rewardAddress, rewardPercentage);
            mn.UpdateLastSeen(lastUpdated);

            if (!CheckNode((CAddress)addr))
            {
                LogPrintf("masternode, dsee+ DEBUG 005 %s \n", pfrom->addr.ToString() );
                mn.ChangePortStatus(false);
            }
            else
            {
                LogPrintf("masternode, dsee+ DEBUG 006 %s \n", pfrom->addr.ToString() );
                addrman.Add(CAddress(addr), pfrom->addr, 2*60*60); // use this as a peer
            }
            
            //LogPrintf("masternode, dsee+ DEBUG 007 %s \n", pfrom->addr.ToString() );

            mn.ChangeNodeStatus(true);
            this->Add(mn);
            
            //LogPrintf("masternode, dsee+ DEBUG 007a %s \n", pfrom->addr.ToString() );

            // if it matches our masternodeprivkey, then we've been remotely activated
            if(pubkey2 == activeMasternode.pubKeyMasternode && protocolVersion == PROTOCOL_VERSION)
            {
                LogPrintf("masternode, dsee+ DEBUG 008 %s \n", pfrom->addr.ToString() );

                activeMasternode.EnableHotColdMasterNode(vin, addr);
            }

            //LogPrintf("masternode, dsee+ DEBUG 008a %s \n", pfrom->addr.ToString() );

            if(count == -1 && !isLocal)
            {

               LogPrintf("*** RGP Process Message DSee+ from %s  Validity Checks strKeyMasterNode %s \n", pfrom->addr.ToString(), strKeyMasternode );
                mnodeman.RelayMasternodeEntry(vin, addr, vchSig, sigTime, pubkey, pubkey2, count, current, lastUpdated, protocolVersion, rewardAddress, rewardPercentage );

             }


            //LogPrintf("masternode, dsee+ DEBUG 008b %s \n", pfrom->addr.ToString() );

        }
        else
        {
            LogPrintf("dsee+ - Rejected masternode entry %s\n", addr.ToString().c_str());

            int nDoS = 0;
            if (state.IsInvalid(nDoS))
            {
                LogPrintf("dsee+ - %s from %s %s was not accepted into the memory pool\n", tx.GetHash().ToString().c_str(),
                    pfrom->addr.ToString().c_str(), pfrom->cleanSubVer.c_str());
                if (nDoS > 0)
                    Misbehaving(pfrom->GetId(), nDoS);
            }
        }
    }
    
    else if (strCommand == "dseep") { //DarkSend Election Entry Ping

        CTxIn vin;
        vector<unsigned char> vchSig;
        int64_t sigTime;
        bool stop;
        vRecv >> vin >> vchSig >> sigTime >> stop;

        //LogPrintf("dseep - Received: vin: %s sigTime: %lld stop: %s\n", vin.ToString().c_str(), sigTime, stop ? "true" : "false");

        if (sigTime > GetAdjustedTime() + 60 * 60) {
            LogPrintf("dseep - Signature rejected, too far into the future %s\n", vin.ToString().c_str());
            return;
        }

        if (sigTime <= GetAdjustedTime() - 60 * 60) {
            LogPrintf("dseep - Signature rejected, too far into the past %s - %d %d \n", vin.ToString().c_str(), sigTime, GetAdjustedTime());
            return;
        }



        // see if we have this masternode
        //CMasternode* pmn = this->Find(vin);

        //if ( !this->Find(vin) )
        //    LogPrintf("*** RGP DSEEP command, vin not found, NEW MN \n");

        CMasternode* pmn = this->Find(vin);


        if(pmn != NULL && pmn->protocolVersion >= MIN_POOL_PEER_PROTO_VERSION)
        {
            // LogPrintf("dseep - Found corresponding mn for vin: %s\n", vin.ToString().c_str());
            // take this only if it's newer
            if(pmn->lastDseep < sigTime)
            {
                std::string strMessage = pmn->addr.ToString() + boost::lexical_cast<std::string>(sigTime) + boost::lexical_cast<std::string>(stop);

                std::string errorMessage = "";
                if(!darkSendSigner.VerifyMessage(pmn->pubkey2, vchSig, strMessage, errorMessage))
                {
                    LogPrintf("dseep - Got bad masternode address signature %s \n", vin.ToString().c_str());
                    //Misbehaving(pfrom->GetId(), 100);
                    return;
                }

                pmn->lastDseep = sigTime;

                if(!pmn->UpdatedWithin(MASTERNODE_MIN_DSEEP_SECONDS))
                {
                    if(stop) pmn->Disable();
                    else
                    {
                        pmn->UpdateLastSeen();
                        pmn->Check();
                        if(!pmn->IsEnabled()) return;
                    }
                    mnodeman.RelayMasternodeEntryPing(vin, vchSig, sigTime, stop);
                }
            }
            return;
        }       

        std::map<COutPoint, int64_t>::iterator i = mWeAskedForMasternodeListEntry.find(vin.prevout);
        if (i != mWeAskedForMasternodeListEntry.end())
        {
            int64_t t = (*i).second;
            if (GetTime() < t) return; // we've asked recently
        }

        // ask for the dseep info once from the node that sent dseep

        LogPrintf("dseep - Asking source node for missing entry %s\n", vin.ToString().c_str());
        pfrom->PushMessage("dseg", vin);
        int64_t askAgain = GetTime()+ MASTERNODE_MIN_DSEEP_SECONDS;
        mWeAskedForMasternodeListEntry[vin.prevout] = askAgain;

    } else if (strCommand == "mvote") { //Masternode Vote

        CTxIn vin;
        vector<unsigned char> vchSig;
        int nVote;
        vRecv >> vin >> vchSig >> nVote;

        // see if we have this Masternode
        CMasternode* pmn = this->Find(vin);
        if(pmn != NULL)
        {
            if((GetAdjustedTime() - pmn->lastVote) > (60*60))
            {
                std::string strMessage = vin.ToString() + boost::lexical_cast<std::string>(nVote);

                std::string errorMessage = "";
                if(!darkSendSigner.VerifyMessage(pmn->pubkey2, vchSig, strMessage, errorMessage))
                {
                    LogPrintf("mvote - Got bad Masternode address signature %s \n", vin.ToString().c_str());
                    return;
                }

                pmn->nVote = nVote;
                pmn->lastVote = GetAdjustedTime();

                //send to all peers
                LOCK(cs_vNodes);
                BOOST_FOREACH(CNode* pnode, vNodes)
                    pnode->PushMessage("mvote", vin, vchSig, nVote);
            }

            return;
        }

    } else if (strCommand == "dseg") { //Get masternode list or specific entry

        CTxIn vin;
        vRecv >> vin;

        if(vin == CTxIn()) { //only should ask for this once
            //local network
            if(!pfrom->addr.IsRFC1918() && Params().NetworkID() == CChainParams::MAIN)
            {
                std::map<CNetAddr, int64_t>::iterator i = mAskedUsForMasternodeList.find(pfrom->addr);
                if (i != mAskedUsForMasternodeList.end())
                {
                    int64_t t = (*i).second;
                    if (GetTime() < t) {
                        Misbehaving(pfrom->GetId(), 34);
                        LogPrintf("dseg - peer already asked me for the list\n");
                        return;
                    }
                }

                int64_t askAgain = GetTime() + MASTERNODES_DSEG_SECONDS;
                mAskedUsForMasternodeList[pfrom->addr] = askAgain;
            }
        } //else, asking for a specific node which is ok

        int count = this->size();
        int i = 0;

        BOOST_FOREACH(CMasternode& mn, vMasternodes) {

            if(mn.addr.IsRFC1918()) continue; //local network

            if(mn.IsEnabled())
            {
                LogPrintf("masternode, dseg - Sending masternode entry - %s \n", mn.addr.ToString().c_str());
                if(vin == CTxIn()){
                    if (mn.isOldNode){
                        pfrom->PushMessage("dsee", mn.vin, mn.addr, mn.sig, mn.sigTime, mn.pubkey, mn.pubkey2, count, i, mn.lastTimeSeen, mn.protocolVersion);
                    } else {
                        pfrom->PushMessage("dsee+", mn.vin, mn.addr, mn.sig, mn.sigTime, mn.pubkey, mn.pubkey2, count, i, mn.lastTimeSeen, mn.protocolVersion, mn.rewardAddress, mn.rewardPercentage);
                    }
                } else if (vin == mn.vin) {
                    if (mn.isOldNode){
                        pfrom->PushMessage("dsee", mn.vin, mn.addr, mn.sig, mn.sigTime, mn.pubkey, mn.pubkey2, count, i, mn.lastTimeSeen, mn.protocolVersion);
                    } else {
                        pfrom->PushMessage("dsee+", mn.vin, mn.addr, mn.sig, mn.sigTime, mn.pubkey, mn.pubkey2, count, i, mn.lastTimeSeen, mn.protocolVersion, mn.rewardAddress, mn.rewardPercentage);
                    }
                    LogPrintf("dseg - Sent 1 masternode entries to %s\n", pfrom->addr.ToString().c_str());
                    return;
                }
                i++;
            }
        }

        //LogPrintf("dseg - Sent %d masternode entries to %s\n", i, pfrom->addr.ToString().c_str());
    }

}

void CMasternodeMan::RelayOldMasternodeEntry(const CTxIn vin, const CService addr, const std::vector<unsigned char> vchSig, const int64_t nNow, const CPubKey pubkey, const CPubKey pubkey2, const int count, const int current, const int64_t lastUpdated, const int protocolVersion)
{
    LOCK(cs_vNodes);

    //LogPrintf("*** RGP CMasternodeMan::RelayOldMasternodeEntry Start \n") ;

    BOOST_FOREACH(CNode* pnode, vNodes)
    {
       // LogPrintf("*** RGP CMasternodeMan::RelayOldMasternodeEntry to node \n" );


        pnode->PushMessage("dsee", vin, addr, vchSig, nNow, pubkey, pubkey2, count, current, lastUpdated, protocolVersion);
    }
}


// RGP Info for debug
// mnodeman.RelayMasternodeEntry(vin, service, vchMasterNodeSignature, masterNodeSignatureTime, pubKeyCollateralAddress,
//                              pubKeyMasternode, -1, -1, masterNodeSignatureTime,
//                              PROTOCOL_VERSION, rewardAddress, rewardPercentage );



void CMasternodeMan::RelayMasternodeEntry(const CTxIn vin, const CService addr, const std::vector<unsigned char> vchSig,
                                          const int64_t nNow, const CPubKey pubkey, const CPubKey pubkey2, const int count,
                                          const int current, const int64_t lastUpdated, const int protocolVersion,
                                          CScript rewardAddress, int rewardPercentage )
{
    LOCK(cs_vNodes);

    /*CTxIn vin;
    CService addr;
    CPubKey pubkey;
    CPubKey pubkey2;
    std::vector<unsigned char> sig;
    int activeState;
    int64_t sigTime; //dsee message times
    int64_t lastDseep;
    int64_t lastTimeSeen;
    int cacheInputAge;
    int cacheInputAgeBlock;
    bool unitTest;
    bool allowFreeTx;
    int protocolVersion;
    int64_t nLastDsq; //the dsq count from the last dsq broadcast of this node
    CScript rewardAddress;
    int rewardPercentage;
    int nVote;
    int64_t lastVote;
    int nScanningErrorCount;
    int nLastScanningErrorBlockHeight;
    int64_t nLastPaid;
    bool isPortOpen;
    bool isOldNode;
    std::string strKeyMasternode;
    */

    std::string vchPubKey(pubkey.begin(), pubkey.end());
    std::string vchPubKey2(pubkey2.begin(), pubkey2.end());
    std::string vchsigtest( vchSig.begin(), vchSig.end() );


    //(strMode == "pubkey")
    //CScript key1;
    //pubkey.SetDestination(mn.pubkey.GetID());
    //CTxDestination address1;
    //ExtractDestination(pubkey, address1);
    //CSocietyGcoinAddress address2(address1);

    //CScript key2;
    //pubkey.SetDestination(mn.pubkey2.GetID());
    //CTxDestination address3;
    //ExtractDestination(pubkey2, address3);
    //CSocietyGcoinAddress address4(address3);


    //ExtractDestination(mn.rewardAddress, address1);

    //LogPrintf("*** RGp pubkey 1 %s \n", address2.ToString() );
    //LogPrintf("*** RGp pubkey 2 %s \n", address4.ToString() );

    //LogPrintf("*** RGP CMasternodeMan::RelayMasternodeEntry Start dsee+ message creation \n") ;

    //LogPrintf("*** RGP CTxin VIN %s \n",  vin.ToString() );
    //LogPrintf("*** RGP Node Addr %s \n",  addr.ToString() );
    //LogPrintf("*** RGP vchSig %s \n",   vchsigtest );
    //LogPrintf("*** RGP Time NOW %d \n",  nNow );
    //LogPrintf("*** RGP pubkey1 %s \n",   vchPubKey );
    //LogPrintf("*** RGP pubkey2 %s \n",   vchPubKey2 );



    //LogPrintf("*** RGP Rewardaddress %s \n",   rewardAddress );
    //LogPrintf("*** RGP Reeward Percent %d \n",   rewardPercentage );


    BOOST_FOREACH(CNode* pnode, vNodes)
    {

        //LogPrintf("*** RGP CMasternodeMan::RelayMasternodeEntry to node %s \n", pnode->addr.ToString().c_str()  );

        pnode->PushMessage("dsee+",
                               vin,
                              addr,
                            vchSig,
                              nNow,
                            pubkey,
                           pubkey2,
                             count,
                           current,
                       lastUpdated,
                   protocolVersion,
                     rewardAddress,
                  rewardPercentage );
    }
}

void CMasternodeMan::RelayMasternodeEntryPing(const CTxIn vin, const std::vector<unsigned char> vchSig, const int64_t nNow, const bool stop)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        //LogPrintf("*** RGP CMasternodeMan::RelayMasternodeEntryPing to node %s \n", pnode->addr.ToString().c_str() );
        //if ( pnode-> )
        pnode->PushMessage("dseep", vin, vchSig, nNow, stop);
    }
}

void CMasternodeMan::Remove(CTxIn vin)
{
    LOCK(cs);

    vector<CMasternode>::iterator it = vMasternodes.begin();
    while(it != vMasternodes.end()){
        if((*it).vin == vin){
            LogPrint("masternode", "CMasternodeMan: Removing Masternode %s - %i now\n", (*it).addr.ToString().c_str(), size() - 1);
            vMasternodes.erase(it);
            break;
        } else {
            ++it;
        }
    }
}

std::string CMasternodeMan::ToString() const
{
    std::ostringstream info;

    info << "masternodes: " << (int)vMasternodes.size() <<
            ", peers who asked us for masternode list: " << (int)mAskedUsForMasternodeList.size() <<
            ", peers we asked for masternode list: " << (int)mWeAskedForMasternodeList.size() <<
            ", entries in Masternode list we asked for: " << (int)mWeAskedForMasternodeListEntry.size() <<
            ", nDsqCount: " << (int)nDsqCount;

    return info.str();
}

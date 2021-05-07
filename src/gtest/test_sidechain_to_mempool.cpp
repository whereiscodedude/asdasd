#include <gtest/gtest.h>
#include <boost/filesystem.hpp>
#include <chainparams.h>
#include <util.h>

#include <txdb.h>
#include <main.h>
#include <zen/forks/fork7_sidechainfork.h>

#include <key.h>
#include <keystore.h>
#include <script/sign.h>

#include "tx_creation_utils.h"
#include <gtest/libzendoo_test_files.h>
#include <consensus/validation.h>

#include <sc/sidechain.h>
#include <txmempool.h>
#include <init.h>
#include <undo.h>
#include <gtest/libzendoo_test_files.h>

class CCoinsOnlyViewDB : public CCoinsViewDB
{
public:
    CCoinsOnlyViewDB(size_t nCacheSize, bool fWipe = false)
        : CCoinsViewDB(nCacheSize, false, fWipe) {}

    bool BatchWrite(CCoinsMap &mapCoins)
    {
        const uint256 hashBlock;
        const uint256 hashAnchor;
        CAnchorsMap mapAnchors;
        CNullifiersMap mapNullifiers;
        CSidechainsMap mapSidechains;
        CSidechainEventsMap mapSidechainEvents;
        CCswNullifiersMap cswNullifiers;

        return CCoinsViewDB::BatchWrite(mapCoins, hashBlock, hashAnchor, mapAnchors, mapNullifiers, mapSidechains, mapSidechainEvents, cswNullifiers);
    }
};

class CNakedCCoinsViewCache : public CCoinsViewCache
{
public:
    CNakedCCoinsViewCache(CCoinsView* pWrappedView): CCoinsViewCache(pWrappedView)
    {
        uint256 dummyAnchor = uint256S("59d2cde5e65c1414c32ba54f0fe4bdb3d67618125286e6a191317917c812c6d7"); //anchor for empty block!?
        this->hashAnchor = dummyAnchor;

        CAnchorsCacheEntry dummyAnchorsEntry;
        dummyAnchorsEntry.entered = true;
        dummyAnchorsEntry.flags = CAnchorsCacheEntry::DIRTY;
        this->cacheAnchors[dummyAnchor] = dummyAnchorsEntry;

    };
    CSidechainsMap& getSidechainMap() {return this->cacheSidechains; };
};

class SidechainsInMempoolTestSuite: public ::testing::Test {
public:
    SidechainsInMempoolTestSuite():
        aMempool(::minRelayTxFee),
        pathTemp(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path()),
        chainStateDbSize(2 * 1024 * 1024),
        pChainStateDb(nullptr),
        minimalHeightForSidechains(SidechainFork().getHeight(CBaseChainParams::REGTEST)),
        csMainLock(cs_main, "cs_main", __FILE__, __LINE__),
        csAMempool(aMempool.cs, "cs_AMempool", __FILE__, __LINE__)
    {
        SelectParams(CBaseChainParams::REGTEST);

        boost::filesystem::create_directories(pathTemp);
        mapArgs["-datadir"] = pathTemp.string();

        pChainStateDb = new CCoinsOnlyViewDB(chainStateDbSize,/*fWipe*/true);
        pcoinsTip     = new CCoinsViewCache(pChainStateDb);
    }

    void SetUp() override {
        chainSettingUtils::ExtendChainActiveToHeight(minimalHeightForSidechains);
        pcoinsTip->SetBestBlock(chainActive.Tip()->GetBlockHash());
        pindexBestHeader = chainActive.Tip();

        InitCoinGeneration();
    }

    void TearDown() override {
        mempool.clear();
        chainActive.SetTip(nullptr);
        mapBlockIndex.clear();
    }

    ~SidechainsInMempoolTestSuite() {
        delete pcoinsTip;
        pcoinsTip = nullptr;

        delete pChainStateDb;
        pChainStateDb = nullptr;

        ClearDatadirCache();
        boost::system::error_code ec;
        boost::filesystem::remove_all(pathTemp.string(), ec);
    }

protected:
    CTxMemPool aMempool;
    CTransaction GenerateScTx(const CAmount & creationTxAmount, int epochLenght = -1, bool ceasedVkDefined = true);
    CTransaction GenerateFwdTransferTx(const uint256 & newScId, const CAmount & fwdTxAmount);
    CTransaction GenerateBtrTx(const uint256 & scId);
    CTxCeasedSidechainWithdrawalInput GenerateCSWInput(const uint256& scId, const std::string& nullifierHex, CAmount amount);
    CTransaction GenerateCSWTx(const std::vector<CTxCeasedSidechainWithdrawalInput>& csws);
    CTransaction GenerateCSWTx(const CTxCeasedSidechainWithdrawalInput& csw);
    CScCertificate GenerateCertificate(const uint256 & scId, int epochNum, const uint256 & endEpochBlockHash,
                                 CAmount inputAmount, CAmount changeTotalAmount/* = 0*/, unsigned int numChangeOut/* = 0*/,
                                 CAmount bwtTotalAmount/* = 1*/, unsigned int numBwt/* = 1*/, int64_t quality,
                                 const CTransactionBase* inputTxBase = nullptr);
    void storeSidechainWithCurrentHeight(CNakedCCoinsViewCache& view, const uint256& scId, const CSidechain& sidechain, int chainActiveHeight);

private:
    boost::filesystem::path  pathTemp;
    const unsigned int       chainStateDbSize;
    CCoinsOnlyViewDB*        pChainStateDb;

    const unsigned int       minimalHeightForSidechains;

    CKey                     coinsKey;
    CBasicKeyStore           keystore;
    CScript                  coinsScript;

    void InitCoinGeneration();
    std::pair<uint256, CCoinsCacheEntry> GenerateCoinsAmount(const CAmount & amountToGenerate);
    bool StoreCoins(const std::pair<uint256, CCoinsCacheEntry>& entryToStore);

    //Critical sections below needed when compiled with --enable-debug, which activates ASSERT_HELD
    CCriticalBlock csMainLock;
    CCriticalBlock csAMempool;
};

TEST_F(SidechainsInMempoolTestSuite, NewSidechainIsAcceptedToMempool) {
    CTransaction scTx = GenerateScTx(CAmount(1));
    CValidationState txState;
    bool missingInputs = false;

    EXPECT_TRUE(AcceptTxToMemoryPool(mempool, txState, scTx, LimitFreeFlag::OFF, &missingInputs, RejectAbsurdFeeFlag::OFF));
}

TEST_F(SidechainsInMempoolTestSuite, FwdTransfersToUnknownSidechainAreNotAllowed) {
    uint256 scId = uint256S("dddd");
    CTransaction fwdTx = GenerateFwdTransferTx(scId, CAmount(10));
    CValidationState fwdTxState;
    bool missingInputs = false;

    EXPECT_FALSE(AcceptTxToMemoryPool(mempool, fwdTxState, fwdTx, LimitFreeFlag::OFF, &missingInputs, RejectAbsurdFeeFlag::OFF));
}

//A proof that https://github.com/HorizenOfficial/zen/issues/215 is solved
TEST_F(SidechainsInMempoolTestSuite, FwdTransfersToUnconfirmedSidechainsAreAllowed) {
    CTransaction scTx = GenerateScTx(CAmount(1));
    const uint256& scId = scTx.GetScIdFromScCcOut(0);
    CValidationState scTxState;
    bool missingInputs = false;
    AcceptTxToMemoryPool(mempool, scTxState, scTx, LimitFreeFlag::OFF, &missingInputs, RejectAbsurdFeeFlag::OFF);
    ASSERT_TRUE(mempool.hasSidechainCreationTx(scId));

    CTransaction fwdTx = GenerateFwdTransferTx(scId, CAmount(10));
    CValidationState fwdTxState;
    EXPECT_TRUE(AcceptTxToMemoryPool(mempool, fwdTxState, fwdTx, LimitFreeFlag::OFF, &missingInputs, RejectAbsurdFeeFlag::OFF));
}

TEST_F(SidechainsInMempoolTestSuite, FwdTransfersToConfirmedSidechainsAreAllowed) {
    int creationHeight = 1789;
    chainSettingUtils::ExtendChainActiveToHeight(creationHeight);

    CTransaction scTx = GenerateScTx(CAmount(10));
    const uint256& scId = scTx.GetScIdFromScCcOut(0);

    CBlock aBlock;
    CCoinsViewCache sidechainsView(pcoinsTip);
    sidechainsView.UpdateSidechain(scTx, aBlock, creationHeight);
    sidechainsView.SetBestBlock(chainActive.Tip()->GetBlockHash());
    sidechainsView.Flush();

    CTransaction fwdTx = GenerateFwdTransferTx(scId, CAmount(10));
    CValidationState fwdTxState;
    bool missingInputs = false;

    EXPECT_TRUE(AcceptTxToMemoryPool(mempool, fwdTxState, fwdTx, LimitFreeFlag::OFF, &missingInputs, RejectAbsurdFeeFlag::OFF));
}

TEST_F(SidechainsInMempoolTestSuite, BtrToUnknownSidechainAreNotAllowed) {
    uint256 scId = uint256S("dddd");
    CTransaction btrTx = GenerateBtrTx(scId);
    CValidationState btrTxState;
    bool missingInputs = false;

    EXPECT_FALSE(AcceptTxToMemoryPool(mempool, btrTxState, btrTx, LimitFreeFlag::OFF, &missingInputs, RejectAbsurdFeeFlag::OFF));
}

TEST_F(SidechainsInMempoolTestSuite, BtrToUnconfirmedSidechainsAreAllowed) {
    CTransaction scTx = GenerateScTx(CAmount(1));
    const uint256& scId = scTx.GetScIdFromScCcOut(0);
    CValidationState scTxState;
    bool missingInputs = false;
    AcceptTxToMemoryPool(mempool, scTxState, scTx, LimitFreeFlag::OFF, &missingInputs, RejectAbsurdFeeFlag::OFF);
    ASSERT_TRUE(mempool.hasSidechainCreationTx(scId));

    CTransaction btrTx = GenerateBtrTx(scId);
    CValidationState btrTxState;
    EXPECT_TRUE(AcceptTxToMemoryPool(mempool, btrTxState, btrTx, LimitFreeFlag::OFF, &missingInputs, RejectAbsurdFeeFlag::OFF));
}

TEST_F(SidechainsInMempoolTestSuite, BtrToConfirmedSidechainsAreAllowed) {
    int creationHeight = 1789;
    chainSettingUtils::ExtendChainActiveToHeight(creationHeight);

    CTransaction scTx = GenerateScTx(CAmount(10));
    const uint256& scId = scTx.GetScIdFromScCcOut(0);

    CBlock aBlock;
    CCoinsViewCache sidechainsView(pcoinsTip);
    sidechainsView.UpdateSidechain(scTx, aBlock, creationHeight);
    sidechainsView.SetBestBlock(chainActive.Tip()->GetBlockHash());
    sidechainsView.Flush();

    CTransaction btrTx = GenerateBtrTx(scId);
    CValidationState btrTxState;
    bool missingInputs = false;

    EXPECT_TRUE(AcceptTxToMemoryPool(mempool, btrTxState, btrTx, LimitFreeFlag::OFF, &missingInputs, RejectAbsurdFeeFlag::OFF));
}

TEST_F(SidechainsInMempoolTestSuite, hasSidechainCreationTxTest) {
    uint256 scId = uint256S("1492");

    //Case 1: no sidechain related tx in mempool
    bool res = aMempool.hasSidechainCreationTx(scId);
    EXPECT_FALSE(res);

    //Case 2: fwd transfer tx only in mempool
    CTransaction fwdTx = GenerateFwdTransferTx(scId, CAmount(10));
    CTxMemPoolEntry fwdPoolEntry(fwdTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    aMempool.addUnchecked(fwdPoolEntry.GetTx().GetHash(), fwdPoolEntry);
    res = aMempool.hasSidechainCreationTx(scId);
    EXPECT_FALSE(res);

    //Case 3: btr tx only in mempool
    CTransaction btrTx = GenerateBtrTx(scId);
    CTxMemPoolEntry btrTxEntry(btrTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    std::map<uint256, CFieldElement> dummyCertDataHashInfo;
    dummyCertDataHashInfo[scId] = CFieldElement{};
    aMempool.addUnchecked(btrTxEntry.GetTx().GetHash(), btrTxEntry, /*fCurrentEstimate*/true, dummyCertDataHashInfo);
    res = aMempool.hasSidechainCreationTx(scId);
    EXPECT_FALSE(res);

    //Case 4: sc creation tx in mempool
    CTransaction scTx  = GenerateScTx(CAmount(10));
    const uint256& scIdOk = scTx.GetScIdFromScCcOut(0);
    CTxMemPoolEntry scPoolEntry(scTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    aMempool.addUnchecked(scPoolEntry.GetTx().GetHash(), scPoolEntry);
    res = aMempool.hasSidechainCreationTx(scIdOk);
    EXPECT_TRUE(res);
}

TEST_F(SidechainsInMempoolTestSuite, ScAndFwdsAndBtrInMempool_ScNonRecursiveRemoval) {
    // Associated scenario: Sidechain creation and some fwds and btr are in mempool.
    // Sc Creation is confirmed, hence it has to be removed from mempool, while fwds stay.

    CTransaction scTx = GenerateScTx(CAmount(10));
    const uint256& scId = scTx.GetScIdFromScCcOut(0);
    CTxMemPoolEntry scEntry(scTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    aMempool.addUnchecked(scTx.GetHash(), scEntry);
    ASSERT_TRUE(aMempool.hasSidechainCreationTx(scId));

    CTransaction fwdTx1 = GenerateFwdTransferTx(scId, CAmount(10));
    CTxMemPoolEntry fwdEntry1(fwdTx1, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    aMempool.addUnchecked(fwdTx1.GetHash(), fwdEntry1);

    CTransaction fwdTx2 = GenerateFwdTransferTx(scId, CAmount(20));
    CTxMemPoolEntry fwdEntry2(fwdTx2, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    aMempool.addUnchecked(fwdTx2.GetHash(), fwdEntry2);

    CTransaction btrTx = GenerateBtrTx(scId);
    CTxMemPoolEntry btrEntry(btrTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);

    std::map<uint256, CFieldElement> dummyCertDataHashInfo;
    dummyCertDataHashInfo[scId] = CFieldElement{};
    aMempool.addUnchecked(btrTx.GetHash(), btrEntry, /*fCurrentEstimate*/true, dummyCertDataHashInfo);

    std::list<CTransaction> removedTxs;
    std::list<CScCertificate> removedCerts;
    aMempool.remove(scTx, removedTxs, removedCerts, /*fRecursive*/false);

    EXPECT_TRUE(std::count(removedTxs.begin(), removedTxs.end(), scTx));
    EXPECT_FALSE(std::count(removedTxs.begin(), removedTxs.end(), fwdTx1));
    EXPECT_FALSE(std::count(removedTxs.begin(), removedTxs.end(), fwdTx2));
    EXPECT_FALSE(std::count(removedTxs.begin(), removedTxs.end(), btrTx));
}

TEST_F(SidechainsInMempoolTestSuite, FwdsAndBtrsOnlyInMempool_FwdNonRecursiveRemoval) {
    // Associated scenario: fwts and btr are in mempool, hence scCreation must be already confirmed
    // A fwd is confirmed hence it, and only it, is removed from mempool
    uint256 scId = uint256S("ababab");

    CTransaction fwdTx1 = GenerateFwdTransferTx(scId, CAmount(10));
    CTxMemPoolEntry fwdEntry1(fwdTx1, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    aMempool.addUnchecked(fwdTx1.GetHash(), fwdEntry1);

    CTransaction fwdTx2 = GenerateFwdTransferTx(scId, CAmount(20));
    CTxMemPoolEntry fwdEntry2(fwdTx2, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    aMempool.addUnchecked(fwdTx2.GetHash(), fwdEntry2);

    CTransaction btrTx = GenerateBtrTx(scId);
    CTxMemPoolEntry btrEntry(btrTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    std::map<uint256, CFieldElement> dummyCertDataHashInfo;
    dummyCertDataHashInfo[scId] = CFieldElement{};
    aMempool.addUnchecked(btrTx.GetHash(), btrEntry, /*fCurrentEstimate*/true, dummyCertDataHashInfo);

    std::list<CTransaction> removedTxs;
    std::list<CScCertificate> removedCerts;
    aMempool.remove(fwdTx1, removedTxs, removedCerts, /*fRecursive*/false);

    EXPECT_TRUE(std::count(removedTxs.begin(), removedTxs.end(), fwdTx1));
    EXPECT_FALSE(std::count(removedTxs.begin(), removedTxs.end(), fwdTx2));
    EXPECT_FALSE(std::count(removedTxs.begin(), removedTxs.end(), btrTx));
}

TEST_F(SidechainsInMempoolTestSuite, FwdsAndBtrsOnlyInMempool_BtrNonRecursiveRemoval) {
    // Associated scenario: fws and btr are in mempool, hence scCreation must be already confirmed
    // A fwd is confirmed hence it, and only it, is removed from mempool

    uint256 scId = uint256S("ababab");

    CTransaction fwdTx1 = GenerateFwdTransferTx(scId, CAmount(10));
    CTxMemPoolEntry fwdEntry1(fwdTx1, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    aMempool.addUnchecked(fwdTx1.GetHash(), fwdEntry1);

    CTransaction fwdTx2 = GenerateFwdTransferTx(scId, CAmount(20));
    CTxMemPoolEntry fwdEntry2(fwdTx2, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    aMempool.addUnchecked(fwdTx2.GetHash(), fwdEntry2);

    CTransaction btrTx = GenerateBtrTx(scId);
    CTxMemPoolEntry btrEntry(btrTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    std::map<uint256, CFieldElement> dummyCertDataHashInfo;
    dummyCertDataHashInfo[scId] = CFieldElement{};
    aMempool.addUnchecked(btrTx.GetHash(), btrEntry, /*fCurrentEstimate*/true, dummyCertDataHashInfo);

    std::list<CTransaction> removedTxs;
    std::list<CScCertificate> removedCerts;
    aMempool.remove(btrTx, removedTxs, removedCerts, /*fRecursive*/false);

    EXPECT_FALSE(std::count(removedTxs.begin(), removedTxs.end(), fwdTx1));
    EXPECT_FALSE(std::count(removedTxs.begin(), removedTxs.end(), fwdTx2));
    EXPECT_TRUE(std::count(removedTxs.begin(), removedTxs.end(), btrTx));
}

TEST_F(SidechainsInMempoolTestSuite, ScAndFwdsAndBtrInMempool_ScRecursiveRemoval) {
    // Associated scenario: Sidechain creation and some fwds/btr are in mempool, e.g. as a result of previous blocks disconnections
    // One of the new blocks about to me mounted double spends the original scTx, hence scCreation is marked for recursive removal by removeForConflicts
    // both scCreation and fwds must be cleared from mempool

    CTransaction scTx = GenerateScTx(CAmount(10));
    const uint256& scId = scTx.GetScIdFromScCcOut(0);
    CTxMemPoolEntry scEntry(scTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    aMempool.addUnchecked(scTx.GetHash(), scEntry);
    ASSERT_TRUE(aMempool.hasSidechainCreationTx(scId));

    CTransaction fwdTx1 = GenerateFwdTransferTx(scId, CAmount(10));
    CTxMemPoolEntry fwdEntry1(fwdTx1, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    aMempool.addUnchecked(fwdTx1.GetHash(), fwdEntry1);

    CTransaction fwdTx2 = GenerateFwdTransferTx(scId, CAmount(20));
    CTxMemPoolEntry fwdEntry2(fwdTx2, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    aMempool.addUnchecked(fwdTx2.GetHash(), fwdEntry2);

    CTransaction btrTx = GenerateBtrTx(scId);
    CTxMemPoolEntry btrEntry(btrTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    std::map<uint256, CFieldElement> dummyCertDataHashInfo;
    dummyCertDataHashInfo[scId] = CFieldElement{};
    aMempool.addUnchecked(btrTx.GetHash(), btrEntry, /*fCurrentEstimate*/true, dummyCertDataHashInfo);

    std::list<CTransaction> removedTxs;
    std::list<CScCertificate> removedCerts;
    aMempool.remove(scTx, removedTxs, removedCerts, /*fRecursive*/true);

    EXPECT_TRUE(std::count(removedTxs.begin(), removedTxs.end(), scTx));
    EXPECT_TRUE(std::count(removedTxs.begin(), removedTxs.end(), fwdTx1));
    EXPECT_TRUE(std::count(removedTxs.begin(), removedTxs.end(), fwdTx2));
    EXPECT_TRUE(std::count(removedTxs.begin(), removedTxs.end(), btrTx));
}

TEST_F(SidechainsInMempoolTestSuite, FwdsAndBtrOnlyInMempool_ScRecursiveRemoval) {
    // Associated scenario: upon block disconnections fwds and btr have entered into mempool.
    // While unmounting block containing scCreation, scCreation cannot make to mempool. fwds and btr must me purged
    CTransaction scTx = GenerateScTx(CAmount(10));
    const uint256& scId = scTx.GetScIdFromScCcOut(0);

    CTransaction fwdTx1 = GenerateFwdTransferTx(scId, CAmount(10));
    CTxMemPoolEntry fwdEntry1(fwdTx1, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    aMempool.addUnchecked(fwdTx1.GetHash(), fwdEntry1);

    CTransaction fwdTx2 = GenerateFwdTransferTx(scId, CAmount(20));
    CTxMemPoolEntry fwdEntry2(fwdTx2, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    aMempool.addUnchecked(fwdTx2.GetHash(), fwdEntry2);

    CTransaction btrTx = GenerateBtrTx(scId);
    CTxMemPoolEntry btrEntry(btrTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    std::map<uint256, CFieldElement> dummyCertDataHashInfo;
    dummyCertDataHashInfo[scId] = CFieldElement{};
    aMempool.addUnchecked(btrTx.GetHash(), btrEntry, /*fCurrentEstimate*/true, dummyCertDataHashInfo);

    std::list<CTransaction> removedTxs;
    std::list<CScCertificate> removedCerts;
    aMempool.remove(scTx, removedTxs, removedCerts, /*fRecursive*/true);

    EXPECT_TRUE(std::count(removedTxs.begin(), removedTxs.end(), fwdTx1));
    EXPECT_TRUE(std::count(removedTxs.begin(), removedTxs.end(), fwdTx2));
    EXPECT_TRUE(std::count(removedTxs.begin(), removedTxs.end(), btrTx));
}

TEST_F(SidechainsInMempoolTestSuite, ScAndFwdsAndBtrInMempool_FwdRecursiveRemoval) {
    // Associated scenario: upon block disconnections a fwd cannot make to mempool.
    // Recursive removal for refused fwd is called, but other fwds are unaffected

    uint256 scId = uint256S("1492");

    CTransaction fwdTx1 = GenerateFwdTransferTx(scId, CAmount(10));
    CTxMemPoolEntry fwdEntry1(fwdTx1, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    aMempool.addUnchecked(fwdTx1.GetHash(), fwdEntry1);

    CTransaction fwdTx2 = GenerateFwdTransferTx(scId, CAmount(20));
    CTxMemPoolEntry fwdEntry2(fwdTx2, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    aMempool.addUnchecked(fwdTx2.GetHash(), fwdEntry2);

    CTransaction btrTx = GenerateBtrTx(scId);
    CTxMemPoolEntry btrEntry(btrTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    std::map<uint256, CFieldElement> dummyCertDataHashInfo;
    dummyCertDataHashInfo[scId] = CFieldElement{};
    aMempool.addUnchecked(btrTx.GetHash(), btrEntry, /*fCurrentEstimate*/true, dummyCertDataHashInfo);

    std::list<CTransaction> removedTxs;
    std::list<CScCertificate> removedCerts;
    aMempool.remove(fwdTx2, removedTxs, removedCerts, /*fRecursive*/true);

    EXPECT_FALSE(std::count(removedTxs.begin(), removedTxs.end(), fwdTx1));
    EXPECT_TRUE(std::count(removedTxs.begin(), removedTxs.end(), fwdTx2));
    EXPECT_FALSE(std::count(removedTxs.begin(), removedTxs.end(), btrTx));
}

TEST_F(SidechainsInMempoolTestSuite, ScAndFwdsAndBtrInMempool_BtrRecursiveRemoval) {
    // Associated scenario: upon block disconnections a btr cannot make to mempool.
    // Recursive removal for refused btr is called, but other fwds are unaffected

    uint256 scId = uint256S("1492");

    CTransaction fwdTx1 = GenerateFwdTransferTx(scId, CAmount(10));
    CTxMemPoolEntry fwdEntry1(fwdTx1, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    aMempool.addUnchecked(fwdTx1.GetHash(), fwdEntry1);

    CTransaction fwdTx2 = GenerateFwdTransferTx(scId, CAmount(20));
    CTxMemPoolEntry fwdEntry2(fwdTx2, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    aMempool.addUnchecked(fwdTx2.GetHash(), fwdEntry2);

    CTransaction btrTx = GenerateBtrTx(scId);
    CTxMemPoolEntry btrEntry(btrTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    std::map<uint256, CFieldElement> dummyCertDataHashInfo;
    dummyCertDataHashInfo[scId] = CFieldElement{};
    aMempool.addUnchecked(btrTx.GetHash(), btrEntry, /*fCurrentEstimate*/true, dummyCertDataHashInfo);

    std::list<CTransaction> removedTxs;
    std::list<CScCertificate> removedCerts;
    aMempool.remove(btrTx, removedTxs, removedCerts, /*fRecursive*/true);

    EXPECT_FALSE(std::count(removedTxs.begin(), removedTxs.end(), fwdTx1));
    EXPECT_FALSE(std::count(removedTxs.begin(), removedTxs.end(), fwdTx2));
    EXPECT_TRUE(std::count(removedTxs.begin(), removedTxs.end(), btrTx));
}

TEST_F(SidechainsInMempoolTestSuite, SimpleCertRemovalFromMempool) {
    //Create and persist sidechain
    CTransaction scTx = GenerateScTx(CAmount(10));
    const uint256& scId = scTx.GetScIdFromScCcOut(0);
    CBlock aBlock;
    CCoinsViewCache sidechainsView(pcoinsTip);
    sidechainsView.UpdateSidechain(scTx, aBlock, /*height*/int(1789));
    sidechainsView.Flush();

    //load certificate in mempool
    CScCertificate cert = txCreationUtils::createCertificate(scId, /*epochNum*/0, /*endEpochBlockHash*/ uint256(),
        /*changeTotalAmount*/CAmount(4),/*numChangeOut*/2, /*bwtAmount*/CAmount(6), /*numBwt*/2);
    CCertificateMemPoolEntry certEntry(cert, /*fee*/CAmount(5), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    mempool.addUnchecked(cert.GetHash(), certEntry);

    //Remove the certificate
    std::list<CTransaction> removedTxs;
    std::list<CScCertificate> removedCerts;
    mempool.remove(cert, removedTxs, removedCerts, /*fRecursive*/false);

    EXPECT_TRUE(removedTxs.size() == 0);
    EXPECT_TRUE(std::count(removedCerts.begin(), removedCerts.end(), cert));
    EXPECT_FALSE(mempool.existsCert(cert.GetHash()));
}

TEST_F(SidechainsInMempoolTestSuite, ConflictingCertRemovalFromMempool) {
    //Create and persist sidechain
    CTransaction scTx = GenerateScTx(CAmount(10));
    const uint256& scId = scTx.GetScIdFromScCcOut(0);
    CBlock aBlock;
    CCoinsViewCache sidechainsView(pcoinsTip);
    sidechainsView.UpdateSidechain(scTx, aBlock, /*height*/int(1789));
    sidechainsView.Flush();

    //load a certificate in mempool
    CScCertificate cert1 = txCreationUtils::createCertificate(scId, /*epochNum*/0, /*endEpochBlockHash*/ uint256(),
        /*changeTotalAmount*/CAmount(4),/*numChangeOut*/2, /*bwtAmount*/CAmount(6), /*numBwt*/2);
    CCertificateMemPoolEntry certEntry1(cert1, /*fee*/CAmount(5), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    mempool.addUnchecked(cert1.GetHash(), certEntry1);

    //Remove the certificate
    std::list<CTransaction> removedTxs;
    std::list<CScCertificate> removedCerts;
    CScCertificate cert2 = txCreationUtils::createCertificate(scId, /*epochNum*/0, /*endEpochBlockHash*/ uint256(),
        /*changeTotalAmount*/CAmount(4),/*numChangeOut*/2, /*bwtAmount*/CAmount(0), /*numBwt*/2);
    mempool.removeConflicts(cert2, removedTxs, removedCerts);

    EXPECT_TRUE(removedTxs.size() == 0);
    EXPECT_TRUE(std::count(removedCerts.begin(), removedCerts.end(), cert1));
    EXPECT_FALSE(mempool.existsCert(cert1.GetHash()));
}

TEST_F(SidechainsInMempoolTestSuite, FwdsAndCertInMempool_CertRemovalDoesNotAffectFwt) {
    //Create and persist sidechain
    CTransaction scTx = GenerateScTx(CAmount(10));
    const uint256& scId = scTx.GetScIdFromScCcOut(0);
    CBlock aBlock;
    CCoinsViewCache sidechainsView(pcoinsTip);
    sidechainsView.UpdateSidechain(scTx, aBlock, /*height*/int(1789));
    sidechainsView.Flush();

    //load a fwt in mempool
    CTransaction fwdTx = GenerateFwdTransferTx(scId, CAmount(20));
    CTxMemPoolEntry fwdEntry(fwdTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    mempool.addUnchecked(fwdTx.GetHash(), fwdEntry);

    //load a certificate in mempool
    CScCertificate cert = txCreationUtils::createCertificate(scId, /*epochNum*/0, /*endEpochBlockHash*/ uint256(),
        /*changeTotalAmount*/CAmount(4),/*numChangeOut*/2, /*bwtAmount*/CAmount(2), /*numBwt*/2);
    CCertificateMemPoolEntry certEntry1(cert, /*fee*/CAmount(5), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    mempool.addUnchecked(cert.GetHash(), certEntry1);

    //Remove the certificate
    std::list<CTransaction> removedTxs;
    std::list<CScCertificate> removedCerts;
    mempool.remove(cert, removedTxs, removedCerts, /*fRecursive*/false);

    EXPECT_TRUE(std::count(removedCerts.begin(), removedCerts.end(), cert));
    EXPECT_FALSE(mempool.existsCert(cert.GetHash()));
    EXPECT_FALSE(std::count(removedTxs.begin(), removedTxs.end(), fwdTx));
    EXPECT_TRUE(mempool.existsTx(fwdTx.GetHash()));
    ASSERT_TRUE(mempool.mapSidechains.count(scId));
    EXPECT_TRUE(mempool.mapSidechains.at(scId).fwdTxHashes.count(fwdTx.GetHash()));
    EXPECT_TRUE(mempool.mapSidechains.at(scId).mBackwardCertificates.empty());
}

TEST_F(SidechainsInMempoolTestSuite, FwdsAndCertInMempool_FwtRemovalDoesNotAffectCert) {
    //Create and persist sidechain
    CTransaction scTx = GenerateScTx(CAmount(10));
    const uint256& scId = scTx.GetScIdFromScCcOut(0);
    CBlock aBlock;
    CCoinsViewCache sidechainsView(pcoinsTip);
    sidechainsView.UpdateSidechain(scTx, aBlock, /*height*/int(1789));
    sidechainsView.Flush();

    //load a fwd in mempool
    CTransaction fwdTx = GenerateFwdTransferTx(scId, CAmount(20));
    CTxMemPoolEntry fwdEntry(fwdTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    mempool.addUnchecked(fwdTx.GetHash(), fwdEntry);

    //load a certificate in mempool
    CScCertificate cert = txCreationUtils::createCertificate(scId, /*epochNum*/0, /*endEpochBlockHash*/ uint256(),
        /*changeTotalAmount*/CAmount(4),/*numChangeOut*/2, /*bwtAmount*/CAmount(2), /*numBwt*/2);
    CCertificateMemPoolEntry certEntry1(cert, /*fee*/CAmount(5), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    mempool.addUnchecked(cert.GetHash(), certEntry1);

    //Remove the certificate
    std::list<CTransaction> removedTxs;
    std::list<CScCertificate> removedCerts;
    mempool.remove(fwdTx, removedTxs, removedCerts, /*fRecursive*/false);

    EXPECT_TRUE(std::count(removedTxs.begin(), removedTxs.end(), fwdTx));
    EXPECT_FALSE(mempool.existsTx(fwdTx.GetHash()));
    EXPECT_FALSE(std::count(removedCerts.begin(), removedCerts.end(), cert));
    EXPECT_TRUE(mempool.existsCert(cert.GetHash()));
    ASSERT_TRUE(mempool.mapSidechains.count(scId));
    EXPECT_FALSE(mempool.mapSidechains.at(scId).fwdTxHashes.count(fwdTx.GetHash()));
    EXPECT_TRUE(mempool.mapSidechains.at(scId).HasCert(cert.GetHash()));
}

TEST_F(SidechainsInMempoolTestSuite, CertCannotSpendSameQualityCertOutput)
{
    CNakedCCoinsViewCache sidechainsView(pcoinsTip);

    // setup sidechain initial state
    CSidechain initialScState;
    uint256 scId = uint256S("aaaa");
    txCreationUtils::storeSidechain(sidechainsView.getSidechainMap(), scId, initialScState);

    int64_t certQuality = 10;
    uint256 dummyBlockHash {};
    CAmount dummyInputAmount{20};
    CAmount dummyNonZeroFee {10};
    CAmount dummyNonZeroChange = dummyInputAmount - dummyNonZeroFee;
    CAmount dummyBwtAmount {0};

    CScCertificate parentCert = GenerateCertificate(scId, /*epochNum*/0, dummyBlockHash, /*inputAmount*/dummyInputAmount,
        /*changeTotalAmount*/dummyNonZeroChange,/*numChangeOut*/1, /*bwtAmount*/dummyBwtAmount, /*numBwt*/2, /*quality*/certQuality);

    CCertificateMemPoolEntry mempoolParentCertCertEntry(parentCert, /*fee*/dummyNonZeroFee, /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    mempool.addUnchecked(parentCert.GetHash(), mempoolParentCertCertEntry);
    ASSERT_TRUE(mempool.exists(parentCert.GetHash()));

    CScCertificate sameQualityChildCert = GenerateCertificate(scId, /*epochNum*/0, dummyBlockHash, /*inputAmount*/dummyInputAmount,
        /*changeTotalAmount*/dummyNonZeroChange,/*numChangeOut*/1, /*bwtAmount*/dummyBwtAmount, /*numBwt*/2, /*quality*/certQuality,
        &parentCert);
    ASSERT_TRUE(sameQualityChildCert.GetHash() != parentCert.GetHash());

    //test
    EXPECT_FALSE(mempool.checkIncomingCertConflicts(sameQualityChildCert));
}

TEST_F(SidechainsInMempoolTestSuite, CertCannotSpendHigherQualityCertOutput)
{
    CNakedCCoinsViewCache sidechainsView(pcoinsTip);

    // setup sidechain initial state
    CSidechain initialScState;
    uint256 scId = uint256S("aaaa");
    txCreationUtils::storeSidechain(sidechainsView.getSidechainMap(), scId, initialScState);

    int64_t topQuality = 10;
    uint256 dummyBlockHash {};
    CAmount dummyInputAmount{20};
    CAmount dummyNonZeroFee {10};
    CAmount dummyNonZeroChange = dummyInputAmount - dummyNonZeroFee;
    CAmount dummyBwtAmount {0};

    CScCertificate parentCert = GenerateCertificate(scId, /*epochNum*/0, dummyBlockHash, /*inputAmount*/dummyInputAmount,
        /*changeTotalAmount*/dummyNonZeroChange,/*numChangeOut*/1, /*bwtAmount*/dummyBwtAmount, /*numBwt*/2, /*quality*/topQuality);

    CCertificateMemPoolEntry mempoolParentCertCertEntry(parentCert, /*fee*/dummyNonZeroFee, /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    mempool.addUnchecked(parentCert.GetHash(), mempoolParentCertCertEntry);
    ASSERT_TRUE(mempool.exists(parentCert.GetHash()));

    CScCertificate lowerQualityChildCert = GenerateCertificate(scId, /*epochNum*/0, dummyBlockHash, /*inputAmount*/dummyInputAmount,
        /*changeTotalAmount*/dummyNonZeroChange,/*numChangeOut*/1, /*bwtAmount*/dummyBwtAmount, /*numBwt*/2, /*quality*/topQuality/2,
        &parentCert);

    //test
    EXPECT_FALSE(mempool.checkIncomingCertConflicts(lowerQualityChildCert));
}

#if 0
TEST_F(SidechainsInMempoolTestSuite, CertInMempool_QualityOfCerts) {

    //Create and persist sidechain
    CTransaction scTx = GenerateScTx(CAmount(10000), /*epochLenght*/5);
    const uint256& scId = scTx.GetScIdFromScCcOut(0);
    CBlock aBlock;
    CCoinsViewCache sidechainsView(pcoinsTip);
    sidechainsView.UpdateSidechain(scTx, aBlock, /*height*/int(401));
    sidechainsView.Flush();

    CBlockUndo dummyBlockUndo;
    for(const CTxScCreationOut& scCreationOut: scTx.GetVscCcOut())
        ASSERT_TRUE(sidechainsView.ScheduleSidechainEvent(scCreationOut, 401));

    std::vector<CScCertificateStatusUpdateInfo> dummy;
    ASSERT_TRUE(sidechainsView.HandleSidechainEvents(401 + Params().ScCoinsMaturity(), dummyBlockUndo, &dummy));
    sidechainsView.Flush();

    chainSettingUtils::ExtendChainActiveToHeight(/*startHeight*/406);

    const uint256& endEpochBlockHash = ArithToUint256(405);
    CValidationState state;
    bool missingInputs = false;

    //load a certificate in mempool (q=3, fee=600)
    CScCertificate cert1 = GenerateCertificate(scId, /*epochNum*/0, endEpochBlockHash, /*inputAmount*/CAmount(1000),
        /*changeTotalAmount*/CAmount(400),/*numChangeOut*/1, /*bwtAmount*/CAmount(2000), /*numBwt*/2, /*quality*/3);

    EXPECT_TRUE(AcceptCertificateToMemoryPool(mempool, state, cert1,
                LimitFreeFlag::OFF, &missingInputs, RejectAbsurdFeeFlag::OFF, DisconnectingFlag::OFF));

    //load a certificate in mempool (q=2, fee=150)
    CScCertificate cert2 = GenerateCertificate(scId, /*epochNum*/0, endEpochBlockHash, /*inputAmount*/CAmount(300),
        /*changeTotalAmount*/CAmount(150),/*numChangeOut*/1, /*bwtAmount*/CAmount(30), /*numBwt*/2, /*quality*/2);

    EXPECT_TRUE(AcceptCertificateToMemoryPool(mempool, state, cert2,
                LimitFreeFlag::OFF, &missingInputs, RejectAbsurdFeeFlag::OFF, DisconnectingFlag::OFF));

    //load a certificate in mempool (q=2, fee=150) ---> dropped because this fee is the same
    CScCertificate cert3 = GenerateCertificate(scId, /*epochNum*/0, endEpochBlockHash, /*inputAmount*/CAmount(400),
        /*changeTotalAmount*/CAmount(250),/*numChangeOut*/1, /*bwtAmount*/CAmount(40), /*numBwt*/2, /*quality*/2);

    EXPECT_FALSE(AcceptCertificateToMemoryPool(mempool, state, cert3,
                 LimitFreeFlag::OFF, &missingInputs, RejectAbsurdFeeFlag::OFF, DisconnectingFlag::OFF));

    //load a certificate in mempool (q=2, fee=100) ---> dropped because this fee is lower
    CScCertificate cert3b = GenerateCertificate(scId, /*epochNum*/0, endEpochBlockHash, /*inputAmount*/CAmount(390),
        /*changeTotalAmount*/CAmount(290),/*numChangeOut*/1, /*bwtAmount*/CAmount(40), /*numBwt*/2, /*quality*/2);

    EXPECT_FALSE(AcceptCertificateToMemoryPool(mempool, state, cert3b,
                 LimitFreeFlag::OFF, &missingInputs, RejectAbsurdFeeFlag::OFF, DisconnectingFlag::OFF));

    //load a certificate in mempool (q=4, fee=100)
    CScCertificate cert4 = GenerateCertificate(scId, /*epochNum*/0, endEpochBlockHash, /*inputAmount*/CAmount(1500),
        /*changeTotalAmount*/CAmount(1400),/*numChangeOut*/2, /*bwtAmount*/CAmount(60), /*numBwt*/2, /*quality*/4);

    EXPECT_TRUE(AcceptCertificateToMemoryPool(mempool, state, cert4,
                LimitFreeFlag::OFF, &missingInputs, RejectAbsurdFeeFlag::OFF, DisconnectingFlag::OFF));

    EXPECT_TRUE(mempool.mapSidechains.at(scId).HasCert(cert1.GetHash()));
    EXPECT_TRUE(mempool.mapSidechains.at(scId).HasCert(cert2.GetHash()));
    EXPECT_FALSE(mempool.mapSidechains.at(scId).HasCert(cert3.GetHash()));
    EXPECT_TRUE(mempool.mapSidechains.at(scId).HasCert(cert4.GetHash()));

    EXPECT_TRUE(mempool.mapSidechains.at(scId).GetTopQualityCert()->second == cert4.GetHash());
    
    // erase a cert from mempool
    std::list<CTransaction> removedTxs;
    std::list<CScCertificate> removedCerts;
    mempool.remove(cert4, removedTxs, removedCerts, /*fRecursive*/false);

    EXPECT_FALSE(mempool.mapSidechains.at(scId).HasCert(cert4.GetHash()));
    EXPECT_TRUE(mempool.mapSidechains.at(scId).GetTopQualityCert()->second == cert1.GetHash());

    int64_t tq = mempool.mapSidechains.at(scId).GetTopQualityCert()->first;
    
    //create a certificate (not loading it in mempool) with quality same as top-quality and remove any conflict in mempool with that 
    // verify that former top-quality has been removed
    CScCertificate cert5 = GenerateCertificate(scId, /*epochNum*/0, endEpochBlockHash, /*inputAmount*/CAmount(0),
        /*changeTotalAmount*/CAmount(0),/*numChangeOut*/0, /*bwtAmount*/CAmount(90), /*numBwt*/2, /*quality*/tq);

    EXPECT_TRUE(mempool.RemoveCertAndSync(mempool.FindCertWithQuality(scId, cert5.quality).first));
    
    EXPECT_FALSE(mempool.mapSidechains.at(scId).HasCert(cert1.GetHash()));
    EXPECT_TRUE(mempool.mapSidechains.at(scId).GetTopQualityCert()->second == cert2.GetHash());
    tq = mempool.mapSidechains.at(scId).GetTopQualityCert()->first;

    //load a certificate in mempool (q=top=2, fee=200) --> former is removed since this fee is higher
    CScCertificate cert6 = GenerateCertificate(scId, /*epochNum*/0, endEpochBlockHash, /*inputAmount*/CAmount(600),
        /*changeTotalAmount*/CAmount(400),/*numChangeOut*/1, /*bwtAmount*/CAmount(30), /*numBwt*/2, /*quality*/tq);

    EXPECT_TRUE(AcceptCertificateToMemoryPool(mempool, state, cert6,
                LimitFreeFlag::OFF, &missingInputs, RejectAbsurdFeeFlag::OFF, DisconnectingFlag::OFF));

    EXPECT_FALSE(mempool.mapSidechains.at(scId).HasCert(cert2.GetHash()));
    EXPECT_TRUE(mempool.mapSidechains.at(scId).GetTopQualityCert()->second == cert6.GetHash());
}
#endif

TEST_F(SidechainsInMempoolTestSuite, DuplicatedCSWsToCeasedSidechainAreRejected) {
    uint256 scId = uint256S("aaa");
    CAmount cswTxCoins = 10;
    CTxCeasedSidechainWithdrawalInput cswInput = GenerateCSWInput(scId, "aabb", cswTxCoins);
    CTransaction cswTx = GenerateCSWTx(cswInput);

    CTxMemPoolEntry cswEntry(cswTx, /*fee*/CAmount(5), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    EXPECT_TRUE(mempool.addUnchecked(cswTx.GetHash(), cswEntry));

    CMutableTransaction duplicatedCswTx = cswTx;
    duplicatedCswTx.addOut(CTxOut(CAmount(5), CScript()));
    ASSERT_TRUE(cswTx.GetHash() != duplicatedCswTx.GetHash());

    CValidationState dummyState;
    libzendoomc::CScProofVerifier verifier = libzendoomc::CScProofVerifier::Disabled();
    EXPECT_FALSE(mempool.checkIncomingTxConflicts(duplicatedCswTx));
}

TEST_F(SidechainsInMempoolTestSuite, UnconfirmedFwtTxToCeasedSidechainsAreRemovedFromMempool) {
    CNakedCCoinsViewCache sidechainsView(pcoinsTip);

    // setup sidechain initial state
    CSidechain initialScState;
    uint256 scId = uint256S("aaaa");
    initialScState.creationBlockHeight = 1492;
    initialScState.creationData.withdrawalEpochLength = 14;
    initialScState.balance = CAmount{1000};
    int heightWhereAlive = initialScState.GetScheduledCeasingHeight() -1;

    storeSidechainWithCurrentHeight(sidechainsView, scId, initialScState, heightWhereAlive);
    ASSERT_TRUE(sidechainsView.GetSidechainState(scId) == CSidechain::State::ALIVE);

    CTransaction fwtTx = GenerateFwdTransferTx(scId, CAmount(10));
    CTxMemPoolEntry fwtEntry(fwtTx, /*fee*/CAmount(5), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    EXPECT_TRUE(mempool.addUnchecked(fwtTx.GetHash(), fwtEntry));

    // Sidechain State is Active. No removed Txs and Certs expected.
    std::list<CTransaction> removedTxs;
    std::list<CScCertificate> removedCerts;
    int dummyHeight{1815};
    mempool.removeStaleTransactions(&sidechainsView, removedTxs, removedCerts);
    EXPECT_TRUE(removedTxs.size() == 0);
    EXPECT_TRUE(removedCerts.size() == 0);

    // Cease sidechains
    chainSettingUtils::ExtendChainActiveToHeight(initialScState.GetScheduledCeasingHeight());
    sidechainsView.SetBestBlock(chainActive.Tip()->GetBlockHash());
    ASSERT_TRUE(sidechainsView.GetSidechainState(scId) == CSidechain::State::CEASED);

    // Sidechain State is Ceased. FT expected to be removed.
    removedTxs.clear();
    removedCerts.clear();
    mempool.removeStaleTransactions(&sidechainsView, removedTxs, removedCerts);
    EXPECT_TRUE(removedTxs.size() == 1);
    EXPECT_TRUE(std::find(removedTxs.begin(), removedTxs.end(), fwtTx) != removedTxs.end());
    EXPECT_TRUE(removedCerts.size() == 0);
}

TEST_F(SidechainsInMempoolTestSuite, UnconfirmedCsw_LargerThanSidechainBalanceAreRemovedFromMempool) {
    // This can happen upon faulty/malicious circuits

    CNakedCCoinsViewCache sidechainsView(pcoinsTip);

    // setup sidechain initial state
    CSidechain initialScState;
    uint256 scId = uint256S("aaaa");
    initialScState.creationBlockHeight = 1492;
    initialScState.creationData.withdrawalEpochLength = 14;
    initialScState.balance = CAmount{1000};
    int heightWhereCeased = initialScState.GetScheduledCeasingHeight();

    storeSidechainWithCurrentHeight(sidechainsView, scId, initialScState, heightWhereCeased);
    ASSERT_TRUE(sidechainsView.GetSidechainState(scId) == CSidechain::State::CEASED);

    // Create and add CSW Tx
    CAmount cswTxCoins = initialScState.balance; // csw coins = total sc mature coins
    CTxCeasedSidechainWithdrawalInput cswInput = GenerateCSWInput(scId, "aabb", cswTxCoins);
    CTransaction cswTx = GenerateCSWTx(cswInput);

    CTxMemPoolEntry cswEntry(cswTx, /*fee*/CAmount(5), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    EXPECT_TRUE(mempool.addUnchecked(cswTx.GetHash(), cswEntry));

    // Sidechain State is Ceased and there is no Sidechain balance conflicts in the mempool. No removed Txs and Certs expected.
    std::list<CTransaction> removedTxs;
    std::list<CScCertificate> removedCerts;
    mempool.removeOutOfScBalanceCsw(&sidechainsView, removedTxs, removedCerts);
    EXPECT_TRUE(removedTxs.size() == 0);
    EXPECT_TRUE(removedCerts.size() == 0);

    // Add without checks another CSW tx to the same sidechain
    CAmount cswTxCoins2 = 1;
    CTxCeasedSidechainWithdrawalInput cswInput2 = GenerateCSWInput(scId, "ddcc", cswTxCoins2);
    CTransaction cswTx2 = GenerateCSWTx(cswInput2);
    CTxMemPoolEntry cswEntry2(cswTx2, /*fee*/CAmount(5), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    EXPECT_TRUE(mempool.addUnchecked(cswTx2.GetHash(), cswEntry2));

    // Mempool CSW Txs total withdrawal amount is greater than Sidechain mature balance -> both Txs expected to be removed.
    removedTxs.clear();
    removedCerts.clear();
    mempool.removeOutOfScBalanceCsw(&sidechainsView, removedTxs, removedCerts);
    EXPECT_TRUE(removedTxs.size() == 2);
    EXPECT_TRUE(std::find(removedTxs.begin(), removedTxs.end(), cswTx) != removedTxs.end());
    EXPECT_TRUE(std::find(removedTxs.begin(), removedTxs.end(), cswTx2) != removedTxs.end());
    EXPECT_TRUE(removedCerts.size() == 0);
}

TEST_F(SidechainsInMempoolTestSuite, UnconfirmedCswForAliveSidechainsAreRemovedFromMempool) {
    //This can happen upon reverting end-of-epoch block

    CNakedCCoinsViewCache sidechainsView(pcoinsTip);

    // setup sidechain initial state
    CSidechain initialScState;
    uint256 scId = uint256S("aaaa");
    initialScState.creationBlockHeight = 1492;
    initialScState.creationData.withdrawalEpochLength = 14;
    initialScState.balance = CAmount{1000};
    int heightWhereCeased = initialScState.GetScheduledCeasingHeight();

    storeSidechainWithCurrentHeight(sidechainsView, scId, initialScState, heightWhereCeased);
    ASSERT_TRUE(sidechainsView.GetSidechainState(scId) == CSidechain::State::CEASED);

    // Create and add CSW Tx
    CAmount cswTxCoins = initialScState.balance; // csw coins = total sc mature coins
    CTxCeasedSidechainWithdrawalInput cswInput = GenerateCSWInput(scId, "aabb", cswTxCoins);
    CTransaction cswTx = GenerateCSWTx(cswInput);

    CTxMemPoolEntry cswEntry(cswTx, /*fee*/CAmount(5), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    EXPECT_TRUE(mempool.addUnchecked(cswTx.GetHash(), cswEntry));

    std::list<CTransaction> removedTxs;
    std::list<CScCertificate> removedCerts;
    int dummyHeight{1789};
    // Sidechain State is Ceased and there is no Sidechain balance conflicts in the mempool. No removed Txs and Certs expected.
    mempool.removeStaleTransactions(&sidechainsView, removedTxs, removedCerts);
    mempool.removeOutOfScBalanceCsw(&sidechainsView, removedTxs, removedCerts);
    EXPECT_TRUE(removedTxs.size() == 0);
    EXPECT_TRUE(removedCerts.size() == 0);

    // revert sidechain state to ACTIVE
    chainSettingUtils::ExtendChainActiveToHeight(initialScState.GetScheduledCeasingHeight()-1);
    sidechainsView.SetBestBlock(chainActive.Tip()->GetBlockHash());
    ASSERT_TRUE(sidechainsView.GetSidechainState(scId) == CSidechain::State::ALIVE);

    // Mempool CSW Txs total withdrawal amount is greater than Sidechain mature balance -> both Txs expected to be removed.
    removedTxs.clear();
    removedCerts.clear();
    mempool.removeStaleTransactions(&sidechainsView, removedTxs, removedCerts);
    EXPECT_TRUE(removedTxs.size() == 1);
    EXPECT_TRUE(std::find(removedTxs.begin(), removedTxs.end(), cswTx) != removedTxs.end());
    EXPECT_TRUE(removedCerts.size() == 0);
}

TEST_F(SidechainsInMempoolTestSuite, SimpleCswRemovalFromMempool) {
    //Create and persist sidechain
    CTransaction scTx = GenerateScTx(CAmount(10));
    const uint256& scId = scTx.GetScIdFromScCcOut(0);
    CBlock aBlock;
    CCoinsViewCache sidechainsView(pcoinsTip);
    sidechainsView.UpdateSidechain(scTx, aBlock, /*height*/int(1789));
    sidechainsView.Flush();

    //load csw tx to mempool
    CAmount dummyAmount(1);
    uint160 dummyPubKeyHash;
    libzendoomc::ScProof dummyScProof;
    CScript dummyRedeemScript;

    CMutableTransaction mutTx;
    CFieldElement nullfier_1{std::vector<unsigned char>(size_t(CFieldElement::ByteSize()), 'a')};
    CFieldElement nullfier_2{std::vector<unsigned char>(size_t(CFieldElement::ByteSize()), 'b')};
    mutTx.vcsw_ccin.push_back(CTxCeasedSidechainWithdrawalInput(dummyAmount, scId, nullfier_1, dummyPubKeyHash, dummyScProof, dummyRedeemScript));
    mutTx.vcsw_ccin.push_back(CTxCeasedSidechainWithdrawalInput(dummyAmount, scId, nullfier_2, dummyPubKeyHash, dummyScProof, dummyRedeemScript));

    CTransaction cswTx(mutTx);
    CTxMemPoolEntry cswEntry(cswTx, /*fee*/CAmount(5), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    mempool.addUnchecked(cswTx.GetHash(), cswEntry);

    //Remove the csw tx
    std::list<CTransaction> removedTxs;
    std::list<CScCertificate> removedCerts;
    mempool.remove(cswTx, removedTxs, removedCerts, /*fRecursive*/false);

    //checks
    EXPECT_TRUE(removedCerts.size() == 0);
    EXPECT_TRUE(std::count(removedTxs.begin(), removedTxs.end(), cswTx));
    EXPECT_FALSE(mempool.existsTx(cswTx.GetHash()));
}

TEST_F(SidechainsInMempoolTestSuite, CSWsToCeasedSidechainWithoutVK) {
    // Create and persist sidechain
    int creationHeight = 1789;
    int epochLength = 10;
    CAmount scCoins = 1000;
    chainSettingUtils::ExtendChainActiveToHeight(creationHeight);
    // NOTE: no Ceased VK in SC creation output
    CTransaction scTx = GenerateScTx(scCoins, epochLength, /*ceasedVkDefined*/false);
    const uint256& scId = scTx.GetScIdFromScCcOut(0);
    CBlock aBlock;
    CCoinsViewCache sidechainsView(pcoinsTip);
    sidechainsView.UpdateSidechain(scTx, aBlock, creationHeight);
    sidechainsView.Flush();
//    for(const CTxScCreationOut& scCreationOut: scTx.GetVscCcOut())
//        ASSERT_TRUE(sidechainsView.ScheduleSidechainEvent(scCreationOut, creationHeight));
//    sidechainsView.Flush();

    // Make coins mature
    CBlockUndo dummyBlockUndo;
    std::vector<CScCertificateStatusUpdateInfo> dummy;
    int coinsMatureHeight = creationHeight + Params().ScCoinsMaturity();
    ASSERT_TRUE(sidechainsView.HandleSidechainEvents(coinsMatureHeight, dummyBlockUndo, &dummy));


    // Cease sidechain
    int safeguardMargin = epochLength/5;
    int ceasingHeight = creationHeight + epochLength + safeguardMargin;
    ASSERT_TRUE(sidechainsView.HandleSidechainEvents(ceasingHeight, dummyBlockUndo, &dummy));
    sidechainsView.Flush();

    chainSettingUtils::ExtendChainActiveToHeight(ceasingHeight);

    // Create and add CSW Tx
    CAmount cswTxCoins = scCoins / 4;
    assert(cswTxCoins > 0);
    CTxCeasedSidechainWithdrawalInput cswInput = GenerateCSWInput(scId,"aabb", cswTxCoins);
    CTransaction cswTx = GenerateCSWTx(cswInput);

    CValidationState cswTxState;
    bool missingInputs = false;

    EXPECT_FALSE(AcceptTxToMemoryPool(mempool, cswTxState, cswTx, LimitFreeFlag::OFF, &missingInputs, RejectAbsurdFeeFlag::OFF));
}

TEST_F(SidechainsInMempoolTestSuite, ConflictingCswRemovalFromMempool) {
    //Create and persist sidechain
    CTransaction scTx = GenerateScTx(CAmount(10));
    const uint256& scId = scTx.GetScIdFromScCcOut(0);
    CBlock aBlock;
    CCoinsViewCache sidechainsView(pcoinsTip);
    sidechainsView.UpdateSidechain(scTx, aBlock, /*height*/int(1789));
    sidechainsView.Flush();

    //load csw tx to mempool
    CAmount dummyAmount(1);
    uint160 dummyPubKeyHash;
    libzendoomc::ScProof dummyScProof;
    CScript dummyRedeemScript;

    CMutableTransaction mutTx;
    mutTx.nVersion = SC_TX_VERSION;
    CFieldElement nullfier_1{std::vector<unsigned char>(size_t(CFieldElement::ByteSize()), 'a')};
    CFieldElement nullfier_2{std::vector<unsigned char>(size_t(CFieldElement::ByteSize()), 'b')};
    mutTx.vcsw_ccin.push_back(CTxCeasedSidechainWithdrawalInput(dummyAmount, scId, nullfier_1, dummyPubKeyHash, dummyScProof, dummyRedeemScript));
    mutTx.vcsw_ccin.push_back(CTxCeasedSidechainWithdrawalInput(dummyAmount, scId, nullfier_2, dummyPubKeyHash, dummyScProof, dummyRedeemScript));

    CTransaction cswTx(mutTx);
    CTxMemPoolEntry cswEntry(cswTx, /*fee*/CAmount(5), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    mempool.addUnchecked(cswTx.GetHash(), cswEntry);

    //Remove the csw tx due to nullifier conflict with cswConfirmedTx
    std::list<CTransaction> removedTxs;
    std::list<CScCertificate> removedCerts;

    CMutableTransaction mutConfirmedTx;
    mutConfirmedTx.vcsw_ccin.push_back(CTxCeasedSidechainWithdrawalInput(dummyAmount, scId, nullfier_1, dummyPubKeyHash, dummyScProof, dummyRedeemScript));
    CTransaction cswConfirmedTx(mutConfirmedTx);
    ASSERT_TRUE(cswTx.GetHash() != cswConfirmedTx.GetHash());
    mempool.removeConflicts(cswConfirmedTx, removedTxs, removedCerts);

    //checks
    EXPECT_TRUE(removedCerts.size() == 0);
    EXPECT_TRUE(std::count(removedTxs.begin(), removedTxs.end(), cswTx));
    EXPECT_FALSE(mempool.existsTx(cswTx.GetHash()));
}


TEST_F(SidechainsInMempoolTestSuite, UnconfirmedTxSpendingImmatureCoinbaseIsDropped) {
    //This may happen in block disconnection, for instance

    //Create a coinbase
    CMutableTransaction mutCoinBase;
    mutCoinBase.vin.push_back(CTxIn(uint256(), -1));
    mutCoinBase.addOut(CTxOut(10,CScript()));
    mutCoinBase.addOut(CTxOut(20,CScript()));
    CTransaction coinBase(mutCoinBase);
    CTxUndo dummyUndo;
    UpdateCoins(coinBase, *pcoinsTip, dummyUndo, chainActive.Height());

    EXPECT_FALSE(pcoinsTip->AccessCoins(coinBase.GetHash())->isOutputMature(0, chainActive.Height()));
    // mature the coinbase
    chainSettingUtils::ExtendChainActiveToHeight(chainActive.Height() + COINBASE_MATURITY);
    EXPECT_TRUE(pcoinsTip->AccessCoins(coinBase.GetHash())->isOutputMature(0, chainActive.Height()));

    //add to mempool txes spending coinbase
    CMutableTransaction mutTx;
    mutTx.vin.push_back(CTxIn(COutPoint(coinBase.GetHash(), 0), CScript(), -1));
    CTransaction mempoolTx1(mutTx);
    CTxMemPoolEntry mempoolEntry1(mempoolTx1, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    mempool.addUnchecked(mempoolTx1.GetHash(), mempoolEntry1);
    EXPECT_TRUE(mempool.exists(mempoolTx1.GetHash()));

    mutTx.vin.clear();
    mutTx.vin.push_back(CTxIn(COutPoint(coinBase.GetHash(), 1), CScript(), -1));
    CTransaction mempoolTx2(mutTx);
    CTxMemPoolEntry mempoolEntry2(mempoolTx2, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    mempool.addUnchecked(mempoolTx2.GetHash(), mempoolEntry2);
    EXPECT_TRUE(mempool.exists(mempoolTx2.GetHash()));

    //Revert chain undoing coinbase maturity, and check mempool cleanup
    chainSettingUtils::ExtendChainActiveToHeight(chainActive.Height() -1);
    // check coinbase is not mature anymore
    EXPECT_FALSE(pcoinsTip->AccessCoins(coinBase.GetHash())->isOutputMature(0, chainActive.Height()));

    //test
    std::list<CTransaction> outdatedTxs;
    std::list<CScCertificate> outdatedCerts;
    mempool.removeStaleTransactions(pcoinsTip, outdatedTxs, outdatedCerts);

    //Check
    EXPECT_FALSE(mempool.exists(mempoolTx1.GetHash()));
    EXPECT_TRUE(std::find(outdatedTxs.begin(), outdatedTxs.end(), mempoolTx1) != outdatedTxs.end());

    EXPECT_FALSE(mempool.exists(mempoolTx2.GetHash()));
    EXPECT_TRUE(std::find(outdatedTxs.begin(), outdatedTxs.end(), mempoolTx2) != outdatedTxs.end());
}

TEST_F(SidechainsInMempoolTestSuite, UnconfirmedFwdsTowardUnconfirmedSidechainsAreNotDropped)
{
    CNakedCCoinsViewCache sidechainsView(pcoinsTip);

    // setup sidechain initial state
    int scCreationHeight {200};
    uint256 inputScCreationTxHash = txCreationUtils::CreateSpendableCoinAtHeight(sidechainsView, scCreationHeight-COINBASE_MATURITY);

    CMutableTransaction mutScCreationTx = txCreationUtils::createNewSidechainTxWith(/*creationTxAmount*/CAmount(10), /*epochLength*/5);
    mutScCreationTx.vin.clear();
    mutScCreationTx.vin.push_back(CTxIn(inputScCreationTxHash, 0, CScript()));
    CTransaction scCreationTx(mutScCreationTx);
    uint256 scId = scCreationTx.GetScIdFromScCcOut(0);
    CTxMemPoolEntry scPoolEntry(scCreationTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    mempool.addUnchecked(scCreationTx.GetHash(), scPoolEntry);
    ASSERT_TRUE(mempool.hasSidechainCreationTx(scId));

    // create coinbase to finance fwt
    int fwtHeight {201};
    uint256 inputTxHash = txCreationUtils::CreateSpendableCoinAtHeight(sidechainsView, fwtHeight-COINBASE_MATURITY);

    //Add fwt to mempool
    CMutableTransaction mutFwdTx = txCreationUtils::createFwdTransferTxWith(scId, /*fwdTxAmount*/CAmount(10));
    mutFwdTx.vin.clear();
    mutFwdTx.vin.push_back(CTxIn(inputTxHash, 0, CScript()));
    CTransaction fwdTx(mutFwdTx);
    CTxMemPoolEntry mempoolEntry(fwdTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/fwtHeight);
    mempool.addUnchecked(fwdTx.GetHash(), mempoolEntry);

    //test
    std::list<CTransaction> outdatedTxs;
    std::list<CScCertificate> outdatedCerts;
    mempool.removeStaleTransactions(&sidechainsView, outdatedTxs, outdatedCerts);

    //checks
    EXPECT_TRUE(mempool.exists(fwdTx.GetHash()));
    EXPECT_FALSE(std::find(outdatedTxs.begin(), outdatedTxs.end(), fwdTx) != outdatedTxs.end());
}

TEST_F(SidechainsInMempoolTestSuite,UnconfirmedFwdsTowardAliveSidechainsAreNotDropped)
{
    CNakedCCoinsViewCache sidechainsView(pcoinsTip);

    // setup sidechain initial state
    CSidechain initialScState;
    uint256 scId = uint256S("aaaa");
    initialScState.creationBlockHeight = 1492;
    initialScState.creationData.withdrawalEpochLength = 14;
    int heightWhereAlive = initialScState.GetScheduledCeasingHeight() -1;

    storeSidechainWithCurrentHeight(sidechainsView, scId, initialScState, heightWhereAlive);
    ASSERT_TRUE(sidechainsView.GetSidechainState(scId) == CSidechain::State::ALIVE);

    // create coinbase to finance fwt
    int fwtHeight = heightWhereAlive;
    uint256 inputTxHash = txCreationUtils::CreateSpendableCoinAtHeight(sidechainsView, fwtHeight-COINBASE_MATURITY);

    //Add fwt to mempool
    CMutableTransaction mutFwdTx = txCreationUtils::createFwdTransferTxWith(scId, /*fwdTxAmount*/CAmount(10));
    mutFwdTx.vin.clear();
    mutFwdTx.vin.push_back(CTxIn(inputTxHash, 0, CScript()));
    CTransaction fwdTx(mutFwdTx);
    CTxMemPoolEntry mempoolEntry(fwdTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/fwtHeight);
    mempool.addUnchecked(fwdTx.GetHash(), mempoolEntry);

    //test
    std::list<CTransaction> outdatedTxs;
    std::list<CScCertificate> outdatedCerts;
    mempool.removeStaleTransactions(&sidechainsView, outdatedTxs, outdatedCerts);

    //checks
    EXPECT_TRUE(mempool.exists(fwdTx.GetHash()));
    EXPECT_FALSE(std::find(outdatedTxs.begin(), outdatedTxs.end(), fwdTx) != outdatedTxs.end());
}

TEST_F(SidechainsInMempoolTestSuite,UnconfirmedFwdsTowardCeasedSidechainsAreDropped)
{
    CNakedCCoinsViewCache sidechainsView(pcoinsTip);

    // setup sidechain initial state
    CSidechain initialScState;
    uint256 scId = uint256S("aaaa");
    initialScState.creationBlockHeight = 1492;
    initialScState.creationData.withdrawalEpochLength = 14;
    int heightWhereCeased = initialScState.GetScheduledCeasingHeight();

    storeSidechainWithCurrentHeight(sidechainsView, scId, initialScState, heightWhereCeased);
    ASSERT_TRUE(sidechainsView.GetSidechainState(scId) == CSidechain::State::CEASED);

    // create coinbase to finance fwt
    int fwtHeight = heightWhereCeased + 2;
    uint256 inputTxHash = txCreationUtils::CreateSpendableCoinAtHeight(sidechainsView, fwtHeight-COINBASE_MATURITY);

    //Add fwt to mempool
    CMutableTransaction mutFwdTx = txCreationUtils::createFwdTransferTxWith(scId, /*fwdTxAmount*/CAmount(10));
    mutFwdTx.vin.clear();
    mutFwdTx.vin.push_back(CTxIn(inputTxHash, 0, CScript()));
    CTransaction fwdTx(mutFwdTx);
    CTxMemPoolEntry mempoolEntry(fwdTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/fwtHeight);
    mempool.addUnchecked(fwdTx.GetHash(), mempoolEntry);

    //test
    std::list<CTransaction> outdatedTxs;
    std::list<CScCertificate> outdatedCerts;
    mempool.removeStaleTransactions(&sidechainsView, outdatedTxs, outdatedCerts);

    //checks
    EXPECT_FALSE(mempool.exists(fwdTx.GetHash()));
    EXPECT_TRUE(std::find(outdatedTxs.begin(), outdatedTxs.end(), fwdTx) != outdatedTxs.end());
}

TEST_F(SidechainsInMempoolTestSuite,UnconfirmedMbtrTowardCeasedSidechainIsDropped)
{
    CNakedCCoinsViewCache sidechainsView(pcoinsTip);

    // setup sidechain initial state
    CSidechain initialScState;
    uint256 scId = uint256S("aaaa");
    initialScState.creationBlockHeight = 1492;
    initialScState.creationData.withdrawalEpochLength = 14;
    initialScState.creationData.wMbtrVk = libzendoomc::ScVk(ParseHex(SAMPLE_VK));
    int heightWhereCeased = initialScState.GetScheduledCeasingHeight();

    storeSidechainWithCurrentHeight(sidechainsView, scId, initialScState, heightWhereCeased);
    ASSERT_TRUE(sidechainsView.GetSidechainState(scId) == CSidechain::State::CEASED);

    // create coinbase to finance mbtr
    int mbtrHeight = heightWhereCeased +1;
    uint256 inputTxHash = txCreationUtils::CreateSpendableCoinAtHeight(sidechainsView, mbtrHeight-COINBASE_MATURITY);

    //Add mbtr to mempool
    CBwtRequestOut mcBwtReq;
    mcBwtReq.scId = scId;
    CMutableTransaction mutMbtrTx;
    mutMbtrTx.nVersion = SC_TX_VERSION;
    mutMbtrTx.vin.clear();
    mutMbtrTx.vin.push_back(CTxIn(inputTxHash, 0, CScript()));
    mutMbtrTx.vmbtr_out.push_back(mcBwtReq);
    CTransaction mbtrTx(mutMbtrTx);
    CTxMemPoolEntry mempoolEntry(mbtrTx, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/mbtrHeight);
    std::map<uint256, CFieldElement> dummyCertDataHashInfo;
    dummyCertDataHashInfo[scId] = CFieldElement{};
    mempool.addUnchecked(mbtrTx.GetHash(), mempoolEntry, /*fCurrentEstimate*/true, dummyCertDataHashInfo);

    //test
    std::list<CTransaction> outdatedTxs;
    std::list<CScCertificate> outdatedCerts;
    mempool.removeStaleTransactions(&sidechainsView, outdatedTxs, outdatedCerts);

    //checks
    EXPECT_FALSE(mempool.exists(mbtrTx.GetHash()));
    EXPECT_TRUE(std::find(outdatedTxs.begin(), outdatedTxs.end(), mbtrTx) != outdatedTxs.end());
}

TEST_F(SidechainsInMempoolTestSuite,UnconfirmedCertTowardAliveSidechainIsNotDropped)
{
    CNakedCCoinsViewCache sidechainsView(pcoinsTip);

    // setup sidechain initial state
    CSidechain initialScState;
    uint256 scId = uint256S("aaaa");
    initialScState.creationBlockHeight = 201;
    initialScState.creationData.withdrawalEpochLength = 9;
    initialScState.lastTopQualityCertReferencedEpoch = 19;
    int heightWhereAlive = initialScState.GetScheduledCeasingHeight()-1;
    storeSidechainWithCurrentHeight(sidechainsView, scId, initialScState, heightWhereAlive);
    ASSERT_TRUE(sidechainsView.GetSidechainState(scId) == CSidechain::State::ALIVE);

    // set relevant heights
    int epochReferredByCert = initialScState.lastTopQualityCertReferencedEpoch + 1;
    int certHeight = initialScState.GetCertSubmissionWindowStart(epochReferredByCert) + 1;
    ASSERT_TRUE(certHeight <= initialScState.GetCertSubmissionWindowEnd(epochReferredByCert));

    // create coinbase to finance cert
    uint256 inputTxHash = txCreationUtils::CreateSpendableCoinAtHeight(sidechainsView, certHeight-COINBASE_MATURITY);

    //Add mbtr to mempool
    uint256 dummyEndBlockHash = uint256S("aaa");
    CMutableScCertificate mutCert = txCreationUtils::createCertificate(scId, epochReferredByCert, dummyEndBlockHash,
        /*changeTotalAmount*/CAmount(4),/*numChangeOut*/2, /*bwtAmount*/CAmount(0), /*numBwt*/2);
    mutCert.vin.clear();
    mutCert.vin.push_back(CTxIn(inputTxHash, 0, CScript()));
    CScCertificate cert(mutCert);
    CCertificateMemPoolEntry mempoolEntry(cert, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/certHeight);
    mempool.addUnchecked(cert.GetHash(), mempoolEntry);

    //test
    std::list<CScCertificate> outdatedCerts;
    mempool.removeStaleCertificates(&sidechainsView, outdatedCerts);

    //checks
    EXPECT_TRUE(mempool.exists(cert.GetHash()));
    EXPECT_FALSE(std::find(outdatedCerts.begin(), outdatedCerts.end(), cert) != outdatedCerts.end());
}

TEST_F(SidechainsInMempoolTestSuite,UnconfirmedCertTowardCeasedSidechainIsDropped)
{
    CNakedCCoinsViewCache sidechainsView(pcoinsTip);

    // setup sidechain initial state
    CSidechain initialScState;
    uint256 scId = uint256S("aaaa");
    initialScState.creationBlockHeight = 1492;
    initialScState.creationData.withdrawalEpochLength = 14;
    int heightWhereCeased = initialScState.GetScheduledCeasingHeight();

    storeSidechainWithCurrentHeight(sidechainsView, scId, initialScState, heightWhereCeased);
    ASSERT_TRUE(sidechainsView.GetSidechainState(scId) == CSidechain::State::CEASED);

    // create coinbase to finance cert
    int certHeight = heightWhereCeased  +1 ;
    uint256 inputTxHash = txCreationUtils::CreateSpendableCoinAtHeight(sidechainsView, certHeight-COINBASE_MATURITY);

    //Add mbtr to mempool
    uint256 dummyEndBlockHash = uint256S("aaa");
    CMutableScCertificate mutCert = txCreationUtils::createCertificate(scId, /*currentEpoch*/0, dummyEndBlockHash,
        /*changeTotalAmount*/CAmount(4),/*numChangeOut*/2, /*bwtAmount*/CAmount(0), /*numBwt*/2);
    mutCert.vin.clear();
    mutCert.vin.push_back(CTxIn(inputTxHash, 0, CScript()));
    CScCertificate cert(mutCert);
    CCertificateMemPoolEntry mempoolEntry(cert, /*fee*/CAmount(1), /*time*/ 1000, /*priority*/1.0, /*height*/certHeight);
    mempool.addUnchecked(cert.GetHash(), mempoolEntry);

    //test
    std::list<CScCertificate> outdatedCerts;
    mempool.removeStaleCertificates(&sidechainsView, outdatedCerts);

    //checks
    EXPECT_FALSE(mempool.exists(cert.GetHash()));
    EXPECT_TRUE(std::find(outdatedCerts.begin(), outdatedCerts.end(), cert) != outdatedCerts.end());
}

TEST_F(SidechainsInMempoolTestSuite, DependenciesInEmptyMempool) {
    // prerequisites
    CAmount dummyAmount(10);
    CScript dummyScript;
    CTxOut dummyOut(dummyAmount, dummyScript);

    CMutableTransaction tx_1;
    tx_1.vin.push_back(CTxIn(uint256(), 0, dummyScript));
    tx_1.addOut(dummyOut);

    //test and checks
    EXPECT_TRUE(aMempool.mempoolDependenciesFrom(tx_1).empty());
    EXPECT_TRUE(aMempool.mempoolDependenciesOf(tx_1).empty());
}

TEST_F(SidechainsInMempoolTestSuite, DependenciesOfSingleTransaction) {
    // prerequisites
    CAmount dummyAmount(10);
    CScript dummyScript;
    CTxOut dummyOut(dummyAmount, dummyScript);

    CMutableTransaction tx_1;
    tx_1.vin.push_back(CTxIn(uint256(), 0, dummyScript));
    tx_1.addOut(dummyOut);
    CTxMemPoolEntry tx_1_entry(tx_1, /*fee*/dummyAmount, /*time*/ 1000, /*priority*/1.0, /*height*/1987);

    //test
    aMempool.addUnchecked(tx_1.GetHash(), tx_1_entry);

    //checks
    EXPECT_TRUE(aMempool.mempoolDependenciesFrom(tx_1).empty());
    EXPECT_TRUE(aMempool.mempoolDependenciesOf(tx_1).empty());
}


TEST_F(SidechainsInMempoolTestSuite, DependenciesOfSimpleChain) {
    // prerequisites
    CAmount dummyAmount(10);
    CScript dummyScript;
    CTxOut dummyOut(dummyAmount, dummyScript);

    // Create chain tx_1 -> tx_2 -> tx_3
    CMutableTransaction tx_1;
    tx_1.vin.push_back(CTxIn(uint256(), 0, dummyScript));
    tx_1.addOut(dummyOut);
    CTxMemPoolEntry tx_1_entry(tx_1, /*fee*/dummyAmount, /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    ASSERT_TRUE(aMempool.addUnchecked(tx_1.GetHash(), tx_1_entry));

    CMutableTransaction tx_2;
    tx_2.vin.push_back(CTxIn(tx_1.GetHash(), 0, dummyScript));
    tx_2.addOut(dummyOut);
    CTxMemPoolEntry tx_2_entry(tx_2, /*fee*/dummyAmount, /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    ASSERT_TRUE(aMempool.addUnchecked(tx_2.GetHash(), tx_2_entry));

    CMutableTransaction tx_3;
    tx_3.vin.push_back(CTxIn(tx_2.GetHash(), 0, dummyScript));
    CTxMemPoolEntry tx_3_entry(tx_3, /*fee*/dummyAmount, /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    ASSERT_TRUE(aMempool.addUnchecked(tx_3.GetHash(), tx_3_entry));

    //checks
    EXPECT_TRUE(aMempool.mempoolDependenciesFrom(tx_1).empty());
    EXPECT_TRUE(aMempool.mempoolDependenciesFrom(tx_2) == std::vector<uint256>({tx_1.GetHash()}));
    EXPECT_TRUE(aMempool.mempoolDependenciesFrom(tx_3) == std::vector<uint256>({tx_2.GetHash(),tx_1.GetHash()}));

    EXPECT_TRUE(aMempool.mempoolDependenciesOf(tx_1) == std::vector<uint256>({tx_2.GetHash(),tx_3.GetHash()}));
    EXPECT_TRUE(aMempool.mempoolDependenciesOf(tx_2) == std::vector<uint256>({tx_3.GetHash()}));
    EXPECT_TRUE(aMempool.mempoolDependenciesOf(tx_3).empty());
}

TEST_F(SidechainsInMempoolTestSuite, DependenciesOfTree) {
    // prerequisites
    CAmount dummyAmount(10);
    CScript dummyScript;
    CTxOut dummyOut_1(dummyAmount, dummyScript);
    CTxOut dummyOut_2(++dummyAmount, dummyScript);

    CMutableTransaction tx_root;
    tx_root.vin.push_back(CTxIn(uint256(), 0, dummyScript));
    tx_root.addOut(dummyOut_1);
    tx_root.addOut(dummyOut_2);
    CTxMemPoolEntry tx_root_entry(tx_root, /*fee*/dummyAmount, /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    ASSERT_TRUE(aMempool.addUnchecked(tx_root.GetHash(), tx_root_entry));

    CMutableTransaction tx_child_1;
    tx_child_1.vin.push_back(CTxIn(tx_root.GetHash(), 0, dummyScript));
    tx_child_1.addOut(dummyOut_1);
    tx_child_1.addOut(dummyOut_2);
    CTxMemPoolEntry tx_child_1_entry(tx_child_1, /*fee*/dummyAmount, /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    ASSERT_TRUE(aMempool.addUnchecked(tx_child_1.GetHash(), tx_child_1_entry));

    CMutableTransaction tx_child_2;
    tx_child_2.vin.push_back(CTxIn(tx_root.GetHash(), 1, dummyScript));
    tx_child_2.addOut(dummyOut_1);
    tx_child_2.addOut(dummyOut_2);
    CTxMemPoolEntry tx_child_2_entry(tx_child_2, /*fee*/dummyAmount, /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    ASSERT_TRUE(aMempool.addUnchecked(tx_child_2.GetHash(), tx_child_2_entry));

    CMutableTransaction tx_grandchild_1;
    tx_grandchild_1.vin.push_back(CTxIn(tx_child_1.GetHash(), 0, dummyScript));
    CTxMemPoolEntry tx_grandchild_1_entry(tx_grandchild_1, /*fee*/dummyAmount, /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    ASSERT_TRUE(aMempool.addUnchecked(tx_grandchild_1.GetHash(), tx_grandchild_1_entry));

    CMutableTransaction tx_grandchild_2;
    tx_grandchild_2.vin.push_back(CTxIn(tx_child_1.GetHash(), 1, dummyScript));
    CTxMemPoolEntry tx_grandchild_2_entry(tx_grandchild_2, /*fee*/dummyAmount, /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    ASSERT_TRUE(aMempool.addUnchecked(tx_grandchild_2.GetHash(), tx_grandchild_2_entry));

    CMutableTransaction tx_grandchild_3;
    tx_grandchild_3.vin.push_back(CTxIn(tx_child_2.GetHash(), 0, dummyScript));
    CTxMemPoolEntry tx_grandchild_3_entry(tx_grandchild_3, /*fee*/dummyAmount, /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    ASSERT_TRUE(aMempool.addUnchecked(tx_grandchild_3.GetHash(), tx_grandchild_3_entry));

    //checks
    EXPECT_TRUE(aMempool.mempoolDependenciesFrom(tx_root).empty());
    EXPECT_TRUE(aMempool.mempoolDependenciesFrom(tx_child_1) == std::vector<uint256>({tx_root.GetHash()}));
    EXPECT_TRUE(aMempool.mempoolDependenciesFrom(tx_child_2) == std::vector<uint256>({tx_root.GetHash()}));
    EXPECT_TRUE(aMempool.mempoolDependenciesFrom(tx_grandchild_1) == std::vector<uint256>({tx_child_1.GetHash(), tx_root.GetHash()}));
    EXPECT_TRUE(aMempool.mempoolDependenciesFrom(tx_grandchild_2) == std::vector<uint256>({tx_child_1.GetHash(), tx_root.GetHash()}));
    EXPECT_TRUE(aMempool.mempoolDependenciesFrom(tx_grandchild_3) == std::vector<uint256>({tx_child_2.GetHash(), tx_root.GetHash()}));

    EXPECT_TRUE(aMempool.mempoolDependenciesOf(tx_root)
        == std::vector<uint256>({tx_child_1.GetHash(), tx_grandchild_2.GetHash(), tx_grandchild_1.GetHash(),
                                 tx_child_2.GetHash(), tx_grandchild_3.GetHash()}));
    EXPECT_TRUE(aMempool.mempoolDependenciesOf(tx_child_1) == std::vector<uint256>({tx_grandchild_1.GetHash(), tx_grandchild_2.GetHash()}));
    EXPECT_TRUE(aMempool.mempoolDependenciesOf(tx_child_2) == std::vector<uint256>({tx_grandchild_3.GetHash()}));
    EXPECT_TRUE(aMempool.mempoolDependenciesOf(tx_grandchild_1).empty());
    EXPECT_TRUE(aMempool.mempoolDependenciesOf(tx_grandchild_2).empty());
    EXPECT_TRUE(aMempool.mempoolDependenciesOf(tx_grandchild_3).empty());
}

TEST_F(SidechainsInMempoolTestSuite, DependenciesOfTDAG) {
    // prerequisites
    CAmount dummyAmount(10);
    CScript dummyScript;
    CTxOut dummyOut_1(dummyAmount, dummyScript);
    CTxOut dummyOut_2(++dummyAmount, dummyScript);

    CMutableTransaction tx_root;
    tx_root.vin.push_back(CTxIn(uint256(), 0, dummyScript));
    tx_root.addOut(dummyOut_1);
    tx_root.addOut(dummyOut_2);
    CTxMemPoolEntry tx_root_entry(tx_root, /*fee*/dummyAmount, /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    ASSERT_TRUE(aMempool.addUnchecked(tx_root.GetHash(), tx_root_entry));

    CMutableTransaction tx_child_1;
    tx_child_1.vin.push_back(CTxIn(tx_root.GetHash(), 0, dummyScript));
    tx_child_1.addOut(dummyOut_1);
    CTxMemPoolEntry tx_child_1_entry(tx_child_1, /*fee*/dummyAmount, /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    ASSERT_TRUE(aMempool.addUnchecked(tx_child_1.GetHash(), tx_child_1_entry));

    CMutableTransaction tx_grandchild_1;
    tx_grandchild_1.vin.push_back(CTxIn(tx_root.GetHash(), 1, dummyScript));
    tx_grandchild_1.vin.push_back(CTxIn(tx_child_1.GetHash(), 0, dummyScript));
    CTxMemPoolEntry tx_grandchild_1_entry(tx_grandchild_1, /*fee*/dummyAmount, /*time*/ 1000, /*priority*/1.0, /*height*/1987);
    ASSERT_TRUE(aMempool.addUnchecked(tx_grandchild_1.GetHash(), tx_grandchild_1_entry));

    //checks
    EXPECT_TRUE(aMempool.mempoolDependenciesFrom(tx_root).empty());
    EXPECT_TRUE(aMempool.mempoolDependenciesFrom(tx_child_1) == std::vector<uint256>({tx_root.GetHash()}));
    EXPECT_TRUE(aMempool.mempoolDependenciesFrom(tx_grandchild_1) == std::vector<uint256>({tx_child_1.GetHash(),tx_root.GetHash()}));

    EXPECT_TRUE(aMempool.mempoolDependenciesOf(tx_root) == std::vector<uint256>({tx_child_1.GetHash(), tx_grandchild_1.GetHash()}));
    EXPECT_TRUE(aMempool.mempoolDependenciesOf(tx_child_1) == std::vector<uint256>({tx_grandchild_1.GetHash()}));
    EXPECT_TRUE(aMempool.mempoolDependenciesOf(tx_grandchild_1).empty());
}

///////////////////////////////////////////////////////////////////////////////
////////////////////////// Test Fixture definitions ///////////////////////////
///////////////////////////////////////////////////////////////////////////////
void SidechainsInMempoolTestSuite::InitCoinGeneration() {
    coinsKey.MakeNewKey(true);
    keystore.AddKey(coinsKey);

    coinsScript << OP_DUP << OP_HASH160 << ToByteVector(coinsKey.GetPubKey().GetID()) << OP_EQUALVERIFY << OP_CHECKSIG;
}

std::pair<uint256, CCoinsCacheEntry> SidechainsInMempoolTestSuite::GenerateCoinsAmount(const CAmount & amountToGenerate) {
    static unsigned int hashSeed = 1987;
    CCoinsCacheEntry entry;
    entry.flags = CCoinsCacheEntry::FRESH | CCoinsCacheEntry::DIRTY;

    entry.coins.fCoinBase = false;
    entry.coins.nVersion = TRANSPARENT_TX_VERSION;
    entry.coins.nHeight = minimalHeightForSidechains;

    entry.coins.vout.resize(1);
    entry.coins.vout[0].nValue = amountToGenerate;
    entry.coins.vout[0].scriptPubKey = coinsScript;

    std::stringstream num;
    num << std::hex << ++hashSeed;

    return std::pair<uint256, CCoinsCacheEntry>(uint256S(num.str()), entry);
}

bool SidechainsInMempoolTestSuite::StoreCoins(const std::pair<uint256, CCoinsCacheEntry>& entryToStore) {
    CCoinsViewCache view(pcoinsTip);
    CCoinsMap tmpCoinsMap;
    tmpCoinsMap[entryToStore.first] = entryToStore.second;

    const uint256 hashBlock = pcoinsTip->GetBestBlock(); //keep same best block as set in Fixture setup
    const uint256 hashAnchor;
    CAnchorsMap mapAnchors;
    CNullifiersMap mapNullifiers;
    CSidechainsMap mapSidechains;
    CSidechainEventsMap mapCeasingScs;
    CCswNullifiersMap cswNullifiers;

    pcoinsTip->BatchWrite(tmpCoinsMap, hashBlock, hashAnchor, mapAnchors, mapNullifiers, mapSidechains, mapCeasingScs, cswNullifiers);

    return view.HaveCoins(entryToStore.first) == true;
}

CTransaction SidechainsInMempoolTestSuite::GenerateScTx(const CAmount & creationTxAmount, int epochLenght, bool ceasedVkDefined) {
    std::pair<uint256, CCoinsCacheEntry> coinData = GenerateCoinsAmount(1000);
    StoreCoins(coinData);

    CMutableTransaction scTx;
    scTx.nVersion = SC_TX_VERSION;
    scTx.vin.resize(1);
    scTx.vin[0].prevout = COutPoint(coinData.first, 0);

    scTx.vsc_ccout.resize(1);
    scTx.vsc_ccout[0].nValue = creationTxAmount;
    scTx.vsc_ccout[0].withdrawalEpochLength = (epochLenght < 0)?getScMinWithdrawalEpochLength(): epochLenght;

    scTx.vsc_ccout[0].wCertVk = libzendoomc::ScVk(ParseHex(SAMPLE_VK));
    scTx.vsc_ccout[0].wMbtrVk = libzendoomc::ScVk(ParseHex(SAMPLE_VK));
    if(ceasedVkDefined) scTx.vsc_ccout[0].wCeasedVk = libzendoomc::ScVk();

    SignSignature(keystore, coinData.second.coins.vout[0].scriptPubKey, scTx, 0);

    return scTx;
}

CTransaction SidechainsInMempoolTestSuite::GenerateFwdTransferTx(const uint256 & newScId, const CAmount & fwdTxAmount) {
    std::pair<uint256, CCoinsCacheEntry> coinData = GenerateCoinsAmount(1000);
    StoreCoins(coinData);

    CMutableTransaction scTx;
    scTx.nVersion = SC_TX_VERSION;
    scTx.vin.resize(1);
    scTx.vin[0].prevout = COutPoint(coinData.first, 0);

    scTx.vft_ccout.resize(1);
    scTx.vft_ccout[0].scId   = newScId;
    scTx.vft_ccout[0].nValue = fwdTxAmount;

    scTx.vft_ccout.resize(2); //testing double deletes
    scTx.vft_ccout[1].scId   = newScId;
    scTx.vft_ccout[1].nValue = fwdTxAmount;

    SignSignature(keystore, coinData.second.coins.vout[0].scriptPubKey, scTx, 0);

    return scTx;
}

CTransaction SidechainsInMempoolTestSuite::GenerateBtrTx(const uint256 & scId) {
    std::pair<uint256, CCoinsCacheEntry> coinData = GenerateCoinsAmount(1000);
    StoreCoins(coinData);

    CMutableTransaction scTx;
    scTx.nVersion = SC_TX_VERSION;
    scTx.vin.resize(1);
    scTx.vin[0].prevout = COutPoint(coinData.first, 0);

    scTx.vmbtr_out.resize(1);
    scTx.vmbtr_out[0].scId   = scId;
    scTx.vmbtr_out[0].scFee = CAmount(1); //dummy amount
    scTx.vmbtr_out[0].scRequestData = CFieldElement{SAMPLE_FIELD};

    scTx.vmbtr_out.resize(2); //testing double deletes
    scTx.vmbtr_out[1].scId   = scId;
    scTx.vmbtr_out[1].scFee = CAmount(2); //dummy amount
    scTx.vmbtr_out[1].scProof = libzendoomc::ScProof(ParseHex(SAMPLE_PROOF));
    scTx.vmbtr_out[1].scRequestData = CFieldElement{SAMPLE_FIELD};

    SignSignature(keystore, coinData.second.coins.vout[0].scriptPubKey, scTx, 0);

    return scTx;
}

CTxCeasedSidechainWithdrawalInput SidechainsInMempoolTestSuite::GenerateCSWInput(const uint256& scId, const std::string& nullifierHex, CAmount amount)
{
    CFieldElement nullifier{};
    std::vector<unsigned char> tmp{nullifierHex.begin(), nullifierHex.end()};
    tmp.resize(CFieldElement::ByteSize(), 0x0);
    nullifier.SetByteArray(tmp);

    uint160 dummyPubKeyHash = coinsKey.GetPubKey().GetID();
    libzendoomc::ScProof dummyScProof;
    CScript dummyRedeemScript;

    return CTxCeasedSidechainWithdrawalInput(amount, scId, nullifier, dummyPubKeyHash, dummyScProof, dummyRedeemScript);
}

CTransaction SidechainsInMempoolTestSuite::GenerateCSWTx(const std::vector<CTxCeasedSidechainWithdrawalInput>& csws)
{
    CMutableTransaction mutTx;
    mutTx.nVersion = SC_TX_VERSION;
    mutTx.vcsw_ccin.insert(mutTx.vcsw_ccin.end(), csws.begin(), csws.end());

    CScript dummyScriptPubKey =
            GetScriptForDestination(CKeyID(uint160(ParseHex("816115944e077fe7c803cfa57f29b36bf87c1d35"))),/*withCheckBlockAtHeight*/true);

    CAmount totalValue = 0;
    for(const CTxCeasedSidechainWithdrawalInput& csw: csws)
        totalValue += csw.nValue;
    mutTx.addOut(CTxOut(totalValue - 1, dummyScriptPubKey));

    // Sign CSW input
    for(const CTxCeasedSidechainWithdrawalInput& csw: csws)
    {
        SignSignature(keystore, csw.scriptPubKey(), mutTx, 0);
    }

    return mutTx;
}

CTransaction SidechainsInMempoolTestSuite::GenerateCSWTx(const CTxCeasedSidechainWithdrawalInput& csw)
{
    std::vector<CTxCeasedSidechainWithdrawalInput> csws;
    csws.push_back(csw);
    return GenerateCSWTx(csws);
}

CScCertificate SidechainsInMempoolTestSuite::GenerateCertificate(const uint256 & scId, int epochNum, const uint256 & endEpochBlockHash,
                 CAmount inputAmount, CAmount changeTotalAmount, unsigned int numChangeOut,
                 CAmount bwtTotalAmount, unsigned int numBwt, int64_t quality, const CTransactionBase* inputTxBase)
{
    CMutableScCertificate res;
    res.nVersion = SC_CERT_VERSION;
    res.scId = scId;
    res.epochNumber = epochNum;
    res.endEpochBlockHash = endEpochBlockHash;
    res.quality = quality;
    res.scProof = libzendoomc::ScProof(ParseHex(SAMPLE_PROOF));

    CScript dummyScriptPubKey =
            GetScriptForDestination(CKeyID(uint160(ParseHex("816115944e077fe7c803cfa57f29b36bf87c1d35"))),/*withCheckBlockAtHeight*/true);
    for(unsigned int idx = 0; idx < numChangeOut; ++idx)
        res.addOut(CTxOut(changeTotalAmount/numChangeOut,dummyScriptPubKey));

    for(unsigned int idx = 0; idx < numBwt; ++idx)
        res.addBwt(CTxOut(bwtTotalAmount/numBwt, dummyScriptPubKey));

    if (inputTxBase)
    {
        res.vin.push_back(CTxIn(COutPoint(inputTxBase->GetHash(), 0), CScript(), -1));
        SignSignature(keystore, inputTxBase->GetVout()[0].scriptPubKey, res, 0);
    }
    else if (inputAmount > 0)
    {
        std::pair<uint256, CCoinsCacheEntry> coinData = GenerateCoinsAmount(inputAmount);
        StoreCoins(coinData);
    
        res.vin.push_back(CTxIn(COutPoint(coinData.first, 0), CScript(), -1));
        SignSignature(keystore, coinData.second.coins.vout[0].scriptPubKey, res, 0);
    }

    return res;
}

void SidechainsInMempoolTestSuite::storeSidechainWithCurrentHeight(CNakedCCoinsViewCache& view, const uint256& scId, const CSidechain& sidechain, int chainActiveHeight)
{
    chainSettingUtils::ExtendChainActiveToHeight(chainActiveHeight);
    view.SetBestBlock(chainActive.Tip()->GetBlockHash());
    txCreationUtils::storeSidechain(view.getSidechainMap(), scId, sidechain);
}
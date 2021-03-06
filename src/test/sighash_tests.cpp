// Copyright (c) 2013 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "consensus/validation.h"
#include "data/sighash.json.h"
#include "main.h"
#include "random.h"
#include "script/interpreter.h"
#include "script/script.h"
#include "serialize.h"
#include "test/test_bitcoin.h"
#include "util.h"
#include "version.h"
#include "sodium.h"

#include <iostream>

#include <boost/test/unit_test.hpp>

#include <univalue.h>

extern UniValue read_json(const std::string& jsondata);

// Old script.cpp SignatureHash function
uint256 static SignatureHashOld(CScript scriptCode, const CTransaction& txTo, unsigned int nIn, int nHashType)
{
    static const uint256 one(uint256S("0000000000000000000000000000000000000000000000000000000000000001"));
    if (nIn >= txTo.vin.size())
    {
        printf("ERROR: SignatureHash(): nIn=%d out of range\n", nIn);
        return one;
    }
    CMutableTransaction txTmp(txTo);

    // Blank out other inputs' signatures
    for (unsigned int i = 0; i < txTmp.vin.size(); i++)
        txTmp.vin[i].scriptSig = CScript();
    txTmp.vin[nIn].scriptSig = scriptCode;

    // Blank out some of the outputs
    if ((nHashType & 0x1f) == SIGHASH_NONE)
    {
        // Wildcard payee
        txTmp.vout.clear();

        // Let the others update at will
        for (unsigned int i = 0; i < txTmp.vin.size(); i++)
            if (i != nIn)
                txTmp.vin[i].nSequence = 0;
    }
    else if ((nHashType & 0x1f) == SIGHASH_SINGLE)
    {
        // Only lock-in the txout payee at same index as txin
        unsigned int nOut = nIn;
        if (nOut >= txTmp.vout.size())
        {
            printf("ERROR: SignatureHash(): nOut=%d out of range\n", nOut);
            return one;
        }
        txTmp.vout.resize(nOut+1);
        for (unsigned int i = 0; i < nOut; i++)
            txTmp.vout[i].SetNull();

        // Let the others update at will
        for (unsigned int i = 0; i < txTmp.vin.size(); i++)
            if (i != nIn)
                txTmp.vin[i].nSequence = 0;
    }

    // Blank out other inputs completely, not recommended for open transactions
    if (nHashType & SIGHASH_ANYONECANPAY)
    {
        txTmp.vin[0] = txTmp.vin[nIn];
        txTmp.vin.resize(1);
    }

    // Blank out the joinsplit signature.
    memset(&txTmp.joinSplitSig[0], 0, txTmp.joinSplitSig.size());

    // Serialize and hash
    CHashWriter ss(SER_GETHASH, 0);
    ss << txTmp << nHashType;
    return ss.GetHash();
}

void static RandomScript(CScript &script) {
    static const opcodetype oplist[] = {OP_FALSE, OP_1, OP_2, OP_3, OP_CHECKSIG, OP_IF, OP_VERIF, OP_RETURN};
    script = CScript();
    int ops = (insecure_rand() % 10);
    for (int i=0; i<ops; i++)
        script << oplist[insecure_rand() % (sizeof(oplist)/sizeof(oplist[0]))];
}

void static RandomTransaction(CMutableTransaction &tx, bool fSingle, bool emptyInputScript = false) {
	bool isGroth = (insecure_rand() % 2) ==  0;
	if (isGroth) {
		tx.nVersion = GROTH_TX_VERSION;
	} else {
		//this can generate negative versions (including GROTH_TX_VERSION)
		// test will also have to verify if negative versions are rejected except GROTH_TX_VERSION
		tx.nVersion = insecure_rand();
	}

    tx.vin.clear();
    tx.vout.clear();
    tx.nLockTime = (insecure_rand() % 2) ? insecure_rand() : 0;
    int ins = (insecure_rand() % 4) + 1;
    int outs = fSingle ? ins : (insecure_rand() % 4) + 1;
    int joinsplits = (insecure_rand() % 4);
    for (int in = 0; in < ins; in++) {
        tx.vin.push_back(CTxIn());
        CTxIn &txin = tx.vin.back();
        txin.prevout.hash = GetRandHash();
        txin.prevout.n = insecure_rand() % 4;
        if(emptyInputScript) {
        	txin.scriptSig = CScript();
        } else {
        	RandomScript(txin.scriptSig);
        }
        txin.nSequence = (insecure_rand() % 2) ? insecure_rand() : (unsigned int)-1;
    }
    for (int out = 0; out < outs; out++) {
        tx.vout.push_back(CTxOut());
        CTxOut &txout = tx.vout.back();
        txout.nValue = insecure_rand() % 100000000;
        RandomScript(txout.scriptPubKey);
    }
    if (tx.nVersion >= PHGR_TX_VERSION || tx.nVersion == GROTH_TX_VERSION) {
        for (int js = 0; js < joinsplits; js++) {
            JSDescription jsdesc = JSDescription::getNewInstance(tx.nVersion == GROTH_TX_VERSION);
            if (insecure_rand() % 2 == 0) {
                jsdesc.vpub_old = insecure_rand() % 100000000;
            } else {
                jsdesc.vpub_new = insecure_rand() % 100000000;
            }

            jsdesc.anchor = GetRandHash();
            jsdesc.nullifiers[0] = GetRandHash();
            jsdesc.nullifiers[1] = GetRandHash();
            jsdesc.ephemeralKey = GetRandHash();
            jsdesc.randomSeed = GetRandHash();
            randombytes_buf(jsdesc.ciphertexts[0].begin(), jsdesc.ciphertexts[0].size());
            randombytes_buf(jsdesc.ciphertexts[1].begin(), jsdesc.ciphertexts[1].size());
            if (tx.nVersion == GROTH_TX_VERSION) {
                libzcash::GrothProof zkproof;
                randombytes_buf(zkproof.begin(), zkproof.size());
                jsdesc.proof = zkproof;
            } else {
                jsdesc.proof = libzcash::PHGRProof::random_invalid();
            }
            jsdesc.macs[0] = GetRandHash();
            jsdesc.macs[1] = GetRandHash();

            tx.vjoinsplit.push_back(jsdesc);
        }

        unsigned char joinSplitPrivKey[crypto_sign_SECRETKEYBYTES];
        crypto_sign_keypair(tx.joinSplitPubKey.begin(), joinSplitPrivKey);

        // Empty output script.
        CScript scriptCode;
        CTransaction signTx(tx);
        uint256 dataToBeSigned = SignatureHash(scriptCode, signTx, NOT_AN_INPUT, SIGHASH_ALL);

        assert(crypto_sign_detached(&tx.joinSplitSig[0], NULL,
                                    dataToBeSigned.begin(), 32,
                                    joinSplitPrivKey
                                    ) == 0);
    }
}

BOOST_FIXTURE_TEST_SUITE(sighash_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(sighash_test)
{
    seed_insecure_rand(false);

    #if defined(PRINT_SIGHASH_JSON)
    std::cout << "[\n";
    std::cout << "\t[\"raw_transaction, script, input_index, hashType, signature_hash (result)\"],\n";
    #endif
    int nRandomTests = 50000;

    #if defined(PRINT_SIGHASH_JSON)
    nRandomTests = 500;
    #endif
    for (int i=0; i<nRandomTests; i++) {
        int nHashType = insecure_rand();
        CMutableTransaction txTo;
        RandomTransaction(txTo, (nHashType & 0x1f) == SIGHASH_SINGLE);
        CScript scriptCode;
        RandomScript(scriptCode);
        int nIn = insecure_rand() % txTo.vin.size();

        uint256 sh, sho;
        sho = SignatureHashOld(scriptCode, txTo, nIn, nHashType);
        sh = SignatureHash(scriptCode, txTo, nIn, nHashType);
        #if defined(PRINT_SIGHASH_JSON)
        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << txTo;

        std::cout << "\t[\"" ;
        std::cout << HexStr(ss.begin(), ss.end()) << "\", \"";
        std::cout << HexStr(scriptCode) << "\", ";
        std::cout << nIn << ", ";
        std::cout << nHashType << ", \"";
        std::cout << sho.GetHex() << "\"]";
        if (i+1 != nRandomTests) {
          std::cout << ",";
        }
        std::cout << "\n";
        #endif
        BOOST_CHECK(sh == sho);
    }
    #if defined(PRINT_SIGHASH_JSON)
    std::cout << "]\n";
    #endif

}

// Goal: check that SignatureHash generates correct hash by checking if serialization matches with the one implemented in CTransaction
BOOST_AUTO_TEST_CASE(sighash_from_tx)
{
	int nRandomTests = 500;

	for (int i=0; i<nRandomTests; i++) {
		CMutableTransaction txTo;
		uint256 interpreterSH, checkSH;
		CScript scriptCode;

		RandomTransaction(txTo, false, true);
		CTransaction::joinsplit_sig_t nullSig = {};
		txTo.joinSplitSig = nullSig;

		interpreterSH = SignatureHash(scriptCode, txTo, NOT_AN_INPUT, SIGHASH_ALL);


		// Serialize and hash
		CHashWriter ss(SER_GETHASH, 0);
		ss << txTo << (int)SIGHASH_ALL;
		checkSH = ss.GetHash();
		BOOST_CHECK(checkSH == interpreterSH);

	}

}

// Goal: check that SignatureHash generates correct hash
BOOST_AUTO_TEST_CASE(sighash_from_data)
{
    UniValue tests = read_json(std::string(json_tests::sighash, json_tests::sighash + sizeof(json_tests::sighash)));

    for (size_t idx = 0; idx < tests.size(); idx++) {
        UniValue test = tests[idx];
        std::string strTest = test.write();
        if (test.size() < 1) // Allow for extra stuff (useful for comments)
        {
            BOOST_ERROR("Bad test: " << strTest);
            continue;
        }
        if (test.size() == 1) continue; // comment

        std::string raw_tx, raw_script, sigHashHex;
        int nIn, nHashType;
        uint256 sh;
        CTransaction tx;
        CScript scriptCode = CScript();

        try {
          // deserialize test data
          raw_tx = test[0].get_str();
          raw_script = test[1].get_str();
          nIn = test[2].get_int();
          nHashType = test[3].get_int();
          sigHashHex = test[4].get_str();

          uint256 sh;
          CDataStream stream(ParseHex(raw_tx), SER_NETWORK, PROTOCOL_VERSION);
          stream >> tx;

          CValidationState state;

          if (tx.nVersion < MIN_OLD_TX_VERSION && tx.nVersion != GROTH_TX_VERSION) {
              // Transaction must be invalid
              BOOST_CHECK_MESSAGE(!CheckTransactionWithoutProofVerification(tx, state), strTest);
              BOOST_CHECK(!state.IsValid());
          } else {
              BOOST_CHECK_MESSAGE(CheckTransactionWithoutProofVerification(tx, state), strTest);
              BOOST_CHECK(state.IsValid());
          }

          std::vector<unsigned char> raw = ParseHex(raw_script);
          scriptCode.insert(scriptCode.end(), raw.begin(), raw.end());
        } catch (const std::exception& e) {
        	BOOST_ERROR("Bad test (exception: \"" << e.what() << "\"), couldn't deserialize data: " << strTest);
        	continue;
        } catch (...) {
          BOOST_ERROR("Bad test, couldn't deserialize data: " << strTest);
          continue;
        }

        sh = SignatureHash(scriptCode, tx, nIn, nHashType);
        BOOST_CHECK_MESSAGE(sh.GetHex() == sigHashHex, strTest);
    }
}
BOOST_AUTO_TEST_SUITE_END()

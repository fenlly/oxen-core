// Copyright (c) 2014-2019, The Monero Project
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers

#pragma once
#include <boost/asio/io_service.hpp>
#include <boost/serialization/serialization.hpp>
#include <boost/serialization/version.hpp>

// Workaround for boost::serialization issue #219
#include <boost/version.hpp>
#include <type_traits>
#if BOOST_VERSION == 107400
#include <boost/serialization/library_version_type.hpp>
#endif

#include <atomic>
#include <boost/multi_index/global_fun.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/serialization/list.hpp>
#include <ethyl/provider.hpp>
#include <functional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "blockchain_db/blockchain_db.h"
#include "checkpoints/checkpoints.h"
#include "common/util.h"
#include "crypto/eth.h"
#include "crypto/hash.h"
#include "cryptonote_basic/cryptonote_basic.h"
#include "cryptonote_basic/difficulty.h"
#include "cryptonote_basic/verification_context.h"
#include "cryptonote_core/oxen_name_system.h"
#include "cryptonote_protocol/cryptonote_protocol_defs.h"
#include "cryptonote_tx_utils.h"
#include "epee/rolling_median.h"
#include "epee/span.h"
#include "epee/string_tools.h"
#include "l2_tracker/l2_tracker.h"
#include "pulse.h"
#include "rpc/core_rpc_server_binary_commands.h"
#include "rpc/core_rpc_server_commands_defs.h"

struct sqlite3;
namespace service_nodes {
class service_node_list;
};
namespace tools {
class Notify;
}

namespace cryptonote {
struct block_and_checkpoint {
    cryptonote::block block;
    checkpoint_t checkpoint;
    bool checkpointed;
};

class tx_memory_pool;
struct tx_pool_options;
struct test_options;
class BlockchainSQLite;

/** Declares ways in which the BlockchainDB backend should be told to sync
 *
 */
enum blockchain_db_sync_mode {
    db_defaultsync,  //!< user didn't specify, use db_async
    db_sync,         //!< handle syncing calls instead of the backing db, synchronously
    db_async,        //!< handle syncing calls instead of the backing db, asynchronously
    db_nosync  //!< Leave syncing up to the backing db (safest, but slowest because of disk I/O)
};

/**
 * @brief Callback routine that returns checkpoints data for specific network type
 *
 * @param network network type
 *
 * @return checkpoints data, empty span if there ain't any checkpoints for specific network type
 */
using GetCheckpointsCallback = std::function<std::string_view(cryptonote::network_type network)>;

/************************************************************************/
/*                                                                      */
/************************************************************************/
class Blockchain {
  public:
    /**
     * @brief container for passing a block and metadata about it on the blockchain
     */
    struct block_extended_info {
        block_extended_info() = default;
        block_extended_info(
                const alt_block_data_t& src, block const& blk, checkpoint_t const* checkpoint);
        block bl;  //!< the block
        bool checkpointed;
        checkpoint_t checkpoint;
        uint64_t height;                        //!< the height of the block in the blockchain
        uint64_t block_cumulative_weight;       //!< the weight of the block
        difficulty_type cumulative_difficulty;  //!< the accumulated difficulty after that block
        uint64_t already_generated_coins;       //!< the total coins minted after that block
    };

    /**
     * @brief Blockchain constructor
     *
     * @param tx_pool a reference to the transaction pool to be kept by the Blockchain
     */
    Blockchain(tx_memory_pool& tx_pool, service_nodes::service_node_list& service_node_list);

    /**
     * @brief Blockchain destructor
     */
    ~Blockchain();

    /**
     * @brief Initialize the Blockchain state.
     *
     * @param db a pointer to the backing store to use for the blockchain.
     * @param nettype network type
     * @param ons_db a raw, unmanaged pointer to the ONS sqlite3 database.  NOTE: the Blockchain
     * object takes over ownership of this pointer, if not nullptr.  Should not be nullptr when
     * operating as a regular oxen node.
     * @param sqlite_db a raw, unmanaged pointer to the BlockchainSQLite object.  NOTE: the
     * Blockchain object takes over ownership of this pointer, if not nullptr.  Should not be
     * nullptr when operating as a regular oxen node.
     * @param l2_tracker a pointer to the L2Tracker instance; this pointer is *not* managed by the
     * Blockchain object, but must remain alive at least as long as the Blockchain object does.
     * Should be nullptr if this node does not track L2 state.
     * @param offline true if running offline, else false
     * @param test_options test parameters
     * @param fixed_difficulty fixed difficulty for testing purposes; 0 means disabled
     * @param get_checkpoints if set, will be called to get checkpoints data
     * @param abort if set, will be checked periodically during subsystem scanning: if it becomes
     * true then initialization will be aborted.
     *
     * @return true on success, false if any initialization steps fail
     */
    bool init(
            std::unique_ptr<BlockchainDB> db,
            const network_type nettype,
            sqlite3* ons_db = nullptr,
            cryptonote::BlockchainSQLite* sqlite_db = nullptr,
            eth::L2Tracker* l2_tracker = nullptr,
            bool offline = false,
            const cryptonote::test_options* test_options = nullptr,
            difficulty_type fixed_difficulty = 0,
            const GetCheckpointsCallback& get_checkpoints = nullptr,
            const std::atomic<bool>* abort = nullptr);

    // Common initializer for test code
    bool init(
            std::unique_ptr<BlockchainDB> db,
            const cryptonote::test_options& test_options,
            cryptonote::BlockchainSQLite* sqlite_db = nullptr) {
        return init(
                std::move(db),
                network_type::FAKECHAIN,
                nullptr,
                sqlite_db,
                nullptr,
                true,
                &test_options);
    }

    /**
     * @brief Uninitializes the blockchain state
     *
     * Saves to disk any state that needs to be maintained
     *
     * @return true on success, false if any uninitialization steps fail
     */
    bool deinit();

    bool get_blocks(
            uint64_t start_offset,
            size_t count,
            std::vector<block>& blocks,
            std::vector<std::string>* txs = nullptr) const;
    /**
     * @brief get blocks and transactions from blocks based on start height and count
     *
     * @param start_offset the height on the blockchain to start at
     * @param count the number of blocks to get, if there are as many after start_offset
     * @param blocks return-by-reference container to put result blocks in
     * @param txs return-by-reference container to put result transactions in
     *
     * @return false if start_offset > blockchain height, else true
     */
    bool get_blocks(
            uint64_t start_offset,
            size_t count,
            std::vector<std::pair<std::string, block>>& blocks,
            std::vector<std::string>& txs) const;

    /**
     * @brief get blocks from blocks based on start height and count
     *
     * @param start_offset the height on the blockchain to start at
     * @param count the number of blocks to get, if there are as many after start_offset
     * @param blocks return-by-reference container to put result blocks in
     *
     * @return false if start_offset > blockchain height, else true
     */
    bool get_blocks(
            uint64_t start_offset,
            size_t count,
            std::vector<std::pair<std::string, block>>& blocks) const;

    /**
     * @brief compiles a list of all blocks stored as alternative chains
     *
     * @param blocks return-by-reference container to put result blocks in
     *
     * @return true
     */
    bool get_alternative_blocks(std::vector<block>& blocks) const;

    /**
     * @brief returns the number of alternative blocks stored
     *
     * @return the number of alternative blocks stored
     */
    size_t get_alternative_blocks_count() const;

    /**
     * @brief gets a block's hash given a height
     *
     * @param height the height of the block
     *
     * @return the hash of the block at the requested height, or a zeroed hash if there is no such
     * block
     */
    crypto::hash get_block_id_by_height(uint64_t height) const;

    /**
     * @brief gets a block's hash given a height
     *
     * Used only by prepare_handle_incoming_blocks. Will look in the list of incoming blocks
     * if the height is contained there.
     *
     * @param height the height of the block
     *
     * @return the hash of the block at the requested height, or a zeroed hash if there is no such
     * block
     */
    crypto::hash get_pending_block_id_by_height(uint64_t height) const;

    /**
     * @brief gets the block with a given hash
     *
     * @param h the hash to look for
     * @param blk return-by-reference variable to put result block in
     * @param size if non-nullptr, this will be set to the stored block data size (just the block,
     * not including txes).
     * @param orphan if non-nullptr, will be set to true if not in the main chain, false otherwise
     *
     * @return true if the block was found, else false
     */
    bool get_block_by_hash(
            const crypto::hash& h,
            block& blk,
            size_t* size = nullptr,
            bool* orphan = nullptr) const;

    /**
     * @brief gets the block at the given height
     *
     * @param h the hash to look for
     * @param blk return-by-reference variable to put result block in
     * @param size if non-nullptr, this will be set to the stored block data size (just the block,
     * not including txes).
     *
     * @return true if the block was found, else false
     */
    bool get_block_by_height(uint64_t height, block& blk, size_t* size = nullptr) const;

    /**
     * @brief performs some preprocessing on a group of incoming blocks to speed up verification
     *
     * @param blocks_entry a list of incoming blocks
     * @param blocks the parsed blocks
     * @param checkpoints the parsed checkpoints
     *
     * @return false on erroneous blocks, else true
     */
    bool prepare_handle_incoming_blocks(
            const std::vector<block_complete_entry>& blocks_entry, std::vector<block>& blocks);

    /**
     * @brief incoming blocks post-processing, cleanup, and disk sync
     *
     * @param force_sync if true, and Blockchain is handling syncing to disk, always sync
     *
     * @return true
     */
    bool cleanup_handle_incoming_blocks(bool force_sync = false);

    /**
     * @brief search the blockchain for a transaction by hash
     *
     * @param id the hash to search for
     *
     * @return true if the tx exists, else false
     */
    bool have_tx(const crypto::hash& id) const;

    /**
     * @brief check if any key image in a transaction has already been spent
     *
     * @param tx the transaction to check
     *
     * @return true if any key image is already spent in the blockchain, else false
     */
    bool have_tx_keyimges_as_spent(const transaction& tx) const;

    /**
     * @brief check if a key image is already spent on the blockchain
     *
     * Whenever a transaction output is used as an input for another transaction
     * (a true input, not just one of a mixing set), a key image is generated
     * and stored in the transaction in order to prevent double spending.  If
     * this key image is seen again, the transaction using it is rejected.
     *
     * @param key_im the key image to search for
     *
     * @return true if the key image is already spent in the blockchain, else false
     */
    bool have_tx_keyimg_as_spent(const crypto::key_image& key_im) const;

    /**
     * @brief get the current height of the blockchain
     *
     * @return the height
     */
    uint64_t get_current_blockchain_height() const;

    /**
     * @brief get the height and hash of the most recent block on the blockchain
     *
     * @return pair of the height and hash
     */
    std::pair<uint64_t, crypto::hash> get_tail_id() const;

    /**
     * @brief returns the difficulty target the next block to be added must meet
     *
     * pulse return difficulty for a pulse block
     *
     * @return the target
     */
    difficulty_type get_difficulty_for_next_block(bool pulse);

    /**
     * @brief adds a block to the blockchain
     *
     * Adds a new block to the blockchain.  If the block's parent is not the
     * current top of the blockchain, the block may be added to an alternate
     * chain.  If the block does not belong, is already in the blockchain
     * or an alternate chain, or is invalid, return false.
     *
     * @param bl the block to be added
     * @param bvc metadata about the block addition's success/failure
     * @param checkpoint optional checkpoint if there is one associated with the block
     *
     * @return true on successful addition to the blockchain, else false
     */
    bool add_new_block(
            const block& bl, block_verification_context& bvc, checkpoint_t const* checkpoint);

    /**
     * @brief clears the blockchain and starts a new one
     *
     * @param b the first block in the new chain (the genesis block)
     *
     * @return true on success, else false
     */
    bool reset_and_set_genesis_block(const block& b);

    /**
     * @brief creates a new block to mine against
     *
     * @param b return-by-reference block to be filled in
     * @param miner_address address new coins for the block will go to
     * @param di return-by-reference tells the miner what the difficulty target is
     * @param height return-by-reference tells the miner what height it's mining against
     * @param expected_reward return-by-reference the total reward awarded to the miner finding this
     * block, including transaction fees
     * @param ex_nonce extra data to be added to the miner transaction's extra
     *
     * @return true if block template filled in successfully, else false
     */
    bool create_next_miner_block_template(
            block& b,
            const account_public_address& miner_address,
            difficulty_type& diffic,
            uint64_t& height,
            uint64_t& expected_reward,
            const std::string& ex_nonce);

    /**
     * @brief creates the next block suitable for using in a Pulse enabled network
     *
     * @param b return-by-reference block to be filled in
     * @param block_producer the service node that will receive the block reward
     * @param round the current pulse round the block is being generated for
     * @param validator_bitset the bitset indicating which validators in the quorum are
     * participating in constructing the block.
     * @param ex_nonce extra data to be added to the miner transaction's extra
     *
     * @return true if block template filled in successfully, else false
     */
    bool create_next_pulse_block_template(
            block& b,
            const service_nodes::payout& block_producer,
            uint8_t round,
            uint16_t validator_bitset,
            uint64_t& height);

    /**
     * @brief checks if a block is known about with a given hash
     *
     * This function checks the main chain, alternate chains, and invalid blocks
     * for a block with the given hash
     *
     * @param id the hash to search for
     *
     * @return true if the block is known, else false
     */
    bool have_block(const crypto::hash& id) const;

    /**
     * @brief gets the total number of transactions on the main chain
     *
     * @return the number of transactions on the main chain
     */
    size_t get_total_transactions() const;

    /**
     * @brief gets the hashes for a sample of the blockchain
     *
     * Builds a list of hashes representing certain blocks from the blockchain in reverse
     * chronological order for comparison of chains when synchronizing.  The sampled blocks include
     * the most recent blocks, then drops off exponentially back to the genesis block.
     *
     * Specifically the blocks chosen for height H, are:
     *   - the most recent 11 (H-1, H-2, ..., H-10, H-11)
     *   - base-2 exponential drop off from there, so: H-13, H-17, H-25, etc... (going down to, at
     * smallest, height 1)
     *   - the genesis block (height 0)
     */
    void get_short_chain_history(std::list<crypto::hash>& ids) const;

    /**
     * @brief get recent block hashes for a foreign chain
     *
     * Find the split point between us and foreign blockchain and return
     * (by reference) the most recent common block hash along with up to
     * BLOCKS_IDS_SYNCHRONIZING_DEFAULT_COUNT additional (more recent) hashes.
     *
     * @param qblock_ids the foreign chain's "short history" (see get_short_chain_history)
     * @param hashes the hashes to be returned, return-by-reference
     * @param start_height the start height, return-by-reference
     * @param current_height the current blockchain height, return-by-reference
     * @param clip_pruned whether to constrain results to unpruned data
     *
     * @return true if a block found in common, else false
     */
    bool find_blockchain_supplement(
            const std::list<crypto::hash>& qblock_ids,
            std::vector<crypto::hash>& hashes,
            uint64_t& start_height,
            uint64_t& current_height,
            bool clip_pruned) const;

    /**
     * @brief get recent block hashes for a foreign chain
     *
     * Find the split point between us and foreign blockchain and return
     * (by reference) the most recent common block hash along with up to
     * BLOCKS_IDS_SYNCHRONIZING_DEFAULT_COUNT additional (more recent) hashes.
     *
     * @param qblock_ids the foreign chain's "short history" (see get_short_chain_history)
     * @param resp return-by-reference the split height and subsequent blocks' hashes
     *
     * @return true if a block found in common, else false
     */
    bool find_blockchain_supplement(
            const std::list<crypto::hash>& qblock_ids,
            NOTIFY_RESPONSE_CHAIN_ENTRY::request& resp) const;

    /**
     * @brief find the most recent common point between ours and a foreign chain
     *
     * This function takes a list of block hashes from another node
     * on the network to find where the split point is between us and them.
     * This is used to see what to send another node that needs to sync.
     *
     * @param qblock_ids the foreign chain's "short history" (see get_short_chain_history)
     * @param starter_offset return-by-reference the most recent common block's height
     *
     * @return true if a block found in common, else false
     */
    bool find_blockchain_supplement(
            const std::list<crypto::hash>& qblock_ids, uint64_t& starter_offset) const;

    struct BlockData {
        std::string block_blob;
        crypto::hash miner_tx_hash;                             // null if no miner tx
        std::vector<std::pair<crypto::hash, std::string>> txs;  // hash => tx blob, in block order
    };

    /**
     * @brief get recent blocks for a foreign chain
     *
     * This function gets recent blocks relative to a foreign chain, starting either at
     * a requested height or whatever height is the most recent ours and the foreign
     * chain have in common.
     *
     * @param req_start_block if non-zero, specifies a start point (otherwise find most recent
     * commonality)
     * @param qblock_ids the foreign chain's "short history" (see get_short_chain_history)
     * @param blocks return-by-reference the blocks and their transactions
     * @param total_height return-by-reference our current blockchain height
     * @param start_height return-by-reference the height of the first block returned
     * @param pruned whether to return full or pruned tx blobs
     * @param max_count the max number of blocks to get
     *
     * @return true if a block found in common or req_start_block specified, else false
     */
    bool find_blockchain_supplement(
            const uint64_t req_start_block,
            const std::list<crypto::hash>& qblock_ids,
            std::vector<BlockData>& blocks,
            uint64_t& total_height,
            uint64_t& start_height,
            bool pruned,
            bool get_miner_tx_hash,
            size_t max_count) const;

    /**
     * @brief retrieves a set of blocks and their transactions
     *
     * the request object encapsulates a list of block hashes.  for each block hash, the block is
     * fetched along with all of that block's transactions.
     *
     * @param arg the request
     * @param rsp return-by-reference the response to fill in
     *
     * @return true unless any blocks or transactions are missing
     */
    bool handle_get_blocks(
            NOTIFY_REQUEST_GET_BLOCKS::request& arg, NOTIFY_RESPONSE_GET_BLOCKS::request& rsp);

    /**
     * @brief retrieves a set of transactions
     *
     * The request object encapsulates a list of tx hashes which are fetched and stored in the
     * response object.
     *
     * @param arg the request
     * @param rsp return-by-reference the response to fill in
     *
     * @return true unless any transactions are missing
     */
    bool handle_get_txs(
            NOTIFY_REQUEST_GET_TXS::request& arg, NOTIFY_NEW_TRANSACTIONS::request& rsp);

    /**
     * @brief get number of outputs of an amount past the minimum spendable age
     *
     * @param amount the output amount
     *
     * @return the number of mature outputs
     */
    uint64_t get_num_mature_outputs(uint64_t amount) const;

    /**
     * @brief get the public key for an output
     *
     * @param amount the output amount
     * @param global_index the output amount-global index
     *
     * @return the public key
     */
    crypto::public_key get_output_key(uint64_t amount, uint64_t global_index) const;

    /**
     * @brief gets specific outputs to mix with
     *
     * This function takes an RPC request for outputs to mix with
     * and creates an RPC response with the resultant output indices.
     *
     * Outputs to mix with are specified in the request.
     *
     * @param req the outputs to return
     * @param res return-by-reference the resultant output indices and keys
     *
     * @return true
     */
    bool get_outs(
            const rpc::GET_OUTPUTS_BIN::request& req, rpc::GET_OUTPUTS_BIN::response& res) const;

    /**
     * @brief gets an output's key and unlocked state
     *
     * @param amount in - the output amount
     * @param index in - the output global amount index
     * @param mask out - the output's RingCT mask
     * @param key out - the output's key
     * @param unlocked out - the output's unlocked state
     */
    void get_output_key_mask_unlocked(
            const uint64_t& amount,
            const uint64_t& index,
            crypto::public_key& key,
            rct::key& mask,
            bool& unlocked) const;

    /**
     * @brief gets per block distribution of outputs of a given amount
     *
     * @param amount the amount to get a ditribution for
     * @param from_height the height before which we do not care about the data
     * @param to_height the height after which we do not care about the data
     * @param return-by-reference start_height the height of the first rct output
     * @param return-by-reference distribution the start offset of the first rct output in this
     * block (same as previous if none)
     * @param return-by-reference base how many outputs of that amount are before the stated
     * distribution
     */
    bool get_output_distribution(
            uint64_t amount,
            uint64_t from_height,
            uint64_t to_height,
            uint64_t& start_height,
            std::vector<uint64_t>& distribution,
            uint64_t& base) const;

    /**
     * @brief gets global output indexes that should not be used, i.e. registration tx outputs
     *
     * @param return-by-reference blacklist global indexes of rct outputs to ignore
     */
    void get_output_blacklist(std::vector<uint64_t>& blacklist) const;

    /**
     * @brief gets the global indices for outputs from a given transaction
     *
     * This function gets the global indices for all outputs belonging
     * to a specific transaction.
     *
     * @param tx_id the hash of the transaction to fetch indices for
     * @param indexs return-by-reference the global indices for the transaction's outputs
     * @param n_txes how many txes in a row to get results for
     *
     * @return false if the transaction does not exist, or if no indices are found, otherwise true
     */
    bool get_tx_outputs_gindexs(const crypto::hash& tx_id, std::vector<uint64_t>& indexs) const;
    bool get_tx_outputs_gindexs(
            const crypto::hash& tx_id,
            size_t n_txes,
            std::vector<std::vector<uint64_t>>& indexs) const;

    /**
     * @brief stores the blockchain
     *
     * If Blockchain is handling storing of the blockchain (rather than BlockchainDB),
     * this initiates a blockchain save.
     *
     * @return true unless saving the blockchain fails
     */
    bool store_blockchain();

    /**
     * @brief validates a transaction's inputs
     *
     * validates a transaction's inputs as correctly used and not previously
     * spent.  also returns the hash and height of the most recent block
     * which contains an output that was used as an input to the transaction.
     * The transaction's rct signatures, if any, are expanded.
     *
     * @param tx the transaction to validate
     * @param pmax_used_block_height return-by-reference block height of most recent input
     * @param max_used_block_id return-by-reference block hash of most recent input
     * @param tvc returned information about tx verification
     * @param kept_by_block whether or not the transaction is from a previously-verified block
     * @param key_image_conflicts if specified then don't fail on duplicate key images but instead
     * add them here for the caller to decide on
     *
     * @return false if any input is invalid, otherwise true
     */
    bool check_tx_inputs(
            transaction& tx,
            tx_verification_context& tvc,
            crypto::hash& max_used_block_id,
            uint64_t& pmax_used_block_height,
            std::unordered_set<crypto::key_image>* key_image_conflicts = nullptr,
            bool kept_by_block = false);

    /**
     * @brief get fee quantization mask
     *
     * The dynamic fee may be quantized, to mask out the last decimal places
     *
     * @return the fee quantized mask
     */
    static uint64_t get_fee_quantization_mask();

    /**
     * @brief get dynamic per-kB or -byte fee and the per-output fee for a given
     * block weight and hard fork version.
     *
     * The dynamic per-byte fee is based on the block weight in a past window,
     * and the current block reward. It is expressed as a per-kiB value before
     * v8, and per byte from v8.
     *
     * The per-output fee is a fixed amount per output created in the
     * transaction beginning in Loki hard fork 13 and will be 0 before v13.
     *
     * @param block_reward the current block reward
     * @param median_block_weight the median block weight in the past window
     * @param version hard fork version for rules and constants to use
     *
     * @return pair of {per-size, per-output} fees
     */
    static byte_and_output_fees get_dynamic_base_fee(
            uint64_t block_reward, size_t median_block_weight, hf version);

    /**
     * @brief get dynamic per kB or byte fee estimate for the next few blocks
     *
     * Returns an estimate of the get_dynamic_base_fee() value that will be
     * valid for the next `grace_blocks` blocks.
     *
     * @param grace_blocks number of blocks we want the fee to be valid for
     *
     * @return the {per-size, per-output} fee estimate
     */
    byte_and_output_fees get_dynamic_base_fee_estimate(uint64_t grace_blocks) const;

    /**
     * @brief validate a transaction's fee
     *
     * This function validates the fee is enough for the transaction.
     * This is based on the weight of the transaction, and, after a
     * height threshold, on the average weight of transaction in a past window.
     * From Loki v13 the amount must also include a per-output-created fee.
     *
     * @param tx_weight the transaction weight
     * @param tx_outs the number of outputs created in the transaction
     * @param fee the miner tx fee (any burned component must already be removed, such as is done by
     * get_tx_miner_fee)
     * @param burned the amount of coin burned in this tx
     * @param opts transaction pool options used to pass through required fee/burn amounts
     *
     * @return true if the fee is enough, false otherwise
     */
    bool check_fee(
            size_t tx_weight,
            size_t tx_outs,
            uint64_t fee,
            uint64_t burned,
            const tx_pool_options& opts) const;

    /**
     * @brief check that a transaction's outputs conform to current standards
     *
     * @param tx the transaction to check the outputs of
     * @param tvc returned info about tx verification
     *
     * @return false if any outputs do not conform, otherwise true
     */
    bool check_tx_outputs(const transaction& tx, tx_verification_context& tvc) const;

    /**
     * @brief gets the block weight limit based on recent blocks
     *
     * @return the limit
     */
    uint64_t get_current_cumulative_block_weight_limit() const;

    /**
     * @brief gets the long term block weight for a new block
     *
     * @return the long term block weight
     */
    uint64_t get_next_long_term_block_weight(uint64_t block_weight) const;

    /**
     * @brief gets the block weight median based on recent blocks (same window as for the limit)
     *
     * @return the median
     */
    uint64_t get_current_cumulative_block_weight_median() const;

    /**
     * @brief gets the difficulty of the block with a given height
     *
     * @param i the height
     *
     * @return the difficulty
     */
    uint64_t block_difficulty(uint64_t i) const;

    /**
     * @brief gets blocks based on a list of block hashes
     *
     * @param block_ids a vector of block hashes for which to get the corresponding blocks
     * @param blocks return-by-reference a vector to store result blocks in
     * @param missed_bs optional pointer to an unordered_set to add missed blocks ids to
     *
     * @return false if an unexpected exception occurs, else true
     */
    bool get_blocks(
            const std::vector<crypto::hash>& block_ids,
            std::vector<std::pair<std::string, block>>& blocks,
            std::unordered_set<crypto::hash>* missed_bs = nullptr) const;

    /**
     * @brief gets transactions based on a list of transaction hashes
     *
     * @param txs_ids a vector of hashes for which to get the corresponding transactions
     * @param txs return-by-reference a vector to store result transactions in
     * @param missed_txs optional pointer to an unordered set to add missed transactions ids to
     * @param pruned whether to return full or pruned blobs
     *
     * @return false if an unexpected exception occurs, else true
     */
    bool get_transactions_blobs(
            const std::vector<crypto::hash>& txs_ids,
            std::vector<std::string>& txs,
            std::unordered_set<crypto::hash>* missed_txs = nullptr,
            bool pruned = false) const;
    bool get_split_transactions_blobs(
            const std::vector<crypto::hash>& txs_ids,
            std::vector<std::tuple<crypto::hash, std::string, crypto::hash, std::string>>& txs,
            std::unordered_set<crypto::hash>* missed_txs = nullptr) const;
    bool get_transactions(
            const std::vector<crypto::hash>& txs_ids,
            std::vector<transaction>& txs,
            std::unordered_set<crypto::hash>* missed_txs = nullptr) const;

    /**
     * @brief looks up transactions based on a list of transaction hashes and returns the block
     * height in which they were mined, or 0 if not found on the blockchain.
     *
     * @param txs_ids vector of hashes to look up
     *
     * @return vector of the same length as txs_ids containing the heights corresponding to the
     * given hashes, or 0 if not found.
     */
    std::vector<uint64_t> get_transactions_heights(const std::vector<crypto::hash>& txs_ids) const;

    // debug functions

    /**
     * @brief loads new checkpoints from a file
     *
     * @param file_path the path of the file to look for and load checkpoints from
     *
     * @return false if any enforced checkpoint type fails to load, otherwise true
     */
    bool update_checkpoints_from_json_file(const fs::path& file_path);

    bool update_checkpoint(checkpoint_t const& checkpoint);

    bool get_checkpoint(uint64_t height, checkpoint_t& checkpoint) const;

    // user options, must be called before calling init()

    /**
     * @brief sets various performance options
     *
     * @param maxthreads max number of threads when preparing blocks for addition
     * @param sync_on_blocks whether to sync based on blocks or bytes
     * @param sync_threshold number of blocks/bytes to cache before syncing to database
     * @param sync_mode the ::blockchain_db_sync_mode to use
     * @param fast_sync sync using built-in block hashes as trusted
     */
    void set_user_options(
            uint64_t maxthreads,
            bool sync_on_blocks,
            uint64_t sync_threshold,
            blockchain_db_sync_mode sync_mode,
            bool fast_sync);

    /**
     * @brief Put DB in safe sync mode
     */
    void safesyncmode(const bool onoff);

    /**
     * @brief Get the nettype
     * @return the nettype
     */
    network_type nettype() const { return m_nettype; }

    /**
     * @brief set whether or not to show/print time statistics
     *
     * @param stats the new time stats setting
     */
    void set_show_time_stats(bool stats) { m_show_time_stats = stats; }

    /**
     * @brief gets the network hard fork version of the blockchain at the given height.
     * If height is omitted, uses the current blockchain height.
     *
     * @return the version
     */
    hf get_network_version(std::optional<uint64_t> height = std::nullopt) const;

    /**
     * @brief remove transactions from the transaction pool (if present)
     *
     * @param txids a list of hashes of transactions to be removed
     *
     * @return false if any removals fail, otherwise true
     */
    bool flush_txes_from_pool(const std::vector<crypto::hash>& txids);

    /**
     * @brief return a histogram of outputs on the blockchain
     *
     * @param amounts optional set of amounts to lookup
     * @param unlocked whether to restrict instances to unlocked ones
     * @param recent_cutoff timestamp to consider outputs as recent
     * @param min_count return only amounts with at least that many instances
     *
     * @return a set of amount/instances
     */
    std::map<uint64_t, std::tuple<uint64_t, uint64_t, uint64_t>> get_output_histogram(
            const std::vector<uint64_t>& amounts,
            bool unlocked,
            uint64_t recent_cutoff,
            uint64_t min_count = 0) const;

    /**
     * @brief perform a check on all key images in the blockchain
     *
     * @param std::function the check to perform, pass/fail
     *
     * @return false if any key image fails the check, otherwise true
     */
    bool for_all_key_images(std::function<bool(const crypto::key_image&)>) const;

    /**
     * @brief perform a check on all blocks in the blockchain in the given range
     *
     * @param h1 the start height
     * @param h2 the end height
     * @param std::function the check to perform, pass/fail
     *
     * @return false if any block fails the check, otherwise true
     */
    bool for_blocks_range(
            const uint64_t& h1,
            const uint64_t& h2,
            std::function<bool(uint64_t, const crypto::hash&, const block&)>) const;

    /**
     * @brief perform a check on all transactions in the blockchain
     *
     * @param std::function the check to perform, pass/fail
     * @param bool pruned whether to return pruned txes only
     *
     * @return false if any transaction fails the check, otherwise true
     */
    bool for_all_transactions(
            std::function<bool(const crypto::hash&, const cryptonote::transaction&)>,
            bool pruned) const;

    /**
     * @brief perform a check on all outputs in the blockchain
     *
     * @param std::function the check to perform, pass/fail
     *
     * @return false if any output fails the check, otherwise true
     */
    bool for_all_outputs(
            std::function<bool(
                    uint64_t amount, const crypto::hash& tx_hash, uint64_t height, size_t tx_idx)>)
            const;

    /**
     * @brief perform a check on all outputs of a given amount in the blockchain
     *
     * @param amount the amount to iterate through
     * @param std::function the check to perform, pass/fail
     *
     * @return false if any output fails the check, otherwise true
     */
    bool for_all_outputs(uint64_t amount, std::function<bool(uint64_t height)>) const;

    /// Returns true if we have a BlockchainDB reference, i.e. if we have been initialized
    bool has_db() const { return static_cast<bool>(m_db); }

    /**
     * @brief get a reference to the BlockchainDB in use by Blockchain
     *
     * @return a reference to the BlockchainDB instance
     */
    const BlockchainDB& db() const { return *m_db; }

    /**
     * @brief get a reference to the BlockchainDB in use by Blockchain
     *
     * @return a reference to the BlockchainDB instance
     */
    BlockchainDB& db() { return *m_db; }

    /// A reference to the service node list
    service_nodes::service_node_list& service_node_list;

    /**
     * @brief computes the "short" and "long" hashes for a set of blocks
     *
     * @param height the height of the first block
     * @param blocks the blocks to be hashed
     * @param map return-by-reference the hashes for each block
     */
    void block_longhash_worker(
            uint64_t height,
            const epee::span<const block>& blocks,
            std::unordered_map<crypto::hash, crypto::hash>& map) const;

    /**
     * @brief returns a set of known alternate chains
     *
     * @return a vector of chains
     */
    std::vector<std::pair<block_extended_info, std::vector<crypto::hash>>> get_alternative_chains()
            const;

    bool is_within_compiled_block_hash_area(uint64_t height) const;
    bool is_within_compiled_block_hash_area() const {
        return is_within_compiled_block_hash_area(m_db->height());
    }
    uint64_t prevalidate_block_hashes(uint64_t height, const std::vector<crypto::hash>& hashes);
    uint32_t get_blockchain_pruning_seed() const { return m_db->get_blockchain_pruning_seed(); }
    bool prune_blockchain(uint32_t pruning_seed = 0);
    bool update_blockchain_pruning();
    bool check_blockchain_pruning();

    uint64_t get_immutable_height() const;

    bool calc_batched_governance_reward(uint64_t height, uint64_t& reward) const;

    void lock() const { m_blockchain_lock.lock(); }
    void unlock() const { m_blockchain_lock.unlock(); }
    bool try_lock() const { return m_blockchain_lock.try_lock(); }

    void cancel();

    /**
     * @brief called when we see a tx originating from a block
     *
     * Used for handling txes from historical blocks in a fast way
     */
    void on_new_tx_from_block(const cryptonote::transaction& tx);

    /**
     * @brief add a hook called during new block handling; should throw to abort adding the block.
     */
    void hook_block_add(BlockAddHook hook) { m_block_add_hooks.push_back(std::move(hook)); }
    /**
     * @brief add a hook to be called after a new block has been added to the (main) chain.  Unlike
     * the above, this only fires after addition is complete and successful, while the above hook is
     * part of the addition process.
     */
    void hook_block_post_add(BlockPostAddHook hook) {
        m_block_post_add_hooks.push_back(std::move(hook));
    }
    /**
     * @brief add a hook called when blocks are removed from the chain.
     */
    void hook_blockchain_detached(BlockchainDetachedHook hook) {
        m_blockchain_detached_hooks.push_back(std::move(hook));
    }
    /**
     * @brief add a hook called during startup and re-initialization
     */
    void hook_init(InitHook hook) { m_init_hooks.push_back(std::move(hook)); }
    /**
     * @brief add a hook to be called to validate miner txes; should throw if the miner tx is
     * invalid.
     */
    void hook_validate_miner_tx(ValidateMinerTxHook hook) {
        m_validate_miner_tx_hooks.push_back(std::move(hook));
    }
    /**
     * @brief add a hook to be called when adding an alt-chain block; should throw to abort adding.
     */
    void hook_alt_block_add(BlockAddHook hook) { m_alt_block_add_hooks.push_back(std::move(hook)); }

    /**
     * @brief returns the timestamps of the last N blocks
     */
    std::vector<time_t> get_last_block_timestamps(unsigned int blocks) const;

    /**
     * @brief removes blocks from the top of the blockchain
     *
     * @param nblocks number of blocks to be removed
     */
    void pop_blocks(uint64_t nblocks);

    /**
     * Rolls back the blockchain to the given height when necessary for admitting blink
     * transactions.
     */
    bool blink_rollback(uint64_t rollback_height);

    ons::name_system_db& name_system_db() { return m_ons_db; }

    const ons::name_system_db& name_system_db() const { return m_ons_db; }

    cryptonote::BlockchainSQLite& sqlite_db();

    /// NOTE: unchecked access; should only be called in service node more where this is guaranteed
    /// to be set.
    eth::L2Tracker& l2_tracker() { return *m_l2_tracker; }

    /// Returns a pointer to the L2 tracker; this will be nullptr if no L2 tracker is configured (an
    /// L2 tracker is required for service nodes, and optional for nodes not running in service node
    /// mode).
    eth::L2Tracker* maybe_l2_tracker() { return m_l2_tracker; }

    /**
     * @brief flush the invalid blocks set
     */
    void flush_invalid_blocks();

    /**
     * @brief returns true if the given pubkey is removable (according to oxend) from the L2
     * contract.
     *
     * To be removable, the node must either be:
     *
     * 1) In the oxend recently removed nodes list.  These are nodes that the oxend network has
     *    removed from service, either by failing requirements and getting deregistered, or by
     *    successfully reaching the end of a requested unlock period.
     *
     * 2) Nodes that are in the contract but neither in the current registered service node list nor
     *    with an incoming but not-yet-confirmed new service node registration event working its way
     *    through the Oxen chain.  This is not a normal case: it indicates that some network
     *    failure, bug, or partially invalid registration (such as an invalid Ed25519 signature)
     *    resulting in the entry missing from oxend's service node list.  Note that this case relies
     *    on infrequently updated cached contract service node pubkey lists; it will not reliably
     *    include a node until at least an hour has passed (or, more precisely,
     *    `netconf.L2_NODE_LIST_PURGE_BLOCKS` L2 blocks) since the event that triggered it (but in
     *    general this is okay because the contract also requires a 2 hour post-registration delay
     *    before a liquidation signature will be accepted).
     *
     *    Note also that there are false positives possible with this second case: in particular a
     *    brand new registration that has not yet been observed and mined into a pulse block will
     *    get returned as a true "case 2" result.  We do not worry about that case here, however, as
     *    such a signature is made useless by the contract refusing to accept removals within two
     *    hours of a contract service node registration, nor can it be saved for submission later
     *    (as the signature embeds a timestamp that must be close to current for the contract to
     *    accept).
     *
     *    Note also that "Case 2" removable nodes are only available if this oxend is running with a
     *    working L2 tracker.
     *
     * Note that both cases can return true for nodes that aren't in the contract anymore: in
     * particular, case 1 nodes might return true for an emitted but not yet confirmed final removal
     * event; and case 2 could return true due to using a cached, potentially outdated contract node
     * list.
     *
     * The `liquidatable` parameter can be specified as true to query for removable and liquidatable
     * (and is equivalent to using the `is_node_liquidatable` method).
     */
    bool is_node_removable(const eth::bls_public_key& bls_pubkey, bool liquidatable = false) const;

    /**
     * @brief Returns true if the node is liquidatable -- that is, removable (see
     * `is_node_removable`) but not in the recently removed node removable-but-not-liquidating
     * waiting period.  (Liquidatable is always a subset of removable).
     */
    bool is_node_liquidatable(const eth::bls_public_key& bls_pubkey) const {
        return is_node_removable(bls_pubkey, true);
    }

    /**
     * @brief Get the sets of nodes that are removable or liquidatable from the L2 contract.
     *
     * @return unordered map of removable nodes.  A key being present (regardless of value)
     * indicates the node is removable; the value being true indicates the node is also
     * liquidatable.
     *
     * This function is effectively an enumerable version of
     * `is_node_removable`/`is_node_liquidatable`: it returns all nodes in the recently removed
     * list, plus all nodes that are in the contract but *not* in the current registered service
     * node list.  See `is_node_removable` and `is_node_liquidatable` for more details on these
     * conditions.
     *
     * TODO: create a batch removal mechanism using this to allow less churn/lower tx fees when
     * there are multiple liquidatable or removable nodes.
     */
    std::unordered_map<eth::bls_public_key, bool> get_removable_nodes() const;

    tx_memory_pool& tx_pool;

#ifndef IN_UNIT_TESTS
  private:
#endif

    struct block_pow_verified {
        bool valid;
        bool precomputed;
        bool per_block_checkpointed;
        crypto::hash proof_of_work;
    };

    /**
     * @brief Verify block proof of work against the expected difficulty
     */
    block_pow_verified verify_block_pow(
            cryptonote::block const& blk,
            difficulty_type difficulty,
            uint64_t chain_height,
            bool alt_block);
    bool basic_block_checks(cryptonote::block const& blk, bool alt_block);

    struct block_template_info {
        bool is_miner;
        account_public_address miner_address;
        service_nodes::payout service_node_payout;
    };

    bool create_block_template_internal(
            block& b,
            block_template_info const& info,
            difficulty_type& di,
            uint64_t& height,
            uint64_t& expected_reward,
            const std::string& ex_nonce);

    bool load_missing_blocks_into_oxen_subsystems(const std::atomic<bool>* abort = nullptr);

    // TODO: evaluate whether or not each of these typedefs are left over from blockchain_storage
    typedef std::unordered_set<crypto::key_image> key_images_container;

    typedef std::vector<block_extended_info> blocks_container;

    typedef std::unordered_map<crypto::hash, block_extended_info> blocks_ext_by_hash;

    std::unique_ptr<BlockchainDB> m_db;

    ons::name_system_db m_ons_db;
    std::unique_ptr<cryptonote::BlockchainSQLite> m_sqlite_db;

    mutable std::recursive_mutex m_blockchain_lock;  // TODO: add here reader/writer lock

    // main chain
    size_t m_current_block_cumul_weight_limit;
    size_t m_current_block_cumul_weight_median;

    // metadata containers
    std::unordered_map<
            crypto::hash,
            std::unordered_map<crypto::key_image, std::vector<output_data_t>>>
            m_scan_table;
    std::unordered_map<crypto::hash, crypto::hash> m_blocks_longhash_table;

    // Keccak hashes for each block and for fast pow checking
    std::vector<crypto::hash> m_blocks_hash_of_hashes;
    std::vector<crypto::hash> m_blocks_hash_check;
    std::vector<crypto::hash> m_blocks_txs_check;

    blockchain_db_sync_mode m_db_sync_mode;
    bool m_fast_sync;
    bool m_show_time_stats;
    bool m_db_default_sync;
    bool m_db_sync_on_blocks;
    uint64_t m_db_sync_threshold;
    unsigned m_max_prepare_blocks_threads;
    std::chrono::nanoseconds m_fake_pow_calc_time;
    std::chrono::nanoseconds m_fake_scan_time;
    uint64_t m_sync_counter;
    uint64_t m_bytes_to_sync;

    uint64_t m_long_term_block_weights_window;
    uint64_t m_long_term_effective_median_block_weight;
    mutable crypto::hash m_long_term_block_weights_cache_tip_hash;
    mutable epee::misc_utils::rolling_median_t<uint64_t>
            m_long_term_block_weights_cache_rolling_median;

    // NOTE: PoW/Difficulty Cache
    // Before HF16, we use timestamps and difficulties only.
    // After HF16, we check if the state of block producing in Pulse and return
    // the PoW difficulty or the fixed Pulse difficulty if we're still eligible
    // for Pulse blocks.
    struct {
        std::mutex m_difficulty_lock;

        // NOTE: PoW Difficulty Calculation Metadata
        std::vector<uint64_t> m_timestamps;
        std::vector<difficulty_type> m_difficulties;

        // NOTE: Cache Invalidation Checks
        uint64_t m_timestamps_and_difficulties_height{0};
        crypto::hash m_difficulty_for_next_block_top_hash{};
        difficulty_type m_difficulty_for_next_miner_block{1};
    } m_cache;

    boost::asio::io_service m_async_service;
    std::thread m_async_thread;
    std::unique_ptr<boost::asio::io_service::work> m_async_work_idle;

    // some invalid blocks
    std::set<crypto::hash> m_invalid_blocks;

    std::vector<BlockAddHook> m_block_add_hooks;
    std::vector<BlockAddHook> m_alt_block_add_hooks;
    std::vector<BlockPostAddHook> m_block_post_add_hooks;
    std::vector<BlockchainDetachedHook> m_blockchain_detached_hooks;
    std::vector<InitHook> m_init_hooks;
    std::vector<ValidateMinerTxHook> m_validate_miner_tx_hooks;

    checkpoints m_checkpoints;

    eth::L2Tracker* m_l2_tracker;
    network_type m_nettype;
    bool m_offline;
    difficulty_type m_fixed_difficulty;

    std::atomic<bool> m_cancel;

    // block template cache
    block m_btc;
    account_public_address m_btc_address;
    std::string m_btc_nonce;
    uint64_t m_btc_height;
    uint64_t m_btc_pool_cookie;
    uint64_t m_btc_expected_reward;
    bool m_btc_valid;

    bool m_batch_success;

    // for prepare_handle_incoming_blocks
    uint64_t m_prepare_height;
    uint64_t m_prepare_nblocks;
    std::vector<block>* m_prepare_blocks;

    std::chrono::steady_clock::time_point last_outdated_warning = {};
    std::mutex last_outdated_warning_mutex;

    /**
     * @brief collects the keys for all outputs being "spent" as an input
     *
     * This function makes sure that each "input" in an input (mixins) exists
     * and collects the public key for each from the transaction it was included in
     * via the visitor passed to it.
     *
     * If pmax_related_block_height is not NULL, its value is set to the height
     * of the most recent block which contains an output used in the input set
     *
     * @tparam visitor_t a class encapsulating tx is unlocked and collect tx key
     * @param tx_in_to_key a transaction input instance
     * @param vis an instance of the visitor to use
     * @param tx_prefix_hash the hash of the associated transaction_prefix
     * @param pmax_related_block_height return-by-pointer the height of the most recent block in the
     * input set
     * @param tx_version version of the tx, if > 1 we also get commitments
     *
     * @return false if any keys are not found or any inputs are not unlocked, otherwise true
     */
    template <class visitor_t>
    bool scan_outputkeys_for_indexes(
            const txin_to_key& tx_in_to_key,
            visitor_t& vis,
            const crypto::hash& tx_prefix_hash,
            uint64_t* pmax_related_block_height = NULL) const;

    /**
     * @brief collect output public keys of a transaction input set
     *
     * This function locates all outputs associated with a given input set (mixins)
     * and validates that they exist and are usable
     * (unlocked, unspent is checked elsewhere).
     *
     * If pmax_related_block_height is not NULL, its value is set to the height
     * of the most recent block which contains an output used in the input set
     *
     * @param txin the transaction input
     * @param tx_prefix_hash the transaction prefix hash, for caching organization
     * @param output_keys return-by-reference the public keys of the outputs in the input set
     * @param pmax_related_block_height return-by-pointer the height of the most recent block in the
     * input set
     *
     * @return false if any output is not yet unlocked, or is missing, otherwise true
     */
    bool check_tx_input(
            const txin_to_key& txin,
            const crypto::hash& tx_prefix_hash,
            std::vector<rct::ctkey>& output_keys,
            uint64_t* pmax_related_block_height);

    /**
     * @brief validate a transaction's inputs and their keys
     *
     * This function validates transaction inputs and their keys.  Previously
     * it also performed double spend checking, but that has been moved to its
     * own function.
     * The transaction's rct signatures, if any, are expanded.
     *
     * If pmax_related_block_height is not NULL, its value is set to the height
     * of the most recent block which contains an output used in any input set
     *
     * Currently this function calls ring signature validation for each
     * transaction.
     *
     * This fails if called on a non-standard metadata transaction such as a deregister; you
     * generally want to call check_tx() instead, which calls this if appropriate.
     *
     * @param tx the transaction to validate
     * @param tvc returned information about tx verification
     * @param pmax_related_block_height return-by-pointer the height of the most recent block in the
     * input set
     * @param key_image_conflicts if specified then don't fail on duplicate key images but instead
     * add them here for the caller to decide on
     *
     * @return false if any validation step fails, otherwise true
     */
    bool check_tx_inputs(
            transaction& tx,
            tx_verification_context& tvc,
            uint64_t* pmax_used_block_height = nullptr,
            std::unordered_set<crypto::key_image>* key_image_conflicts = nullptr);

    /**
     * @brief performs a blockchain reorganization according to the longest chain rule
     *
     * This function aggregates all the actions necessary to switch to a
     * newly-longer chain.  If any step in the reorganization process fails,
     * the blockchain is reverted to its previous state.
     *
     * @param alt_chain the chain to switch to
     * @param keep_disconnected_chain whether or not to keep the old chain as an alternate
     *
     * @return false if the reorganization fails, otherwise true
     */
    bool switch_to_alternative_blockchain(
            const std::list<block_extended_info>& alt_chain, bool keep_disconnected_chain);

    /**
     * @brief removes the most recent block from the blockchain
     *
     * @return the block removed
     */
    block pop_block_from_blockchain(bool pop_batching_rewards);

    /**
     * @brief validate and add a new block to the end of the blockchain
     *
     * When a block is given to Blockchain to be added to the blockchain, it
     * is passed here if it is determined to belong at the end of the current
     * chain.
     *
     * @param bl the block to be added
     * @param id the hash of the block
     * @param bvc metadata concerning the block's validity
     * @param notify if set to true, fires post-add hooks on success
     *
     * @return true if the block was added successfully, otherwise false
     */
    bool handle_block_to_main_chain(
            const block& bl,
            const crypto::hash& id,
            block_verification_context& bvc,
            checkpoint_t const* checkpoint,
            bool notify = true);

    /**
     * @brief validate and add a new block to an alternate blockchain
     *
     * If a block to be added does not belong to the main chain, but there
     * is an alternate chain to which it should be added, that is handled
     * here.
     *
     * @param b the block to be added
     * @param id the hash of the block
     * @param bvc metadata concerning the block's validity
     *
     * @return true if the block was added successfully, otherwise false
     */
    bool handle_alternative_block(
            const block& b,
            const crypto::hash& id,
            block_verification_context& bvc,
            checkpoint_t const* checkpoint);

    /**
     * @brief builds a list of blocks connecting a block to the main chain
     *
     * @param prev_id the block hash of the tip of the alt chain
     * @param alt_chain the chain to be added to
     * @param timestamps returns the timestamps of previous blocks
     * @param bvc the block verification context for error return
     *
     * @return true on success, false otherwise
     */
    bool build_alt_chain(
            const crypto::hash& prev_id,
            std::list<block_extended_info>& alt_chain,
            std::vector<uint64_t>& timestamps,
            block_verification_context& bvc,
            int* num_alt_checkpoints,
            int* num_checkpoints);

    /**
     * @brief gets the difficulty requirement for a new block on an alternate chain
     *
     * @param alt_chain the chain to be added to
     * @param bei the block being added (and metadata, see ::block_extended_info)
     * @param pulse if the block to add is a block generated by Pulse
     *
     * @return the difficulty requirement
     */
    difficulty_type get_difficulty_for_alternative_chain(
            const std::list<block_extended_info>& alt_chain, uint64_t height, bool pulse) const;

    /**
     * @brief sanity checks a block's rewards (in the block in HF21+, or the miner transaction
     * before HF21) before validating an entire block.
     *
     * This function merely checks basic things like the structure of the miner
     * transaction, the unlock time, and that the amount doesn't overflow.
     *
     * @param b the block containing the miner transaction
     * @param height the height at which the block will be added
     * @param hf_version the consensus rules to apply
     *
     * @return false if anything is found wrong with the miner transaction, otherwise true
     */
    bool prevalidate_block_rewards(const block& b, uint64_t height, hf hf_version);

    /**
     * @brief validates a miner (coinbase) transaction
     *
     * This function makes sure that the miner calculated his reward correctly
     * and that his miner transaction totals reward + fee.
     *
     * @param b the block containing the miner transaction to be validated
     * @param cumulative_block_weight the block's weight
     * @param fee the total fees collected in the block
     * @param base_reward return-by-reference the new block's generated coins
     * @param already_generated_coins the amount of currency generated prior to this block
     * @param version hard fork version for that transaction
     *
     * @return false if anything is found wrong with the miner transaction, otherwise true
     */
    bool validate_block_rewards(
            const block& b,
            size_t cumulative_block_weight,
            uint64_t fee,
            uint64_t& base_reward,
            uint64_t already_generated_coins,
            hf version);

    /**
     * @brief returns the (consensus-required) calculated ETH reward for the next block.
     *
     * This uses the smallest `l2_reward` value of the last L2_REWARD_CONSENSUS_BLOCKS blocks before
     * `height`.  Returns 0 if given a height before the feature::ETH_BLS hard fork; returns the
     * hard-coded ETH_BLS_INITIAL_REWARD for the initial ETH_BLS blocks; throws if any of the
     * previous L2_REWARD_CONSENSUS_BLOCKS blocks don't exist.
     *
     * @param height the height of the block to query; the L2_REWARD_CONSENSUS_BLOCKS before this
     * height must exist in the blockchain db.
     *
     * @return the block reward that applies for the given height.
     */
    uint64_t eth_consensus_reward(uint64_t height) const;

    /**
     * @brief returns the (consensus-required) minimum and maximum l2_reward values for a block of
     * the given height.
     *
     * The caps are defined by allowing a maximum relative change from the previous block's
     * l2_reward value, based on the L2_REWARD_MAX_{DE,IN}CREASE_DIVISOR constants.
     *
     * @param height the height of the block being checked; the previous block (height-1) must exist
     * in the blockchain db.
     *
     * @returns pair of [min, max] l2_reward values, or [0,0] if this is not a ETH_BLS block height.
     */
    std::pair<uint64_t, uint64_t> l2_reward_range(uint64_t height) const;

    /**
     * @brief reverts the blockchain to its previous state following a failed switch
     *
     * If Blockchain fails to switch to an alternate chain when it means
     * to do so, this function reverts the blockchain to how it was before
     * the attempted switch.
     *
     * @param original_chain the chain to switch back to
     * @param rollback_height the height to revert to before appending the original chain
     *
     * @return false if something goes wrong with reverting (very bad), otherwise true
     */
    bool rollback_blockchain_switching(
            const std::list<block_and_checkpoint>& original_chain, uint64_t rollback_height);

    /**
     * @brief gets recent block weights for median calculation
     *
     * get the block weights of the last <count> blocks, and return by reference <weights>.
     *
     * @param weights return-by-reference the list of weights
     * @param count the number of blocks to get weights for
     */
    void get_last_n_blocks_weights(std::vector<uint64_t>& weights, size_t count) const;

    /**
     * @brief gets block long term weight median
     *
     * get the block long term weight median of <count> blocks starting at <start_height>
     *
     * @param start_height the block height of the first block to query
     * @param count the number of blocks to get weights for
     *
     * @return the long term median block weight
     */
    uint64_t get_long_term_block_weight_median(uint64_t start_height, size_t count) const;

    /**
     * @brief checks if a transaction is unlocked (its outputs spendable)
     *
     * This function checks to see if an output is unlocked.
     * unlock_time is either a block index or a unix time.
     *
     * @param unlock_time the unlock parameter (height or time)
     *
     * @return true if spendable, otherwise false
     */
    bool is_output_spendtime_unlocked(uint64_t unlock_time) const;

    /**
     * @brief stores an invalid block in a separate container
     *
     * Storing invalid blocks allows quick dismissal of the same block
     * if it is seen again.
     *
     * @param bl the invalid block
     * @param h the block's hash
     *
     * @return false if the block cannot be stored for some reason, otherwise true
     */
    bool add_block_as_invalid(const cryptonote::block& block);

    /**
     * @brief checks a block's timestamp
     *
     * This function grabs the timestamps from the most recent <n> blocks,
     * where n = BLOCKCHAIN_TIMESTAMP_CHECK_WINDOW.  If there are not those many
     * blocks in the blockchain, the timestap is assumed to be valid.  If there
     * are, this function returns:
     *   true if the block's timestamp is not less than the timestamp of the
     *       median of the selected blocks
     *   false otherwise
     *
     * @param b the block to be checked
     * @param median_ts return-by-reference the median of timestamps
     *
     * @return true if the block's timestamp is valid, otherwise false
     */
    bool check_block_timestamp(const block& b, uint64_t& median_ts) const;
    bool check_block_timestamp(const block& b) const {
        uint64_t median_ts;
        return check_block_timestamp(b, median_ts);
    }

    /**
     * @brief checks a block's timestamp
     *
     * If the block is not more recent than the median of the recent
     * timestamps passed here, it is considered invalid.
     *
     * @param timestamps a list of the most recent timestamps to check against
     * @param b the block to be checked
     *
     * @return true if the block's timestamp is valid, otherwise false
     */
    bool check_block_timestamp(
            std::vector<uint64_t> timestamps, const block& b, uint64_t& median_ts) const;
    bool check_block_timestamp(std::vector<uint64_t> timestamps, const block& b) const {
        uint64_t median_ts;
        return check_block_timestamp(std::move(timestamps), b, median_ts);
    }

    /**
     * @brief get the "adjusted time"
     *
     * Currently this simply returns the current time according to the
     * user's machine.
     *
     * @return the current time
     */
    uint64_t get_adjusted_time() const;

    /**
     * @brief finish an alternate chain's timestamp window from the main chain
     *
     * for an alternate chain, get the timestamps from the main chain to complete
     * the needed number of timestamps for the BLOCKCHAIN_TIMESTAMP_CHECK_WINDOW.
     *
     * @param start_height the alternate chain's attachment height to the main chain
     * @param timestamps return-by-value the timestamps set to be populated
     *
     * @return true unless start_height is greater than the current blockchain height
     */
    bool complete_timestamps_vector(uint64_t start_height, std::vector<uint64_t>& timestamps) const;

    /**
     * @brief calculate the block weight limit for the next block to be added
     *
     * @param long_term_effective_median_block_weight optionally return that value
     *
     * @return true
     */
    bool update_next_cumulative_weight_limit(
            uint64_t* long_term_effective_median_block_weight = NULL);
    void return_tx_to_pool(std::vector<std::pair<transaction, std::string>>& txs);

    /**
     * @brief make sure a transaction isn't attempting a double-spend
     *
     * @param tx the transaction to check
     * @param keys_this_block a cumulative list of spent keys for the current block
     *
     * @return false if a double spend was detected, otherwise true
     */
    bool check_for_double_spend(const transaction& tx, key_images_container& keys_this_block) const;

    /**
     * @brief validates a transaction input's ring signature
     *
     * @param tx_prefix_hash the transaction prefix' hash
     * @param key_image the key image generated from the true input
     * @param pubkeys the public keys for each input in the ring signature
     * @param sig the signature generated for each input in the ring signature
     * @param result false if the ring signature is invalid, otherwise true
     */
    void check_ring_signature(
            const crypto::hash& tx_prefix_hash,
            const crypto::key_image& key_image,
            const std::vector<rct::ctkey>& pubkeys,
            const std::vector<crypto::signature>& sig,
            uint64_t& result) const;

    /**
     * @brief loads block hashes from compiled-in data set
     *
     * A (possibly empty) set of block hashes can be compiled into the
     * monero daemon binary.  This function loads those hashes into
     * a useful state.
     *
     * @param get_checkpoints if set, will be called to get checkpoints data
     */
    void load_compiled_in_block_hashes(const GetCheckpointsCallback& get_checkpoints);

    /**
     * @brief expands v2 transaction data from blockchain
     *
     * RingCT transactions do not transmit some of their data if it
     * can be reconstituted by the receiver. This function expands
     * that implicit data.
     */
    bool expand_transaction_2(
            transaction& tx,
            const crypto::hash& tx_prefix_hash,
            const std::vector<std::vector<rct::ctkey>>& pubkeys) const;

    /**
     * @brief invalidates any cached block template
     */
    void invalidate_block_template_cache();

    /**
     * @brief stores a new cached block template
     *
     * At some point, may be used to push an update to miners
     */
    void cache_block_template(
            const block& b,
            const cryptonote::account_public_address& address,
            const std::string& nonce,
            const difficulty_type& diff,
            uint64_t height,
            uint64_t expected_reward,
            uint64_t pool_cookie);
};
}  // namespace cryptonote

// Copyright (c) 2019, The Monero Project
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

#define IN_UNIT_TESTS

#include "gtest/gtest.h"
#include "cryptonote_core/blockchain.h"
#include "cryptonote_core/tx_pool.h"
#include "cryptonote_core/cryptonote_core.h"
#include "cryptonote_core/uptime_proof.h"
#include "blockchain_utilities/blockchain_objects.h"
#include "blockchain_db/sqlite/db_sqlite.h"
#include "blockchain_db/testdb.h"

#define TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW 5000

namespace
{

class TestDB: public cryptonote::BaseTestDB
{
private:
  struct block_t
  {
    size_t weight;
    uint64_t long_term_weight;
  };

public:
  TestDB() { m_open = true; }

  virtual void add_block( const cryptonote::block& blk
                        , size_t block_weight
                        , uint64_t long_term_block_weight
                        , const cryptonote::difficulty_type& cumulative_difficulty
                        , const uint64_t& coins_generated
                        , uint64_t num_rct_outs
                        , const crypto::hash& blk_hash
                        ) override {
    blocks.push_back({block_weight, long_term_block_weight});
  }
  virtual uint64_t height() const override { return blocks.size(); }
  virtual size_t get_block_weight(const uint64_t &h) const override { return blocks[h].weight; }
  virtual uint64_t get_block_long_term_weight(const uint64_t &h) const override { return blocks[h].long_term_weight; }
  virtual std::vector<uint64_t> get_block_weights(uint64_t start_height, size_t count) const override {
    std::vector<uint64_t> ret;
    ret.reserve(count);
    while (count-- && start_height < blocks.size()) ret.push_back(blocks[start_height++].weight);
    return ret;
  }
  virtual std::vector<uint64_t> get_long_term_block_weights(uint64_t start_height, size_t count) const override {
    std::vector<uint64_t> ret;
    ret.reserve(count);
    while (count-- && start_height < blocks.size()) ret.push_back(blocks[start_height++].long_term_weight);
    return ret;
  }
  virtual crypto::hash get_block_hash_from_height(const uint64_t &height) const override {
    crypto::hash hash{};
    *(uint64_t*)hash.data() = height;
    return hash;
  }
  virtual crypto::hash top_block_hash(uint64_t *block_height = NULL) const override {
    uint64_t h = height();
    crypto::hash top{};
    if (h)
      *(uint64_t*)top.data() = h - 1;
    if (block_height)
      *block_height = h - 1;
    return top;
  }
  virtual void pop_block(cryptonote::block &blk, std::vector<cryptonote::transaction> &txs) override { blocks.pop_back(); }

private:
  std::vector<block_t> blocks;
};

static uint32_t lcg_seed = 0;

static uint32_t lcg()
{
  lcg_seed = (lcg_seed * 0x100000001b3 + 0xcbf29ce484222325) & 0xffffffff;
  return lcg_seed;
}

}

#define PREFIX_WINDOW(hf_version,window) \
  blockchain_objects_t bc_objects = {}; \
  const std::vector<cryptonote::hard_fork> hard_forks{ \
    {cryptonote::hf::hf7,0,0,0}, {hf_version,0,1,0}}; \
  const cryptonote::test_options test_options = { \
    hard_forks, \
    window, \
  }; \
  cryptonote::Blockchain *bc = &bc_objects.m_blockchain; \
  auto sqliteDB = std::make_unique<cryptonote::BlockchainSQLite>(cryptonote::network_type::FAKECHAIN, ":memory:"); \
  bool r = bc->init(std::make_unique<TestDB>(), test_options, sqliteDB.release()); \
  ASSERT_TRUE(r)

#define PREFIX(hf_version) PREFIX_WINDOW(hf_version, TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW)

TEST(long_term_block_weight, empty_short)
{
  PREFIX(cryptonote::hf::hf9_service_nodes);

  ASSERT_TRUE(bc->update_next_cumulative_weight_limit());

  ASSERT_EQ(bc->get_current_cumulative_block_weight_median(), cryptonote::BLOCK_GRANTED_FULL_REWARD_ZONE_V5);
  ASSERT_EQ(bc->get_current_cumulative_block_weight_limit(), cryptonote::BLOCK_GRANTED_FULL_REWARD_ZONE_V5 * 2);
}

static cryptonote::block empty_block() {
    cryptonote::block b{};
    b.miner_tx.emplace();
    return b;
}

TEST(long_term_block_weight, identical_before_fork)
{
  PREFIX(cryptonote::hf::hf9_service_nodes);

  for (uint64_t h = 1; h < 10 * TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW; ++h)
  {
    size_t w = h < cryptonote::REWARD_BLOCKS_WINDOW ? cryptonote::BLOCK_GRANTED_FULL_REWARD_ZONE_V5 : bc->get_current_cumulative_block_weight_limit();
    uint64_t ltw = bc->get_next_long_term_block_weight(w);
    bc->db().add_block(std::make_pair(empty_block(), ""), w, ltw, h, h, {});
    ASSERT_TRUE(bc->update_next_cumulative_weight_limit());
  }
  for (uint64_t h = 0; h < 10 * TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW; ++h)
  {
    ASSERT_EQ(bc->db().get_block_long_term_weight(h), bc->db().get_block_weight(h));
  }
}

TEST(long_term_block_weight, identical_after_fork_before_long_term_window)
{
  PREFIX(cryptonote::feature::LONG_TERM_BLOCK_WEIGHT);

  for (uint64_t h = 1; h <= TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW; ++h)
  {
    size_t w = h < TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW ? cryptonote::BLOCK_GRANTED_FULL_REWARD_ZONE_V5 : bc->get_current_cumulative_block_weight_limit();
    uint64_t ltw = bc->get_next_long_term_block_weight(w);
    bc->db().add_block(std::make_pair(empty_block(), ""), w, ltw, h, h, {});
    ASSERT_TRUE(bc->update_next_cumulative_weight_limit());
  }
  for (uint64_t h = 0; h < TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW; ++h)
  {
    ASSERT_EQ(bc->db().get_block_long_term_weight(h), bc->db().get_block_weight(h));
  }
}

TEST(long_term_block_weight, ceiling_at_30000000)
{
  PREFIX(cryptonote::feature::LONG_TERM_BLOCK_WEIGHT);

  for (uint64_t h = 0; h < TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW + TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW / 2 - 1; ++h)
  {
    size_t w = h < TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW ? cryptonote::BLOCK_GRANTED_FULL_REWARD_ZONE_V5 : bc->get_current_cumulative_block_weight_limit();
    uint64_t ltw = bc->get_next_long_term_block_weight(w);
    bc->db().add_block(std::make_pair(empty_block(), ""), w, ltw, h, h, {});
    ASSERT_TRUE(bc->update_next_cumulative_weight_limit());
  }
  ASSERT_EQ(bc->get_current_cumulative_block_weight_median(), 15000000);
  ASSERT_EQ(bc->get_current_cumulative_block_weight_limit(), 30000000);
}

TEST(long_term_block_weight, multi_pop)
{
  PREFIX(cryptonote::feature::LONG_TERM_BLOCK_WEIGHT);

  for (uint64_t h = 1; h <= TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW + 20; ++h)
  {
    size_t w = h < TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW ? cryptonote::BLOCK_GRANTED_FULL_REWARD_ZONE_V5 : bc->get_current_cumulative_block_weight_limit();
    uint64_t ltw = bc->get_next_long_term_block_weight(w);
    bc->db().add_block(std::make_pair(empty_block(), ""), w, ltw, h, h, {});
    ASSERT_TRUE(bc->update_next_cumulative_weight_limit());
  }

  const uint64_t effective_median = bc->get_current_cumulative_block_weight_median();
  const uint64_t effective_limit = bc->get_current_cumulative_block_weight_limit();

  const uint64_t num_pop = 4;
  for (uint64_t h = 0; h < num_pop; ++h)
  {
    size_t w = bc->get_current_cumulative_block_weight_limit();
    uint64_t ltw = bc->get_next_long_term_block_weight(w);
    bc->db().add_block(std::make_pair(empty_block(), ""), w, ltw, h, h, {});
    ASSERT_TRUE(bc->update_next_cumulative_weight_limit());
  }

  cryptonote::block b;
  std::vector<cryptonote::transaction> txs;
  for (uint64_t h = 0; h < num_pop; ++h)
    bc->db().pop_block(b, txs);
  ASSERT_TRUE(bc->update_next_cumulative_weight_limit());

  ASSERT_EQ(effective_median, bc->get_current_cumulative_block_weight_median());
  ASSERT_EQ(effective_limit, bc->get_current_cumulative_block_weight_limit());
}

TEST(long_term_block_weight, multiple_updates)
{
  PREFIX(cryptonote::feature::LONG_TERM_BLOCK_WEIGHT);

  for (uint64_t h = 1; h <= 3 * TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW; ++h)
  {
    size_t w = h < TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW ? cryptonote::BLOCK_GRANTED_FULL_REWARD_ZONE_V5 : bc->get_current_cumulative_block_weight_limit();
    uint64_t ltw = bc->get_next_long_term_block_weight(w);
    bc->db().add_block(std::make_pair(empty_block(), ""), w, ltw, h, h, {});
    ASSERT_TRUE(bc->update_next_cumulative_weight_limit());
    const uint64_t effective_median = bc->get_current_cumulative_block_weight_median();
    const uint64_t effective_limit = bc->get_current_cumulative_block_weight_limit();
    ASSERT_TRUE(bc->update_next_cumulative_weight_limit());
    ASSERT_EQ(effective_median, bc->get_current_cumulative_block_weight_median());
    ASSERT_EQ(effective_limit, bc->get_current_cumulative_block_weight_limit());
    ASSERT_TRUE(bc->update_next_cumulative_weight_limit());
    ASSERT_EQ(effective_median, bc->get_current_cumulative_block_weight_median());
    ASSERT_EQ(effective_limit, bc->get_current_cumulative_block_weight_limit());
    ASSERT_TRUE(bc->update_next_cumulative_weight_limit());
    ASSERT_EQ(effective_median, bc->get_current_cumulative_block_weight_median());
    ASSERT_EQ(effective_limit, bc->get_current_cumulative_block_weight_limit());
  }
}

TEST(long_term_block_weight, pop_invariant_max)
{
  PREFIX(cryptonote::feature::LONG_TERM_BLOCK_WEIGHT);

  for (uint64_t h = 1; h < TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW - 10; ++h)
  {
    size_t w = bc->db().height() < TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW ? cryptonote::BLOCK_GRANTED_FULL_REWARD_ZONE_V5 : bc->get_current_cumulative_block_weight_limit();
    uint64_t ltw = bc->get_next_long_term_block_weight(w);
    bc->db().add_block(std::make_pair(empty_block(), ""), w, ltw, h, h, {});
    ASSERT_TRUE(bc->update_next_cumulative_weight_limit());
  }

  for (int n = 0; n < 1000; ++n)
  {
    // pop some blocks, then add some more
    int remove = 1 + (n * 17) % 8;
    int add = (n * 23) % 12;

    // save long term block weights we're about to remove
    uint64_t old_ltbw[16], h0 = bc->db().height() - remove - 1;
    for (int i = -2; i < remove; ++i)
    {
      old_ltbw[i + 2] = bc->db().get_block_long_term_weight(h0 + i);
    }

    for (int i = 0; i < remove; ++i)
    {
      cryptonote::block b;
      std::vector<cryptonote::transaction> txs;
      bc->db().pop_block(b, txs);
      ASSERT_TRUE(bc->update_next_cumulative_weight_limit());
    }
    for (int i = 0; i < add; ++i)
    {
      size_t w = bc->db().height() < TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW ? cryptonote::BLOCK_GRANTED_FULL_REWARD_ZONE_V5 : bc->get_current_cumulative_block_weight_limit();
      uint64_t ltw = bc->get_next_long_term_block_weight(w);
      bc->db().add_block(std::make_pair(empty_block(), ""), w, ltw, bc->db().height(), bc->db().height(), {});
      ASSERT_TRUE(bc->update_next_cumulative_weight_limit());
    }

    // check the new values are the same as the old ones
    for (int i = -2; i < std::min(add, remove); ++i)
    {
      ASSERT_EQ(bc->db().get_block_long_term_weight(h0 + i), old_ltbw[i + 2]);
    }
  }
}

TEST(long_term_block_weight, pop_invariant_random)
{
  PREFIX(cryptonote::feature::LONG_TERM_BLOCK_WEIGHT);

  for (uint64_t h = 1; h < 2 * TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW - 10; ++h)
  {
    lcg_seed = bc->db().height();
    uint32_t r = lcg();
    size_t w = bc->db().height() < TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW ? cryptonote::BLOCK_GRANTED_FULL_REWARD_ZONE_V5 : (r % bc->get_current_cumulative_block_weight_limit());
    uint64_t ltw = bc->get_next_long_term_block_weight(w);
    bc->db().add_block(std::make_pair(empty_block(), ""), w, ltw, h, h, {});
    ASSERT_TRUE(bc->update_next_cumulative_weight_limit());
  }

  for (int n = 0; n < 1000; ++n)
  {
    // pop some blocks, then add some more
    int remove = 1 + (n * 17) % 8;
    int add = (n * 23) % 123;

    // save long term block weights we're about to remove
    uint64_t old_ltbw[16], h0 = bc->db().height() - remove - 1;
    for (int i = -2; i < remove; ++i)
    {
      old_ltbw[i + 2] = bc->db().get_block_long_term_weight(h0 + i);
    }

    for (int i = 0; i < remove; ++i)
    {
      cryptonote::block b;
      std::vector<cryptonote::transaction> txs;
      bc->db().pop_block(b, txs);
      ASSERT_TRUE(bc->update_next_cumulative_weight_limit());
      const uint64_t effective_median = bc->get_current_cumulative_block_weight_median();
      const uint64_t effective_limit = bc->get_current_cumulative_block_weight_limit();
      ASSERT_TRUE(bc->update_next_cumulative_weight_limit());
      ASSERT_EQ(effective_median, bc->get_current_cumulative_block_weight_median());
      ASSERT_EQ(effective_limit, bc->get_current_cumulative_block_weight_limit());
    }
    for (int i = 0; i < add; ++i)
    {
      lcg_seed = bc->db().height();
      uint32_t r = lcg();
      size_t w = bc->db().height() < TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW ? cryptonote::BLOCK_GRANTED_FULL_REWARD_ZONE_V5 : (r % bc->get_current_cumulative_block_weight_limit());
      uint64_t ltw = bc->get_next_long_term_block_weight(w);
      bc->db().add_block(std::make_pair(empty_block(), ""), w, ltw, bc->db().height(), bc->db().height(), {});
      ASSERT_TRUE(bc->update_next_cumulative_weight_limit());
      const uint64_t effective_median = bc->get_current_cumulative_block_weight_median();
      const uint64_t effective_limit = bc->get_current_cumulative_block_weight_limit();
      ASSERT_TRUE(bc->update_next_cumulative_weight_limit());
      ASSERT_EQ(effective_median, bc->get_current_cumulative_block_weight_median());
      ASSERT_EQ(effective_limit, bc->get_current_cumulative_block_weight_limit());
    }

    // check the new values are the same as the old ones
    for (int i = -2; i < std::min(add, remove); ++i)
    {
      ASSERT_EQ(bc->db().get_block_long_term_weight(h0 + i), old_ltbw[i + 2]);
    }
  }
}

TEST(long_term_block_weight, long_growth_spike_and_drop)
{
  PREFIX(cryptonote::feature::LONG_TERM_BLOCK_WEIGHT);

  uint64_t long_term_effective_median_block_weight;

  // constant init
  for (uint64_t h = 0; h < TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW; ++h)
  {
    size_t w = cryptonote::BLOCK_GRANTED_FULL_REWARD_ZONE_V5;
    uint64_t ltw = bc->get_next_long_term_block_weight(w);
    bc->db().add_block(std::make_pair(empty_block(), ""), w, ltw, h, h, {});
    ASSERT_TRUE(bc->update_next_cumulative_weight_limit(&long_term_effective_median_block_weight));
  }
  ASSERT_EQ(long_term_effective_median_block_weight, 300000);

  // slow 10% yearly for a year (scaled down by 100000 / TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW) -> 8% change
  for (uint64_t h = 0; h < 365 * 720 * TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW / 100000; ++h)
  {
    //size_t w = bc->get_current_cumulative_block_weight_median() * rate;
    float t = h / float(365 * 720 * TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW / 100000);
    size_t w = 300000 + t * 30000;
    uint64_t ltw = bc->get_next_long_term_block_weight(w);
    bc->db().add_block(std::make_pair(empty_block(), ""), w, ltw, h, h, {});
    ASSERT_TRUE(bc->update_next_cumulative_weight_limit(&long_term_effective_median_block_weight));
  }
  ASSERT_GT(long_term_effective_median_block_weight, 300000 * 1.07);
  ASSERT_LT(long_term_effective_median_block_weight, 300000 * 1.09);

  // spike over three weeks - does not move much
  for (uint64_t h = 0; h < 21 * 720 * TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW / 100000; ++h)
  {
    size_t w = bc->get_current_cumulative_block_weight_limit();
    uint64_t ltw = bc->get_next_long_term_block_weight(w);
    bc->db().add_block(std::make_pair(empty_block(), ""), w, ltw, h, h, {});
    ASSERT_TRUE(bc->update_next_cumulative_weight_limit(&long_term_effective_median_block_weight));
  }
  ASSERT_GT(long_term_effective_median_block_weight, 300000 * 1.07);
  ASSERT_LT(long_term_effective_median_block_weight, 300000 * 1.09);

  // drop - does not move much
  for (uint64_t h = 0; h < 21 * 720 * TEST_LONG_TERM_BLOCK_WEIGHT_WINDOW / 100000; ++h)
  {
    size_t w = bc->get_current_cumulative_block_weight_median() * .25;
    uint64_t ltw = bc->get_next_long_term_block_weight(w);
    bc->db().add_block(std::make_pair(empty_block(), ""), w, ltw, h, h, {});
    ASSERT_TRUE(bc->update_next_cumulative_weight_limit(&long_term_effective_median_block_weight));
  }
  ASSERT_GT(long_term_effective_median_block_weight, 300000 * 1.07);
  ASSERT_LT(long_term_effective_median_block_weight, 300000 * 1.09);
}

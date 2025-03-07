//
// Created by Dusan Klinec on 2019-02-28.
//

#include "wallet_tools.h"
#include "cryptonote_core/uptime_proof.h"
#include <random>

using namespace crypto;
using namespace cryptonote;

void wallet_accessor_test::set_account(tools::wallet2 * wallet, cryptonote::account_base& account)
{
  wallet->clear();
  wallet->m_account = account;

  wallet->m_key_device_type = account.get_device().get_type();
  wallet->m_account_public_address = account.get_keys().m_account_address;
  wallet->m_watch_only = false;
  wallet->m_multisig = false;
  wallet->m_multisig_threshold = 0;
  wallet->m_multisig_signers.clear();
  wallet->m_device_name = account.get_device().get_name();

  wallet->m_subaddress_lookahead_major = 5;
  wallet->m_subaddress_lookahead_minor = 20;

  wallet->setup_new_blockchain();  // generates also subadress register
}

void wallet_accessor_test::process_parsed_blocks(tools::wallet2 * wallet, uint64_t start_height, const std::vector<cryptonote::block_complete_entry> &blocks, const std::vector<tools::wallet2::parsed_block> &parsed_blocks, uint64_t& blocks_added)
{
  wallet->process_parsed_blocks(start_height, blocks, parsed_blocks, blocks_added);
}

void wallet_tools::process_transactions(tools::wallet2 * wallet, const std::vector<test_event_entry>& events, const cryptonote::block& blk_head, block_tracker &bt, const std::optional<crypto::hash>& blk_tail)
{
  map_hash2tx_t mtx;
  std::vector<const cryptonote::block*> blockchain;
  find_block_chain(events, blockchain, mtx, get_block_hash(blk_head));

  if (blk_tail){
    trim_block_chain(blockchain, *blk_tail);
  }

  process_transactions(wallet, blockchain, mtx, bt);
}

void wallet_tools::process_transactions(tools::wallet2 * wallet, const std::vector<const cryptonote::block*>& blockchain, const map_hash2tx_t & mtx, block_tracker &bt)
{
  uint64_t start_height=0, blocks_added=0;
  std::vector<cryptonote::block_complete_entry> v_bche;
  std::vector<tools::wallet2::parsed_block> v_parsed_block;

  v_bche.reserve(blockchain.size());
  v_parsed_block.reserve(blockchain.size());

  size_t idx = 0;
  for(auto bl : blockchain)
  {
    idx += 1;
    uint64_t height;
    v_bche.emplace_back();
    v_parsed_block.emplace_back();

    wallet_tools::gen_block_data(bt, bl, mtx, v_bche.back(), v_parsed_block.back(), idx == 1 ? start_height : height);
  }

  if (wallet)
    wallet_accessor_test::process_parsed_blocks(wallet, start_height, v_bche, v_parsed_block, blocks_added);
}

bool wallet_tools::fill_tx_sources(tools::wallet2 * wallet, std::vector<cryptonote::tx_source_entry>& sources, size_t mixin, const std::optional<size_t>& num_utxo, const std::optional<uint64_t>& min_amount, block_tracker &bt, std::vector<size_t> &selected, uint64_t cur_height, ssize_t offset, int step, const std::optional<fnc_accept_tx_source_t>& fnc_accept)
{
  CHECK_AND_ASSERT_THROW_MES(step != 0, "Step is zero");
  sources.clear();

  auto & transfers = wallet_accessor_test::get_transfers(wallet);
  std::unordered_set<size_t> selected_idx;
  std::unordered_set<crypto::key_image> selected_kis;
  const size_t ntrans = wallet->get_num_transfer_details();
  size_t roffset = offset >= 0 ? offset : ntrans - offset - 1;
  size_t iters = 0;
  uint64_t sum = 0;
  size_t cur_utxo = 0;
  bool abort = false;
  unsigned brk_cond = 0;
  unsigned brk_thresh = num_utxo && min_amount ? 2 : (num_utxo || min_amount ? 1 : 0);

#define EVAL_BRK_COND() do {                         \
  brk_cond = 0;                                      \
  if (num_utxo && *num_utxo <= cur_utxo)             \
    brk_cond += 1;                                   \
  if (min_amount && *min_amount <= sum)              \
    brk_cond += 1;                                   \
  } while(0)

  for(ssize_t i = roffset; iters < ntrans && !abort; i += step, ++iters)
  {
    EVAL_BRK_COND();
    if (brk_cond >= brk_thresh)
      break;

    i = i < 0 ? (i + ntrans) : i % ntrans;
    auto & td = transfers[i];
    if (td.m_spent)
      continue;
    if (td.m_block_height + MINED_MONEY_UNLOCK_WINDOW > cur_height)
      continue;
    if (selected_idx.find((size_t)i) != selected_idx.end()){
      oxen::log::error(globallogcat, "Should not happen (selected_idx not found): {}", i);
      continue;
    }
    if (selected_kis.find(td.m_key_image) != selected_kis.end()){
      oxen::log::error(globallogcat, "Should not happen (selected KI): {} ki: {}", i, dump_keys(td.m_key_image.data()));
      continue;
    }

    try {
      cryptonote::tx_source_entry src;
      wallet_tools::gen_tx_src(mixin, cur_height, td, src, bt);

      // Acceptor function
      if (fnc_accept){
        tx_source_info_crate_t c_info{.td=&td, .src=&src, .selected_idx=&selected_idx, .selected_kis=&selected_kis,
            .ntrans=ntrans, .iters=iters, .sum=sum, .cur_utxo=cur_utxo};

        bool take_it = (*fnc_accept)(c_info, abort);
        if (!take_it){
          continue;
        }
      }

      oxen::log::debug(globallogcat, "Selected {} from tx: {} ki: {} amnt: {} rct: {} glob: {}", i, dump_keys(td.m_txid.data()), dump_keys(td.m_key_image.data()), td.amount(), td.is_rct(), td.m_global_output_index);

      sum += td.amount();
      cur_utxo += 1;

      sources.emplace_back(src);
      selected.push_back((size_t)i);
      selected_idx.insert((size_t)i);
      selected_kis.insert(td.m_key_image);

    } catch(const std::exception &e){
      oxen::log::trace(globallogcat, "Output {}, from: {} amnt: {}, rct: {}, glob: {} is not applicable: {}", i, dump_keys(td.m_txid.data()), td.amount(), td.is_rct(), td.m_global_output_index, e.what());
    }
  }

  EVAL_BRK_COND();
  return brk_cond >= brk_thresh;
#undef EVAL_BRK_COND
}

void wallet_tools::gen_tx_src(size_t mixin, uint64_t cur_height, const tools::wallet2::transfer_details& td, cryptonote::tx_source_entry& src, block_tracker& bt)
{
  CHECK_AND_ASSERT_THROW_MES(mixin != 0, "mixin is zero");
  src.amount = td.amount();
  src.rct = td.is_rct();

  std::vector<tools::wallet2::get_outs_entry> outs;
  bt.get_fake_outs(mixin, td.is_rct() ? 0 : td.amount(), td.m_global_output_index, cur_height, outs);

  for (size_t n = 0; n < mixin; ++n)
  {
    auto& oe = src.outputs.emplace_back();
    oe.first = std::get<0>(outs[n]);
    oe.second.dest = rct::pk2rct(std::get<1>(outs[n]));
    oe.second.mask = std::get<2>(outs[n]);
  }

  size_t real_idx = crypto::rand<size_t>() % mixin;

  cryptonote::tx_source_entry::output_entry &real_oe = src.outputs[real_idx];
  real_oe.first = td.m_global_output_index;
  real_oe.second.dest = rct::pk2rct(var::get<txout_to_key>(td.m_tx.vout[td.m_internal_output_index].target).key);
  real_oe.second.mask = rct::commit(td.amount(), td.m_mask);

  std::sort(src.outputs.begin(), src.outputs.end(), [&](const cryptonote::tx_source_entry::output_entry i0, const cryptonote::tx_source_entry::output_entry i1) {
    return i0.first < i1.first;
  });

  for (size_t i = 0; i < src.outputs.size(); ++i){
    if (src.outputs[i].first == td.m_global_output_index){
      src.real_output = i;
      break;
    }
  }

  src.mask = td.m_mask;
  src.real_out_tx_key = get_tx_pub_key_from_extra(td.m_tx, td.m_pk_index);
  src.real_out_additional_tx_keys = get_additional_tx_pub_keys_from_extra(td.m_tx);
  src.real_output_in_tx_index = td.m_internal_output_index;
  src.multisig_kLRki = rct::multisig_kLRki({rct::zero(), rct::zero(), rct::zero(), rct::zero()});
}

void wallet_tools::gen_block_data(block_tracker &bt, const cryptonote::block *bl, const map_hash2tx_t &mtx, cryptonote::block_complete_entry &bche, tools::wallet2::parsed_block &parsed_block, uint64_t &height)
{
  std::vector<const transaction*> vtx;
  vtx.push_back(&bl->miner_tx.value());
  height = bl->get_height();

  for (const auto &h : bl->tx_hashes) {
    const map_hash2tx_t::const_iterator cit = mtx.find(h);
    CHECK_AND_ASSERT_THROW_MES(mtx.end() != cit, "block contains an unknown tx hash @ {}, {}", height, h);
    vtx.push_back(cit->second);
  }

  bche.block = "NA";
  bche.txs.resize(bl->tx_hashes.size());

  parsed_block.error = false;
  parsed_block.hash = get_block_hash(*bl);
  parsed_block.block = *bl;
  parsed_block.txes.reserve(bl->tx_hashes.size());

  auto& o_indices = parsed_block.o_indices["indices"];

  size_t cur = 0;
  for (const transaction *tx : vtx){
    cur += 1;
    o_indices.emplace_back();
    bt.process(bl, tx, cur - 1);
    std::vector<uint64_t> indices = o_indices.back()["indices"];
    bt.global_indices(tx, indices);

    if (cur > 1)  // miner not included
      parsed_block.txes.push_back(*tx);
  }
}

void wallet_tools::compute_subaddresses(std::unordered_map<crypto::public_key, cryptonote::subaddress_index> &subaddresses, cryptonote::account_base & creds, size_t account, size_t minors)
{
  auto &hwdev = hw::get_device("default");
  const std::vector<crypto::public_key> pkeys = hwdev.get_subaddress_spend_public_keys(creds.get_keys(), account, 0, minors);

  for(uint32_t c = 0; c < pkeys.size(); ++c){
    cryptonote::subaddress_index sidx{(uint32_t)account, c};
    subaddresses[pkeys[c]] = sidx;
  }
}

cryptonote::account_public_address get_address(const tools::wallet2* inp)
{
  return (inp)->get_account().get_keys().m_account_address;
}

bool construct_tx_to_key(cryptonote::transaction& tx,
                         tools::wallet2 * sender_wallet, const var_addr_t& to, uint64_t amount,
                         std::vector<cryptonote::tx_source_entry> &sources,
                         uint64_t fee, rct::RangeProofType range_proof_type, int bp_version)
{
  tx_destination_entry change_addr = {};
  std::vector<tx_destination_entry> destinations;
  fill_tx_destinations(sender_wallet->get_account(), get_address(to), amount, fee, sources, destinations);
  return construct_tx_rct(sender_wallet, sources, destinations, change_addr, std::vector<uint8_t>(), tx, 0, range_proof_type, bp_version);
}

bool construct_tx_to_key(cryptonote::transaction& tx,
                         tools::wallet2 * sender_wallet,
                         const std::vector<cryptonote::tx_destination_entry>& destinations,
                         std::vector<cryptonote::tx_source_entry> &sources,
                         uint64_t fee, rct::RangeProofType range_proof_type, int bp_version)
{
  tx_destination_entry change_addr = {};
  std::vector<tx_destination_entry> all_destinations;
  fill_tx_destinations(sender_wallet->get_account(), destinations, fee, sources, all_destinations, true);
  return construct_tx_rct(sender_wallet, sources, all_destinations, change_addr, std::vector<uint8_t>(), tx, 0, range_proof_type, bp_version);
}

bool construct_tx_rct(tools::wallet2 * sender_wallet, std::vector<cryptonote::tx_source_entry>& sources, const std::vector<cryptonote::tx_destination_entry>& destinations, const std::optional<cryptonote::tx_destination_entry>& change_addr, std::vector<uint8_t> extra, cryptonote::transaction& tx, uint64_t unlock_time, rct::RangeProofType range_proof_type, int bp_version)
{
  subaddresses_t & subaddresses = wallet_accessor_test::get_subaddresses(sender_wallet);
  crypto::secret_key tx_key;
  std::vector<crypto::secret_key> additional_tx_keys;
  std::vector<tx_destination_entry> destinations_copy = destinations;
  rct::RCTConfig rct_config = {range_proof_type, bp_version};
  return construct_tx_and_get_tx_key(sender_wallet->get_account().get_keys(), subaddresses, sources, destinations_copy, change_addr, extra, tx, unlock_time, tx_key, additional_tx_keys, rct_config, nullptr);
}

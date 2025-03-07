// Copyright (c) 2017-2019, The Monero Project
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

#ifndef MONERO_DEVICE_TREZOR_H
#define MONERO_DEVICE_TREZOR_H

#include "device/device.hpp"
#include "trezor.hpp"

#ifdef WITH_DEVICE_TREZOR
#include <cstddef>
#include <mutex>
#include <string>

#include "cryptonote_config.h"
#include "device/device_cold.hpp"
#include "device/device_default.hpp"
#include "device_trezor_base.hpp"
#include "wallet/transfer_details.h"
#include "wallet/tx_sets.h"
#endif

namespace hw::trezor {

void register_all();
void register_all(std::map<std::string, std::unique_ptr<device>>& registry);

#ifdef WITH_DEVICE_TREZOR
class device_trezor;

/**
 * Main device
 */
class device_trezor : public hw::trezor::device_trezor_base, public hw::device_cold {
  protected:
    std::atomic<bool> m_live_refresh_in_progress;
    std::chrono::steady_clock::time_point m_last_live_refresh_time;
    std::optional<std::thread> m_live_refresh_thread;
    std::atomic<bool> m_live_refresh_thread_running;
    bool m_live_refresh_enabled;
    size_t m_num_transations_to_sign;

    unsigned client_version();
    void transaction_versions_check(
            const wallet::unsigned_tx_set& unsigned_tx, hw::tx_aux_data& aux_data);
    void transaction_pre_check(
            std::shared_ptr<messages::monero::MoneroTransactionInitRequest> init_msg);
    void transaction_check(const protocol::tx::TData& tdata, const hw::tx_aux_data& aux_data);
    void device_state_initialize_unsafe() override;
    void live_refresh_start_unsafe();
    void live_refresh_finish_unsafe();
    void live_refresh_thread_main();

    /**
     * Signs particular transaction idx in the unsigned set, keeps state in the signer
     */
    virtual void tx_sign(
            wallet_shim* wallet,
            const wallet::unsigned_tx_set& unsigned_tx,
            size_t idx,
            hw::tx_aux_data& aux_data,
            std::shared_ptr<protocol::tx::Signer>& signer);

  public:
    device_trezor();
    virtual ~device_trezor() override;

    device_trezor(const device_trezor& device) = delete;
    device_trezor& operator=(const device_trezor& device) = delete;

    explicit operator bool() const override { return true; }

    bool init() override;
    bool release() override;
    bool disconnect() override;

    device_protocol_t device_protocol() const override { return PROTOCOL_COLD; };

    bool has_ki_cold_sync() const override { return true; }
    bool has_tx_cold_sign() const override { return true; }
    void set_network_type(cryptonote::network_type network_type) override {
        this->network_type = network_type;
    }
    void set_live_refresh_enabled(bool enabled) { m_live_refresh_enabled = enabled; }
    bool live_refresh_enabled() const { return m_live_refresh_enabled; }

    /* ======================================================================= */
    /*                             WALLET & ADDRESS                            */
    /* ======================================================================= */
    bool get_public_address(cryptonote::account_public_address& pubkey) override;
    bool get_secret_keys(crypto::secret_key& viewkey, crypto::secret_key& spendkey) override;
    void display_address(
            const cryptonote::subaddress_index& index,
            const std::optional<crypto::hash8>& payment_id) override;

    /* ======================================================================= */
    /*                              TREZOR PROTOCOL                            */
    /* ======================================================================= */

    /**
     * Get address. Throws.
     */
    std::shared_ptr<messages::monero::MoneroAddress> get_address(
            const std::optional<cryptonote::subaddress_index>& subaddress = std::nullopt,
            const std::optional<crypto::hash8>& payment_id = std::nullopt,
            bool show_address = false,
            const std::optional<std::vector<uint32_t>>& path = std::nullopt,
            const std::optional<cryptonote::network_type>& network_type = std::nullopt);

    /**
     * Get watch key from device. Throws.
     */
    std::shared_ptr<messages::monero::MoneroWatchKey> get_view_key(
            const std::optional<std::vector<uint32_t>>& path = std::nullopt,
            const std::optional<cryptonote::network_type>& network_type = std::nullopt);

    /**
     * Get_tx_key support check
     */
    bool is_get_tx_key_supported() const override;

    /**
     * Loads tx aux data
     */
    void load_tx_key_data(
            ::hw::device_cold::tx_key_data_t& res, const std::string& tx_aux_data) override;

    /**
     * TX key load with the Trezor
     */
    void get_tx_key(
            std::vector<::crypto::secret_key>& tx_keys,
            const ::hw::device_cold::tx_key_data_t& tx_aux_data,
            const ::crypto::secret_key& view_key_priv) override;

    /**
     * Key image sync with the Trezor.
     */
    void ki_sync(
            wallet_shim* wallet,
            const std::vector<wallet::transfer_details>& transfers,
            hw::device_cold::exported_key_image& ski) override;

    bool is_live_refresh_supported() const override;

    bool is_live_refresh_enabled() const;

    bool has_ki_live_refresh() const override;

    void live_refresh_start() override;

    void live_refresh(
            const ::crypto::secret_key& view_key_priv,
            const crypto::public_key& out_key,
            const crypto::key_derivation& recv_derivation,
            size_t real_output_index,
            const cryptonote::subaddress_index& received_index,
            cryptonote::keypair& in_ephemeral,
            crypto::key_image& ki) override;

    void live_refresh_finish() override;

    /**
     * Letting device know the KI computation started / ended.
     * During refresh
     */
    void computing_key_images(bool started) override;

    /**
     * Implements hw::device interface
     * called from generate_key_image_helper_precomp()
     */
    bool compute_key_image(
            const ::cryptonote::account_keys& ack,
            const ::crypto::public_key& out_key,
            const ::crypto::key_derivation& recv_derivation,
            size_t real_output_index,
            const ::cryptonote::subaddress_index& received_index,
            ::cryptonote::keypair& in_ephemeral,
            ::crypto::key_image& ki) override;

    /**
     * Signs unsigned transaction with the Trezor.
     */
    void tx_sign(
            wallet_shim* wallet,
            const wallet::unsigned_tx_set& unsigned_tx,
            wallet::signed_tx_set& signed_tx,
            hw::tx_aux_data& aux_data) override;
};

#endif

}  // namespace hw::trezor
#endif  // MONERO_DEVICE_TREZOR_H

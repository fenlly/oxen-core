#include "bls_aggregator.h"

#include <blockchain_db/sqlite/db_sqlite.h>
#include <common/bigint.h>
#include <common/exception.h>
#include <common/guts.h>
#include <common/string_util.h>
#include <crypto/crypto.h>
#include <cryptonote_core/cryptonote_core.h>
#include <l2_tracker/contracts.h>
#include <logging/oxen_logger.h>
#include <oxenc/bt_producer.h>
#include <oxenmq/oxenmq.h>

#include <chrono>
#include <ethyl/utils.hpp>

#include "bls_crypto.h"

namespace eth {

// When a service node receives a request to sign an exit request for a BLS node, this value
// determines the age cutoff for what we are willing to sign.
constexpr auto BLS_EXIT_REQUEST_MAX_AGE = 3min;

static constexpr std::string_view to_string(bls_exit_type type) {
    switch (type) {
        case bls_exit_type::normal: return "Exit";
        case bls_exit_type::liquidate: return "Liquidation";
    }
    return "bls_exit_type_label_ERROR";
}

std::string bytes_to_hex_dot_truncate_middle(std::span<const unsigned char> bytes) {
    size_t dot_size = 3;  // How many dots to show in the middle
    size_t head_hex = 6;  // How many hex characters to preserve from head of string
    size_t tail_hex = 6;  // How many hex characters to preserve from tail of string

    std::string hex = oxenc::to_hex(bytes.begin(), bytes.end());

    head_hex = std::min(head_hex, hex.size());
    std::string_view head = tools::string_safe_substr(hex, 0, head_hex);

    tail_hex = std::min(tail_hex, head.size());
    std::string_view tail = tools::string_safe_substr(head, head.size() - tail_hex, tail_hex);

    std::string result = fmt::format("{}{:.>{}}{}", head, "", dot_size, tail);
    return result;
}

namespace {
    auto logcat = oxen::log::Cat("bls_aggregator");
    constexpr std::string_view OMQ_BLS_CATEGORY = "bls";
    constexpr std::string_view OMQ_REWARDS_ENDPOINT = "get_rewards";
    constexpr std::string_view OMQ_LIQUIDATE_ENDPOINT = "get_liquidation";
    constexpr std::string_view OMQ_EXIT_ENDPOINT = "get_exit";
    const std::string OMQ_BLS_EXIT_ENDPOINT = "{}.{}"_format(OMQ_BLS_CATEGORY, OMQ_EXIT_ENDPOINT);
    const std::string OMQ_BLS_LIQUIDATE_ENDPOINT =
            "{}.{}"_format(OMQ_BLS_CATEGORY, OMQ_LIQUIDATE_ENDPOINT);
    const std::string OMQ_BLS_REWARDS_ENDPOINT =
            "{}.{}"_format(OMQ_BLS_CATEGORY, OMQ_REWARDS_ENDPOINT);

    std::vector<uint8_t> get_reward_balance_msg_to_sign(
            cryptonote::network_type nettype,
            const address& eth_addr,
            std::array<std::byte, 32> amount_be) {
        // TODO(doyle): See BLSSigner::proofOfPossession
        const auto tag = build_tag_hash(tag::REWARD, nettype);
        std::vector<uint8_t> result;
        result.reserve(tag.size() + eth_addr.size() + amount_be.size());
        result.insert(result.end(), tag.begin(), tag.end());
        result.insert(result.end(), eth_addr.begin(), eth_addr.end());
        result.insert(
                result.end(),
                reinterpret_cast<uint8_t*>(amount_be.begin()),
                reinterpret_cast<uint8_t*>(amount_be.end()));
        return result;
    }

    std::vector<uint8_t> get_exit_msg_to_sign(
            cryptonote::network_type nettype,
            bls_exit_type type,
            const bls_public_key& remove_pk,
            uint64_t unix_ts) {
        auto unix_ts_be = tools::encode_integer_be<32>(unix_ts);
        crypto::hash tag{};

        if (type == bls_exit_type::normal) {
            tag = build_tag_hash(tag::EXIT, nettype);
        } else {
            assert(type == bls_exit_type::liquidate);
            tag = build_tag_hash(tag::LIQUIDATE, nettype);
        }

        // TODO(doyle): See BLSSigner::proofOfPossession
        std::vector<uint8_t> result;
        result.reserve(tag.size() + remove_pk.size() + unix_ts_be.size());
        result.insert(result.end(), tag.begin(), tag.end());
        result.insert(result.end(), remove_pk.begin(), remove_pk.end());
        auto* ts_ptr = reinterpret_cast<uint8_t*>(unix_ts_be.data());
        result.insert(result.end(), ts_ptr, ts_ptr + unix_ts_be.size());
        return result;
    }

    struct bls_exit_request {
        bool good;
        bls_public_key remove_pk;
        std::chrono::seconds timestamp;
    };

    bls_exit_request extract_exit_request(oxenmq::Message& m) {
        bls_exit_request result{};
        if (m.data.size() != 1) {
            m.send_reply(
                    "400",
                    "Bad request: BLS exit command should have one data part; received {}"_format(
                            m.data.size()));
            return result;
        }

        try {
            oxenc::bt_dict_consumer d{m.data[0]};
            result.remove_pk =
                    tools::make_from_guts<bls_public_key>(d.require<std::string_view>("bls_"
                                                                                      "pubke"
                                                                                      "y"));
            result.timestamp = std::chrono::seconds{d.require<uint64_t>("timestamp")};
        } catch (const std::exception& e) {
            m.send_reply(
                    "400",
                    "Bad request: BLS exit command specified bad bls pubkey or timestamp: {}"_format(
                            e.what()));
            return result;
        }

        // NOTE: Check if the request is too old. If it's too old we will reject it
        auto unix_now = std::chrono::system_clock::now().time_since_epoch();
        auto time_since_initial_request = result.timestamp > unix_now ? result.timestamp - unix_now
                                                                      : unix_now - result.timestamp;
        if (time_since_initial_request > BLS_EXIT_REQUEST_MAX_AGE) {
            m.send_reply(
                    "400",
                    "Bad request: BLS exit was too old ({}) to sign"_format(
                            tools::friendly_duration(time_since_initial_request)));
            return result;
        }

        result.good = true;
        return result;
    }

#ifndef NDEBUG
    // Redo the BLS aggregation steps given the list of signers and:
    //
    //   - Calculate the full aggregate bls public key (e.g. all signers)
    //   - Aggregate bls public key w/ the given signers
    //   - Detect if there's a session node that's in oxen but not in the smart
    //     contract
    //   - Detect if there's a session node that's in the smart contract but not
    //     in oxen
    //
    // Using this node's list of session nodes and their BLS public keys as well
    // as a snapshot of the rewards contract's 'allServiceNodeIDs' issued
    // at the invocation of this call.
    void debug_redo_bls_aggregation_steps_locally(
            cryptonote::core& core, std::span<const bls_public_key> signature_signers) {
        const RewardsContract::ServiceNodeIDs contract_ids =
                core.l2_tracker().get_all_service_node_ids(std::nullopt);

        // NOTE: Detect if the smart contract state has diverged.
        {
            // NOTE: Check if key is in Oxen but not the smart contract
            std::vector<service_nodes::service_node_pubkey_info> sn_list_info =
                    core.service_node_list.get_service_node_list_state();
            size_t missing_count = 0;
            for (const auto& sn_info : sn_list_info) {
                bool found = false;
                for (size_t index = 0; index < contract_ids.ids.size(); index++) {
                    const bls_public_key& contract_bls_pkey = contract_ids.bls_pubkeys[index];
                    if (sn_info.info->bls_public_key == contract_bls_pkey) {
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    missing_count++;
                    oxen::log::warning(
                            logcat,
                            "Session node {} exists in Oxen but not in the contract",
                            sn_info.pubkey);
                }
            }

            // NOTE: Check if key is in smart contract but not in Oxen
            for (size_t index = 0; index < contract_ids.ids.size(); index++) {
                uint64_t contract_id = contract_ids.ids[index];
                const bls_public_key& contract_bls_pkey = contract_ids.bls_pubkeys[index];
                bool found = false;
                for (const auto& sn_info : sn_list_info) {
                    if (sn_info.info->bls_public_key == contract_bls_pkey) {
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    missing_count++;
                    oxen::log::warning(
                            logcat,
                            "Session node {} {} exists in contract but not in oxen",
                            contract_id,
                            contract_bls_pkey);
                }
            }

            if (missing_count == 0)
                oxen::log::debug(logcat, "No missing nodes detected!");
        }

        // NOTE: Re-derive the BLS aggregate key and the key subtraction step
        {
            // NOTE: Aggregate all keys
            eth::pubkey_aggregator cpp_agg_pubkey;
            for (size_t index = 0; index < contract_ids.ids.size(); index++)
                cpp_agg_pubkey.add(contract_ids.bls_pubkeys[index]);

            oxen::log::debug(logcat, "Full BLS aggregate public key {}", cpp_agg_pubkey.get());

            // NOTE: Subtract non-signers
            for (size_t index = 0; index < contract_ids.ids.size(); index++) {
                bool found = false;
                const bls_public_key& contract_bls_pkey = contract_ids.bls_pubkeys[index];
                for (const bls_public_key& pkey : signature_signers) {
                    if (pkey == contract_bls_pkey) {
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    oxen::log::debug(
                            logcat,
                            "  Subtracting BLS key from Session Node {} {}",
                            contract_ids.ids[index],
                            contract_bls_pkey);
                    cpp_agg_pubkey.subtract(contract_bls_pkey);
                }
            }

            // NOTE: Dump the key we re-derived (e.g. includes the non-signers)
            oxen::log::debug(
                    logcat,
                    "Re-derived (via subtraction) BLS aggregate public key {}",
                    cpp_agg_pubkey.get());
        }
    }
#endif

    struct agg_log {
        service_nodes::service_node_address addr;
        std::string msg;
    };

    struct agg_log_list {
        std::vector<agg_log> success;
        std::vector<agg_log> error;
    };

    void dump_agg_log_list(const agg_log_list& lister) {
        size_t total_aggs = lister.error.size() + lister.success.size();
        size_t success_pct = lister.success.size() / static_cast<float>(total_aggs) * 100.f;
        fmt::memory_buffer buffer;
        fmt::format_to(
                std::back_inserter(buffer),
                "{} results {}/{} sigs ({}% success, {} failures):\n",
                OMQ_BLS_REWARDS_ENDPOINT,
                lister.success.size(),
                total_aggs,
                success_pct,
                lister.error.size());

        struct agg_log_entry {
            std::span<const agg_log> list;
            bool success;
        } array[] = {
                {lister.success, true},
                {lister.error, false},
        };

        for (auto it : array) {
            if (it.list.empty())
                continue;

            fmt::format_to(
                    std::back_inserter(buffer), "{} runs:\n", it.success ? "Successful" : "Failed");
            for (const auto& item : it.list) {
                fmt::format_to(
                        std::back_inserter(buffer),
                        "  - SN {} BLS {} XKEY {} @ {:<21} => {}\n",
                        bytes_to_hex_dot_truncate_middle(item.addr.sn_pubkey),
                        bytes_to_hex_dot_truncate_middle(item.addr.bls_pubkey),
                        bytes_to_hex_dot_truncate_middle(item.addr.x_pubkey),
                        "{}:{}"_format(
                                epee::string_tools::get_ip_string_from_int32(item.addr.ip),
                                item.addr.port),
                        item.msg);
            }
        }
        oxen::log::debug(logcat, "{}", fmt::to_string(buffer));
    }

    void log_aggregation_result(
            std::string_view agg_type,
            std::span<const bls_public_key> signers,
            std::chrono::high_resolution_clock::time_point started,
            const bls_signature& sig,
            const agg_log_list& agg_log_lister,
            // Used only in a debug build:
            [[maybe_unused]] cryptonote::core& core,
            [[maybe_unused]] std::span<const uint8_t> msg) {

        if (oxen::log::get_level(logcat) <= oxen::log::Level::debug)
            dump_agg_log_list(agg_log_lister);

#ifndef NDEBUG
        eth::bls_public_key agg_pub;
        {
            eth::pubkey_aggregator aggregator;
            for (const auto& blspk : signers)
                aggregator.add(blspk);
            agg_pub = aggregator.get();
        }
#endif
        auto elapsed = std::chrono::duration_cast<std::chrono::duration<float>>(
                               std::chrono::high_resolution_clock::now() - started)
                               .count();
        oxen::log::debug(
                logcat,
                "BLS aggregate pubkey for {} requests: {} ({} aggregations) with signature {} "
                "in {:.2f}s",
                agg_type,
#if defined(NDEBUG)
                "",
#else
                agg_pub,
#endif
                signers.size(),
                sig,
                elapsed);

#ifndef NDEBUG

        debug_redo_bls_aggregation_steps_locally(core, signers);

        if (eth::verify(core.get_nettype(), sig, agg_pub, msg)) {
            oxen::log::debug(logcat, "BLS final aggregate signature verification confirmed");
        } else {
            oxen::log::warning(
                    logcat,
                    "{} BLS final aggregate signature verifivation FAILED:"
                    "\nagg bls pub: {}\nagg bls sig: {}\nmessage:     {}",
                    agg_type,
                    agg_pub,
                    sig,
                    oxenc::to_hex(msg.begin(), msg.end()));
        }
#endif
    }
}  // namespace

std::string bls_exit_liquidation_response::to_string() const {
    std::string result =
            "BLS exit response was:\n"
            "\n"
            "  - type:          {}\n"
            "  - remove_pubkey: {}\n"
            "  - timestamp:     {}\n"
            "  - signature:     {}\n"
            "  - msg_to_sign:   {}\n"_format(
                    type,
                    remove_pubkey,
                    timestamp,
                    signature,
                    oxenc::to_hex(msg_to_sign.begin(), msg_to_sign.end()));
    return result;
}

std::string bls_rewards_response::to_string() const {
    std::string result =
            "BLS rewards response was:\n"
            "\n"
            "  - address:     {}\n"
            "  - amount:      {}\n"
            "  - height:      {}\n"
            "  - signature:   {}\n"
            "  - msg_to_sign: {}\n"_format(
                    addr,
                    amount,
                    height,
                    signature,
                    oxenc::to_hex(msg_to_sign.begin(), msg_to_sign.end()));
    return result;
}

bls_aggregator::bls_aggregator(cryptonote::core& _core) : core{_core} {
    if (!core.service_node())
        return;

    // NOTE: Register endpoints contactable by OMQ for BLS aggregation
    auto& omq = core.omq();
    omq.add_category(std::string(OMQ_BLS_CATEGORY), oxenmq::Access{oxenmq::AuthLevel::none})
            .add_request_command(
                    std::string(OMQ_REWARDS_ENDPOINT), [this](auto& m) { get_rewards(m); })
            .add_request_command(
                    std::string(OMQ_EXIT_ENDPOINT),
                    [this](auto& m) { get_exit_liquidation(m, bls_exit_type::normal); })
            .add_request_command(std::string(OMQ_LIQUIDATE_ENDPOINT), [this](auto& m) {
                get_exit_liquidation(m, bls_exit_type::liquidate);
            });

    // NOTE: Add timers to cull the cached responses that have gone stale peroidically
    omq.add_timer(
            [this] {
                {
                    // NOTE: Handle overflow if STORE_RECENT_REWARDS is large or you're on some
                    // fresh network that has hardly any blocks (e.g. localdev).
                    uint64_t top_height = core.blockchain.get_current_blockchain_height() - 1;
                    uint64_t cutoff = top_height - core.get_net_config().STORE_RECENT_REWARDS;
                    if (cutoff < top_height) {
                        std::lock_guard lock{rewards_response_cache_mutex};
                        std::erase_if(rewards_response_cache, [&cutoff](const auto& item) {
                            return item.second.height < cutoff;
                        });
                    }
                }
                {
                    const uint64_t cutoff =
                            std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch() -
                                    contract::REWARDS_EXIT_SIGNATURE_EXPIRY)
                                    .count();

                    std::lock_guard lock{exit_liquidation_response_cache_mutex};
                    std::erase_if(exit_liquidation_response_cache, [&cutoff](const auto& item) {
                        return item.second.timestamp < cutoff;
                    });
                }
            },
            5min);
}

bls_registration_response bls_aggregator::registration(
        const address& operator_addr, const crypto::public_key& sn_pubkey) const {

    auto& blspk = core.get_service_keys().pub_bls;
    return bls_registration_response{
            .bls_pubkey = blspk,
            .proof_of_possession = eth::proof_of_possession(
                    core.get_nettype(),
                    operator_addr,
                    sn_pubkey,
                    core.get_service_keys().key_bls,
                    &blspk),
            .addr = operator_addr,
            .sn_pubkey = sn_pubkey,
            .ed_signature = crypto::null<crypto::ed25519_signature>};
}

uint64_t bls_aggregator::nodes_request(
        std::string_view request_name, std::string_view message, const request_callback& callback) {
    std::mutex connection_mutex;
    std::condition_variable cv;
    size_t active_connections = 0;
    const size_t MAX_CONNECTIONS = 900;

    // FIXME: make this function async rather than blocking
    std::vector<service_nodes::service_node_address> snodes;
    core.service_node_list.copy_reachable_service_node_addresses(
            std::back_inserter(snodes), core.get_nettype());

    auto& omq = core.omq();
    size_t result = 0;
    for (size_t i = 0; i < snodes.size(); i++) {
        auto& snode = snodes[i];

        // TODO: We should query non-active SNs as well and try and get them to participate in the
        // aggregation step to reduce the number of non-signers. They might still be contactable.
        if (core.service_node_list.is_service_node(snode.sn_pubkey, /*require_active*/ true)) {
            if (1) {
                std::lock_guard connection_lock(connection_mutex);
                ++active_connections;
                ++result;
            } else {
                // TODO(doyle): Rate limit
                std::unique_lock connection_lock(connection_mutex);
                cv.wait(connection_lock,
                        [&active_connections] { return active_connections < MAX_CONNECTIONS; });
            }

            omq.request(
                    tools::view_guts(snode.x_pubkey),
                    request_name,
                    [i, &snodes, &connection_mutex, &active_connections, &cv, &callback](
                            bool success, std::vector<std::string> data) {
                        callback(bls_response{snodes[i], success}, data);
                        std::lock_guard connection_lock{connection_mutex};
                        assert(active_connections);
                        if (--active_connections == 0)
                            cv.notify_all();
                    },
                    message);
        } else {
// TODO: This appears to work now that we currently generate the endpoint to contact the
// node on (and actually use their xkey for authenticated comms). The localdev script
// fails though but it progresses further than it did before when this was last enabled.
#if 0
            if (1) {
                std::lock_guard connection_lock(connection_mutex);
                ++active_connections;
            } else {
                // TODO(doyle): Rate limit
                std::unique_lock connection_lock(connection_mutex);
                cv.wait(connection_lock,
                        [&active_connections] { return active_connections < MAX_CONNECTIONS; });
            }

            auto addr = oxenmq::address{"curve://{}:{}/{:x}"_format(
                    epee::string_tools::get_ip_string_from_int32(snode.ip), snode.port, snode.x_pubkey)};
            auto conn = omq.connect_remote(
                    addr,
                    [](oxenmq::ConnectionID) { /* Successfully connected */ },
                    [](oxenmq::ConnectionID, std::string_view) { /* Failed to connect */ });

            omq.request(
                    conn,
                    request_name,
                    [i, &snodes, &connection_mutex, &active_connections, &cv, &callback, conn, &omq](
                            bool success, std::vector<std::string> data) {
                        callback(bls_response{snodes[i], success}, data);
                        std::lock_guard connection_lock{connection_mutex};
                        assert(active_connections);
                        if (--active_connections == 0)
                            cv.notify_all();
                        omq.disconnect(conn);
                    },
                    message);
#endif
        }
    }

    std::unique_lock connection_lock{connection_mutex};
    cv.wait(connection_lock, [&active_connections] { return active_connections == 0; });
    return result;
}

void bls_aggregator::get_rewards(oxenmq::Message& m) const {
    oxen::log::trace(logcat, "Received omq rewards signature request");

    if (m.data.size() != 1) {
        m.send_reply(
                "400",
                "Bad request: BLS rewards signature request should have one data part; received {}"_format(
                        m.data.size()));
        return;
    }

    eth::address eth_addr;
    uint64_t height;
    try {
        oxenc::bt_dict_consumer d{m.data[0]};
        eth_addr = tools::make_from_guts<eth::address>(d.require<std::string_view>("address"));
        height = d.require<uint64_t>("height");
    } catch (const std::exception& e) {
        m.send_reply(
                "400",
                "Bad request: BLS rewards signature request had address or height: {}"_format(
                        e.what()));
        return;
    }

    auto maybe_amount = core.blockchain.sqlite_db().get_accrued_rewards(eth_addr, height);
    if (!maybe_amount) {
        m.send_reply("410", "Balances for height {} are not available"_format(height));
        return;
    }
    auto amount = *maybe_amount;

    // We sign H(H(rewardTag || chainid || contract) || recipientAddress ||
    // recipientAmount),
    // where everything is in bytes, and recipientAmount is a 32-byte big
    // endian integer value.
    std::array<std::byte, 32> amount_be = tools::encode_integer_be<32>(amount);

    std::vector<uint8_t> msg =
            get_reward_balance_msg_to_sign(core.get_nettype(), eth_addr, amount_be);
    bls_signature sig = eth::sign(core.get_nettype(), core.get_service_keys().key_bls, msg);

    oxenc::bt_dict_producer d;
    d.append("address", tools::view_guts(eth_addr));  // Address requesting balance
    d.append("amount", amount);                       // Balance
    d.append("height", height);                       // Height of balance
    d.append("signature", tools::view_guts(sig));     // Signature of addr + balance

    m.send_reply("200", std::move(d).str());
}

bls_rewards_response bls_aggregator::rewards_request(const address& addr, uint64_t height) {

    auto begin_ts = std::chrono::high_resolution_clock::now();
    auto maybe_amount = core.blockchain.sqlite_db().get_accrued_rewards(addr, height);
    auto amount = maybe_amount.value_or(0);

    // FIXME: make this async
    oxen::log::trace(
            logcat,
            "Initiating rewards request of {} SENT for {} at height {}",
            amount,
            addr,
            height);

    if (!maybe_amount)
        throw oxen::traced<std::invalid_argument>(fmt::format(
                "Aggregating a rewards request for '{}' at height {} is invalid because "
                "reward data is not available for that height. Request rejected.",
                addr,
                height));

    // NOTE: Validate the arguments
    if (!addr) {
        throw oxen::traced<std::invalid_argument>(
                "Aggregating a rewards request for the zero address for {} SENT at height {} is "
                "invalid. Request rejected"_format(
                        addr, amount, height, core.service_node_list.height()));
    }

    if (amount == 0) {
        throw oxen::traced<std::invalid_argument>(
                "Aggregating a rewards request for '{}' for 0 SENT at height {} is invalid because "
                "no rewards are available. Request rejected."_format(addr, height));
    }

    // NOTE: Serve the response from our cache if it's a repeated request
    {
        std::lock_guard lock{rewards_response_cache_mutex};
        auto cache_it = rewards_response_cache.find(addr);
        if (cache_it != rewards_response_cache.end()) {
            const bls_rewards_response& cache_response = cache_it->second;
            if (cache_response.height == height && cache_response.amount == amount) {
                log::trace(
                        logcat,
                        "Serving rewards request from cache for address {} at height {} with "
                        "rewards {} amount",
                        addr,
                        height,
                        amount);
                return cache_response;
            }
        }
    }

    bls_rewards_response result{};
    result.addr = addr;
    result.amount = amount;
    result.height = height;
    result.msg_to_sign = get_reward_balance_msg_to_sign(
            core.get_nettype(), result.addr, tools::encode_integer_be<32>(amount));

    // `nodesRequest` dispatches to a threadpool hence we require synchronisation:
    std::mutex sig_mutex;
    eth::signature_aggregator agg_sig;

    oxenc::bt_dict_producer d;
    d.append("address", tools::view_guts(addr));
    d.append("height", height);

    // NOTE: Send aggregate rewards request to the remainder of the network. This is a blocking
    // call (FIXME -- it should not be!)
    agg_log_list agg_log_lister = {};
    uint64_t total_requests = nodes_request(
            OMQ_BLS_REWARDS_ENDPOINT,
            std::move(d).str(),
            [&agg_log_lister, &agg_sig, &result, &sig_mutex, nettype = core.get_nettype()](
                    const bls_response& response, const std::vector<std::string>& data) {
                bls_rewards_response rewards_response = {};
                if (!response.success || data.size() != 2 || data[0] != "200") {
                    agg_log_lister.error.emplace_back(
                            response.sn,
                            "Data payload was not valid, {}"_format(fmt::join(data, " ")));
                    return;
                }

                // NOTE: Extract parameters
                oxenc::bt_dict_consumer d{data[1]};
                try {
                    rewards_response.addr =
                            tools::make_from_guts<address>(d.require<std::string_view>("address"));
                } catch (const std::exception& e) {
                    agg_log_lister.error.emplace_back(
                            response.sn,
                            "Address was not {} bytes, {}"_format(
                                    sizeof(address), fmt::join(data, " ")));
                    return;
                }

                try {
                    rewards_response.amount = d.require<uint64_t>("amount");
                } catch (const std::exception& e) {
                    agg_log_lister.error.emplace_back(
                            response.sn,
                            "Amount was not a valid U64, {}"_format(fmt::join(data, " ")));
                    return;
                }

                try {
                    rewards_response.height = d.require<uint64_t>("height");
                } catch (const std::exception& e) {
                    agg_log_lister.error.emplace_back(
                            response.sn,
                            "Height was not a valid U64, {}"_format(fmt::join(data, " ")));
                    return;
                }

                try {
                    rewards_response.signature =
                            tools::make_from_guts<bls_signature>(d.require<std::string_view>("signa"
                                                                                             "tur"
                                                                                             "e"));
                } catch (const std::exception& e) {
                    agg_log_lister.error.emplace_back(
                            response.sn,
                            "Signature was not a valid U64, {}"_format(fmt::join(data, " ")));
                    return;
                }

                rewards_response.msg_to_sign = get_reward_balance_msg_to_sign(
                        nettype,
                        rewards_response.addr,
                        tools::encode_integer_be<32>(rewards_response.amount));

                // NOTE: Verify parameters
                if (rewards_response.addr != result.addr) {
                    agg_log_lister.error.emplace_back(
                            response.sn,
                            "Response ETH address {} does not match the request address {}"_format(
                                    rewards_response.addr, result.addr));
                    return;
                }

                if (rewards_response.amount != result.amount ||
                    rewards_response.height != result.height) {
                    agg_log_lister.error.emplace_back(
                            response.sn,
                            "Balance/height mismatch: expected {:L}/{:L}, got {:L}/{:L}"_format(
                                    result.amount,
                                    result.height,
                                    rewards_response.amount,
                                    rewards_response.height));
                    return;
                }

                if (!eth::verify(
                            nettype,
                            rewards_response.signature,
                            response.sn.bls_pubkey,
                            result.msg_to_sign)) {
                    agg_log_lister.error.emplace_back(response.sn, "Invalid BLS signature");
                    return;
                }

                // NOTE: Aggregate parameters
                {
                    std::lock_guard lock{sig_mutex};
                    agg_sig.add(rewards_response.signature);
                    result.signers_bls_pubkeys.push_back(response.sn.bls_pubkey);
                }

                agg_log_lister.success.emplace_back(
                        response.sn, "Success, sig: {}"_format(rewards_response.signature));
            });

    result.signature = agg_sig.get();

    // NOTE: Dump the aggregate pubkey and other info that was generated
    log_aggregation_result(
            "rewards",
            result.signers_bls_pubkeys,
            begin_ts,
            result.signature,
            agg_log_lister,
            core,
            result.msg_to_sign);

    // NOTE: Store the response in to the cache if the number of non-signers is small enough to
    // constitute a valid signature.
    uint64_t non_signers_count = total_requests - result.signers_bls_pubkeys.size();
    if (non_signers_count <= contract::rewards_bls_non_signer_threshold(total_requests)) {
        std::lock_guard lock{rewards_response_cache_mutex};
        rewards_response_cache[addr] = result;
    }

    return result;
}

void bls_aggregator::get_exit_liquidation(oxenmq::Message& m, bls_exit_type type) const {
    oxen::log::trace(logcat, "Received omq {} signature request", type);
    bls_exit_request request = extract_exit_request(m);
    if (!request.good)
        return;

    bool removable = false;
    switch (type) {
        case bls_exit_type::normal: {
            removable = core.blockchain.is_node_removable(request.remove_pk);
        } break;
        case bls_exit_type::liquidate: {
            removable = core.blockchain.is_node_liquidatable(request.remove_pk);
        } break;
    }

    if (!removable) {
        m.send_reply(
                "403",
                "Forbidden: The BLS pubkey {} is not currently {}."_format(
                        request.remove_pk, type));
        return;
    }

    std::vector<uint8_t> msg = get_exit_msg_to_sign(
            core.get_nettype(), type, request.remove_pk, request.timestamp.count());
    bls_signature sig = eth::sign(core.get_nettype(), core.get_service_keys().key_bls, msg);

    oxenc::bt_dict_producer d;
    d.append("remove", tools::view_guts(request.remove_pk));  // BLS pubkey to remove
    d.append("signature", tools::view_guts(sig));  // Signs over the exit key and timestamp
    m.send_reply("200", std::move(d).str());
}

// Common code for exit and liquidation requests, which only differ in three ways:
// - the endpoint they go to;
// - the tag that gets used in the msg_to_sign hash; and
// - the key under which the signed pubkey gets confirmed back to us.
bls_exit_liquidation_response bls_aggregator::exit_liquidation_request(
        const crypto::public_key& pubkey, bls_exit_type type) {

    // NOTE: Trace entry into function
    oxen::log::debug(logcat, "Initiating {} request for SN {}", type, pubkey);

    auto begin_ts = std::chrono::high_resolution_clock::now();

    // NOTE: Lookup the BLS pubkey associated with the Ed25519 pubkey.
    std::optional<eth::bls_public_key> maybe_bls_pubkey{};
    core.service_node_list.for_each_recently_removed_node([&](const auto& node) {
        if (node.service_node_pubkey == pubkey) {
            maybe_bls_pubkey = node.info.bls_public_key;
            return true;
        }
        return false;
    });

    if (!maybe_bls_pubkey) {
        throw oxen::traced<std::invalid_argument>(
                "{} request for SN {} at height {} is invalid, node is not in the list of recently"
                " exited nodes. Request rejected"_format(
                        type, pubkey, core.blockchain.get_current_blockchain_height()));
    }

    // NOTE: Validate the arguments
    const bls_public_key& bls_pubkey = *maybe_bls_pubkey;
    if (!bls_pubkey) {
        throw oxen::traced<std::invalid_argument>(
                "{} request for SN {} w/ the zero BLS pkey at height {} is invalid. Request "
                "rejected"_format(type, pubkey, core.blockchain.get_current_blockchain_height()));
    }

    // NOTE: Serve the response from our cache if it's a repeated request
    {
        std::lock_guard lock{exit_liquidation_response_cache_mutex};
        auto it = exit_liquidation_response_cache.find(bls_pubkey);
        if (it != exit_liquidation_response_cache.end()) {
            const bls_exit_liquidation_response& response = it->second;
            auto now_unix_ts = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch());
            auto cache_ts = std::chrono::seconds(response.timestamp);
            if (response.type == type && now_unix_ts >= cache_ts) {
                std::chrono::seconds cache_age = now_unix_ts - cache_ts;
                if (cache_age <= contract::REWARDS_EXIT_SIGNATURE_EXPIRY) {
                    log::debug(
                            logcat,
                            "Serving {} response from cache for SN {} (cached {})\n{}",
                            type,
                            pubkey,
                            tools::get_human_readable_timespan(cache_age),
                            response);
                    return response;
                }
            }
        }
    }

    // NOTE: The OMQ endpoint to hit
    bool removable = false;
    std::string_view endpoint = "";
    switch (type) {
        case bls_exit_type::normal: {
            endpoint = OMQ_BLS_EXIT_ENDPOINT;
            removable = core.blockchain.is_node_removable(bls_pubkey);
        } break;
        case bls_exit_type::liquidate: {
            endpoint = OMQ_BLS_LIQUIDATE_ENDPOINT;
            removable = core.blockchain.is_node_liquidatable(bls_pubkey);
        } break;
    }

    if (!removable) {
        throw oxen::traced<std::invalid_argument>(
                "{} request for SN {} (BLS {}) at height {} is invalid. Node cannot "
                "be removed yet. Request rejected"_format(
                        type, pubkey, bls_pubkey, core.blockchain.get_current_blockchain_height()));
    }

    bls_exit_liquidation_response result;
    result.remove_pubkey = bls_pubkey;
    result.type = type;
    result.timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    result.msg_to_sign =
            get_exit_msg_to_sign(core.get_nettype(), type, bls_pubkey, result.timestamp);

    std::mutex signers_mutex;
    eth::signature_aggregator agg_sig;

    oxenc::bt_dict_producer message_dict;
    message_dict.append("bls_pubkey", tools::view_guts(bls_pubkey));
    message_dict.append("timestamp", result.timestamp);

    // FIXME: make this async
    agg_log_list agg_log_lister = {};
    uint64_t total_requests = nodes_request(
            endpoint,
            std::move(message_dict).str(),
            [&agg_log_lister, &agg_sig, &result, &signers_mutex, nettype = core.get_nettype()](
                    const bls_response& response, const std::vector<std::string>& data) {
                bls_exit_liquidation_response exit_response = {};
                if (!response.success || data.size() != 2 || data[0] != "200")
                    throw oxen::traced<std::runtime_error>{
                            "Request returned an error: {}"_format(fmt::join(data, " "))};

                oxenc::bt_dict_consumer d{data[1]};

                // NOTE: Extract parameters
                try {
                    exit_response.remove_pubkey =
                            tools::make_from_guts<bls_public_key>(d.require<std::string_view>("remo"
                                                                                              "v"
                                                                                              "e"));
                } catch (const std::exception& e) {
                    agg_log_lister.error.emplace_back(
                            response.sn,
                            "Remove (pubkey) was not {} bytes, {}"_format(
                                    sizeof(bls_public_key), fmt::join(data, " ")));
                    return;
                }

                try {
                    exit_response.signature =
                            tools::make_from_guts<bls_signature>(d.require<std::string_view>("signa"
                                                                                             "tur"
                                                                                             "e"));
                } catch (const std::exception& e) {
                    agg_log_lister.error.emplace_back(
                            response.sn,
                            "Signature was not {} bytes, {}"_format(
                                    sizeof(bls_signature), fmt::join(data, " ")));
                    return;
                }

                // NOTE: Verify parameters
                if (exit_response.remove_pubkey != result.remove_pubkey) {
                    agg_log_lister.error.emplace_back(
                            response.sn,
                            "BLS response pubkey {} does not match the request pubkey {}"_format(
                                    exit_response.remove_pubkey, result.remove_pubkey));
                    return;
                }

                if (!eth::verify(
                            nettype,
                            exit_response.signature,
                            response.sn.bls_pubkey,
                            result.msg_to_sign)) {
                    agg_log_lister.error.emplace_back(response.sn, "Invalid BLS signature");
                    return;
                }

                // NOTE: Aggregate parameters
                {
                    std::lock_guard<std::mutex> lock(signers_mutex);
                    agg_sig.add(exit_response.signature);
                    result.signers_bls_pubkeys.push_back(response.sn.bls_pubkey);
                }

                agg_log_lister.success.emplace_back(
                        response.sn, "Success, sig: {}"_format(exit_response.signature));
            });

    result.signature = agg_sig.get();

    // NOTE: Dump the aggregate pubkey and other info that was generated
    log_aggregation_result(
            type == bls_exit_type::normal ? "exit" : "liquidation",
            result.signers_bls_pubkeys,
            begin_ts,
            result.signature,
            agg_log_lister,
            core,
            result.msg_to_sign);

    // NOTE: Store the response in to the cache if the number of non-signers is small enough to
    // constitute a valid signature.
    uint64_t non_signers_count = total_requests - result.signers_bls_pubkeys.size();
    if (non_signers_count <= contract::rewards_bls_non_signer_threshold(total_requests)) {
        std::lock_guard lock{exit_liquidation_response_cache_mutex};
        exit_liquidation_response_cache[bls_pubkey] = result;
    }

    return result;
}
}  // namespace eth

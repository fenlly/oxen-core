#pragma once

#include <cassert>
#include <cstdint>
#include <string_view>

#include "common/formattable.h"

namespace cryptonote {

using namespace std::literals;

enum class txversion : uint16_t {
    v0 = 0,
    v1,
    v2_ringct,
    v3_per_output_unlock_times,
    v4_tx_types,
    _count,
};
// NOTE: the order and underlying values here may not change (the underlying numeric value is
// serialized directly into the transaction); new entries must always be added to the end (just
// before _count).
enum class txtype : uint16_t {
    standard = 0,
    state_change,
    key_image_unlock,
    stake,
    oxen_name_system,
    ethereum_service_node_exit_request,
    ethereum_service_node_exit,
    ethereum_staking_requirement_updated,
    ethereum_purge_missing_service_node,
    ethereum_new_service_node_v2,
    _count
};

inline constexpr bool is_l2_event_tx(txtype type) {
    return type >= txtype::ethereum_service_node_exit_request &&
           type <= txtype::ethereum_new_service_node_v2;
}

inline constexpr std::string_view to_string(txversion v) {
    switch (v) {
        case txversion::v1: return "1"sv;
        case txversion::v2_ringct: return "2_ringct"sv;
        case txversion::v3_per_output_unlock_times: return "3_per_output_unlock_times"sv;
        case txversion::v4_tx_types: return "4_tx_types"sv;
        default: assert(false); return "xx_unhandled_version"sv;
    }
}

inline constexpr std::string_view to_string(txtype type) {
    switch (type) {
        case txtype::standard: return "standard"sv;
        case txtype::state_change: return "state_change"sv;
        case txtype::key_image_unlock: return "key_image_unlock"sv;
        case txtype::stake: return "stake"sv;
        case txtype::oxen_name_system: return "oxen_name_system"sv;
        case txtype::ethereum_new_service_node_v2: return "ethereum_new_service_node_V2"sv;
        case txtype::ethereum_service_node_exit_request:
            return "ethereum_service_node_exit_request"sv;
        case txtype::ethereum_service_node_exit: return "ethereum_service_node_exit"sv;
        case txtype::ethereum_staking_requirement_updated:
            return "ethereum_staking_requirement_updated"sv;
        case txtype::ethereum_purge_missing_service_node:
            return "ethereum_purge_missing_service_node"sv;
        case txtype::_count:;
    }
    assert(false);
    return "xx_unhandled_type"sv;
}

}  // namespace cryptonote

template <>
inline constexpr bool formattable::via_to_string<cryptonote::txversion> = true;
template <>
inline constexpr bool formattable::via_to_string<cryptonote::txtype> = true;

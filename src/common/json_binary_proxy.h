#pragma once

#include <common/exception.h>
#include <oxenc/common.h>

#include <nlohmann/json.hpp>
#include <string_view>
#include <unordered_set>

#include "crypto/crypto.h"
#include "crypto/eth.h"
#include "ringct/rctTypes.h"

using namespace std::literals;

namespace tools {

// Binary types that we support for rpc input/output.  For json, these must be specified as hex or
// base64; for bt-encoded requests these can be accepted as binary, hex, or base64.
template <typename T>
inline constexpr bool json_is_binary = false;
template <>
inline constexpr bool json_is_binary<crypto::hash> = true;
template <>
inline constexpr bool json_is_binary<crypto::hash4> = true;
template <>
inline constexpr bool json_is_binary<crypto::public_key> = true;
template <>
inline constexpr bool json_is_binary<crypto::ed25519_public_key> = true;
template <>
inline constexpr bool json_is_binary<crypto::ed25519_signature> = true;
template <>
inline constexpr bool json_is_binary<crypto::x25519_public_key> = true;
template <>
inline constexpr bool json_is_binary<eth::bls_public_key> = true;
template <>
inline constexpr bool json_is_binary<eth::bls_signature> = true;
// eth::address gets handled specially: we parse as binary in, but not on the way out, because on
// the way out we want it always 0x prefixed and checksum formatted.
// template <>
// inline constexpr bool json_is_binary<eth::address> = true;
template <>
inline constexpr bool json_is_binary<crypto::key_image> = true;
template <>
inline constexpr bool json_is_binary<rct::key> = true;

template <typename T>
inline constexpr bool json_is_binary_container = false;
template <typename T>
inline constexpr bool json_is_binary_container<std::vector<T>> = json_is_binary<T>;
template <typename T>
inline constexpr bool json_is_binary_container<std::unordered_set<T>> = json_is_binary<T>;

// De-referencing wrappers around the above:
template <typename T>
inline constexpr bool json_is_binary<const T&> = json_is_binary<T>;
template <typename T>
inline constexpr bool json_is_binary<T&&> = json_is_binary<T>;
template <typename T>
inline constexpr bool json_is_binary_container<const T&> = json_is_binary_container<T>;
template <typename T>
inline constexpr bool json_is_binary_container<T&&> = json_is_binary_container<T>;

template <typename T>
concept binary_json = json_is_binary<T>;

template <typename T>
concept binary_json_container = json_is_binary_container<T>;

void load_binary_parameter_impl(
        std::string_view bytes, size_t raw_size, bool allow_raw, uint8_t* val_data);

// Loads a binary value from a string_view which may contain hex, base64, and (optionally) raw
// bytes.
template <typename T>
    requires binary_json<T> || std::same_as<T, eth::address>
void load_binary_parameter(std::string_view bytes, bool allow_raw, T& val) {
    load_binary_parameter_impl(bytes, sizeof(T), allow_raw, reinterpret_cast<uint8_t*>(&val));
}

// Wrapper around a nlohmann::json that assigns a binary value either as binary (for bt-encoding);
// or as hex or base64 (for json-encoding).
class json_binary_proxy {
  public:
    nlohmann::json& e;
    enum class fmt { bt, hex, base64 } format;
    explicit json_binary_proxy(nlohmann::json& elem, fmt format) : e{elem}, format{format} {}
    json_binary_proxy() = delete;

    json_binary_proxy(const json_binary_proxy&) = default;
    json_binary_proxy(json_binary_proxy&&) = default;

    /// Dereferencing a proxy element accesses the underlying nlohmann::json
    nlohmann::json& operator*() { return e; }
    nlohmann::json* operator->() { return &e; }

    /// Descends into the json object, returning a new binary value proxy around the child element.
    template <typename T>
    json_binary_proxy operator[](T&& key) {
        return json_binary_proxy{e[std::forward<T>(key)], format};
    }

    /// Returns a binary value proxy around the first/last element (requires an underlying list)
    json_binary_proxy front() { return json_binary_proxy{e.front(), format}; }
    json_binary_proxy back() { return json_binary_proxy{e.back(), format}; }

    /// Assigns binary data from a string_view/string/etc.
    nlohmann::json& operator=(std::string_view binary_data);

    /// Assigns binary data from a string_view over a 1-byte, non-char type (e.g. unsigned char or
    /// uint8_t).
    template <oxenc::basic_char Char>
        requires(!std::same_as<Char, char>)
    nlohmann::json& operator=(std::basic_string_view<Char> binary_data) {
        return *this = std::string_view{
                       reinterpret_cast<const char*>(binary_data.data()), binary_data.size()};
    }

    /// Takes a trivial, no-padding data structure (e.g. a crypto::hash) as the value and dumps its
    /// contents as the binary value.
    template <binary_json T>
    nlohmann::json& operator=(const T& val) {
        return *this = std::string_view{reinterpret_cast<const char*>(&val), sizeof(val)};
    }

    /// Takes a vector of some json_binary_proxy-assignable type and builds an array by assigning
    /// each one into a new array of binary values.
    template <binary_json_container T>
    nlohmann::json& operator=(const T& vals) {
        auto a = nlohmann::json::array();
        for (auto& val : vals)
            json_binary_proxy{a.emplace_back(), format} = val;
        return e = std::move(a);
    }
    /// Emplaces a new nlohman::json to the end of an underlying list and returns a
    /// json_binary_proxy wrapping it.
    ///
    /// Example:
    ///
    ///     auto child = wrappedelem.emplace_back({"key1": 1}, {"key2": 2});
    ///     child["binary-key"] = some_binary_thing;
    template <typename... Args>
    json_binary_proxy emplace_back(Args&&... args) {
        return json_binary_proxy{e.emplace_back(std::forward<Args>(args)...), format};
    }

    /// Adds an element to an underlying list, then copies or moves the given argument onto it via
    /// json_binary_proxy assignment.
    template <typename T>
    void push_back(T&& val) {
        emplace_back() = std::forward<T>(val);
    }
};

}  // namespace tools

// Specializations of binary types for deserialization; when receiving these from json we expect
// them encoded in hex or base64.  These may *not* be used for serialization, and will throw if so
// invoked; for serialization you need to use RPC_COMMAND::response_hex (or _b64) instead.
namespace nlohmann {
template <typename T>
    requires tools::binary_json<T> || std::same_as<T, eth::address>
struct adl_serializer<T> {
    static_assert(std::is_trivially_copyable_v<T> && std::has_unique_object_representations_v<T>);

    static void to_json(json&, const T&) {
        throw oxen::traced<std::logic_error>{
                "Internal error: binary types are not directly serializable"};
    }
    static void from_json(const json& j, T& val) {
        tools::load_binary_parameter(j.get<std::string_view>(), false /*no raw*/, val);
    }
};
}  // namespace nlohmann

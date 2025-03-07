// Copyright (c) 2006-2013, Andrey N. Sabelnikov, www.sabelnikov.net
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
// * Neither the name of the Andrey N. Sabelnikov nor the
// names of its contributors may be used to endorse or promote products
// derived from this software without specific prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER  BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 



#ifndef _NET_UTILS_BASE_H_
#define _NET_UTILS_BASE_H_

#include <boost/asio/io_service.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <typeinfo>
#include <type_traits>
#include "../shared_sv.h"
#include "enums.h"
#include "../misc_log_ex.h"
#include "../serialization/keyvalue_serialization.h"
#include "../int-util.h"
#include <fmt/core.h>

#undef OXEN_DEFAULT_LOG_CATEGORY
#define OXEN_DEFAULT_LOG_CATEGORY "net"

#ifndef MAKE_IP
#define MAKE_IP( a1, a2, a3, a4 )	(a1|(a2<<8)|(a3<<16)|(((uint32_t)a4)<<24))
#endif

#if BOOST_VERSION >= 107000
#define GET_IO_SERVICE(s) ((boost::asio::io_context&)(s).get_executor().context())
#else
#define GET_IO_SERVICE(s) ((s).get_io_service())
#endif

namespace epee
{

struct connection_id_t : std::array<unsigned char, 16> {
    // Makes a random connection id.  *NOT* cryptographically secure random.
    static connection_id_t random();

    constexpr bool is_nil() const {
        return std::all_of(begin(), end(), [](auto x) { return x == 0; });
    }
};

std::ostream& operator<<(std::ostream& out, const connection_id_t& c);

namespace net_utils
{
	class ipv4_network_address
	{
		uint32_t m_ip;
		uint16_t m_port;

	public:
		constexpr ipv4_network_address() noexcept
			: ipv4_network_address(0, 0)
		{}

		constexpr ipv4_network_address(uint32_t ip, uint16_t port) noexcept
			: m_ip(ip), m_port(port) {}

		bool equal(const ipv4_network_address& other) const noexcept;
		bool less(const ipv4_network_address& other) const noexcept;
		constexpr bool is_same_host(const ipv4_network_address& other) const noexcept
		{ return ip() == other.ip(); }

		constexpr uint32_t ip() const noexcept { return m_ip; }
		constexpr uint16_t port() const noexcept { return m_port; }
		std::string str() const; 
		std::string host_str() const;
		bool is_loopback() const;
		bool is_local() const;
		static constexpr address_type get_type_id() noexcept { return address_type::ipv4; }
		static constexpr zone get_zone() noexcept { return zone::public_; }
		static constexpr bool is_blockable() noexcept { return true; }

		BEGIN_KV_SERIALIZE_MAP()
			if (is_store)
			{
				uint32_t ip = SWAP32LE(this_ref.m_ip);
				epee::serialization::perform_serialize<is_store>(ip, stg, parent_section, "m_ip");
			}
			else
			{
				KV_SERIALIZE(m_ip)
				const_cast<ipv4_network_address&>(this_ref).m_ip = SWAP32LE(this_ref.m_ip);
			}
			KV_SERIALIZE(m_port)
		END_KV_SERIALIZE_MAP()
	};

	inline bool operator==(const ipv4_network_address& lhs, const ipv4_network_address& rhs) noexcept
	{ return lhs.equal(rhs); }
	inline bool operator!=(const ipv4_network_address& lhs, const ipv4_network_address& rhs) noexcept
	{ return !lhs.equal(rhs); }
	inline bool operator<(const ipv4_network_address& lhs, const ipv4_network_address& rhs) noexcept
	{ return lhs.less(rhs); }
	inline bool operator<=(const ipv4_network_address& lhs, const ipv4_network_address& rhs) noexcept
	{ return !rhs.less(lhs); }
	inline bool operator>(const ipv4_network_address& lhs, const ipv4_network_address& rhs) noexcept
	{ return rhs.less(lhs); }
	inline bool operator>=(const ipv4_network_address& lhs, const ipv4_network_address& rhs) noexcept
	{ return !lhs.less(rhs); }

	class ipv4_network_subnet
	{
		uint32_t m_ip;
		uint8_t m_mask;

	public:
		constexpr ipv4_network_subnet() noexcept
			: ipv4_network_subnet(0, 0)
		{}

		constexpr ipv4_network_subnet(uint32_t ip, uint8_t mask) noexcept
			: m_ip(ip), m_mask(mask) {}

		bool equal(const ipv4_network_subnet& other) const noexcept;
		bool less(const ipv4_network_subnet& other) const noexcept;
		constexpr bool is_same_host(const ipv4_network_subnet& other) const noexcept
		{ return subnet() == other.subnet(); }
                bool matches(const ipv4_network_address &address) const;

		constexpr uint32_t subnet() const noexcept { return m_ip & ~(0xffffffffull << m_mask); }
		std::string str() const;
		std::string host_str() const;
		bool is_loopback() const;
		bool is_local() const;
		static constexpr address_type get_type_id() noexcept { return address_type::invalid; }
		static constexpr zone get_zone() noexcept { return zone::public_; }
		static constexpr bool is_blockable() noexcept { return true; }

		BEGIN_KV_SERIALIZE_MAP()
			KV_SERIALIZE(m_ip)
			KV_SERIALIZE(m_mask)
		END_KV_SERIALIZE_MAP()
	};

	inline bool operator==(const ipv4_network_subnet& lhs, const ipv4_network_subnet& rhs) noexcept
	{ return lhs.equal(rhs); }
	inline bool operator!=(const ipv4_network_subnet& lhs, const ipv4_network_subnet& rhs) noexcept
	{ return !lhs.equal(rhs); }
	inline bool operator<(const ipv4_network_subnet& lhs, const ipv4_network_subnet& rhs) noexcept
	{ return lhs.less(rhs); }
	inline bool operator<=(const ipv4_network_subnet& lhs, const ipv4_network_subnet& rhs) noexcept
	{ return !rhs.less(lhs); }
	inline bool operator>(const ipv4_network_subnet& lhs, const ipv4_network_subnet& rhs) noexcept
	{ return rhs.less(lhs); }
	inline bool operator>=(const ipv4_network_subnet& lhs, const ipv4_network_subnet& rhs) noexcept
	{ return !lhs.less(rhs); }

	class ipv6_network_address
	{
	protected:
		boost::asio::ip::address_v6 m_address;
		uint16_t m_port;

	public:
		ipv6_network_address()
			: ipv6_network_address(boost::asio::ip::address_v6::loopback(), 0)
		{}

		ipv6_network_address(const boost::asio::ip::address_v6& ip, uint16_t port)
			: m_address(ip), m_port(port)
		{
		}

		bool equal(const ipv6_network_address& other) const noexcept;
		bool less(const ipv6_network_address& other) const noexcept;
		bool is_same_host(const ipv6_network_address& other) const noexcept
		{ return m_address == other.m_address; }

		boost::asio::ip::address_v6 ip() const noexcept { return m_address; }
		uint16_t port() const noexcept { return m_port; }
		std::string str() const;
		std::string host_str() const;
		bool is_loopback() const;
		bool is_local() const;
		static constexpr address_type get_type_id() noexcept { return address_type::ipv6; }
		static constexpr zone get_zone() noexcept { return zone::public_; }
		static constexpr bool is_blockable() noexcept { return true; }

		static const uint8_t ID = 2;
		BEGIN_KV_SERIALIZE_MAP()
			boost::asio::ip::address_v6::bytes_type bytes = this_ref.m_address.to_bytes();
			epee::serialization::perform_serialize_blob<is_store>(bytes, stg, parent_section, "addr");
			const_cast<boost::asio::ip::address_v6&>(this_ref.m_address) = boost::asio::ip::address_v6(bytes);
			KV_SERIALIZE(m_port)
		END_KV_SERIALIZE_MAP()
	};

	inline bool operator==(const ipv6_network_address& lhs, const ipv6_network_address& rhs) noexcept
	{ return lhs.equal(rhs); }
	inline bool operator!=(const ipv6_network_address& lhs, const ipv6_network_address& rhs) noexcept
	{ return !lhs.equal(rhs); }
	inline bool operator<(const ipv6_network_address& lhs, const ipv6_network_address& rhs) noexcept
	{ return lhs.less(rhs); }
	inline bool operator<=(const ipv6_network_address& lhs, const ipv6_network_address& rhs) noexcept
	{ return !rhs.less(lhs); }
	inline bool operator>(const ipv6_network_address& lhs, const ipv6_network_address& rhs) noexcept
	{ return rhs.less(lhs); }
	inline bool operator>=(const ipv6_network_address& lhs, const ipv6_network_address& rhs) noexcept
	{ return !lhs.less(rhs); }

	class network_address
	{
		struct interface
		{
                        virtual ~interface() {};

			virtual bool equal(const interface&) const = 0;
			virtual bool less(const interface&) const = 0;
			virtual bool is_same_host(const interface&) const = 0;

			virtual std::string str() const = 0;
			virtual std::string host_str() const = 0;
			virtual uint16_t port() const = 0;
			virtual bool is_loopback() const = 0;
			virtual bool is_local() const = 0;
			virtual address_type get_type_id() const = 0;
			virtual zone get_zone() const = 0;
			virtual bool is_blockable() const = 0;
		};

		template<typename T>
		struct implementation final : interface
		{
			T value;

			implementation(const T& src) : value(src) {}
			~implementation() = default;

			// Type-checks for cast are done in cpp
			static const T& cast(const interface& src) noexcept
			{ return static_cast<const implementation<T>&>(src).value; }

			virtual bool equal(const interface& other) const override
			{ return value.equal(cast(other)); }

			virtual bool less(const interface& other) const override
			{ return value.less(cast(other)); }

			virtual bool is_same_host(const interface& other) const override
			{ return value.is_same_host(cast(other)); }

			virtual std::string str() const override { return value.str(); }
			virtual std::string host_str() const override { return value.host_str(); }
			virtual uint16_t port() const override { return value.port(); }
			virtual bool is_loopback() const override { return value.is_loopback(); }
			virtual bool is_local() const override { return value.is_local(); }
			virtual address_type get_type_id() const override { return value.get_type_id(); }
			virtual zone get_zone() const override { return value.get_zone(); }
			virtual bool is_blockable() const override { return value.is_blockable(); }
		};

		std::shared_ptr<interface> self;

		template<typename Type>
		Type& as_mutable() const
		{
			// types `implmentation<Type>` and `implementation<const Type>` are unique
			using Type_ = std::remove_const_t<Type>;
			network_address::interface* const self_ = self.get(); // avoid clang warning in typeid
			if (!self_ || typeid(implementation<Type_>) != typeid(*self_))
				throw std::bad_cast{};
			return static_cast<implementation<Type_>*>(self_)->value;
		}

		// Const: we're serializing
		template<typename T, typename t_storage>
		bool serialize_addr(t_storage& stg, epee::serialization::section* parent) const
		{
		  return epee::serialization::perform_serialize<true>(as<T>(), stg, parent, "addr");
		}

		// Non-const: we're deserializing
		template<typename T, typename t_storage>
		bool serialize_addr(t_storage& stg, epee::serialization::section* parent)
		{
			T addr{};
			if (!epee::serialization::perform_serialize<false>(addr, stg, parent, "addr"))
				return false;
			*this = std::move(addr);
			return true;
		}

	public:
		network_address() : self(nullptr) {}
		template<typename T>
		network_address(const T& src)
			: self(std::make_shared<implementation<T>>(src)) {}
		bool equal(const network_address &other) const;
		bool less(const network_address &other) const;
		bool is_same_host(const network_address &other) const;
		std::string str() const { return self ? self->str() : "<none>"; }
		std::string host_str() const { return self ? self->host_str() : "<none>"; }
		uint16_t port() const { return self ? self->port() : 0; }
		bool is_loopback() const { return self ? self->is_loopback() : false; }
		bool is_local() const { return self ? self->is_local() : false; }
		address_type get_type_id() const { return self ? self->get_type_id() : address_type::invalid; }
		zone get_zone() const { return self ? self->get_zone() : zone::invalid; }
		bool is_blockable() const { return self ? self->is_blockable() : false; }
		template<typename Type> const Type &as() const { return as_mutable<const Type>(); }

        // This serialization is unspeakably disgusting: someone (in Monero PR #5091) decided to add
        // code outside of epee but then put a circular dependency inside this serialization code so
        // that the code won't even compile unless epee links to code outside epee.  But because it
        // was all hidden in templated code (with a template type that NEVER CHANGES) compilation
        // got deferred -- but would fail if anything tried to access this serialization code
        // *without* the external tor/i2p dependencies.  To deal with this unspeakably disgusting
        // hack, this serialization implementation is outside of epee, in
        // src/net/epee_network_address.cpp.
        //
        // They left this comment in the serialization code, which I preserve here as a HUGE red
        // flag that the code stinks, and yet it was still approved by upstream Monero without even
        // a comment about this garbage:
        //
        // // need to `#include "net/[i2p|tor]_address.h"` when serializing `network_address`
        //
        KV_MAP_SERIALIZABLE
	};

	inline bool operator==(const network_address& lhs, const network_address& rhs)
	{ return lhs.equal(rhs); }
	inline bool operator!=(const network_address& lhs, const network_address& rhs)
	{ return !lhs.equal(rhs); }
	inline bool operator<(const network_address& lhs, const network_address& rhs)
	{ return lhs.less(rhs); }
	inline bool operator<=(const network_address& lhs, const network_address& rhs)
	{ return !rhs.less(lhs); }
	inline bool operator>(const network_address& lhs, const network_address& rhs)
	{ return rhs.less(lhs); }
	inline bool operator>=(const network_address& lhs, const network_address& rhs)
	{ return !lhs.less(rhs); }

	/************************************************************************/
	/*                                                                      */
	/************************************************************************/
	struct connection_context_base
	{
    const connection_id_t m_connection_id;
    const network_address m_remote_address;
    const bool     m_is_income;
    std::chrono::steady_clock::time_point m_started;
    std::chrono::steady_clock::time_point m_last_recv;
    std::chrono::steady_clock::time_point m_last_send;
    uint64_t m_recv_cnt;
    uint64_t m_send_cnt;
    double m_current_speed_down;
    double m_current_speed_up;
    double m_max_speed_down;
    double m_max_speed_up;

    connection_context_base(connection_id_t connection_id,
                            const network_address &remote_address, bool is_income,
                            std::chrono::steady_clock::time_point last_recv = std::chrono::steady_clock::time_point::min(),
                            std::chrono::steady_clock::time_point last_send = std::chrono::steady_clock::time_point::min(),
                            uint64_t recv_cnt = 0, uint64_t send_cnt = 0):
                                            m_connection_id(connection_id),
                                            m_remote_address(remote_address),
                                            m_is_income(is_income),
                                            m_started(std::chrono::steady_clock::now()),
                                            m_last_recv(last_recv),
                                            m_last_send(last_send),
                                            m_recv_cnt(recv_cnt),
                                            m_send_cnt(send_cnt),
                                            m_current_speed_down(0),
                                            m_current_speed_up(0),
                                            m_max_speed_down(0),
                                            m_max_speed_up(0)
    {}

    connection_context_base(): m_connection_id(),
                               m_remote_address(),
                               m_is_income(false),
                               m_started(std::chrono::steady_clock::now()),
                               m_last_recv(std::chrono::steady_clock::time_point::min()),
                               m_last_send(std::chrono::steady_clock::time_point::min()),
                               m_recv_cnt(0),
                               m_send_cnt(0),
                               m_current_speed_down(0),
                               m_current_speed_up(0),
                               m_max_speed_down(0),
                               m_max_speed_up(0)
    {}

    connection_context_base(const connection_context_base& a): connection_context_base()
    {
      set_details(a.m_connection_id, a.m_remote_address, a.m_is_income);
    }

    connection_context_base& operator=(const connection_context_base& a)
    {
      set_details(a.m_connection_id, a.m_remote_address, a.m_is_income);
      return *this;
    }
    
  private:
    template<class t_protocol_handler>
    friend class connection;
    void set_details(connection_id_t connection_id, const network_address &remote_address, bool is_income)
    {
      this->~connection_context_base();
      new(this) connection_context_base(connection_id, remote_address, is_income);
    }

	};

	/************************************************************************/
	/*                                                                      */
	/************************************************************************/
	struct i_service_endpoint
	{
    virtual bool do_send(shared_sv message)=0;
    virtual bool close()=0;
    virtual bool send_done()=0;
    virtual bool call_run_once_service_io()=0;
    virtual bool request_callback()=0;
    virtual boost::asio::io_service& get_io_service()=0;
    //protect from deletion connection object(with protocol instance) during external call "invoke"
    virtual bool add_ref()=0;
    virtual bool release()=0;
  protected:
    virtual ~i_service_endpoint() noexcept(false) {}
	};


  //some helpers
  std::string print_connection_context(const connection_context_base& ctx);
  std::string print_connection_context_short(const connection_context_base& ctx);

  inline std::ostream& operator<<(std::ostream& os, const connection_context_base& ct)
  {
    os << "[" << epee::net_utils::print_connection_context_short(ct) << "] ";
    return os;
  }

#define CHECK_AND_ASSERT_MES_CC(condition, return_val, ...) CHECK_AND_ASSERT_MES(condition, return_val, "{}: {}", context, fmt::format(__VA_ARGS__))

}
}

namespace std {
template <>
struct hash<epee::connection_id_t> {
    size_t operator()(const epee::connection_id_t& id) const {
        constexpr size_t inverse_golden_ratio = sizeof(size_t) >= 8 ? 0x9e37'79b9'7f4a'7c15 : 0x9e37'79b9;

        uint64_t a, b;
        std::memcpy(&a, id.data(), 8);
        std::memcpy(&b, id.data() + 8, 8);
        auto h = hash<uint64_t>{}(a);
        return hash<uint64_t>{}(b) + inverse_golden_ratio + (h << 6) + (h >> 2);
    }
};
}  // namespace std

template <std::derived_from<epee::net_utils::connection_context_base> T, typename Char>
struct fmt::formatter<
        T,
        Char,
        // SFINAE shouldn't be needed here in C++20, but gcc-10 disagrees:
        std::enable_if_t<std::is_base_of_v<epee::net_utils::connection_context_base, T>>>
        : fmt::formatter<std::string_view> {
    auto format(
            const epee::net_utils::connection_context_base& connection_context,
            format_context& ctx) const {
        return formatter<std::string_view>::format(
                fmt::format("[{}]", epee::net_utils::print_connection_context_short(connection_context)),
                ctx);
    }
};

#endif //_NET_UTILS_BASE_H_

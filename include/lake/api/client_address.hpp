#pragma once

// Which address a request is counted against for per-client limits (lake#1083): the client
// address a trusted proxy reported in X-Real-IP or X-Forwarded-For when the connection's peer is
// one of the trusted proxies (LAKE_TRUSTED_PROXY_CIDRS), else the peer address itself.
// docs/deployment/client-addresses.md.

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lake::api {

// One address range: an IPv4 or IPv6 network and its prefix length. IPv4 is held as IPv4, never
// mapped; an IPv4-mapped IPv6 peer (::ffff:a.b.c.d) is matched as its IPv4 address.
struct IpPrefix {
    bool v6 = false;
    std::array<std::uint8_t, 16> bytes{};
    int length = 0;

    bool operator==(const IpPrefix&) const = default;
};

// The peers whose X-Real-IP and X-Forwarded-For are believed.
struct TrustedProxies {
    std::vector<IpPrefix> prefixes;
    // The setting's text, normalised, for the startup log line.
    std::string description;

    bool operator==(const TrustedProxies&) const = default;
};

// The ranges used when LAKE_TRUSTED_PROXY_CIDRS is unset or empty: loopback and the private
// ranges (RFC 1918, RFC 6598 shared address space, IPv6 unique-local and link-local). Inside a
// cluster every proxy, the ingress controller included, has an address in one of them, so an
// image rolled before its operator sets the variable keeps counting each visitor on their own;
// a client that reaches lake directly from a public address is counted on that address.
inline constexpr std::string_view default_trusted_proxy_cidrs =
    "127.0.0.0/8,::1/128,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,100.64.0.0/10,fc00::/7,fe80::/10";

// Parses LAKE_TRUSTED_PROXY_CIDRS: comma-separated CIDRs (a bare address is a /32 or /128);
// empty for the default above; `none` trusts no peer, so every request is counted on its peer
// address. An error names the entry that is not a CIDR, or one with host bits set.
std::expected<TrustedProxies, std::string> parse_trusted_proxies(std::string_view setting);

// Parses one address (IPv4, IPv6 or IPv4-mapped IPv6) as a full-length prefix.
std::optional<IpPrefix> parse_ip_address(std::string_view address);

// True when `peer`, the connection's address as text, is inside one of the trusted ranges.
bool is_trusted_proxy(const TrustedProxies& trusted, std::string_view peer);

// The address a request is counted against. `peer` is the connection's address (empty when it
// is not known, which counts as untrusted). From a trusted peer: X-Real-IP, else the last
// X-Forwarded-For hop, else the peer. From any other peer: the peer, whatever headers it sent;
// "direct" when the peer is not known. At most 64 bytes.
std::string client_address(const TrustedProxies& trusted,
                           std::string_view peer,
                           std::optional<std::string_view> real_ip,
                           std::optional<std::string_view> forwarded_for);

} // namespace lake::api

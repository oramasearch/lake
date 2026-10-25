#include "lake/api/client_address.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <charconv>

namespace lake::api {
namespace {

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t");
    return text.substr(first, last - first + 1);
}

// An IPv4-mapped IPv6 address (::ffff:a.b.c.d) becomes its IPv4 address.
IpPrefix unmap(IpPrefix address) {
    static constexpr std::array<std::uint8_t, 12> mapped{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (address.v6 && std::equal(mapped.begin(), mapped.end(), address.bytes.begin())) {
        IpPrefix v4;
        std::copy(address.bytes.begin() + 12, address.bytes.end(), v4.bytes.begin());
        v4.length = address.length == 128 ? 32 : std::max(0, address.length - 96);
        return v4;
    }
    return address;
}

bool contains(const IpPrefix& range, const IpPrefix& address) {
    if (range.v6 != address.v6) {
        return false;
    }
    const int full = range.length / 8;
    for (int i = 0; i < full; ++i) {
        if (range.bytes[i] != address.bytes[i]) {
            return false;
        }
    }
    const int rest = range.length % 8;
    if (rest == 0) {
        return true;
    }
    const auto mask = static_cast<std::uint8_t>(0xff << (8 - rest));
    return (range.bytes[full] & mask) == (address.bytes[full] & mask);
}

} // namespace

std::optional<IpPrefix> parse_ip_address(std::string_view text) {
    if (text.empty() || text.size() >= INET6_ADDRSTRLEN) {
        return std::nullopt;
    }
    const std::string address(text);
    IpPrefix parsed;
    parsed.v6 = address.find(':') != std::string::npos;
    if (inet_pton(parsed.v6 ? AF_INET6 : AF_INET, address.c_str(), parsed.bytes.data()) != 1) {
        return std::nullopt;
    }
    parsed.length = parsed.v6 ? 128 : 32;
    return unmap(parsed);
}

std::expected<TrustedProxies, std::string> parse_trusted_proxies(std::string_view setting) {
    std::string_view text = trim(setting);
    if (text.empty()) {
        text = default_trusted_proxy_cidrs;
    }
    TrustedProxies trusted;
    if (text == "none") {
        trusted.description = "none";
        return trusted;
    }
    while (!text.empty()) {
        const auto comma = text.find(',');
        const std::string_view entry = trim(text.substr(0, comma));
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        if (entry.empty()) {
            continue;
        }
        const auto slash = entry.find('/');
        auto address = parse_ip_address(entry.substr(0, slash));
        if (!address) {
            return std::unexpected("LAKE_TRUSTED_PROXY_CIDRS: '" + std::string(entry) +
                                   "' is not an IPv4 or IPv6 address or CIDR");
        }
        if (slash != std::string_view::npos) {
            const std::string_view digits = entry.substr(slash + 1);
            int length = -1;
            const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), length);
            const bool mapped = !address->v6 && entry.substr(0, slash).find(':') != std::string_view::npos;
            const int maximum = address->v6 ? 128 : (mapped ? 128 : 32);
            if (digits.empty() || parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() ||
                length < 0 || length > maximum) {
                return std::unexpected("LAKE_TRUSTED_PROXY_CIDRS: '" + std::string(entry) +
                                       "' has no valid prefix length");
            }
            if (mapped) {
                if (length < 96) {
                    return std::unexpected("LAKE_TRUSTED_PROXY_CIDRS: '" + std::string(entry) +
                                           "' is an IPv4-mapped range shorter than /96");
                }
                length -= 96;
            }
            address->length = length;
        }
        IpPrefix network = *address;
        for (int bit = network.length; bit < (network.v6 ? 128 : 32); ++bit) {
            if ((network.bytes[bit / 8] & (0x80 >> (bit % 8))) != 0) {
                return std::unexpected("LAKE_TRUSTED_PROXY_CIDRS: '" + std::string(entry) +
                                       "' has host bits set past its prefix length");
            }
        }
        trusted.prefixes.push_back(network);
        if (!trusted.description.empty()) {
            trusted.description += ",";
        }
        trusted.description += entry;
    }
    if (trusted.prefixes.empty()) {
        return std::unexpected(std::string("LAKE_TRUSTED_PROXY_CIDRS lists no CIDR; use 'none' to trust no peer"));
    }
    return trusted;
}

bool is_trusted_proxy(const TrustedProxies& trusted, std::string_view peer) {
    const auto address = parse_ip_address(peer);
    if (!address) {
        return false;
    }
    for (const auto& range : trusted.prefixes) {
        if (contains(range, *address)) {
            return true;
        }
    }
    return false;
}

std::string client_address(const TrustedProxies& trusted,
                           std::string_view peer,
                           std::optional<std::string_view> real_ip,
                           std::optional<std::string_view> forwarded_for) {
    if (!is_trusted_proxy(trusted, peer)) {
        return peer.empty() || peer == "unknown" ? std::string("direct") : std::string(peer.substr(0, 64));
    }
    if (real_ip) {
        return std::string(real_ip->substr(0, 64));
    }
    if (forwarded_for) {
        std::string_view forwarded = *forwarded_for;
        const auto comma = forwarded.rfind(',');
        forwarded = trim(comma == std::string_view::npos ? forwarded : forwarded.substr(comma + 1));
        if (!forwarded.empty()) {
            return std::string(forwarded.substr(0, 64));
        }
    }
    return std::string(peer.substr(0, 64));
}

} // namespace lake::api

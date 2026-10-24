// Which address per-client limits count a request against (lake#1083,
// docs/deployment/client-addresses.md).
#include <gtest/gtest.h>

#include "lake/api/client_address.hpp"

namespace {

using lake::api::client_address;
using lake::api::is_trusted_proxy;
using lake::api::parse_trusted_proxies;

lake::api::TrustedProxies trust(std::string_view setting) {
    auto parsed = parse_trusted_proxies(setting);
    EXPECT_TRUE(parsed.has_value()) << setting << ": " << (parsed ? "" : parsed.error());
    return parsed ? *parsed : lake::api::TrustedProxies{};
}

TEST(ClientAddress, UnsetTrustsLoopbackAndPrivateRangesOnly) {
    const auto unset = trust("");
    EXPECT_EQ(unset, trust("   "));
    EXPECT_EQ(unset.prefixes, trust(lake::api::default_trusted_proxy_cidrs).prefixes);
    for (const char* peer : {"127.0.0.1", "::1", "10.244.2.176", "172.16.0.9", "172.31.255.255",
                             "192.168.1.1", "100.64.0.1", "fd00::1", "fe80::1", "::ffff:10.1.2.3"}) {
        EXPECT_TRUE(is_trusted_proxy(unset, peer)) << peer;
    }
    for (const char* peer : {"203.0.113.7", "172.32.0.1", "8.8.8.8", "2001:db8::1",
                             "::ffff:203.0.113.7", "", "unknown", "10.0.0.1.5"}) {
        EXPECT_FALSE(is_trusted_proxy(unset, peer)) << peer;
    }
}

TEST(ClientAddress, TrustedPeerIsCountedOnTheAddressItReports) {
    const auto proxies = trust("10.244.0.0/16");
    EXPECT_EQ(client_address(proxies, "10.244.2.176", "203.0.113.7", "198.51.100.1, 203.0.113.8"),
              "203.0.113.7");
    // Without X-Real-IP, the last X-Forwarded-For hop: the one the nearest proxy appended.
    EXPECT_EQ(client_address(proxies, "10.244.2.176", std::nullopt, "198.51.100.1,  203.0.113.8 "),
              "203.0.113.8");
    EXPECT_EQ(client_address(proxies, "10.244.2.176", std::nullopt, "203.0.113.9"), "203.0.113.9");
    // Neither header, or an empty last hop: the proxy's own address, never one shared bucket.
    EXPECT_EQ(client_address(proxies, "10.244.2.176", std::nullopt, std::nullopt), "10.244.2.176");
    EXPECT_EQ(client_address(proxies, "10.244.2.176", std::nullopt, "203.0.113.9, "), "10.244.2.176");
    EXPECT_EQ(client_address(proxies, "10.244.2.176", std::string(100, 'a'), std::nullopt).size(), 64U);
}

TEST(ClientAddress, UntrustedPeerIsCountedOnItsOwnAddressWhateverItSends) {
    const auto proxies = trust("10.244.0.0/16");
    for (const auto& real_ip : {"192.0.2.1", "192.0.2.2", "192.0.2.3"}) {
        EXPECT_EQ(client_address(proxies, "10.0.5.5", real_ip, "198.51.100.1"), "10.0.5.5");
    }
    EXPECT_EQ(client_address(proxies, "203.0.113.50", std::nullopt, "198.51.100.1"), "203.0.113.50");
    EXPECT_EQ(client_address(proxies, "2001:db8::5", "192.0.2.1", std::nullopt), "2001:db8::5");
    // A peer the server cannot name is no proxy: one shared bucket.
    EXPECT_EQ(client_address(proxies, "", "192.0.2.1", std::nullopt), "direct");
    EXPECT_EQ(client_address(proxies, "unknown", "192.0.2.1", std::nullopt), "direct");
}

TEST(ClientAddress, NoneTrustsNoPeer) {
    const auto none = trust("none");
    EXPECT_TRUE(none.prefixes.empty());
    EXPECT_EQ(none.description, "none");
    EXPECT_FALSE(is_trusted_proxy(none, "127.0.0.1"));
    EXPECT_EQ(client_address(none, "127.0.0.1", "192.0.2.1", "192.0.2.2"), "127.0.0.1");
}

TEST(ClientAddress, ParsesAddressesPrefixesAndMappedRanges) {
    const auto proxies = trust(" 10.244.2.176 , 2001:db8:1::/48,::ffff:192.168.0.0/112,0.0.0.0/0 ");
    EXPECT_EQ(proxies.prefixes.size(), 4U);
    EXPECT_EQ(proxies.description, "10.244.2.176,2001:db8:1::/48,::ffff:192.168.0.0/112,0.0.0.0/0");
    EXPECT_EQ(proxies.prefixes[0].length, 32);
    EXPECT_EQ(proxies.prefixes[2].length, 16);
    EXPECT_FALSE(proxies.prefixes[2].v6);
    EXPECT_TRUE(is_trusted_proxy(trust("2001:db8:1::/48"), "2001:db8:1:ffff::1"));
    EXPECT_FALSE(is_trusted_proxy(trust("2001:db8:1::/48"), "2001:db8:2::1"));
    EXPECT_TRUE(is_trusted_proxy(trust("::ffff:192.168.0.0/112"), "192.168.4.4"));
    EXPECT_TRUE(is_trusted_proxy(trust("192.168.0.0/16"), "::ffff:192.168.4.4"));
    EXPECT_TRUE(is_trusted_proxy(trust("10.244.2.176"), "10.244.2.176"));
    EXPECT_FALSE(is_trusted_proxy(trust("10.244.2.176"), "10.244.2.177"));
    EXPECT_TRUE(is_trusted_proxy(trust("10.0.0.0/9"), "10.127.0.1"));
    EXPECT_FALSE(is_trusted_proxy(trust("10.0.0.0/9"), "10.128.0.1"));
}

TEST(ClientAddress, RefusesWhatIsNotACidr) {
    for (const char* bad : {"10.0.0.0/33", "10.0.0.1/24", "::1/129", "ingress-nginx", "10.0.0.0/",
                            "10.0.0.0/x", "10.0.0.0/8,bogus", ",", "::ffff:10.0.0.0/64", "10.0.0.0/-1"}) {
        const auto parsed = parse_trusted_proxies(bad);
        EXPECT_FALSE(parsed.has_value()) << bad;
        if (!parsed) {
            EXPECT_NE(parsed.error().find("LAKE_TRUSTED_PROXY_CIDRS"), std::string::npos) << bad;
        }
    }
}

} // namespace

namespace {

// lake#1132: the proxy listener's port.
TEST(ClientAddress, ProxyPortParsesAPortOrNothing) {
    using lake::api::parse_proxy_port;
    EXPECT_EQ(*parse_proxy_port(""), std::nullopt);
    EXPECT_EQ(*parse_proxy_port("  "), std::nullopt);
    EXPECT_EQ(*parse_proxy_port("4901"), std::optional<unsigned short>{4901});
    EXPECT_EQ(*parse_proxy_port(" 1 "), std::optional<unsigned short>{1});
    EXPECT_EQ(*parse_proxy_port("65535"), std::optional<unsigned short>{65535});
    for (const char* bad : {"0", "65536", "-1", "49o1", "4901x", "http", "4901,4902"}) {
        const auto parsed = parse_proxy_port(bad);
        ASSERT_FALSE(parsed.has_value()) << bad;
        EXPECT_NE(parsed.error().find(bad), std::string::npos) << parsed.error();
    }
}

// With a proxy listener, the main listener believes nobody unless LAKE_TRUSTED_PROXY_CIDRS says so.
TEST(ClientAddress, AProxyListenerMakesTheMainListenerTrustNobodyByDefault) {
    EXPECT_EQ(lake::api::default_trusted_proxy_setting(false),
              lake::api::default_trusted_proxy_cidrs);
    EXPECT_EQ(lake::api::default_trusted_proxy_setting(true), "none");
    const auto with_proxy = trust(lake::api::default_trusted_proxy_setting(true));
    EXPECT_FALSE(is_trusted_proxy(with_proxy, "10.244.2.176"));
    EXPECT_FALSE(is_trusted_proxy(with_proxy, "127.0.0.1"));
}

// The decided form: believed takes the headers whatever the peer; not believed takes the peer.
TEST(ClientAddress, BelievedHeadersAreTakenWhateverThePeer) {
    EXPECT_EQ(client_address(true, "10.244.9.9", "203.0.113.7", "198.51.100.1"), "203.0.113.7");
    EXPECT_EQ(client_address(true, "8.8.8.8", std::nullopt, "198.51.100.1, 203.0.113.9"),
              "203.0.113.9");
    EXPECT_EQ(client_address(true, "10.244.9.9", std::nullopt, std::nullopt), "10.244.9.9");
    EXPECT_EQ(client_address(false, "10.244.9.9", "203.0.113.7", "198.51.100.1"), "10.244.9.9");
    EXPECT_EQ(client_address(false, "", "203.0.113.7", std::nullopt), "direct");
}

} // namespace

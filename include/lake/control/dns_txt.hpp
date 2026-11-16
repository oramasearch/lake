#pragma once

// TXT lookups for custom-domain proof of control (lake#949, docs/api/platform.md#custom-domains).
//
// getaddrinfo answers addresses only, so lake asks the name servers its host is configured with
// (/etc/resolv.conf: in a pod, the cluster's DNS) for TXT records itself: one question over UDP,
// again over TCP when the answer is truncated, each bounded in time. It follows nothing and caches
// nothing: the answer is what the resolver says now.

#include <chrono>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lake::control {

// The DNS query message for `name`'s TXT records (RFC 1035 section 4, recursion desired), with
// message id `id`. Refuses a name that is not a valid DNS name (an empty label, a label over 63
// octets, a name over 253).
std::expected<std::vector<std::uint8_t>, std::string> encode_txt_query(std::string_view name,
                                                                       std::uint16_t id);

// What a TXT answer to the query with id `id` says. Each TXT record's character-strings are joined
// into one string. A name that does not exist, or has no TXT records, answers an empty list. A
// truncated answer, a server failure, a refusal or a malformed message is an error. `truncated` is
// set when the answer was cut short (the caller asks again over TCP).
struct TxtAnswer {
    std::vector<std::string> records;
    bool truncated = false;
};
std::expected<TxtAnswer, std::string> decode_txt_answer(std::span<const std::uint8_t> message,
                                                        std::uint16_t id);

// The `nameserver` addresses of a resolv.conf text, in order (IPv4 and IPv6 literals).
std::vector<std::string> resolv_conf_nameservers(std::string_view text);

// The TXT records of `name`, asked of the host's name servers (/etc/resolv.conf, else 127.0.0.1)
// in order until one answers, each within `per_server`. Blocking: an asynchronous handler runs it
// with run_blocking.
std::expected<std::vector<std::string>, std::string>
lookup_txt(const std::string& name,
           std::chrono::milliseconds per_server = std::chrono::milliseconds(2000));

} // namespace lake::control

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The tunnel client downloads (lake#745, docs/api/tunnel-clients.md): the `zetta-tunnel` binaries
// a developer runs to open their Developer Access tunnel, copied into Lake's image from the private
// `oramasearch/zetta-tunnel-clients` image and handed out by `tunnel_client_downloads` as signed,
// short-lived links that `GET /tunnel-clients/<platform>` serves to a plain `curl`.
namespace lake::api {

// A link a plain HTTP client follows without a session: HMAC-SHA-256 under LAKE_INSTANCE_KEK over
// the link's fields, base64url without padding. The Hub's basic-LFS upload hrefs were signed this
// way first; the tunnel client links reuse it (docs/serving/http.md#signed-links). Empty when the
// key is empty or the HMAC fails.
std::string signed_link_signature(std::string_view key, std::string_view payload);
// Whether `signature` is the payload's signature under `key`, compared in constant time. False when
// the key is empty.
bool signed_link_matches(std::string_view key, std::string_view payload, std::string_view signature);

// Where the image puts the clients unless LAKE_TUNNEL_CLIENTS_DIR or --tunnel-clients-dir says
// otherwise.
inline constexpr std::string_view default_tunnel_clients_dir = "/usr/share/lake/tunnel-clients";
// How long a download link works.
inline constexpr std::int64_t tunnel_client_link_seconds = 15 * 60;

// The platforms Lake hands a client out for: the allow-list. A request for any other name is
// refused before anything is read, so a platform can never name a path.
struct TunnelClientPlatform {
    std::string_view platform;  // darwin-arm64
    std::string_view os;        // darwin
    std::string_view arch;      // arm64
    std::string_view file_name; // zetta-tunnel-darwin-arm64, the file in the directory
};
std::span<const TunnelClientPlatform> tunnel_client_platforms();
const TunnelClientPlatform* find_tunnel_client_platform(std::string_view platform);

struct TunnelClient {
    std::string platform;
    std::string file_name;
    std::string sha256; // lowercase hex, from SHA256SUMS
    std::uint64_t size = 0;
    std::filesystem::path path;
};
struct TunnelClients {
    std::string version; // VERSION, trimmed
    std::vector<TunnelClient> clients; // in tunnel_client_platforms() order
};

// What the directory holds: VERSION, and each allow-listed file that is a regular file with a
// well-formed line in SHA256SUMS. nullopt when the directory is not configured or missing, has no
// usable VERSION, or holds no client, so a Lake without the clients (local development) answers
// that they are unavailable.
std::optional<TunnelClients> read_tunnel_clients(const std::filesystem::path& directory);

// The signed fields of one link. The version is signed too, so a link made before the clients
// were replaced stops working instead of serving different bytes than its sha256 says.
std::string tunnel_client_link_payload(std::string_view platform,
                                       std::string_view version,
                                       std::int64_t expires);
std::string tunnel_client_signature(std::string_view key,
                                    std::string_view platform,
                                    std::string_view version,
                                    std::int64_t expires);
// The link's path and query: /tunnel-clients/<platform>?expires=<unix seconds>&signature=<sig>.
std::string tunnel_client_link(std::string_view platform,
                               std::int64_t expires,
                               std::string_view signature);

enum class TunnelClientLinkCheck { valid, invalid, expired };
// Checks a link's signature, then its expiry, for the clients `version` now served.
TunnelClientLinkCheck check_tunnel_client_link(std::string_view key,
                                               std::string_view platform,
                                               std::string_view version,
                                               std::string_view expires,
                                               std::string_view signature,
                                               std::int64_t now);

} // namespace lake::api

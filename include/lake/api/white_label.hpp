#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// A white-labelled Lake (lake#1136, lake#1129, docs/api/clerk.md): a Lake that
// serves a customer's own domain and signs people in there, through a Clerk instance for that
// domain or the customer's own identity provider. The operator supplies every value as an
// environment variable (orama-operator#947); with none set Lake behaves as before.
namespace lake::api {

// The hosts this Lake answers as itself.
struct InstanceHosts {
    // LAKE_PUBLIC_HOST: the host every link, callback and passkey names (the customer's host on a
    // white-labelled Lake).
    std::string public_host;
    // LAKE_HOST_ALIASES: other hosts that are this Lake too (the Orama host it was created on).
    // Browser pages opened there are redirected to the public host; everything else is served as
    // on the public host. Lowercased, without ports.
    std::vector<std::string> aliases;
    // LAKE_CONTROL_HOST: this Lake's name on the operator's orama_control rows. It stays the Orama
    // host when the public host moves to the customer's, so the operator, which keys the rows on
    // that host too, keeps matching them. Defaults to the public host.
    std::string control_host;
};

// Reads LAKE_PUBLIC_HOST, LAKE_HOST_ALIASES (comma-separated host names) and LAKE_CONTROL_HOST.
// An error names the variable it is about, and startup stops on it: an alias that is not a host
// name, an alias equal to the public host, or aliases or a control host without a public host.
std::expected<InstanceHosts, std::string> instance_hosts_from_environment(
    std::string_view public_host, std::string_view aliases, std::string_view control_host);

// Whether `host` (any case, an optional :port) is one of `aliases`.
bool is_host_alias(const std::vector<std::string>& aliases, std::string_view host);

// The location a browser page opened on an alias is sent to: https://<public host><target>, where
// target is the request's path and query. Only a path that starts with one '/' is kept; anything
// else goes to the public host's root.
std::string alias_redirect_location(std::string_view public_host, std::string_view target);

// LAKE_SIGN_IN_SSO_PROVIDER: the id of the enterprise SSO provider that is this Lake's only way in
// for people (the customer's identity provider). Empty: Lake's usual sign-in. It cannot be combined
// with Clerk: a Lake signs in through Clerk or through the customer's provider, never both.
std::expected<std::string, std::string> sso_sign_in_from_environment(std::string_view provider_id,
                                                                     bool clerk_enabled);

// Under SSO-only sign-in, the reason `capability` with `input` is refused, else nullopt. Refused in
// Lake's own pool (`pool_id` "lake" or absent): password sign-up, sign-in and its MFA step, TAAP
// sign-in, passkey sign-in, social OAuth, and email recovery. Enterprise SSO starts are refused for
// a provider other than the configured one when that provider's pool is Lake's own
// (`provider_pool`, empty when it is not known or not an SSO capability). Applications' own pools
// are not affected: they are the customer's applications' users, with their own settings.
std::optional<std::string> sso_only_refusal(std::string_view configured_provider,
                                            std::string_view capability,
                                            const nlohmann::json& input,
                                            std::string_view provider_pool);

// Whether `capability` names an enterprise SSO provider by `provider_id` (so the gate needs that
// provider's pool).
bool names_sso_provider(std::string_view capability);

} // namespace lake::api

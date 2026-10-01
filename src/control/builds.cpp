#include "lake/control/builds.hpp"

#include <openssl/crypto.h>

namespace lake::control {

std::optional<std::string> build_token_purpose(const Build& build, std::string_view digest) {
    if (digest.size() != 64 || build.desired == "cancelled" || build.phase == "Cancelled" ||
        build.phase == "Succeeded" || build.phase == "Failed") {
        return std::nullopt;
    }
    std::optional<std::string> matched;
    for (const std::string purpose : {"source", "push", "events"}) {
        const auto hash = build.token_hashes.find(purpose);
        if (hash == build.token_hashes.end() || !hash->is_string()) {
            continue;
        }
        const std::string& expected = hash->get_ref<const std::string&>();
        if (expected.size() != digest.size() ||
            CRYPTO_memcmp(expected.data(), digest.data(), digest.size()) != 0) {
            continue;
        }
        if (matched) {
            return std::nullopt;
        }
        matched = purpose;
    }
    return matched;
}

std::vector<std::string> build_token_scopes(std::string_view purpose) {
    if (purpose == "source") {
        return {"repository_tree", "repository_file"};
    }
    if (purpose == "push") {
        return {"registry_push", "registry_pull"};
    }
    if (purpose == "events") {
        return {"publish_build_event"};
    }
    return {};
}

namespace {

nlohmann::json public_image(const Build& build, std::string_view public_host) {
    if (build.phase != "Succeeded" || build.digest.empty() || public_host.empty()) {
        return nullptr;
    }
    const std::size_t path = build.target.find('/');
    const std::size_t tag = build.target.rfind(':');
    if (path == std::string::npos || tag == std::string::npos || tag <= path + 1) {
        return nullptr;
    }
    while (public_host.ends_with('/')) {
        public_host.remove_suffix(1);
    }
    if (public_host.empty()) {
        return nullptr;
    }
    const std::string repository = build.target.substr(path + 1, tag - path - 1);
    return std::string(public_host) + "/" + repository + "@" + build.digest;
}

} // namespace

nlohmann::json build_json(const Build& build, std::string_view public_host) {
    return {
        {"id", build.id},
        {"customer_id", build.customer_id},
        {"workspace_id", build.workspace_id},
        {"repository", build.repository},
        {"revision", build.revision},
        {"context", build.context},
        {"dockerfile", build.dockerfile},
        {"target", build.target},
        {"build_args", build.build_args},
        {"desired", build.desired},
        {"phase", build.phase},
        {"digest", build.digest.empty() ? nlohmann::json(nullptr) : nlohmann::json(build.digest)},
        {"image", public_image(build, public_host)},
        {"size", build.size},
        {"message", build.message},
        {"created_at", build.created_at},
        {"updated_at", build.updated_at}};
}

} // namespace lake::control

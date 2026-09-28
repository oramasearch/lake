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

nlohmann::json build_json(const Build& build) {
    return {
        {"id", build.id},
        {"customer_id", build.customer_id},
        {"repository", build.repository},
        {"revision", build.revision},
        {"context", build.context},
        {"dockerfile", build.dockerfile},
        {"target", build.target},
        {"build_args", build.build_args},
        {"desired", build.desired},
        {"phase", build.phase},
        {"digest", build.digest.empty() ? nlohmann::json(nullptr) : nlohmann::json(build.digest)},
        {"size", build.size},
        {"message", build.message},
        {"created_at", build.created_at},
        {"updated_at", build.updated_at}};
}

} // namespace lake::control

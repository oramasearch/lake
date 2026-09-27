#pragma once

#include <chrono>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "lake/audit/audit.hpp"
#include "lake/control/store.hpp"

namespace lake::control::agents {

struct Error {
    std::string code;
    std::string message;
};

template <class T> using Result = std::expected<T, Error>;
using Clock = std::function<std::chrono::sys_seconds()>;

struct MintRequest {
    std::string customer_id;
    std::string durable_agent_taap_id;
    std::string dispatcher_taap_id;
    std::vector<std::string> scopes;
    int lifetime_seconds = 900;
    // lake#234: no expiry at all. Only when the customer and the instance allow it (checked by
    // the capability); the key stays revocable.
    bool never_expires = false;
};

struct MintedSession {
    NewAgentSession record;
    std::string private_key;
};

struct VerifiedDrip {
    std::string durable_agent_taap_id;
    std::string dispatcher_taap_id;
    std::string session_taap_id;
    std::vector<std::string> scopes;
    std::string expires_at;
};

class Service {
public:
    explicit Service(Clock clock = {});

    Result<MintedSession> mint(Store& store,
                               const MintRequest& request,
                               const audit::IdentityOptions& identity_options) const;
    Result<VerifiedDrip> verify_drip(const AgentSession& session,
                                     std::span<const std::byte> canonical_body,
                                     std::span<const std::byte> encoded_signature) const;

private:
    Clock clock_;
};

} // namespace lake::control::agents

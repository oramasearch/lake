// SPDX-License-Identifier: MIT

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
    // The repository the session acts for (lake#1346): its lake UUID, kept with the session, and
    // the hex id liquid gave it, written as the delegation's `repository` (liquid#515).
    std::string repository_id;
    std::string liquid_repository_id;
    // Every key expires (liquid#514's `not_after`); at most fifteen minutes.
    int lifetime_seconds = 900;
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
    std::string repository_id;
};

class Service {
public:
    explicit Service(Clock clock = {});

    // acting_identity comes from authentication, independently of the requested dispatcher.
    Result<MintedSession> mint(Store& store,
                               const MintRequest& request,
                               const std::string& acting_identity,
                               const audit::IdentityOptions& identity_options) const;
    Result<VerifiedDrip> verify_drip(const AgentSession& session,
                                     std::span<const std::byte> canonical_body,
                                     std::span<const std::byte> encoded_signature) const;

private:
    Clock clock_;
};

} // namespace lake::control::agents

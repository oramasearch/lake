#pragma once

#include <string_view>

namespace lake::policy {

// The Phase 6 policy boundary. lake #19 replaces AdminAuthorizer with the
// attestation-backed graph resolver without changing callers.
class Authorizer {
public:
    virtual ~Authorizer() = default;

    [[nodiscard]] virtual bool authorize(std::string_view principal,
                                         std::string_view object,
                                         std::string_view action) const = 0;
};

// Interim policy while lake #16/#19 lands: the existing admin bearer has all
// repository permissions; every other principal is denied.
class AdminAuthorizer final : public Authorizer {
public:
    [[nodiscard]] bool authorize(std::string_view principal,
                                 std::string_view object,
                                 std::string_view action) const override;
};

} // namespace lake::policy

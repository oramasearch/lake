#include "lake/policy/authorization.hpp"

namespace lake::policy {

bool AdminAuthorizer::authorize(std::string_view principal,
                                std::string_view object,
                                std::string_view action) const {
    static_cast<void>(object);
    static_cast<void>(action);
    return principal == "admin";
}

} // namespace lake::policy

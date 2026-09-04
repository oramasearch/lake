#include "lake/policy/authorize.hpp"

namespace lake::policy {
namespace {

class BootstrapAuthorizer final : public Authorizer {
public:
  [[nodiscard]] std::vector<bool>
  authorize(const std::string &principal,
            const std::vector<Request> &requests) const override {
    return std::vector<bool>(requests.size(), principal == "admin");
  }
};

} // namespace

std::shared_ptr<const Authorizer> make_bootstrap_authorizer() {
  return std::make_shared<BootstrapAuthorizer>();
}

} // namespace lake::policy

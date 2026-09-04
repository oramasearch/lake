#include "lake/control/lake_identity.hpp"

namespace lake::control {
namespace {

class UnavailableLakeIdentity final : public LakeIdentity {
public:
  std::expected<LakeIdentityDocument, std::string> document() const override {
    return std::unexpected("lake identity is provided by issue #18");
  }

  std::expected<std::string, std::string>
  sign_passkey_binding(const std::string &) const override {
    return std::unexpected("lake identity is provided by issue #18");
  }
};

} // namespace

std::unique_ptr<LakeIdentity> make_lake_identity_stub() {
  return std::make_unique<UnavailableLakeIdentity>();
}

} // namespace lake::control

#pragma once

#include <expected>
#include <memory>
#include <string>

namespace lake::control {

struct LakeIdentityDocument {
  std::string identifier;
  std::string document;
};

// Lake #18 owns generation, KEK wrapping, publication, and signing by this
// instance identity. Identity core depends only on this boundary so passkey
// bindings can be attested consistently.
class LakeIdentity {
public:
  virtual ~LakeIdentity() = default;
  virtual std::expected<LakeIdentityDocument, std::string> document() const = 0;
  virtual std::expected<std::string, std::string>
  sign_passkey_binding(const std::string &binding) const = 0;
};

// This implementation boundary is intentionally available before audit issue #18
// lands. The placeholder fails closed; callers cannot accidentally mint an
// unsigned passkey binding.
std::unique_ptr<LakeIdentity> make_lake_identity_stub();

} // namespace lake::control

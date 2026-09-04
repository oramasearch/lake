#pragma once

#include <expected>
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

} // namespace lake::control

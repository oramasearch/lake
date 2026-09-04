#pragma once

#include <memory>
#include <string>
#include <vector>

namespace lake::policy {

struct Object {
  std::string type;
  std::string id;
};

struct Request {
  Object object;
  std::string action;
};

class Authorizer {
public:
  virtual ~Authorizer() = default;
  [[nodiscard]] virtual std::vector<bool>
  authorize(const std::string &principal,
            const std::vector<Request> &requests) const = 0;
};

std::shared_ptr<const Authorizer> make_bootstrap_authorizer();

} // namespace lake::policy

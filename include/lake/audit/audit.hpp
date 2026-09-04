#pragma once

#include <expected>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lake::control {
class Store;
}

namespace lake::audit {

inline constexpr int retention_floor_days = 400;

struct Error {
  std::string code;
  std::string message;
};

template <class T> using Result = std::expected<T, Error>;

struct EventDraft {
  std::string customer_id;
  std::string kind;
  std::string principal;
  std::string object;
  std::string action;
  std::string outcome;
  std::string request_id;
  std::string method;
  std::string device;
  nlohmann::json details = nlohmann::json::object();
};

struct Event {
  std::string id;
  std::string occurred_at;
  EventDraft draft;
  std::string previous_digest;
  std::string signer;
  std::string signature;
  std::string digest;
};

struct Filter {
  std::string principal;
  std::string object;
  std::string outcome;
  std::string from;
  std::string to;
  int limit = 100;
};

struct Identity {
  std::string id;
  std::string taap_id;
  std::string public_key;
  std::string wrapped_seed;
  std::string created_at;
  std::string rotation_document;
  bool current = true;
};

struct IdentityOptions {
  std::string configured_seed;
  std::string instance_kek;
};

nlohmann::json to_json(const Event &event);
// The inverse of to_json for events read back through the registry (pages never read the
// store directly). Absent or null optional fields decode as empty strings.
Event event_from_json(const nlohmann::json &value);
nlohmann::json identity_document(const Identity &identity);
std::string json_lines(const std::vector<Event> &events);

// Appends one signed event. The store enforces the predecessor supplied by this
// function.
Result<Event> record(control::Store &store, const EventDraft &draft,
                     const IdentityOptions &options);

// Recomputes every digest, signature and predecessor relation in oldest-first
// order.
Result<void> verify(control::Store &store, const std::vector<Event> &events);

// Returns the current instance identity, creating and persisting it when
// necessary.
Result<Identity> identity(control::Store &store,
                          const IdentityOptions &options);

// Creates a dual-signed TAAP rotation document and makes the new identity
// current.
Result<Identity> rotate_identity(control::Store &store,
                                 const IdentityOptions &options);

Result<void> validate_retention_days(int days);

} // namespace lake::audit

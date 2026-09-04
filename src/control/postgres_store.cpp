// The one translation unit that names libpqxx. Each call opens (or reuses) one
// connection and runs one transaction, inline on the caller's thread; SQL
// errors map to the store's error codes.
#include "lake/control/postgres_store.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

#include <pqxx/pqxx>

#include "lake/control/policy.hpp"

namespace lake::control {

namespace identity_crypto {
namespace {

std::optional<std::string> nullable(const std::string &s) {
  if (s.empty())
    return std::nullopt;
  return s;
}

std::string hex_bytes(const unsigned char *bytes, std::size_t count) {
  std::string result;
  result.reserve(count * 2);
  for (std::size_t index = 0; index < count; ++index) {
    result += std::format("{:02x}", static_cast<unsigned>(bytes[index]));
  }
  return result;
}

std::optional<std::vector<unsigned char>> decode_hex(std::string_view value) {
  if (value.size() % 2 != 0) {
    return std::nullopt;
  }
  std::vector<unsigned char> result;
  result.reserve(value.size() / 2);
  for (std::size_t index = 0; index < value.size(); index += 2) {
    unsigned parsed = 0;
    const auto [end, error] = std::from_chars(
        value.data() + index, value.data() + index + 2, parsed, 16);
    if (error != std::errc{} || end != value.data() + index + 2) {
      return std::nullopt;
    }
    result.push_back(static_cast<unsigned char>(parsed));
  }
  return result;
}

std::optional<std::vector<unsigned char>>
decode_base64(std::string_view value) {
  if (value.empty() || value.size() % 4 != 0) {
    return std::nullopt;
  }
  std::vector<unsigned char> result((value.size() / 4) * 3);
  const int count = EVP_DecodeBlock(
      result.data(), reinterpret_cast<const unsigned char *>(value.data()),
      static_cast<int>(value.size()));
  if (count < 0) {
    return std::nullopt;
  }
  std::size_t padding = 0;
  if (value.ends_with('=')) {
    ++padding;
  }
  if (value.size() > 1 && value[value.size() - 2] == '=') {
    ++padding;
  }
  result.resize(static_cast<std::size_t>(count) - padding);
  return result;
}

struct VerifiedIdentityDocument {
  std::string identifier;
  std::string document;
  std::string hash;
};

constexpr const char *user_columns =
    "id::text, coalesce(taap_id,''), coalesce(email,''), display_name, "
    "created_at::text, updated_at::text";
constexpr const char *organization_columns =
    "id::text, coalesce(taap_id,''), display_name, created_at::text, "
    "updated_at::text";
constexpr const char *customer_columns =
    "id::text, customer_type, coalesce(user_id::text,''), "
    "coalesce(organization_id::text,''), name, "
    "created_at::text, updated_at::text";

constexpr const char *repository_columns =
    "id::text, customer_id::text, name, description, visibility, "
    "coalesce(deleted_at::text,''), coalesce(retention_until::text,''), "
    "created_at::text, updated_at::text";
constexpr const char *team_columns =
    "id::text, organization_id::text, name, display_name, created_at::text, "
    "updated_at::text";
constexpr const char *membership_columns =
    "id::text, organization_id::text, coalesce(team_id::text,''), "
    "principal_type, principal_id, "
    "principal_taap_id, role, attestation_digest, granted_by_taap_id, "
    "created_at::text, "
    "updated_at::text";
constexpr const char *invitation_columns =
    "id::text, organization_id::text, coalesce(team_id::text,''), "
    "invitee_type, invitee, role, "
    "expires_at::text, status, coalesce(accepted_membership_id::text,''), "
    "created_by_taap_id, "
    "created_at::text, updated_at::text";
constexpr const char *object_columns =
    "id::text, object_type, external_id, coalesce(parent_id::text,''), "
    "inherit, created_at::text, "
    "updated_at::text";
constexpr const char *grant_columns =
    "id::text, principal_taap_id, object_id::text, role, attestation_digest, "
    "granted_by_taap_id, "
    "created_at::text, updated_at::text";
constexpr const char *identity_columns =
    "id::text, taap_id, public_key, wrapped_seed, created_at::text, "
    "coalesce(rotation_document::text,''), current";
constexpr const char *audit_event_columns =
    "id::text, to_char(occurred_at AT TIME ZONE 'UTC', "
    "'YYYY-MM-DD\"T\"HH24:MI:SS.US\"Z\"'), "
    "coalesce(customer_id::text,''), kind, principal, object, action, outcome, "
    "request_id::text, "
    "coalesce(method,''), coalesce(device,''), details::text, "
    "coalesce(previous_digest,''), "
    "signer, signature, digest";

Repository repository_of(const pqxx::row &r) {
  return Repository{
      r[0].as<std::string>(), r[1].as<std::string>(), r[2].as<std::string>(),
      r[3].as<std::string>(), r[4].as<std::string>(), r[5].as<std::string>(),
      r[6].as<std::string>(), r[7].as<std::string>(), r[8].as<std::string>()};
}

RepositoryRef repository_ref_of(const pqxx::row &row) {
  return RepositoryRef{row[0].as<std::string>(), row[1].as<std::string>(),
                       row[2].as<std::string>(), row[3].as<std::string>()};
}

Result<void> require_live_repository(pqxx::work &tx,
                                     const std::string &repository_id) {
  const pqxx::result rows = tx.exec_params(
      "SELECT deleted_at FROM repositories WHERE id = $1::uuid FOR UPDATE",
      repository_id);
  if (rows.empty()) {
    return std::unexpected(
        StoreError{"not_found", "no repository " + repository_id});
  }
  if (!rows[0][0].is_null()) {
    return std::unexpected(StoreError{"conflict", "repository is tombstoned"});
  }
  return {};
}

Team team_of(const pqxx::row &row) {
  return Team{row[0].as<std::string>(), row[1].as<std::string>(),
              row[2].as<std::string>(), row[3].as<std::string>(),
              row[4].as<std::string>(), row[5].as<std::string>()};
}

Membership membership_of(const pqxx::row &row) {
  return Membership{row[0].as<std::string>(), row[1].as<std::string>(),
                    row[2].as<std::string>(), row[3].as<std::string>(),
                    row[4].as<std::string>(), row[5].as<std::string>(),
                    row[6].as<std::string>(), row[7].as<std::string>(),
                    row[8].as<std::string>(), row[9].as<std::string>(),
                    row[10].as<std::string>()};
}

Invitation invitation_of(const pqxx::row &row) {
  return Invitation{row[0].as<std::string>(),  row[1].as<std::string>(),
                    row[2].as<std::string>(),  row[3].as<std::string>(),
                    row[4].as<std::string>(),  row[5].as<std::string>(),
                    row[6].as<std::string>(),  row[7].as<std::string>(),
                    row[8].as<std::string>(),  row[9].as<std::string>(),
                    row[10].as<std::string>(), row[11].as<std::string>()};
}

PolicyObject object_of(const pqxx::row &row) {
  return PolicyObject{row[0].as<std::string>(), row[1].as<std::string>(),
                      row[2].as<std::string>(), row[3].as<std::string>(),
                      row[5].as<std::string>(), row[6].as<std::string>(),
                      row[4].as<bool>()};
}

Grant grant_of(const pqxx::row &row) {
  return Grant{row[0].as<std::string>(), row[1].as<std::string>(),
               row[2].as<std::string>(), row[3].as<std::string>(),
               row[4].as<std::string>(), row[5].as<std::string>(),
               row[6].as<std::string>(), row[7].as<std::string>()};
}

std::string uuid_array(const std::vector<std::string> &values) {
  std::string out = "{";
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      out += ',';
    }
    out += values[index];
  }
  out += '}';
  return out;
}

User user_of(const pqxx::row &r) {
  return User{r[0].as<std::string>(), r[1].as<std::string>(),
              r[2].as<std::string>(), r[3].as<std::string>(),
              r[4].as<std::string>(), r[5].as<std::string>()};
}
Organization organization_of(const pqxx::row &r) {
  return Organization{r[0].as<std::string>(), r[1].as<std::string>(),
                      r[2].as<std::string>(), r[3].as<std::string>(),
                      r[4].as<std::string>()};
}
Customer customer_of(const pqxx::row &r) {
  return Customer{r[0].as<std::string>(), r[1].as<std::string>(),
                  r[2].as<std::string>(), r[3].as<std::string>(),
                  r[4].as<std::string>(), r[5].as<std::string>(),
                  r[6].as<std::string>()};
}

audit::Identity identity_of(const pqxx::row &r) {
  return audit::Identity{
      .id = r[0].as<std::string>(),
      .taap_id = r[1].as<std::string>(),
      .public_key = r[2].as<std::string>(),
      .wrapped_seed = r[3].as<std::string>(),
      .created_at = r[4].as<std::string>(),
      .rotation_document = r[5].as<std::string>(),
      .current = r[6].as<bool>(),
  };
}

audit::Event audit_event_of(const pqxx::row &r) {
  return audit::Event{
      .id = r[0].as<std::string>(),
      .occurred_at = r[1].as<std::string>(),
      .draft =
          audit::EventDraft{
              .customer_id = r[2].as<std::string>(),
              .kind = r[3].as<std::string>(),
              .principal = r[4].as<std::string>(),
              .object = r[5].as<std::string>(),
              .action = r[6].as<std::string>(),
              .outcome = r[7].as<std::string>(),
              .request_id = r[8].as<std::string>(),
              .method = r[9].as<std::string>(),
              .device = r[10].as<std::string>(),
              .details = nlohmann::json::parse(r[11].as<std::string>()),
          },
      .previous_digest = r[12].as<std::string>(),
      .signer = r[13].as<std::string>(),
      .signature = r[14].as<std::string>(),
      .digest = r[15].as<std::string>(),
  };
}

class PostgresStore final : public Store {
public:
  explicit PostgresStore(std::string uri) : uri_(std::move(uri)) {}
  std::string kind() const override { return "postgres"; }

  Result<void> ping() override {
    Result<void> result = with_tx<void>([](pqxx::work &tx) -> Result<void> {
      tx.exec("SET LOCAL statement_timeout = '1000ms'");
      tx.exec("SELECT 1");
      return {};
    });
    if (!result)
      return std::unexpected(
          StoreError{"store_unavailable", result.error().message});
    return {};
  }

  Result<User> create_user(const NewUser &in) override {
    return with_tx<User>([&](pqxx::work &tx) -> Result<User> {
      const pqxx::result rows = tx.exec_params(
          std::string("INSERT INTO users (taap_id, email, display_name) VALUES "
                      "($1, $2, $3) RETURNING ") +
              user_columns,
          nullable(in.taap_id), nullable(in.email), in.display_name);
      return user_of(rows[0]);
    });
  }
  Result<std::vector<User>> list_users(int limit) override {
    return with_tx<std::vector<User>>(
        [&](pqxx::work &tx) -> Result<std::vector<User>> {
          const pqxx::result rows = tx.exec_params(
              std::string("SELECT ") + user_columns +
                  " FROM users ORDER BY created_at DESC LIMIT $1",
              limit);
          std::vector<User> out;
          for (const auto &r : rows)
            out.push_back(user_of(r));
          return out;
        });
  }
  Result<User> get_user(const std::string &id) override {
    return with_tx<User>([&](pqxx::work &tx) -> Result<User> {
      const pqxx::result rows =
          tx.exec_params(std::string("SELECT ") + user_columns +
                             " FROM users WHERE id = $1::uuid",
                         id);
      if (rows.empty())
        return std::unexpected(StoreError{"not_found", "no user " + id});
      return user_of(rows[0]);
    });
  }

  Result<Organization> create_organization(const NewOrganization &in) override {
    return with_tx<Organization>([&](pqxx::work &tx) -> Result<Organization> {
      std::optional<std::string> parent_id;
      if (!in.customer_id.empty()) {
        const pqxx::result parents =
            tx.exec_params("SELECT id::text FROM objects WHERE object_type = "
                           "'customer' AND external_id = $1",
                           in.customer_id);
        if (parents.empty()) {
          return std::unexpected(
              StoreError{"invalid_input",
                         "customer_id does not reference a policy object"});
        }
        parent_id = parents[0][0].as<std::string>();
      }
      const pqxx::result rows = tx.exec_params(
          std::string("INSERT INTO organizations (taap_id, display_name) "
                      "VALUES ($1, $2) RETURNING ") +
              organization_columns,
          nullable(in.taap_id), in.display_name);
      Organization organization = organization_of(rows[0]);
      tx.exec_params(
          "INSERT INTO objects (object_type, external_id, parent_id) "
          "VALUES ('organization', $1, $2::uuid)",
          organization.id, parent_id);
      return organization;
    });
  }
  Result<std::vector<Organization>> list_organizations(int limit) override {
    return with_tx<std::vector<Organization>>(
        [&](pqxx::work &tx) -> Result<std::vector<Organization>> {
          const pqxx::result rows = tx.exec_params(
              std::string("SELECT ") + organization_columns +
                  " FROM organizations ORDER BY created_at DESC LIMIT $1",
              limit);
          std::vector<Organization> out;
          for (const auto &r : rows)
            out.push_back(organization_of(r));
          return out;
        });
  }
  Result<Organization> get_organization(const std::string &id) override {
    return with_tx<Organization>([&](pqxx::work &tx) -> Result<Organization> {
      const pqxx::result rows =
          tx.exec_params(std::string("SELECT ") + organization_columns +
                             " FROM organizations WHERE id = $1::uuid",
                         id);
      if (rows.empty())
        return std::unexpected(
            StoreError{"not_found", "no organization " + id});
      return organization_of(rows[0]);
    });
  }

  Result<Customer> create_customer(const NewCustomer &in) override {
    return with_tx<Customer>([&](pqxx::work &tx) -> Result<Customer> {
      const pqxx::result rows = tx.exec_params(
          std::string("INSERT INTO customers (customer_type, user_id, "
                      "organization_id, name) "
                      "VALUES ($1, $2::uuid, $3::uuid, $4) RETURNING ") +
              customer_columns,
          in.customer_type, nullable(in.user_id), nullable(in.organization_id),
          in.name);
      Customer customer = customer_of(rows[0]);
      const pqxx::result objects =
          tx.exec_params("INSERT INTO objects (object_type, external_id) "
                         "VALUES ('customer', $1) "
                         "RETURNING id::text",
                         customer.id);
      if (!customer.organization_id.empty()) {
        tx.exec_params(
            "UPDATE objects SET parent_id = $1::uuid "
            "WHERE object_type = 'organization' AND external_id = $2",
            objects[0][0].as<std::string>(), customer.organization_id);
      }
      return customer;
    });
  }
  Result<std::vector<Customer>> list_customers(int limit) override {
    return with_tx<std::vector<Customer>>(
        [&](pqxx::work &tx) -> Result<std::vector<Customer>> {
          const pqxx::result rows = tx.exec_params(
              std::string("SELECT ") + customer_columns +
                  " FROM customers ORDER BY created_at DESC LIMIT $1",
              limit);
          std::vector<Customer> out;
          for (const auto &r : rows)
            out.push_back(customer_of(r));
          return out;
        });
  }
  Result<Customer> get_customer(const std::string &id) override {
    return with_tx<Customer>([&](pqxx::work &tx) -> Result<Customer> {
      const pqxx::result rows =
          tx.exec_params(std::string("SELECT ") + customer_columns +
                             " FROM customers WHERE id = $1::uuid",
                         id);
      if (rows.empty())
        return std::unexpected(StoreError{"not_found", "no customer " + id});
      return customer_of(rows[0]);
    });
  }

  Result<Repository> create_repository(const NewRepository &in) override {
    return with_tx<Repository>([&](pqxx::work &tx) -> Result<Repository> {
      const pqxx::result parents =
          tx.exec_params("SELECT id::text FROM objects WHERE object_type = "
                         "'customer' AND external_id = $1",
                         in.customer_id);
      if (parents.empty()) {
        return std::unexpected(StoreError{
            "invalid_input", "customer_id does not reference a policy object"});
      }
      const pqxx::result rows = tx.exec_params(
          std::string("INSERT INTO repositories (customer_id, name, "
                      "description, visibility) "
                      "VALUES ($1::uuid, $2, $3, $4) RETURNING ") +
              repository_columns,
          in.customer_id, in.name, in.description,
          in.visibility.empty() ? std::string("private") : in.visibility);
      Repository repository = repository_of(rows[0]);
      tx.exec_params(
          "INSERT INTO objects (object_type, external_id, parent_id) "
          "VALUES ('repository', $1, $2::uuid)",
          repository.id, parents[0][0].as<std::string>());
      return repository;
    });
  }
  Result<std::vector<Repository>>
  list_repositories(const std::string &customer_id, int limit) override {
    return with_tx<
        std::vector<Repository>>([&](pqxx::work &tx)
                                     -> Result<std::vector<Repository>> {
      const pqxx::result rows =
          customer_id.empty()
              ? tx.exec_params(
                    std::string("SELECT ") + repository_columns +
                        " FROM repositories ORDER BY created_at DESC LIMIT $1",
                    limit)
              : tx.exec_params(std::string("SELECT ") + repository_columns +
                                   " FROM repositories WHERE customer_id = "
                                   "$1::uuid ORDER BY created_at DESC LIMIT $2",
                               customer_id, limit);
      std::vector<Repository> out;
      for (const auto &r : rows)
        out.push_back(repository_of(r));
      return out;
    });
  }
  Result<Repository> get_repository(const std::string &id) override {
    return with_tx<Repository>([&](pqxx::work &tx) -> Result<Repository> {
      const pqxx::result rows =
          tx.exec_params(std::string("SELECT ") + repository_columns +
                             " FROM repositories WHERE id = $1::uuid",
                         id);
      if (rows.empty())
        return std::unexpected(StoreError{"not_found", "no repository " + id});
      return repository_of(rows[0]);
    });
  }

  Result<Repository> rename_repository(const std::string &id,
                                       const std::string &name) override {
    return with_tx<Repository>([&](pqxx::work &tx) -> Result<Repository> {
      if (const auto live = require_live_repository(tx, id); !live) {
        return std::unexpected(live.error());
      }
      const pqxx::result rows =
          tx.exec_params(std::string("UPDATE repositories SET name = $2 WHERE "
                                     "id = $1::uuid RETURNING ") +
                             repository_columns,
                         id, name);
      return repository_of(rows[0]);
    });
  }

  Result<Repository>
  transfer_repository(const std::string &id,
                      const std::string &customer_id) override {
    return with_tx<Repository>([&](pqxx::work &tx) -> Result<Repository> {
      if (const auto live = require_live_repository(tx, id); !live) {
        return std::unexpected(live.error());
      }
      const pqxx::result customer = tx.exec_params(
          "SELECT id FROM customers WHERE id = $1::uuid", customer_id);
      if (customer.empty()) {
        return std::unexpected(StoreError{
            "invalid_input", "customer_id does not reference a customer"});
      }
      const pqxx::result parent =
          tx.exec_params("SELECT id::text FROM objects WHERE object_type = "
                         "'customer' AND external_id = $1",
                         customer_id);
      if (parent.empty()) {
        return std::unexpected(
            StoreError{"store_error", "customer policy object is missing"});
      }
      const pqxx::result rows =
          tx.exec_params(std::string("UPDATE repositories SET customer_id = "
                                     "$2::uuid WHERE id = $1::uuid "
                                     "RETURNING ") +
                             repository_columns,
                         id, customer_id);
      tx.exec_params("UPDATE objects SET parent_id = $1::uuid WHERE "
                     "object_type = 'repository' "
                     "AND external_id = $2",
                     parent[0][0].as<std::string>(), id);
      return repository_of(rows[0]);
    });
  }

  Result<Repository>
  set_repository_visibility(const std::string &id,
                            const std::string &visibility) override {
    return with_tx<Repository>([&](pqxx::work &tx) -> Result<Repository> {
      if (visibility != "private" && visibility != "organization" &&
          visibility != "public") {
        return std::unexpected(
            StoreError{"invalid_input",
                       "visibility must be private, organization, or public"});
      }
      if (const auto live = require_live_repository(tx, id); !live) {
        return std::unexpected(live.error());
      }
      const pqxx::result rows =
          tx.exec_params(std::string("UPDATE repositories SET visibility = $2 "
                                     "WHERE id = $1::uuid RETURNING ") +
                             repository_columns,
                         id, visibility);
      return repository_of(rows[0]);
    });
  }

  Result<Repository>
  delete_repository(const std::string &id,
                    const RepositoryDeletion &deletion) override {
    return with_tx<Repository>([&](pqxx::work &tx) -> Result<Repository> {
      if (const auto live = require_live_repository(tx, id); !live) {
        return std::unexpected(live.error());
      }
      const pqxx::result rows = tx.exec_params(
          std::string("UPDATE repositories SET deleted_at = $2::timestamptz, "
                      "retention_until = $3::timestamptz WHERE id = $1::uuid "
                      "RETURNING ") +
              repository_columns,
          id, deletion.deleted_at, deletion.retention_until);
      return repository_of(rows[0]);
    });
  }

  Result<std::vector<RepositoryRef>>
  list_repository_refs(const std::string &repository_id) override {
    return with_tx<std::vector<RepositoryRef>>(
        [&](pqxx::work &tx) -> Result<std::vector<RepositoryRef>> {
          const pqxx::result rows =
              tx.exec_params("SELECT repository_id::text, name, head, "
                             "updated_at::text FROM repository_refs "
                             "WHERE repository_id = $1::uuid ORDER BY name",
                             repository_id);
          std::vector<RepositoryRef> references;
          for (const auto &row : rows) {
            references.push_back(repository_ref_of(row));
          }
          return references;
        });
  }

  Result<RepositoryRef> set_repository_ref(const std::string &repository_id,
                                           const std::string &name,
                                           const std::string &head) override {
    return with_tx<RepositoryRef>([&](pqxx::work &tx) -> Result<RepositoryRef> {
      const pqxx::result rows = tx.exec_params(
          "INSERT INTO repository_refs (repository_id, name, head) VALUES "
          "($1::uuid, $2, $3) ON CONFLICT (repository_id, name) DO UPDATE SET "
          "head = EXCLUDED.head, updated_at = now() RETURNING "
          "repository_id::text, "
          "name, head, updated_at::text",
          repository_id, name, head);
      return repository_ref_of(rows[0]);
    });
  }

  Result<Team> create_team(const NewTeam &in) override {
    return with_tx<Team>([&](pqxx::work &tx) -> Result<Team> {
      const pqxx::result parents = tx.exec_params(
          "SELECT id::text FROM objects "
          "WHERE object_type = 'organization' AND external_id = $1",
          in.organization_id);
      if (parents.empty()) {
        return std::unexpected(
            StoreError{"invalid_input",
                       "organization_id does not reference a policy object"});
      }
      const pqxx::result rows = tx.exec_params(
          std::string("INSERT INTO teams (organization_id, name, display_name) "
                      "VALUES ($1::uuid, $2, $3) RETURNING ") +
              team_columns,
          in.organization_id, in.name, in.display_name);
      Team team = team_of(rows[0]);
      const pqxx::result objects = tx.exec_params(
          "INSERT INTO objects (object_type, external_id, parent_id) "
          "VALUES ('team', $1, $2::uuid) RETURNING id::text",
          team.id, parents[0][0].as<std::string>());
      return team;
    });
  }

  Result<std::vector<Team>> list_teams(const std::string &organization_id,
                                       int limit) override {
    return with_tx<std::vector<Team>>(
        [&](pqxx::work &tx) -> Result<std::vector<Team>> {
          const pqxx::result rows =
              organization_id.empty()
                  ? tx.exec_params(
                        std::string("SELECT ") + team_columns +
                            " FROM teams ORDER BY created_at DESC LIMIT $1",
                        limit)
                  : tx.exec_params(
                        std::string("SELECT ") + team_columns +
                            " FROM teams WHERE organization_id = $1::uuid "
                            "ORDER BY created_at DESC LIMIT $2",
                        organization_id, limit);
          std::vector<Team> out;
          for (const auto &row : rows) {
            out.push_back(team_of(row));
          }
          return out;
        });
  }

  Result<Team> get_team(const std::string &id) override {
    return with_tx<Team>([&](pqxx::work &tx) -> Result<Team> {
      const pqxx::result rows =
          tx.exec_params(std::string("SELECT ") + team_columns +
                             " FROM teams WHERE id = $1::uuid",
                         id);
      if (rows.empty()) {
        return std::unexpected(StoreError{"not_found", "no team " + id});
      }
      return team_of(rows[0]);
    });
  }

  Result<Membership> create_membership(const NewMembership &in) override {
    return with_tx<Membership>([&](pqxx::work &tx) -> Result<Membership> {
      if (in.principal_type == "user") {
        const pqxx::result users = tx.exec_params(
            "SELECT 1 FROM users WHERE id = $1::uuid AND taap_id = $2",
            in.principal_id, in.principal_taap_id);
        if (users.empty()) {
          return std::unexpected(
              StoreError{"invalid_input",
                         "user principal does not match its TAAP identity"});
        }
      }
      const std::string parent_type =
          in.team_id.empty() ? "organization" : "team";
      const std::string parent_external_id =
          in.team_id.empty() ? in.organization_id : in.team_id;
      if (!in.team_id.empty()) {
        const pqxx::result teams =
            tx.exec_params("SELECT 1 FROM teams WHERE id = $1::uuid AND "
                           "organization_id = $2::uuid",
                           in.team_id, in.organization_id);
        if (teams.empty()) {
          return std::unexpected(StoreError{
              "invalid_input", "team_id does not belong to the organization"});
        }
      }
      const pqxx::result parents =
          tx.exec_params("SELECT id::text FROM objects WHERE object_type = $1 "
                         "AND external_id = $2",
                         parent_type, parent_external_id);
      if (parents.empty()) {
        return std::unexpected(
            StoreError{"invalid_input", "membership policy parent is missing"});
      }
      const pqxx::result rows = tx.exec_params(
          std::string(
              "INSERT INTO memberships (organization_id, team_id, "
              "principal_type, "
              "principal_id, principal_taap_id, role, attestation_digest, "
              "granted_by_taap_id) VALUES ($1::uuid, $2::uuid, $3, $4, $5, $6, "
              "$7, "
              "$8) RETURNING ") +
              membership_columns,
          in.organization_id, nullable(in.team_id), in.principal_type,
          in.principal_id, in.principal_taap_id, in.role, in.attestation_digest,
          in.granted_by_taap_id);
      Membership membership = membership_of(rows[0]);
      const pqxx::result objects = tx.exec_params(
          "INSERT INTO objects (object_type, external_id, parent_id) "
          "VALUES ('member', $1, $2::uuid) RETURNING id::text",
          membership.id, parents[0][0].as<std::string>());
      const std::string grant_role = in.role == "member" ? "read" : "admin";
      tx.exec_params("INSERT INTO grants (principal_taap_id, object_id, role, "
                     "attestation_digest, "
                     "granted_by_taap_id) VALUES ($1, $2::uuid, $3, $4, $5)",
                     in.principal_taap_id, parents[0][0].as<std::string>(),
                     grant_role, in.attestation_digest, in.granted_by_taap_id);
      return membership;
    });
  }

  Result<std::vector<Membership>>
  list_memberships(const std::string &organization_id,
                   const std::string &team_id, int limit) override {
    return with_tx<std::vector<Membership>>(
        [&](pqxx::work &tx) -> Result<std::vector<Membership>> {
          std::string where = " WHERE true";
          pqxx::params params;
          if (!organization_id.empty()) {
            params.append(organization_id);
            where += " AND organization_id = $" +
                     std::to_string(params.size()) + "::uuid";
          }
          if (!team_id.empty()) {
            params.append(team_id);
            where +=
                " AND team_id = $" + std::to_string(params.size()) + "::uuid";
          }
          params.append(limit);
          const std::string query = std::string("SELECT ") +
                                    membership_columns + " FROM memberships" +
                                    where +
                                    " ORDER BY created_at DESC LIMIT $" +
                                    std::to_string(params.size());
          const pqxx::result rows = tx.exec(query, params);
          std::vector<Membership> out;
          for (const auto &row : rows) {
            out.push_back(membership_of(row));
          }
          return out;
        });
  }

  Result<Invitation> create_invitation(const NewInvitation &in) override {
    return with_tx<Invitation>([&](pqxx::work &tx) -> Result<Invitation> {
      if (!in.team_id.empty()) {
        const pqxx::result teams =
            tx.exec_params("SELECT 1 FROM teams WHERE id = $1::uuid AND "
                           "organization_id = $2::uuid",
                           in.team_id, in.organization_id);
        if (teams.empty()) {
          return std::unexpected(StoreError{
              "invalid_input", "team_id does not belong to the organization"});
        }
      }
      const pqxx::result rows = tx.exec_params(
          std::string("INSERT INTO invitations (organization_id, team_id, "
                      "invitee_type, "
                      "invitee, role, expires_at, created_by_taap_id) "
                      "VALUES ($1::uuid, $2::uuid, $3, lower($4), $5, "
                      "$6::timestamptz, $7) "
                      "RETURNING ") +
              invitation_columns,
          in.organization_id, nullable(in.team_id), in.invitee_type, in.invitee,
          in.role, in.expires_at, in.created_by_taap_id);
      Invitation invitation = invitation_of(rows[0]);
      const std::string target_type =
          in.team_id.empty() ? "organization" : "team";
      const std::string target_external_id =
          in.team_id.empty() ? in.organization_id : in.team_id;
      const pqxx::result objects =
          tx.exec_params("SELECT id::text FROM objects WHERE object_type = $1 "
                         "AND external_id = $2",
                         target_type, target_external_id);
      return invitation;
    });
  }

  Result<std::vector<Invitation>>
  list_invitations(const std::string &organization_id,
                   const std::string &team_id, int limit) override {
    return with_tx<std::vector<Invitation>>(
        [&](pqxx::work &tx) -> Result<std::vector<Invitation>> {
          std::string where = " WHERE true";
          pqxx::params params;
          if (!organization_id.empty()) {
            params.append(organization_id);
            where += " AND organization_id = $" +
                     std::to_string(params.size()) + "::uuid";
          }
          if (!team_id.empty()) {
            params.append(team_id);
            where +=
                " AND team_id = $" + std::to_string(params.size()) + "::uuid";
          }
          params.append(limit);
          const std::string query = std::string("SELECT ") +
                                    invitation_columns + " FROM invitations" +
                                    where +
                                    " ORDER BY created_at DESC LIMIT $" +
                                    std::to_string(params.size());
          const pqxx::result rows = tx.exec(query, params);
          std::vector<Invitation> out;
          for (const auto &row : rows) {
            out.push_back(invitation_of(row));
          }
          return out;
        });
  }

  Result<Invitation> get_invitation(const std::string &id) override {
    return with_tx<Invitation>([&](pqxx::work &tx) -> Result<Invitation> {
      const pqxx::result rows =
          tx.exec_params(std::string("SELECT ") + invitation_columns +
                             " FROM invitations WHERE id = $1::uuid",
                         id);
      if (rows.empty()) {
        return std::unexpected(StoreError{"not_found", "no invitation " + id});
      }
      return invitation_of(rows[0]);
    });
  }

  Result<Membership> accept_invitation(const AcceptInvitation &in) override {
    return with_tx<Membership>([&](pqxx::work &tx) -> Result<Membership> {
      const pqxx::result invitations =
          tx.exec_params(std::string("SELECT ") + invitation_columns +
                             " FROM invitations WHERE id = $1::uuid FOR UPDATE",
                         in.invitation_id);
      if (invitations.empty()) {
        return std::unexpected(
            StoreError{"not_found", "no invitation " + in.invitation_id});
      }
      Invitation invitation = invitation_of(invitations[0]);
      if (invitation.status != "pending") {
        return std::unexpected(
            StoreError{"conflict", "invitation is not pending"});
      }
      if (tx.exec_params("SELECT $1::timestamptz <= now()",
                         invitation.expires_at)[0][0]
              .as<bool>()) {
        return std::unexpected(
            StoreError{"conflict", "invitation has expired"});
      }
      if (in.principal_type == "user") {
        const pqxx::result users = tx.exec_params(
            "SELECT 1 FROM users WHERE id = $1::uuid AND taap_id = $2",
            in.principal_id, in.principal_taap_id);
        if (users.empty()) {
          return std::unexpected(
              StoreError{"invalid_input",
                         "user principal does not match its TAAP identity"});
        }
      }
      const std::string parent_type =
          invitation.team_id.empty() ? "organization" : "team";
      const std::string parent_external_id = invitation.team_id.empty()
                                                 ? invitation.organization_id
                                                 : invitation.team_id;
      const pqxx::result parents =
          tx.exec_params("SELECT id::text FROM objects WHERE object_type = $1 "
                         "AND external_id = $2",
                         parent_type, parent_external_id);
      if (parents.empty()) {
        return std::unexpected(
            StoreError{"store_error", "membership policy parent is missing"});
      }
      const pqxx::result rows = tx.exec_params(
          std::string(
              "INSERT INTO memberships (organization_id, team_id, "
              "principal_type, "
              "principal_id, principal_taap_id, role, attestation_digest, "
              "granted_by_taap_id) VALUES ($1::uuid, $2::uuid, $3, $4, $5, $6, "
              "$7, "
              "$8) RETURNING ") +
              membership_columns,
          invitation.organization_id, nullable(invitation.team_id),
          in.principal_type, in.principal_id, in.principal_taap_id,
          invitation.role, in.attestation_digest, in.accepted_by_taap_id);
      Membership membership = membership_of(rows[0]);
      const pqxx::result objects = tx.exec_params(
          "INSERT INTO objects (object_type, external_id, parent_id) "
          "VALUES ('member', $1, $2::uuid) RETURNING id::text",
          membership.id, parents[0][0].as<std::string>());
      const std::string grant_role =
          invitation.role == "member" ? "read" : "admin";
      tx.exec_params("INSERT INTO grants (principal_taap_id, object_id, role, "
                     "attestation_digest, "
                     "granted_by_taap_id) VALUES ($1, $2::uuid, $3, $4, $5)",
                     in.principal_taap_id, parents[0][0].as<std::string>(),
                     grant_role, in.attestation_digest, in.accepted_by_taap_id);
      tx.exec_params("UPDATE invitations SET status = 'accepted', "
                     "accepted_membership_id = $1::uuid "
                     "WHERE id = $2::uuid",
                     membership.id, invitation.id);
      return membership;
    });
  }

  Result<PolicyObject>
  create_policy_object(const NewPolicyObject &in) override {
    return with_tx<PolicyObject>([&](pqxx::work &tx) -> Result<PolicyObject> {
      if (in.parent_id.empty() && in.object_type != "customer") {
        return std::unexpected(
            StoreError{"invalid_input",
                       "only a customer policy object may have no parent"});
      }
      if (!in.parent_id.empty()) {
        const pqxx::result parents = tx.exec_params(
            "SELECT object_type FROM objects WHERE id = $1::uuid",
            in.parent_id);
        if (parents.empty()) {
          return std::unexpected(StoreError{
              "invalid_input", "parent_id does not reference a policy object"});
        }
        if (!object_parent_allowed(in.object_type,
                                   parents[0][0].as<std::string>())) {
          return std::unexpected(StoreError{
              "invalid_input", "object type is not valid under this parent"});
        }
      }
      const pqxx::result rows = tx.exec_params(
          std::string("INSERT INTO objects (object_type, external_id, "
                      "parent_id, inherit) "
                      "VALUES ($1, $2, $3::uuid, $4) RETURNING ") +
              object_columns,
          in.object_type, in.external_id, nullable(in.parent_id), in.inherit);
      PolicyObject object = object_of(rows[0]);
      return object;
    });
  }

  Result<PolicyObject> get_policy_object(const std::string &id) override {
    return with_tx<PolicyObject>([&](pqxx::work &tx) -> Result<PolicyObject> {
      const pqxx::result rows =
          tx.exec_params(std::string("SELECT ") + object_columns +
                             " FROM objects WHERE id = $1::uuid",
                         id);
      if (rows.empty()) {
        return std::unexpected(
            StoreError{"not_found", "no policy object " + id});
      }
      return object_of(rows[0]);
    });
  }

  Result<PolicyObject>
  find_policy_object(const std::string &object_type,
                     const std::string &external_id) override {
    return with_tx<PolicyObject>([&](pqxx::work &tx) -> Result<PolicyObject> {
      const pqxx::result rows = tx.exec_params(
          std::string("SELECT ") + object_columns +
              " FROM objects WHERE object_type = $1 AND external_id = $2",
          object_type, external_id);
      if (rows.empty()) {
        return std::unexpected(StoreError{
            "not_found", "no " + object_type + " object " + external_id});
      }
      return object_of(rows[0]);
    });
  }

  Result<std::vector<PolicyObject>>
  list_policy_objects(const std::string &parent_id,
                      const std::string &object_type, int limit) override {
    return with_tx<std::vector<PolicyObject>>(
        [&](pqxx::work &tx) -> Result<std::vector<PolicyObject>> {
          std::string where = " WHERE true";
          pqxx::params params;
          if (!parent_id.empty()) {
            params.append(parent_id);
            where +=
                " AND parent_id = $" + std::to_string(params.size()) + "::uuid";
          }
          if (!object_type.empty()) {
            params.append(object_type);
            where += " AND object_type = $" + std::to_string(params.size());
          }
          params.append(limit);
          const std::string query = std::string("SELECT ") + object_columns +
                                    " FROM objects" + where +
                                    " ORDER BY created_at DESC LIMIT $" +
                                    std::to_string(params.size());
          const pqxx::result rows = tx.exec(query, params);
          std::vector<PolicyObject> out;
          for (const auto &row : rows) {
            out.push_back(object_of(row));
          }
          return out;
        });
  }

  Result<std::vector<ObjectPathEntry>>
  policy_paths(const std::vector<std::string> &requested_object_ids) override {
    return with_tx<std::vector<
        ObjectPathEntry>>([&](pqxx::work &tx)
                              -> Result<std::vector<ObjectPathEntry>> {
      if (requested_object_ids.empty()) {
        return std::vector<ObjectPathEntry>{};
      }
      const pqxx::result rows = tx.exec_params(
          "WITH RECURSIVE walk AS ("
          "SELECT objects.id AS requested_id, objects.id, objects.object_type, "
          "objects.external_id, objects.parent_id, objects.inherit, "
          "objects.created_at, "
          "objects.updated_at, 0 AS depth FROM objects "
          "WHERE objects.id = ANY($1::uuid[]) "
          "UNION ALL "
          "SELECT walk.requested_id, parent.id, parent.object_type, "
          "parent.external_id, "
          "parent.parent_id, parent.inherit, parent.created_at, "
          "parent.updated_at, "
          "walk.depth + 1 FROM walk JOIN objects AS parent ON parent.id = "
          "walk.parent_id "
          "WHERE walk.inherit) "
          "SELECT requested_id::text, id::text, object_type, external_id, "
          "coalesce(parent_id::text,''), inherit, created_at::text, "
          "updated_at::text, depth "
          "FROM walk ORDER BY requested_id, depth",
          uuid_array(requested_object_ids));
      std::vector<ObjectPathEntry> out;
      for (const auto &row : rows) {
        PolicyObject object{row[1].as<std::string>(), row[2].as<std::string>(),
                            row[3].as<std::string>(), row[4].as<std::string>(),
                            row[6].as<std::string>(), row[7].as<std::string>(),
                            row[5].as<bool>()};
        out.push_back(ObjectPathEntry{row[0].as<std::string>(),
                                      std::move(object), row[8].as<int>()});
      }
      return out;
    });
  }

  Result<Grant> create_grant(const NewGrant &in) override {
    return with_tx<Grant>([&](pqxx::work &tx) -> Result<Grant> {
      const pqxx::result objects = tx.exec_params(
          "SELECT object_type FROM objects WHERE id = $1::uuid", in.object_id);
      if (objects.empty()) {
        return std::unexpected(StoreError{
            "invalid_input", "object_id does not reference a policy object"});
      }
      if (!role_valid_for_object_type(objects[0][0].as<std::string>(),
                                      in.role)) {
        return std::unexpected(StoreError{
            "invalid_input", "role is not valid for this object type"});
      }
      const pqxx::result rows = tx.exec_params(
          std::string("INSERT INTO grants (principal_taap_id, object_id, role, "
                      "attestation_digest, granted_by_taap_id) "
                      "VALUES ($1, $2::uuid, $3, $4, $5) RETURNING ") +
              grant_columns,
          in.principal_taap_id, in.object_id, in.role, in.attestation_digest,
          in.granted_by_taap_id);
      Grant grant = grant_of(rows[0]);
      return grant;
    });
  }

  Result<std::vector<Grant>>
  list_grants(const std::vector<std::string> &object_ids,
              const std::optional<std::string> &principal_taap_id,
              int limit) override {
    return with_tx<std::vector<Grant>>(
        [&](pqxx::work &tx) -> Result<std::vector<Grant>> {
          std::string where = " WHERE true";
          pqxx::params params;
          if (!object_ids.empty()) {
            params.append(uuid_array(object_ids));
            where += " AND object_id = ANY($" + std::to_string(params.size()) +
                     "::uuid[])";
          }
          if (principal_taap_id) {
            params.append(*principal_taap_id);
            where +=
                " AND principal_taap_id = $" + std::to_string(params.size());
          }
          params.append(limit);
          const std::string query = std::string("SELECT ") + grant_columns +
                                    " FROM grants" + where +
                                    " ORDER BY created_at DESC LIMIT $" +
                                    std::to_string(params.size());
          const pqxx::result rows = tx.exec(query, params);
          std::vector<Grant> out;
          for (const auto &row : rows) {
            out.push_back(grant_of(row));
          }
          return out;
        });
  }

  audit::Result<audit::Identity> current_lake_identity() override {
    return with_audit_tx<
        audit::Identity>([](pqxx::work &tx) -> audit::Result<audit::Identity> {
      const pqxx::result rows = tx.exec(
          std::string("SELECT ") + identity_columns +
          " FROM lake_identity WHERE current ORDER BY created_at DESC LIMIT 1");
      if (rows.empty()) {
        return std::unexpected(
            audit::Error{"not_found", "no lake identity exists"});
      }
      return identity_of(rows[0]);
    });
  }

  audit::Result<audit::Identity>
  lake_identity_by_taap_id(const std::string &taap_id) override {
    return with_audit_tx<audit::Identity>(
        [&](pqxx::work &tx) -> audit::Result<audit::Identity> {
          const pqxx::result rows =
              tx.exec_params(std::string("SELECT ") + identity_columns +
                                 " FROM lake_identity WHERE taap_id = $1",
                             taap_id);
          if (rows.empty()) {
            return std::unexpected(
                audit::Error{"not_found", "no lake identity for " + taap_id});
          }
          return identity_of(rows[0]);
        });
  }

  audit::Result<void>
  create_lake_identity(const audit::Identity &identity) override {
    return with_audit_tx<void>([&](pqxx::work &tx) -> audit::Result<void> {
      tx.exec_params("INSERT INTO lake_identity "
                     "(id, taap_id, public_key, wrapped_seed, created_at, "
                     "rotation_document, current) "
                     "VALUES ($1::uuid, $2, $3, $4, $5::timestamptz, "
                     "NULLIF($6, '')::jsonb, true)",
                     identity.id, identity.taap_id, identity.public_key,
                     identity.wrapped_seed, identity.created_at,
                     identity.rotation_document);
      return {};
    });
  }

  audit::Result<void>
  rotate_lake_identity(const audit::Identity &previous,
                       const audit::Identity &replacement) override {
    return with_audit_tx<void>([&](pqxx::work &tx) -> audit::Result<void> {
      const pqxx::result updated =
          tx.exec_params("UPDATE lake_identity SET current = false "
                         "WHERE id = $1::uuid AND current",
                         previous.id);
      if (updated.affected_rows() != 1) {
        return std::unexpected(audit::Error{
            "conflict", "the lake identity changed before rotation"});
      }
      tx.exec_params(
          "INSERT INTO lake_identity "
          "(id, taap_id, public_key, wrapped_seed, created_at, "
          "rotation_document, current) "
          "VALUES ($1::uuid, $2, $3, $4, $5::timestamptz, $6::jsonb, true)",
          replacement.id, replacement.taap_id, replacement.public_key,
          replacement.wrapped_seed, replacement.created_at,
          replacement.rotation_document);
      return {};
    });
  }

  Result<void> append_audit_event(const audit::Event &event) override {
    return with_tx<void>([&](pqxx::work &tx) -> Result<void> {
      tx.exec("SELECT pg_advisory_xact_lock(hashtext('lake_audit_events'))");
      const pqxx::result previous = tx.exec(
          "SELECT digest FROM audit_events ORDER BY sequence DESC LIMIT 1");
      const std::string digest =
          previous.empty() ? "" : previous[0][0].as<std::string>();
      if (event.previous_digest != digest) {
        return std::unexpected(StoreError{
            "conflict", "audit event predecessor changed before append"});
      }
      tx.exec_params(
          "INSERT INTO audit_events "
          "(id, occurred_at, customer_id, kind, principal, object, action, "
          "outcome, "
          "request_id, method, device, details, previous_digest, signer, "
          "signature, digest) "
          "VALUES ($1::uuid, $2::timestamptz, NULLIF($3, '')::uuid, $4, $5, "
          "$6, $7, $8, "
          "$9, NULLIF($10, ''), NULLIF($11, ''), $12::jsonb, NULLIF($13, ''), "
          "$14, "
          "$15, $16)",
          event.id, event.occurred_at, event.draft.customer_id,
          event.draft.kind, event.draft.principal, event.draft.object,
          event.draft.action, event.draft.outcome, event.draft.request_id,
          event.draft.method, event.draft.device, event.draft.details.dump(),
          event.previous_digest, event.signer, event.signature, event.digest);
      return {};
    });
  }

  Result<audit::Event> latest_audit_event() override {
    return with_tx<audit::Event>([](pqxx::work &tx) -> Result<audit::Event> {
      const pqxx::result rows =
          tx.exec(std::string("SELECT ") + audit_event_columns +
                  " FROM audit_events ORDER BY sequence DESC LIMIT 1");
      if (rows.empty()) {
        return std::unexpected(
            StoreError{"not_found", "no audit events exist"});
      }
      return audit_event_of(rows[0]);
    });
  }

  Result<std::vector<audit::Event>>
  list_audit_events(const audit::Filter &filter) override {
    return with_tx<
        std::vector<audit::Event>>([&](pqxx::work &tx)
                                       -> Result<std::vector<audit::Event>> {
      const pqxx::result rows = tx.exec_params(
          std::string("SELECT ") + audit_event_columns +
              " FROM audit_events "
              "WHERE ($1 = '' OR principal = $1) AND ($2 = '' OR object = $2) "
              "AND ($3 = '' OR outcome = $3) "
              "AND ($4 = '' OR occurred_at >= $4::timestamptz) "
              "AND ($5 = '' OR occurred_at <= $5::timestamptz) "
              "ORDER BY sequence ASC LIMIT $6",
          filter.principal, filter.object, filter.outcome, filter.from,
          filter.to, filter.limit);
      std::vector<audit::Event> events;
      for (const pqxx::row &row : rows) {
        events.push_back(audit_event_of(row));
      }
      return events;
    });
  }

  Result<void> set_audit_retention_days(const std::string &customer_id,
                                        int days) override {
    return with_tx<void>([&](pqxx::work &tx) -> Result<void> {
      const pqxx::result updated = tx.exec_params(
          "INSERT INTO audit_retention (customer_id, retention_days) VALUES "
          "($1::uuid, $2) "
          "ON CONFLICT (customer_id) DO UPDATE SET retention_days = "
          "EXCLUDED.retention_days "
          "WHERE audit_retention.retention_days <= EXCLUDED.retention_days",
          customer_id, days);
      if (updated.affected_rows() != 1) {
        return std::unexpected(
            StoreError{"conflict", "audit retention cannot be reduced"});
      }
      return {};
    });
  }

private:
  template <class T, class F> audit::Result<T> with_audit_tx(F &&body) {
    std::lock_guard lock(mutex_);
    try {
      if (!connection_ || !connection_->is_open())
        connection_ = std::make_unique<pqxx::connection>(uri_);
      pqxx::work tx(*connection_);
      audit::Result<T> result = body(tx);
      if (result)
        tx.commit();
      return result;
    } catch (const pqxx::unique_violation &error) {
      return std::unexpected(audit::Error{"conflict", error.what()});
    } catch (const pqxx::broken_connection &error) {
      connection_.reset();
      return std::unexpected(audit::Error{"store_unavailable", error.what()});
    } catch (const std::exception &error) {
      return std::unexpected(audit::Error{"store_error", error.what()});
    }
  }
  template <class T, class F> Result<T> with_tx(F &&body) {
    std::lock_guard lock(mutex_);
    try {
      if (!connection_ || !connection_->is_open())
        connection_ = std::make_unique<pqxx::connection>(uri_);
      pqxx::work tx(*connection_);
      Result<T> result = body(tx);
      if (result)
        tx.commit();
      return result;
    } catch (const pqxx::broken_connection &error) {
      connection_.reset();
      return std::unexpected(StoreError{"store_unavailable", error.what()});
    } catch (const pqxx::unique_violation &error) {
      return std::unexpected(StoreError{"conflict", error.what()});
    } catch (const pqxx::check_violation &error) {
      return std::unexpected(StoreError{"invalid_input", error.what()});
    } catch (const pqxx::foreign_key_violation &error) {
      return std::unexpected(StoreError{"invalid_input", error.what()});
    } catch (const pqxx::data_exception &error) {
      return std::unexpected(StoreError{"invalid_input", error.what()});
    } catch (const std::exception &error) {
      return std::unexpected(StoreError{"store_error", error.what()});
    }
  }

  std::string uri_;
  std::mutex mutex_;
  std::unique_ptr<pqxx::connection> connection_;
};

} // namespace

std::unique_ptr<Store> make_postgres_store(std::string connection_uri) {
  return std::make_unique<PostgresStore>(std::move(connection_uri));
}

} // namespace lake::control

// Audit cryptography is intentionally compiled through this provider-bound
// translation unit. The audit API remains provider-neutral while
// external-library names stay confined to the designated server and
// control-store translation units.
#include "../audit/audit.cpp"

// The one translation unit that names libpqxx. Each call opens (or reuses) one
// connection and runs one transaction, inline on the caller's thread; SQL
// errors map to the store's error codes.
#include "lake/control/postgres_store.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

#include <pqxx/pqxx>

#include "lake/control/policy.hpp"

namespace lake::control {
namespace {

std::optional<std::string> nullable(const std::string &s) {
    if (s.empty())
        return std::nullopt;
    return s;
}

constexpr const char *user_columns =
    "id::text, coalesce(taap_id,''), coalesce(email,''), display_name, "
    "created_at::text, updated_at::text";
constexpr const char *organization_columns =
    "id::text, coalesce(taap_id,''), display_name, created_at::text, "
    "updated_at::text";
constexpr const char *customer_columns = "id::text, customer_type, coalesce(user_id::text,''), "
                                         "coalesce(organization_id::text,''), name, "
                                         "created_at::text, updated_at::text";

constexpr const char *repository_columns =
    "id::text, customer_id::text, name, description, visibility, "
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

Repository repository_of(const pqxx::row &r) {
    return Repository{r[0].as<std::string>(), r[1].as<std::string>(), r[2].as<std::string>(),
                      r[3].as<std::string>(), r[4].as<std::string>(), r[5].as<std::string>(),
                      r[6].as<std::string>()};
}

Team team_of(const pqxx::row &row) {
    return Team{row[0].as<std::string>(), row[1].as<std::string>(), row[2].as<std::string>(),
                row[3].as<std::string>(), row[4].as<std::string>(), row[5].as<std::string>()};
}

Membership membership_of(const pqxx::row &row) {
    return Membership{row[0].as<std::string>(), row[1].as<std::string>(), row[2].as<std::string>(),
                      row[3].as<std::string>(), row[4].as<std::string>(), row[5].as<std::string>(),
                      row[6].as<std::string>(), row[7].as<std::string>(), row[8].as<std::string>(),
                      row[9].as<std::string>(), row[10].as<std::string>()};
}

Invitation invitation_of(const pqxx::row &row) {
    return Invitation{
        row[0].as<std::string>(), row[1].as<std::string>(),  row[2].as<std::string>(),
        row[3].as<std::string>(), row[4].as<std::string>(),  row[5].as<std::string>(),
        row[6].as<std::string>(), row[7].as<std::string>(),  row[8].as<std::string>(),
        row[9].as<std::string>(), row[10].as<std::string>(), row[11].as<std::string>()};
}

PolicyObject object_of(const pqxx::row &row) {
    return PolicyObject{row[0].as<std::string>(), row[1].as<std::string>(),
                        row[2].as<std::string>(), row[3].as<std::string>(),
                        row[5].as<std::string>(), row[6].as<std::string>(),
                        row[4].as<bool>()};
}

Grant grant_of(const pqxx::row &row) {
    return Grant{row[0].as<std::string>(), row[1].as<std::string>(), row[2].as<std::string>(),
                 row[3].as<std::string>(), row[4].as<std::string>(), row[5].as<std::string>(),
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

void append_audit_event(pqxx::work &tx, std::string_view acting_principal,
                        const std::optional<std::string> &object_id, std::string_view action,
                        std::string_view request_id, const nlohmann::json &detail) {
    tx.exec_params("INSERT INTO audit_events (acting_principal, object_id, "
                   "action, outcome, request_id, detail) "
                   "VALUES ($1, $2::uuid, $3, 'allowed', $4, $5::jsonb)",
                   acting_principal, object_id, action, request_id, detail.dump());
}

User user_of(const pqxx::row &r) {
    return User{r[0].as<std::string>(), r[1].as<std::string>(), r[2].as<std::string>(),
                r[3].as<std::string>(), r[4].as<std::string>(), r[5].as<std::string>()};
}
Organization organization_of(const pqxx::row &r) {
    return Organization{r[0].as<std::string>(), r[1].as<std::string>(), r[2].as<std::string>(),
                        r[3].as<std::string>(), r[4].as<std::string>()};
}
Customer customer_of(const pqxx::row &r) {
    return Customer{r[0].as<std::string>(), r[1].as<std::string>(), r[2].as<std::string>(),
                    r[3].as<std::string>(), r[4].as<std::string>(), r[5].as<std::string>(),
                    r[6].as<std::string>()};
}

class PostgresStore final : public Store {
  public:
    explicit PostgresStore(std::string uri) : uri_(std::move(uri)) {
    }
    std::string kind() const override {
        return "postgres";
    }

    Result<void> ping() override {
        Result<void> result = with_tx<void>([](pqxx::work &tx) -> Result<void> {
            tx.exec("SET LOCAL statement_timeout = '1000ms'");
            tx.exec("SELECT 1");
            return {};
        });
        if (!result)
            return std::unexpected(StoreError{"store_unavailable", result.error().message});
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
        return with_tx<std::vector<User>>([&](pqxx::work &tx) -> Result<std::vector<User>> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + user_columns +
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
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + user_columns + " FROM users WHERE id = $1::uuid", id);
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
                    return std::unexpected(StoreError{
                        "invalid_input", "customer_id does not reference a policy object"});
                }
                parent_id = parents[0][0].as<std::string>();
            }
            const pqxx::result rows =
                tx.exec_params(std::string("INSERT INTO organizations (taap_id, display_name) "
                                           "VALUES ($1, $2) RETURNING ") +
                                   organization_columns,
                               nullable(in.taap_id), in.display_name);
            Organization organization = organization_of(rows[0]);
            tx.exec_params("INSERT INTO objects (object_type, external_id, parent_id) "
                           "VALUES ('organization', $1, $2::uuid)",
                           organization.id, parent_id);
            return organization;
        });
    }
    Result<std::vector<Organization>> list_organizations(int limit) override {
        return with_tx<std::vector<Organization>>(
            [&](pqxx::work &tx) -> Result<std::vector<Organization>> {
                const pqxx::result rows =
                    tx.exec_params(std::string("SELECT ") + organization_columns +
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
            const pqxx::result rows = tx.exec_params(std::string("SELECT ") + organization_columns +
                                                         " FROM organizations WHERE id = $1::uuid",
                                                     id);
            if (rows.empty())
                return std::unexpected(StoreError{"not_found", "no organization " + id});
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
                in.customer_type, nullable(in.user_id), nullable(in.organization_id), in.name);
            Customer customer = customer_of(rows[0]);
            const pqxx::result objects =
                tx.exec_params("INSERT INTO objects (object_type, external_id) "
                               "VALUES ('customer', $1) "
                               "RETURNING id::text",
                               customer.id);
            if (!customer.organization_id.empty()) {
                tx.exec_params("UPDATE objects SET parent_id = $1::uuid "
                               "WHERE object_type = 'organization' AND external_id = $2",
                               objects[0][0].as<std::string>(), customer.organization_id);
            }
            return customer;
        });
    }
    Result<std::vector<Customer>> list_customers(int limit) override {
        return with_tx<std::vector<Customer>>([&](pqxx::work &tx) -> Result<std::vector<Customer>> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + customer_columns +
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
            const pqxx::result rows = tx.exec_params(std::string("SELECT ") + customer_columns +
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
                return std::unexpected(
                    StoreError{"invalid_input", "customer_id does not reference a policy object"});
            }
            const pqxx::result rows =
                tx.exec_params(std::string("INSERT INTO repositories (customer_id, name, "
                                           "description, visibility) "
                                           "VALUES ($1::uuid, $2, $3, $4) RETURNING ") +
                                   repository_columns,
                               in.customer_id, in.name, in.description,
                               in.visibility.empty() ? std::string("private") : in.visibility);
            Repository repository = repository_of(rows[0]);
            tx.exec_params("INSERT INTO objects (object_type, external_id, parent_id) "
                           "VALUES ('repository', $1, $2::uuid)",
                           repository.id, parents[0][0].as<std::string>());
            return repository;
        });
    }
    Result<std::vector<Repository>> list_repositories(const std::string &customer_id,
                                                      int limit) override {
        return with_tx<std::vector<Repository>>(
            [&](pqxx::work &tx) -> Result<std::vector<Repository>> {
                const pqxx::result rows =
                    customer_id.empty()
                        ? tx.exec_params(std::string("SELECT ") + repository_columns +
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
            const pqxx::result rows = tx.exec_params(std::string("SELECT ") + repository_columns +
                                                         " FROM repositories WHERE id = $1::uuid",
                                                     id);
            if (rows.empty())
                return std::unexpected(StoreError{"not_found", "no repository " + id});
            return repository_of(rows[0]);
        });
    }

    Result<Team> create_team(const NewTeam &in) override {
        return with_tx<Team>([&](pqxx::work &tx) -> Result<Team> {
            const pqxx::result parents =
                tx.exec_params("SELECT id::text FROM objects "
                               "WHERE object_type = 'organization' AND external_id = $1",
                               in.organization_id);
            if (parents.empty()) {
                return std::unexpected(StoreError{
                    "invalid_input", "organization_id does not reference a policy object"});
            }
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO teams (organization_id, name, display_name) "
                            "VALUES ($1::uuid, $2, $3) RETURNING ") +
                    team_columns,
                in.organization_id, in.name, in.display_name);
            Team team = team_of(rows[0]);
            const pqxx::result objects =
                tx.exec_params("INSERT INTO objects (object_type, external_id, parent_id) "
                               "VALUES ('team', $1, $2::uuid) RETURNING id::text",
                               team.id, parents[0][0].as<std::string>());
            append_audit_event(tx, in.acting_principal, objects[0][0].as<std::string>(),
                               "create_team", in.request_id, nlohmann::json{{"team_id", team.id}});
            return team;
        });
    }

    Result<std::vector<Team>> list_teams(const std::string &organization_id, int limit) override {
        return with_tx<std::vector<Team>>([&](pqxx::work &tx) -> Result<std::vector<Team>> {
            const pqxx::result rows =
                organization_id.empty()
                    ? tx.exec_params(std::string("SELECT ") + team_columns +
                                         " FROM teams ORDER BY created_at DESC LIMIT $1",
                                     limit)
                    : tx.exec_params(std::string("SELECT ") + team_columns +
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
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + team_columns + " FROM teams WHERE id = $1::uuid", id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no team " + id});
            }
            return team_of(rows[0]);
        });
    }

    Result<Membership> create_membership(const NewMembership &in) override {
        return with_tx<Membership>([&](pqxx::work &tx) -> Result<Membership> {
            if (in.principal_type == "user") {
                const pqxx::result users =
                    tx.exec_params("SELECT 1 FROM users WHERE id = $1::uuid AND taap_id = $2",
                                   in.principal_id, in.principal_taap_id);
                if (users.empty()) {
                    return std::unexpected(StoreError{
                        "invalid_input", "user principal does not match its TAAP identity"});
                }
            }
            const std::string parent_type = in.team_id.empty() ? "organization" : "team";
            const std::string parent_external_id =
                in.team_id.empty() ? in.organization_id : in.team_id;
            if (!in.team_id.empty()) {
                const pqxx::result teams =
                    tx.exec_params("SELECT 1 FROM teams WHERE id = $1::uuid AND "
                                   "organization_id = $2::uuid",
                                   in.team_id, in.organization_id);
                if (teams.empty()) {
                    return std::unexpected(
                        StoreError{"invalid_input", "team_id does not belong to the organization"});
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
                std::string("INSERT INTO memberships (organization_id, team_id, "
                            "principal_type, "
                            "principal_id, principal_taap_id, role, attestation_digest, "
                            "granted_by_taap_id) VALUES ($1::uuid, $2::uuid, $3, $4, $5, $6, "
                            "$7, "
                            "$8) RETURNING ") +
                    membership_columns,
                in.organization_id, nullable(in.team_id), in.principal_type, in.principal_id,
                in.principal_taap_id, in.role, in.attestation_digest, in.granted_by_taap_id);
            Membership membership = membership_of(rows[0]);
            const pqxx::result objects =
                tx.exec_params("INSERT INTO objects (object_type, external_id, parent_id) "
                               "VALUES ('member', $1, $2::uuid) RETURNING id::text",
                               membership.id, parents[0][0].as<std::string>());
            const std::string grant_role = in.role == "member" ? "read" : "admin";
            tx.exec_params("INSERT INTO grants (principal_taap_id, object_id, role, "
                           "attestation_digest, "
                           "granted_by_taap_id) VALUES ($1, $2::uuid, $3, $4, $5)",
                           in.principal_taap_id, parents[0][0].as<std::string>(), grant_role,
                           in.attestation_digest, in.granted_by_taap_id);
            append_audit_event(
                tx, in.granted_by_taap_id, objects[0][0].as<std::string>(), "create_membership",
                in.request_id,
                nlohmann::json{{"membership_id", membership.id}, {"role", membership.role}});
            return membership;
        });
    }

    Result<std::vector<Membership>> list_memberships(const std::string &organization_id,
                                                     const std::string &team_id,
                                                     int limit) override {
        return with_tx<std::vector<Membership>>(
            [&](pqxx::work &tx) -> Result<std::vector<Membership>> {
                std::string where = " WHERE true";
                pqxx::params params;
                if (!organization_id.empty()) {
                    params.append(organization_id);
                    where += " AND organization_id = $" + std::to_string(params.size()) + "::uuid";
                }
                if (!team_id.empty()) {
                    params.append(team_id);
                    where += " AND team_id = $" + std::to_string(params.size()) + "::uuid";
                }
                params.append(limit);
                const std::string query =
                    std::string("SELECT ") + membership_columns + " FROM memberships" + where +
                    " ORDER BY created_at DESC LIMIT $" + std::to_string(params.size());
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
                    return std::unexpected(
                        StoreError{"invalid_input", "team_id does not belong to the organization"});
                }
            }
            const pqxx::result rows =
                tx.exec_params(std::string("INSERT INTO invitations (organization_id, team_id, "
                                           "invitee_type, "
                                           "invitee, role, expires_at, created_by_taap_id) "
                                           "VALUES ($1::uuid, $2::uuid, $3, lower($4), $5, "
                                           "$6::timestamptz, $7) "
                                           "RETURNING ") +
                                   invitation_columns,
                               in.organization_id, nullable(in.team_id), in.invitee_type,
                               in.invitee, in.role, in.expires_at, in.created_by_taap_id);
            Invitation invitation = invitation_of(rows[0]);
            const std::string target_type = in.team_id.empty() ? "organization" : "team";
            const std::string target_external_id =
                in.team_id.empty() ? in.organization_id : in.team_id;
            const pqxx::result objects =
                tx.exec_params("SELECT id::text FROM objects WHERE object_type = $1 "
                               "AND external_id = $2",
                               target_type, target_external_id);
            append_audit_event(tx, in.created_by_taap_id,
                               objects.empty()
                                   ? std::optional<std::string>{}
                                   : std::optional<std::string>{objects[0][0].as<std::string>()},
                               "create_invitation", in.request_id,
                               nlohmann::json{{"invitation_id", invitation.id},
                                              {"invitee_type", invitation.invitee_type}});
            return invitation;
        });
    }

    Result<std::vector<Invitation>> list_invitations(const std::string &organization_id,
                                                     const std::string &team_id,
                                                     int limit) override {
        return with_tx<std::vector<Invitation>>(
            [&](pqxx::work &tx) -> Result<std::vector<Invitation>> {
                std::string where = " WHERE true";
                pqxx::params params;
                if (!organization_id.empty()) {
                    params.append(organization_id);
                    where += " AND organization_id = $" + std::to_string(params.size()) + "::uuid";
                }
                if (!team_id.empty()) {
                    params.append(team_id);
                    where += " AND team_id = $" + std::to_string(params.size()) + "::uuid";
                }
                params.append(limit);
                const std::string query =
                    std::string("SELECT ") + invitation_columns + " FROM invitations" + where +
                    " ORDER BY created_at DESC LIMIT $" + std::to_string(params.size());
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
            const pqxx::result rows = tx.exec_params(std::string("SELECT ") + invitation_columns +
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
                return std::unexpected(StoreError{"conflict", "invitation is not pending"});
            }
            if (tx.exec_params("SELECT $1::timestamptz <= now()", invitation.expires_at)[0][0]
                    .as<bool>()) {
                return std::unexpected(StoreError{"conflict", "invitation has expired"});
            }
            if (in.principal_type == "user") {
                const pqxx::result users =
                    tx.exec_params("SELECT 1 FROM users WHERE id = $1::uuid AND taap_id = $2",
                                   in.principal_id, in.principal_taap_id);
                if (users.empty()) {
                    return std::unexpected(StoreError{
                        "invalid_input", "user principal does not match its TAAP identity"});
                }
            }
            const std::string parent_type = invitation.team_id.empty() ? "organization" : "team";
            const std::string parent_external_id =
                invitation.team_id.empty() ? invitation.organization_id : invitation.team_id;
            const pqxx::result parents =
                tx.exec_params("SELECT id::text FROM objects WHERE object_type = $1 "
                               "AND external_id = $2",
                               parent_type, parent_external_id);
            if (parents.empty()) {
                return std::unexpected(
                    StoreError{"store_error", "membership policy parent is missing"});
            }
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO memberships (organization_id, team_id, "
                            "principal_type, "
                            "principal_id, principal_taap_id, role, attestation_digest, "
                            "granted_by_taap_id) VALUES ($1::uuid, $2::uuid, $3, $4, $5, $6, "
                            "$7, "
                            "$8) RETURNING ") +
                    membership_columns,
                invitation.organization_id, nullable(invitation.team_id), in.principal_type,
                in.principal_id, in.principal_taap_id, invitation.role, in.attestation_digest,
                in.accepted_by_taap_id);
            Membership membership = membership_of(rows[0]);
            const pqxx::result objects =
                tx.exec_params("INSERT INTO objects (object_type, external_id, parent_id) "
                               "VALUES ('member', $1, $2::uuid) RETURNING id::text",
                               membership.id, parents[0][0].as<std::string>());
            const std::string grant_role = invitation.role == "member" ? "read" : "admin";
            tx.exec_params("INSERT INTO grants (principal_taap_id, object_id, role, "
                           "attestation_digest, "
                           "granted_by_taap_id) VALUES ($1, $2::uuid, $3, $4, $5)",
                           in.principal_taap_id, parents[0][0].as<std::string>(), grant_role,
                           in.attestation_digest, in.accepted_by_taap_id);
            tx.exec_params("UPDATE invitations SET status = 'accepted', "
                           "accepted_membership_id = $1::uuid "
                           "WHERE id = $2::uuid",
                           membership.id, invitation.id);
            append_audit_event(
                tx, in.accepted_by_taap_id, objects[0][0].as<std::string>(), "accept_invitation",
                in.request_id,
                nlohmann::json{{"invitation_id", invitation.id}, {"membership_id", membership.id}});
            return membership;
        });
    }

    Result<PolicyObject> create_policy_object(const NewPolicyObject &in) override {
        return with_tx<PolicyObject>([&](pqxx::work &tx) -> Result<PolicyObject> {
            if (in.parent_id.empty() && in.object_type != "customer") {
                return std::unexpected(StoreError{
                    "invalid_input", "only a customer policy object may have no parent"});
            }
            if (!in.parent_id.empty()) {
                const pqxx::result parents = tx.exec_params(
                    "SELECT object_type FROM objects WHERE id = $1::uuid", in.parent_id);
                if (parents.empty()) {
                    return std::unexpected(StoreError{
                        "invalid_input", "parent_id does not reference a policy object"});
                }
                if (!object_parent_allowed(in.object_type, parents[0][0].as<std::string>())) {
                    return std::unexpected(
                        StoreError{"invalid_input", "object type is not valid under this parent"});
                }
            }
            const pqxx::result rows =
                tx.exec_params(std::string("INSERT INTO objects (object_type, external_id, "
                                           "parent_id, inherit) "
                                           "VALUES ($1, $2, $3::uuid, $4) RETURNING ") +
                                   object_columns,
                               in.object_type, in.external_id, nullable(in.parent_id), in.inherit);
            PolicyObject object = object_of(rows[0]);
            append_audit_event(tx, in.acting_principal, object.id, "create_object", in.request_id,
                               nlohmann::json{{"object_type", object.object_type},
                                              {"external_id", object.external_id}});
            return object;
        });
    }

    Result<PolicyObject> get_policy_object(const std::string &id) override {
        return with_tx<PolicyObject>([&](pqxx::work &tx) -> Result<PolicyObject> {
            const pqxx::result rows = tx.exec_params(
                std::string("SELECT ") + object_columns + " FROM objects WHERE id = $1::uuid", id);
            if (rows.empty()) {
                return std::unexpected(StoreError{"not_found", "no policy object " + id});
            }
            return object_of(rows[0]);
        });
    }

    Result<PolicyObject> find_policy_object(const std::string &object_type,
                                            const std::string &external_id) override {
        return with_tx<PolicyObject>([&](pqxx::work &tx) -> Result<PolicyObject> {
            const pqxx::result rows =
                tx.exec_params(std::string("SELECT ") + object_columns +
                                   " FROM objects WHERE object_type = $1 AND external_id = $2",
                               object_type, external_id);
            if (rows.empty()) {
                return std::unexpected(
                    StoreError{"not_found", "no " + object_type + " object " + external_id});
            }
            return object_of(rows[0]);
        });
    }

    Result<std::vector<PolicyObject>> list_policy_objects(const std::string &parent_id,
                                                          const std::string &object_type,
                                                          int limit) override {
        return with_tx<std::vector<PolicyObject>>(
            [&](pqxx::work &tx) -> Result<std::vector<PolicyObject>> {
                std::string where = " WHERE true";
                pqxx::params params;
                if (!parent_id.empty()) {
                    params.append(parent_id);
                    where += " AND parent_id = $" + std::to_string(params.size()) + "::uuid";
                }
                if (!object_type.empty()) {
                    params.append(object_type);
                    where += " AND object_type = $" + std::to_string(params.size());
                }
                params.append(limit);
                const std::string query =
                    std::string("SELECT ") + object_columns + " FROM objects" + where +
                    " ORDER BY created_at DESC LIMIT $" + std::to_string(params.size());
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
        return with_tx<std::vector<ObjectPathEntry>>(
            [&](pqxx::work &tx) -> Result<std::vector<ObjectPathEntry>> {
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
                    out.push_back(ObjectPathEntry{row[0].as<std::string>(), std::move(object),
                                                  row[8].as<int>()});
                }
                return out;
            });
    }

    Result<Grant> create_grant(const NewGrant &in) override {
        return with_tx<Grant>([&](pqxx::work &tx) -> Result<Grant> {
            const pqxx::result objects =
                tx.exec_params("SELECT object_type FROM objects WHERE id = $1::uuid", in.object_id);
            if (objects.empty()) {
                return std::unexpected(
                    StoreError{"invalid_input", "object_id does not reference a policy object"});
            }
            if (!role_valid_for_object_type(objects[0][0].as<std::string>(), in.role)) {
                return std::unexpected(
                    StoreError{"invalid_input", "role is not valid for this object type"});
            }
            const pqxx::result rows = tx.exec_params(
                std::string("INSERT INTO grants (principal_taap_id, object_id, role, "
                            "attestation_digest, granted_by_taap_id) "
                            "VALUES ($1, $2::uuid, $3, $4, $5) RETURNING ") +
                    grant_columns,
                in.principal_taap_id, in.object_id, in.role, in.attestation_digest,
                in.granted_by_taap_id);
            Grant grant = grant_of(rows[0]);
            append_audit_event(tx, in.granted_by_taap_id, grant.object_id, "create_grant",
                               in.request_id,
                               nlohmann::json{{"grant_id", grant.id}, {"role", grant.role}});
            return grant;
        });
    }

    Result<std::vector<Grant>> list_grants(const std::vector<std::string> &object_ids,
                                           const std::optional<std::string> &principal_taap_id,
                                           int limit) override {
        return with_tx<std::vector<Grant>>([&](pqxx::work &tx) -> Result<std::vector<Grant>> {
            std::string where = " WHERE true";
            pqxx::params params;
            if (!object_ids.empty()) {
                params.append(uuid_array(object_ids));
                where += " AND object_id = ANY($" + std::to_string(params.size()) + "::uuid[])";
            }
            if (principal_taap_id) {
                params.append(*principal_taap_id);
                where += " AND principal_taap_id = $" + std::to_string(params.size());
            }
            params.append(limit);
            const std::string query = std::string("SELECT ") + grant_columns + " FROM grants" +
                                      where + " ORDER BY created_at DESC LIMIT $" +
                                      std::to_string(params.size());
            const pqxx::result rows = tx.exec(query, params);
            std::vector<Grant> out;
            for (const auto &row : rows) {
                out.push_back(grant_of(row));
            }
            return out;
        });
    }

  private:
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

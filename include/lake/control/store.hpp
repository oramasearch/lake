#pragma once
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/audit/audit.hpp"

// The control-plane store: users and organizations (identity), customers (the
// tenant that owns everything). One interface, two implementations — Postgres
// (production, the schema in db/schema.sql) and in-memory (tests, local demos).
// No HTTP type, no libpq type: the Postgres implementation lives in its own
// library.
namespace lake::control {

struct StoreError {
    std::string code; // not_found | conflict | invalid_input | store_unavailable
                      // | store_error
    std::string message;
};
template <class T> using Result = std::expected<T, StoreError>;

struct User {
    std::string id, taap_id, email, display_name, created_at, updated_at;
};
struct Organization {
    std::string id, taap_id, display_name, created_at, updated_at;
};
struct Customer {
    std::string id, customer_type, user_id, organization_id, name, created_at, updated_at;
};
struct Repository {
    std::string id, customer_id, name, description, visibility, created_at, updated_at;
};
struct Team {
    std::string id, organization_id, name, display_name, created_at, updated_at;
};
struct Membership {
    std::string id, organization_id, team_id, principal_type, principal_id, principal_taap_id, role,
        attestation_digest, granted_by_taap_id, created_at, updated_at;
};
struct Invitation {
    std::string id, organization_id, team_id, invitee_type, invitee, role, expires_at, status,
        accepted_membership_id, created_by_taap_id, created_at, updated_at;
};
struct PolicyObject {
    std::string id, object_type, external_id, parent_id, created_at, updated_at;
    bool inherit = true;
};
struct Grant {
    std::string id, principal_taap_id, object_id, role, attestation_digest, granted_by_taap_id,
        created_at, updated_at;
};
struct ObjectPathEntry {
    std::string requested_object_id;
    PolicyObject object;
    int depth = 0;
};
struct NewUser {
    std::string taap_id, email, display_name;
};
struct NewOrganization {
    std::string taap_id, display_name, customer_id;
};
struct NewCustomer {
    std::string customer_type, user_id, organization_id, name;
};
struct NewRepository {
    std::string customer_id, name, description,
        visibility; // visibility empty = private
};
struct NewTeam {
    std::string organization_id, name, display_name, acting_principal, request_id;
};
struct NewMembership {
    std::string organization_id, team_id, principal_type, principal_id, principal_taap_id, role,
        attestation_digest, granted_by_taap_id, request_id;
};
struct NewInvitation {
    std::string organization_id, team_id, invitee_type, invitee, role, expires_at,
        created_by_taap_id, request_id;
};
struct AcceptInvitation {
    std::string invitation_id, principal_type, principal_id, principal_taap_id, attestation_digest,
        accepted_by_taap_id, request_id;
};
struct NewPolicyObject {
    std::string object_type, external_id, parent_id, acting_principal, request_id;
    bool inherit = true;
};
struct NewGrant {
    std::string principal_taap_id, object_id, role, attestation_digest, granted_by_taap_id,
        request_id;
};

nlohmann::json to_json(const User &);
nlohmann::json to_json(const Organization &);
nlohmann::json to_json(const Customer &);
nlohmann::json to_json(const Repository &);
nlohmann::json to_json(const Team &);
nlohmann::json to_json(const Membership &);
nlohmann::json to_json(const Invitation &);
nlohmann::json to_json(const PolicyObject &);
nlohmann::json to_json(const Grant &);
std::string new_uuid_v4();
std::string now_iso8601();

class Store {
  public:
    virtual ~Store() = default;
    [[nodiscard]] virtual std::string kind() const = 0; // "postgres" | "memory"
    virtual Result<void> ping() = 0;

    virtual Result<User> create_user(const NewUser &) = 0;
    virtual Result<std::vector<User>> list_users(int limit) = 0;
    virtual Result<User> get_user(const std::string &id) = 0;

    virtual Result<Organization> create_organization(const NewOrganization &) = 0;
    virtual Result<std::vector<Organization>> list_organizations(int limit) = 0;
    virtual Result<Organization> get_organization(const std::string &id) = 0;

    virtual Result<Customer> create_customer(const NewCustomer &) = 0;
    virtual Result<std::vector<Customer>> list_customers(int limit) = 0;
    virtual Result<Customer> get_customer(const std::string &id) = 0;

    virtual Result<Repository> create_repository(const NewRepository &) = 0;
    // customer_id empty = every customer's repositories.
    virtual Result<std::vector<Repository>> list_repositories(const std::string &customer_id,
                                                              int limit) = 0;
    virtual Result<Repository> get_repository(const std::string &id) = 0;

    virtual Result<Team> create_team(const NewTeam &) = 0;
    virtual Result<std::vector<Team>> list_teams(const std::string &organization_id, int limit) = 0;
    virtual Result<Team> get_team(const std::string &id) = 0;

    virtual Result<Membership> create_membership(const NewMembership &) = 0;
    virtual Result<std::vector<Membership>>
    list_memberships(const std::string &organization_id, const std::string &team_id, int limit) = 0;
    virtual Result<Membership> accept_invitation(const AcceptInvitation &) = 0;

    virtual Result<Invitation> create_invitation(const NewInvitation &) = 0;
    virtual Result<std::vector<Invitation>>
    list_invitations(const std::string &organization_id, const std::string &team_id, int limit) = 0;
    virtual Result<Invitation> get_invitation(const std::string &id) = 0;

    virtual Result<PolicyObject> create_policy_object(const NewPolicyObject &) = 0;
    virtual Result<PolicyObject> get_policy_object(const std::string &id) = 0;
    virtual Result<PolicyObject> find_policy_object(const std::string &object_type,
                                                    const std::string &external_id) = 0;
    virtual Result<std::vector<PolicyObject>> list_policy_objects(const std::string &parent_id,
                                                                  const std::string &object_type,
                                                                  int limit) = 0;
    virtual Result<std::vector<ObjectPathEntry>>
    policy_paths(const std::vector<std::string> &requested_object_ids) = 0;

    virtual Result<Grant> create_grant(const NewGrant &) = 0;
    virtual Result<std::vector<Grant>>
    list_grants(const std::vector<std::string> &object_ids,
                const std::optional<std::string> &principal_taap_id, int limit) = 0;

    virtual audit::Result<audit::Identity> current_lake_identity() = 0;
    virtual audit::Result<audit::Identity> lake_identity_by_taap_id(const std::string &taap_id) = 0;
    virtual audit::Result<void> create_lake_identity(const audit::Identity &identity) = 0;
    virtual audit::Result<void> rotate_lake_identity(const audit::Identity &previous,
                                                     const audit::Identity &replacement) = 0;

    virtual Result<void> append_audit_event(const audit::Event &event) = 0;
    virtual Result<audit::Event> latest_audit_event() = 0;
    virtual Result<std::vector<audit::Event>> list_audit_events(const audit::Filter &filter) = 0;
    virtual Result<void> set_audit_retention_days(const std::string &customer_id, int days) = 0;
};

} // namespace lake::control

#pragma once
#include <expected>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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
    std::string id;
    std::string customer_id;
    std::string name;
    std::string description;
    std::string visibility;
    std::string deleted_at;
    std::string retention_until;
    std::string created_at;
    std::string updated_at;
};
struct AuditEvent {
    std::string id;
    std::string repository_id;
    std::string actor;
    std::string action;
    std::string outcome;
    std::string detail;
    std::string created_at;
};
struct RepositoryRef {
    std::string repository_id;
    std::string name;
    std::string head;
    std::string updated_at;
};
struct NewUser {
    std::string taap_id, email, display_name;
};
struct NewOrganization {
    std::string taap_id, display_name;
};
struct NewCustomer {
    std::string customer_type, user_id, organization_id, name;
};
struct NewRepository {
    std::string customer_id, name, description,
        visibility; // visibility empty = private
};
struct RepositoryDeletion {
    std::string deleted_at;
    std::string retention_until;
};

nlohmann::json to_json(const User&);
nlohmann::json to_json(const Organization&);
nlohmann::json to_json(const Customer&);
nlohmann::json to_json(const Repository&);
nlohmann::json to_json(const AuditEvent&);
nlohmann::json to_json(const RepositoryRef&);

class Store {
public:
    virtual ~Store() = default;
    [[nodiscard]] virtual std::string kind() const = 0; // "postgres" | "memory"
    virtual Result<void> ping() = 0;

    virtual Result<User> create_user(const NewUser&) = 0;
    virtual Result<std::vector<User>> list_users(int limit) = 0;
    virtual Result<User> get_user(const std::string& id) = 0;

    virtual Result<Organization> create_organization(const NewOrganization&) = 0;
    virtual Result<std::vector<Organization>> list_organizations(int limit) = 0;
    virtual Result<Organization> get_organization(const std::string& id) = 0;

    virtual Result<Customer> create_customer(const NewCustomer&) = 0;
    virtual Result<std::vector<Customer>> list_customers(int limit) = 0;
    virtual Result<Customer> get_customer(const std::string& id) = 0;

    virtual Result<Repository> create_repository(const NewRepository&,
                                                 const std::string& actor) = 0;
    // customer_id empty = every customer's repositories.
    virtual Result<std::vector<Repository>> list_repositories(const std::string& customer_id,
                                                              int limit) = 0;
    virtual Result<Repository> get_repository(const std::string& id) = 0;
    virtual Result<Repository>
    rename_repository(const std::string& id, const std::string& name, const std::string& actor) = 0;
    virtual Result<Repository> transfer_repository(const std::string& id,
                                                   const std::string& customer_id,
                                                   const std::string& actor) = 0;
    virtual Result<Repository> set_repository_visibility(const std::string& id,
                                                         const std::string& visibility,
                                                         const std::string& actor) = 0;
    virtual Result<Repository> delete_repository(const std::string& id,
                                                 const RepositoryDeletion& deletion,
                                                 const std::string& actor) = 0;
    virtual Result<std::vector<AuditEvent>>
    list_repository_audit_events(const std::string& repository_id, int limit) = 0;
    virtual Result<std::vector<RepositoryRef>>
    list_repository_refs(const std::string& repository_id) = 0;
    virtual Result<RepositoryRef> set_repository_ref(const std::string& repository_id,
                                                     const std::string& name,
                                                     const std::string& head) = 0;
};

} // namespace lake::control

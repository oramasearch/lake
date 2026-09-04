#pragma once
#include <expected>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/audit/audit.hpp"

// The control-plane store: users and organizations (identity), customers (the tenant that owns
// everything). One interface, two implementations — Postgres (production, the schema in
// db/schema.sql) and in-memory (tests, local demos). No HTTP type, no libpq type: the Postgres
// implementation lives in its own library.
namespace lake::control {

struct StoreError {
    std::string code;  // not_found | conflict | invalid_input | store_unavailable | store_error
    std::string message;
};
template <class T>
using Result = std::expected<T, StoreError>;

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
    std::string customer_id, name, description, visibility;  // visibility empty = private
};

nlohmann::json to_json(const User&);
nlohmann::json to_json(const Organization&);
nlohmann::json to_json(const Customer&);
nlohmann::json to_json(const Repository&);

class Store {
public:
    virtual ~Store() = default;
    [[nodiscard]] virtual std::string kind() const = 0;  // "postgres" | "memory"
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

    virtual Result<Repository> create_repository(const NewRepository&) = 0;
    // customer_id empty = every customer's repositories.
    virtual Result<std::vector<Repository>> list_repositories(const std::string& customer_id, int limit) = 0;
    virtual Result<Repository> get_repository(const std::string& id) = 0;

    virtual audit::Result<audit::Identity> current_lake_identity() = 0;
    virtual audit::Result<audit::Identity> lake_identity_by_taap_id(const std::string& taap_id) = 0;
    virtual audit::Result<void> create_lake_identity(const audit::Identity& identity) = 0;
    virtual audit::Result<void> rotate_lake_identity(const audit::Identity& previous,
                                                      const audit::Identity& replacement) = 0;

    virtual Result<void> append_audit_event(const audit::Event& event) = 0;
    virtual Result<audit::Event> latest_audit_event() = 0;
    virtual Result<std::vector<audit::Event>> list_audit_events(const audit::Filter& filter) = 0;
    virtual Result<void> set_audit_retention_days(const std::string& customer_id, int days) = 0;
};

}  // namespace lake::control

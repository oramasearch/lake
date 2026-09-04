#pragma once
#include <mutex>
#include <vector>

#include "lake/control/store.hpp"

namespace lake::control {

// In-memory store enforcing the same constraints as db/schema.sql: one customer per identity,
// individual xor organization, one global handle namespace, the handle format.
class InMemoryStore final : public Store {
public:
    std::string kind() const override { return "memory"; }
    Result<void> ping() override;
    Result<User> create_user(const NewUser&) override;
    Result<std::vector<User>> list_users(int limit) override;
    Result<User> get_user(const std::string& id) override;
    Result<Organization> create_organization(const NewOrganization&) override;
    Result<std::vector<Organization>> list_organizations(int limit) override;
    Result<Organization> get_organization(const std::string& id) override;
    Result<Customer> create_customer(const NewCustomer&) override;
    Result<std::vector<Customer>> list_customers(int limit) override;
    Result<Customer> get_customer(const std::string& id) override;
    Result<Repository> create_repository(const NewRepository&) override;
    Result<std::vector<Repository>> list_repositories(const std::string& customer_id, int limit) override;
    Result<Repository> get_repository(const std::string& id) override;
    audit::Result<audit::Identity> current_lake_identity() override;
    audit::Result<audit::Identity> lake_identity_by_taap_id(const std::string& taap_id) override;
    audit::Result<void> create_lake_identity(const audit::Identity& identity) override;
    audit::Result<void> rotate_lake_identity(const audit::Identity& previous,
                                              const audit::Identity& replacement) override;
    Result<void> append_audit_event(const audit::Event& event) override;
    Result<audit::Event> latest_audit_event() override;
    Result<std::vector<audit::Event>> list_audit_events(const audit::Filter& filter) override;
    Result<void> set_audit_retention_days(const std::string& customer_id, int days) override;

private:
    std::mutex mutex_;
    std::vector<User> users_;
    std::vector<Organization> organizations_;
    std::vector<Customer> customers_;
    std::vector<Repository> repositories_;
    std::vector<audit::Identity> identities_;
    std::vector<audit::Event> audit_events_;
    std::vector<std::pair<std::string, int>> audit_retentions_;
};

std::string new_uuid_v4();
std::string now_iso8601();

}  // namespace lake::control

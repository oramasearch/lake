#pragma once
#include <mutex>
#include <vector>

#include "lake/control/store.hpp"

namespace lake::control {

// In-memory store enforcing the same constraints as db/schema.sql: one customer
// per identity, individual xor organization, one global handle namespace, the
// handle format.
class InMemoryStore final : public Store {
public:
    std::string kind() const override {
        return "memory";
    }
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
    Result<Repository> create_repository(const NewRepository&, const std::string& actor) override;
    Result<std::vector<Repository>> list_repositories(const std::string& customer_id,
                                                      int limit) override;
    Result<Repository> get_repository(const std::string& id) override;
    Result<Repository> rename_repository(const std::string& id,
                                         const std::string& name,
                                         const std::string& actor) override;
    Result<Repository> transfer_repository(const std::string& id,
                                           const std::string& customer_id,
                                           const std::string& actor) override;
    Result<Repository> set_repository_visibility(const std::string& id,
                                                 const std::string& visibility,
                                                 const std::string& actor) override;
    Result<Repository> delete_repository(const std::string& id,
                                         const RepositoryDeletion& deletion,
                                         const std::string& actor) override;
    Result<std::vector<AuditEvent>> list_repository_audit_events(const std::string& repository_id,
                                                                 int limit) override;
    Result<std::vector<RepositoryRef>>
    list_repository_refs(const std::string& repository_id) override;
    Result<RepositoryRef> set_repository_ref(const std::string& repository_id,
                                             const std::string& name,
                                             const std::string& head) override;

private:
    std::mutex mutex_;
    std::vector<User> users_;
    std::vector<Organization> organizations_;
    std::vector<Customer> customers_;
    std::vector<Repository> repositories_;
    std::vector<AuditEvent> audit_events_;
    std::vector<RepositoryRef> repository_refs_;
};

std::string new_uuid_v4();
std::string now_iso8601();

} // namespace lake::control

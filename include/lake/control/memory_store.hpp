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
    Result<User> create_user(const NewUser &) override;
    Result<std::vector<User>> list_users(int limit) override;
    Result<User> get_user(const std::string &id) override;
    Result<Organization> create_organization(const NewOrganization &) override;
    Result<std::vector<Organization>> list_organizations(int limit) override;
    Result<Organization> get_organization(const std::string &id) override;
    Result<Customer> create_customer(const NewCustomer &) override;
    Result<std::vector<Customer>> list_customers(int limit) override;
    Result<Customer> get_customer(const std::string &id) override;
    Result<Repository> create_repository(const NewRepository &) override;
    Result<std::vector<Repository>> list_repositories(const std::string &customer_id,
                                                      int limit) override;
    Result<Repository> get_repository(const std::string &id) override;
    Result<Team> create_team(const NewTeam &) override;
    Result<std::vector<Team>> list_teams(const std::string &organization_id, int limit) override;
    Result<Team> get_team(const std::string &id) override;
    Result<Membership> create_membership(const NewMembership &) override;
    Result<std::vector<Membership>> list_memberships(const std::string &organization_id,
                                                     const std::string &team_id,
                                                     int limit) override;
    Result<Membership> accept_invitation(const AcceptInvitation &) override;
    Result<Invitation> create_invitation(const NewInvitation &) override;
    Result<std::vector<Invitation>> list_invitations(const std::string &organization_id,
                                                     const std::string &team_id,
                                                     int limit) override;
    Result<Invitation> get_invitation(const std::string &id) override;
    Result<PolicyObject> create_policy_object(const NewPolicyObject &) override;
    Result<PolicyObject> get_policy_object(const std::string &id) override;
    Result<PolicyObject> find_policy_object(const std::string &object_type,
                                            const std::string &external_id) override;
    Result<std::vector<PolicyObject>> list_policy_objects(const std::string &parent_id,
                                                          const std::string &object_type,
                                                          int limit) override;
    Result<std::vector<ObjectPathEntry>>
    policy_paths(const std::vector<std::string> &requested_object_ids) override;
    Result<Grant> create_grant(const NewGrant &) override;
    Result<std::vector<Grant>> list_grants(const std::vector<std::string> &object_ids,
                                           const std::optional<std::string> &principal_taap_id,
                                           int limit) override;
    audit::Result<audit::Identity> current_lake_identity() override;
    audit::Result<audit::Identity> lake_identity_by_taap_id(const std::string &taap_id) override;
    audit::Result<void> create_lake_identity(const audit::Identity &identity) override;
    audit::Result<void> rotate_lake_identity(const audit::Identity &previous,
                                             const audit::Identity &replacement) override;
    Result<void> append_audit_event(const audit::Event &event) override;
    Result<audit::Event> latest_audit_event() override;
    Result<std::vector<audit::Event>> list_audit_events(const audit::Filter &filter) override;
    Result<void> set_audit_retention_days(const std::string &customer_id, int days) override;

  private:
    std::mutex mutex_;
    std::vector<User> users_;
    std::vector<Organization> organizations_;
    std::vector<Customer> customers_;
    std::vector<Repository> repositories_;
    std::vector<Team> teams_;
    std::vector<Membership> memberships_;
    std::vector<Invitation> invitations_;
    std::vector<PolicyObject> policy_objects_;
    std::vector<Grant> grants_;
    std::vector<audit::Identity> identities_;
    std::vector<audit::Event> audit_events_;
    std::vector<std::pair<std::string, int>> audit_retentions_;
};

} // namespace lake::control

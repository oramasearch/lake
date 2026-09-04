#pragma once
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "lake/api/sync_store.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"
#include "lake/search/index.hpp"

// Pure renderers: data in, HTML out. No HTTP type, no I/O. Pages are
// server-rendered, self-contained (one stylesheet at /style.css, no script) and
// unit-tested here.
namespace lake::api {

std::string web_ui_escape(std::string_view text);
std::string web_ui_stylesheet();

// The page frame: title, the navigation (Home, Customers, Repositories, API;
// Sign in/out), the page content. signed_in switches the session link.
std::string web_ui_shell(std::string_view title,
                         std::string_view meta,
                         std::string_view content,
                         bool signed_in = false);

std::string
web_ui_hello(std::string_view node_name, std::string_view version, bool signed_in = false);
std::string web_ui_login(std::string_view error);
std::string web_ui_sign_up(std::string_view error);
std::string web_ui_sign_in(std::string_view error);
std::string web_ui_mfa(std::string_view error,
                       std::string_view otpauth_uri,
                       const std::vector<std::string>& recovery_codes,
                       bool signed_in);
std::string web_ui_passkeys(std::string_view error);
std::string web_ui_passkey_sign_in(std::string_view error);
std::string web_ui_sessions(const std::vector<control::Session>& sessions,
                            const std::vector<control::Device>& devices,
                            std::string_view error);
std::string web_ui_tokens(const std::vector<control::Session>& sessions,
                          std::string_view issued_secret,
                          std::string_view error);
std::string web_ui_customers(const std::vector<control::Customer>& customers);
// owners: customer id → customer, for the owner column.
std::string web_ui_repositories(const std::vector<control::Repository>& repositories,
                                const std::map<std::string, control::Customer>& owners,
                                bool signed_in = true);
std::string web_ui_repository(const control::Repository& repository,
                              const control::Customer* owner,
                              const std::vector<RepositoryContentRef>& refs = {},
                              bool signed_in = true);
std::string web_ui_repository_tree(const control::Repository& repository,
                                   std::string_view ref,
                                   const std::vector<RepositoryTreeEntry>& entries,
                                   bool signed_in);
std::string web_ui_repository_file(const control::Repository& repository,
                                   const RepositoryFile& file,
                                   bool signed_in);
std::string web_ui_repository_log(const control::Repository& repository,
                                  const std::vector<RepositoryChange>& changes,
                                  bool signed_in);
std::string web_ui_change(const control::Repository& repository,
                          const RepositoryChange& change,
                          bool signed_in);
std::string web_ui_repository_conflicts(const control::Repository& repository,
                                        const std::vector<RepositoryConflict>& conflicts,
                                        bool signed_in);
std::string
web_ui_artifact(std::string_view repository_id, std::string_view result_id, bool signed_in);
std::string web_ui_organizations(const std::vector<control::Organization>& organizations);
std::string web_ui_teams(std::string_view organization_id, const std::vector<control::Team>& teams);
std::string web_ui_memberships(std::string_view organization_id,
                               std::string_view team_id,
                               const std::vector<control::Membership>& memberships);
std::string web_ui_invitations(std::string_view organization_id,
                               std::string_view team_id,
                               const std::vector<control::Invitation>& invitations);
std::string web_ui_grants(const control::PolicyObject& object,
                          const std::vector<control::Grant>& grants);
std::string web_ui_effective_access(const control::PolicyObject& object,
                                    const std::vector<control::EffectiveAccessEntry>& entries);
std::string web_ui_search(std::string_view query, const std::vector<search::Hit>& hits);
std::string web_ui_audit(const std::vector<audit::Event>& events, const audit::Filter& filter);

} // namespace lake::api

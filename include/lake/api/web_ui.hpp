#pragma once
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/api/clerk.hpp"
#include "lake/api/drive.hpp"
#include "lake/api/sync_store.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"
#include "lake/search/index.hpp"

// Pure renderers: data in, HTML out. No HTTP type, no I/O. Pages are
// server-rendered, self-contained (one content-hashed stylesheet and only the
// documented progressive-enhancement scripts) and unit-tested here.
namespace lake::api {

// Presentation state from the common credential resolver. It never grants access;
// registry policy still authorizes every data read and action.
struct WebUiViewer {
    bool signed_in = false;
    bool administrator = false;
    // The request path, for choosing the current navigation item. Empty when a renderer is
    // called outside a request (tests, error pages built without one); the page title decides
    // then, as it did before the path was carried.
    std::string path;
    // The signed-in user's id, so pages can mark the viewer's own personal space.
    std::string user_id;
    // True when Clerk is lake's sign-in (docs/api/clerk.md); lake's own passkey page is then not
    // offered.
    bool clerk = false;

    WebUiViewer() = default;
    WebUiViewer(bool authenticated) : signed_in(authenticated) {}
    WebUiViewer(bool authenticated, bool operator_credential)
        : signed_in(authenticated), administrator(authenticated && operator_credential) {}
};

std::string web_ui_escape(std::string_view text);
std::string web_ui_markdown(std::string_view markdown);
std::string
web_ui_error(std::string_view code, std::string_view message, WebUiViewer viewer = false);
std::string web_ui_loading(std::string_view subject, WebUiViewer viewer = false);
std::string web_ui_hashed_asset_url(std::string_view path, std::string_view contents);
std::string_view web_ui_stylesheet();
std::string_view web_ui_stylesheet_url();
std::span<const std::string_view> web_ui_font_names();
std::string_view web_ui_font_url(std::string_view name);
std::string_view web_ui_asset_cache_control(std::string_view path);
std::string web_ui_icon(std::string_view name);

// A self-hosted IBM Plex WOFF2 face embedded in the binary. Unknown names return an empty view.
std::string_view web_ui_font(std::string_view name);

// The page frame uses the resolved viewer for navigation and session controls.
std::string web_ui_shell(std::string_view title,
                         std::string_view meta,
                         std::string_view content,
                         WebUiViewer viewer = false);

std::string web_ui_home(WebUiViewer viewer = false);
std::string web_ui_login(std::string_view error, WebUiViewer viewer = false);
std::string web_ui_sso(std::string_view organization_id,
                       const std::vector<control::SsoProvider>& providers,
                       const std::vector<control::FederationIssuer>& issuers,
                       const std::vector<control::ProvisioningLog>& logs,
                       WebUiViewer viewer = true);
std::string web_ui_secret_mounts(const std::vector<control::SecretMount>& mounts,
                                 WebUiViewer viewer = true);
std::string web_ui_secrets(const control::SecretMount& mount,
                           std::string_view name,
                           const control::SecretMetadata* metadata,
                           WebUiViewer viewer = true);
std::string web_ui_encryption_keys(const control::SecretMount& mount,
                                   const std::vector<control::EncryptionKey>& keys,
                                   WebUiViewer viewer = true);
std::string web_ui_sign_up(std::string_view error, WebUiViewer viewer = false);
std::string web_ui_sign_in(std::string_view error, WebUiViewer viewer = false);
std::string web_ui_mfa(std::string_view error,
                       std::string_view otpauth_uri,
                       const std::vector<std::string>& recovery_codes,
                       WebUiViewer viewer);
std::string web_ui_passkeys(std::string_view error, WebUiViewer viewer = true);
std::string web_ui_passkey_sign_in(std::string_view error, WebUiViewer viewer = false);
std::string web_ui_application_sign_up(const control::Application& application,
                                       std::string_view error);
std::string web_ui_application_sign_in(const control::Application& application,
                                       std::string_view error);
std::string web_ui_application_mfa(const control::Application& application, std::string_view error);
std::string web_ui_application_passkey_sign_in(const control::Application& application,
                                               std::string_view error);
std::string web_ui_applications(const std::vector<control::Application>& applications,
                                const std::vector<control::Customer>& customers,
                                std::string_view error,
                                WebUiViewer viewer = true);
std::string web_ui_application(const control::Application& application, WebUiViewer viewer = true);
std::string web_ui_application_users(const control::Application& application,
                                     const std::vector<control::ApplicationUser>& users,
                                     WebUiViewer viewer = true);
std::string
web_ui_application_organizations(const control::Application& application,
                                 const std::vector<control::ApplicationOrganization>& organizations,
                                 WebUiViewer viewer = true);
std::string web_ui_application_events(const control::Application& application,
                                      const std::vector<control::ApplicationWebhookEvent>& events,
                                      std::string_view webhook_secret = {},
                                      WebUiViewer viewer = true);
std::string web_ui_sessions(const std::vector<control::Session>& sessions,
                            const std::vector<control::Device>& devices,
                            std::string_view error,
                            WebUiViewer viewer = true);
std::string web_ui_tokens(const std::vector<control::Session>& sessions,
                          std::string_view issued_secret,
                          std::string_view error,
                          WebUiViewer viewer = true);
std::string web_ui_customers(const std::vector<control::Customer>& customers,
                             WebUiViewer viewer = true);
// owners: customer id → customer, for the owner column.
std::string web_ui_repositories(const std::vector<control::Repository>& repositories,
                                const std::map<std::string, control::Customer>& owners,
                                WebUiViewer viewer = true);

struct RepositoryHomeData {
    std::vector<RepositoryContentRef> refs;
    std::vector<RepositoryTreeEntry> tree;
    std::vector<RepositoryChange> changes;
    std::optional<RepositoryFile> readme;
    std::vector<control::EffectiveAccessEntry> access;
    std::vector<control::ForgeRecord> releases;
    // Validated instance configuration, never inferred from a request header.
    std::string external_origin;
};

std::string web_ui_repository(const control::Repository& repository,
                              const control::Customer* owner,
                              const RepositoryHomeData& data,
                              WebUiViewer viewer = true);
std::string web_ui_repository(const control::Repository& repository,
                              const control::Customer* owner,
                              const std::vector<RepositoryContentRef>& refs = {},
                              WebUiViewer viewer = true);
// The sign-in or sign-up page when Clerk is configured: Clerk's form, then a one-time exchange of
// Clerk's `lake` template token for a lake session at POST /identity/clerk-session. `mode` is
// "sign-in" or "sign-up"; `signed_out` signs the browser out of Clerk first.
std::string web_ui_clerk_sign_in(const ClerkConfig& clerk,
                                 std::string_view mode,
                                 std::string_view error,
                                 bool signed_out,
                                 WebUiViewer viewer = {});

// A customer the viewer may act for, for the workspace pages' selector.
struct WorkspaceCustomer {
    std::string id;
    std::string handle;
    bool yours = false; // the viewer's personal space
};

// One Design, Train or Ship page over a customer's Zetta workspace (docs/api/workspaces.md).
// `section` is projects, project, data, runs, run, models or indexes. `state` is the
// workspace_status value; `unavailable` explains why workspaces cannot be used at all; `data` holds
// the Zetta values the section renders, keyed by capability.
struct WorkspacePage {
    std::string section;
    std::vector<WorkspaceCustomer> customers;
    std::string customer_id;
    nlohmann::json state = nlohmann::json::object();
    std::string unavailable;
    nlohmann::json data = nlohmann::json::object();
    std::string subject; // the project name or run id of a detail page
    std::string error;
};
std::string web_ui_workspace(const WorkspacePage& page, WebUiViewer viewer = true);

// Ship > Packages: the registries' catalog ({oci, npm, python}: [{name, versions}]). `origin` is
// the public origin for install commands; empty omits them.
std::string web_ui_packages(const nlohmann::json& catalog,
                            bool available,
                            std::string_view origin,
                            WebUiViewer viewer = true);

// One environment of one repository, for the cross-repository deployments page.
struct DeploymentSummary {
    control::Repository repository;
    nlohmann::json environment;
};
std::string web_ui_deployments(const std::vector<DeploymentSummary>& environments,
                               std::string_view stale_since,
                               WebUiViewer viewer = true);
std::string web_ui_environments(const control::Repository& repository,
                                const nlohmann::json& environments,
                                WebUiViewer viewer = true);
std::string web_ui_environment(const control::Repository& repository,
                               const nlohmann::json& environment,
                               const nlohmann::json& deployments,
                               WebUiViewer viewer = true);
std::string web_ui_logs(const control::Repository& repository,
                        std::string_view environment,
                        const nlohmann::json& logs,
                        std::string_view stale_since,
                        WebUiViewer viewer = true);
std::string web_ui_repository_tree(const control::Repository& repository,
                                   std::string_view ref,
                                   const std::vector<RepositoryTreeEntry>& entries,
                                   WebUiViewer viewer);
std::string web_ui_repository_file(const control::Repository& repository,
                                   std::string_view ref,
                                   const RepositoryFile& file,
                                   const std::vector<RepositoryBlameLine>& blame,
                                   WebUiViewer viewer);
std::string web_ui_repository_file(const control::Repository& repository,
                                   const RepositoryFile& file,
                                   WebUiViewer viewer);
std::string web_ui_repository_log(const control::Repository& repository,
                                  const std::vector<RepositoryChange>& changes,
                                  WebUiViewer viewer);
struct ChangeReviewData {
    std::vector<control::ForgeRecord> reviews;
    std::vector<control::ForgeRecord> comments;
    RepositoryArtifactPage checks;
};

std::string web_ui_change(const control::Repository& repository,
                          const RepositoryChange& change,
                          const std::vector<RepositoryDiffEntry>& differences,
                          const ChangeReviewData& review,
                          WebUiViewer viewer);
std::string web_ui_change(const control::Repository& repository,
                          const RepositoryChange& change,
                          const std::vector<RepositoryDiffEntry>& differences,
                          WebUiViewer viewer);
std::string web_ui_issues(const control::Repository& repository,
                          const std::vector<control::ForgeRecord>& issues,
                          std::string_view state,
                          std::string_view label,
                          std::string_view assignee,
                          WebUiViewer viewer);
std::string web_ui_issue(const control::Repository& repository,
                         const control::ForgeRecord& issue,
                         const std::vector<control::ForgeRecord>& comments,
                         WebUiViewer viewer);
std::string web_ui_releases(const control::Repository& repository,
                            const std::vector<control::ForgeRecord>& releases,
                            WebUiViewer viewer);
std::string web_ui_webhooks(const control::Repository& repository,
                            const std::vector<control::ForgeRecord>& webhooks,
                            const std::vector<control::ForgeRecord>& deliveries,
                            WebUiViewer viewer);
std::string web_ui_branch_protection(const control::Repository& repository,
                                     const std::vector<control::ForgeRecord>& rules,
                                     const std::vector<audit::Event>& events,
                                     WebUiViewer viewer);
std::string web_ui_repository_conflicts(const control::Repository& repository,
                                        const std::vector<RepositoryConflict>& conflicts,
                                        WebUiViewer viewer);
std::string web_ui_artifacts(const control::Repository& repository,
                             const RepositoryArtifactPage& page,
                             const RepositoryArtifactFilter& filter,
                             WebUiViewer viewer);
std::string web_ui_artifact(const control::Repository& repository,
                            const RepositoryArtifact& artifact,
                            WebUiViewer viewer);
std::string web_ui_organizations(const std::vector<control::Organization>& organizations,
                                 WebUiViewer viewer = true);
struct OrganizationSettingsData {
    std::vector<control::Team> teams;
    std::vector<control::Membership> memberships;
    std::vector<control::Invitation> invitations;
    std::vector<control::EffectiveAccessEntry> access;
    std::vector<control::SsoProvider> providers;
    std::vector<control::Application> applications;
    nlohmann::json metering;
    std::vector<audit::Event> audit_events;
};
std::string web_ui_organization(const control::Organization& organization,
                                const OrganizationSettingsData& data,
                                std::string_view tab,
                                std::string_view error = {},
                                WebUiViewer viewer = true);
std::string web_ui_team(const control::Team& team,
                        const std::vector<control::Membership>& memberships,
                        const std::vector<control::EffectiveAccessEntry>& access,
                        WebUiViewer viewer = true);
std::string web_ui_member(const control::Membership& membership,
                          const std::vector<control::EffectiveAccessEntry>& access,
                          std::string_view error = {},
                          bool confirm_removal = false,
                          WebUiViewer viewer = true);
std::string web_ui_settings(std::string_view identity,
                            const std::vector<control::Session>& sessions,
                            const std::vector<control::Device>& devices,
                            std::string_view tab,
                            std::string_view error = {},
                            WebUiViewer viewer = true);
std::string web_ui_teams(std::string_view organization_id,
                         const std::vector<control::Team>& teams,
                         WebUiViewer viewer = true);
std::string web_ui_memberships(std::string_view organization_id,
                               std::string_view team_id,
                               const std::vector<control::Membership>& memberships,
                               WebUiViewer viewer = true);
std::string web_ui_invitations(std::string_view organization_id,
                               std::string_view team_id,
                               const std::vector<control::Invitation>& invitations,
                               WebUiViewer viewer = true);
std::string web_ui_grants(const control::PolicyObject& object,
                          const std::vector<control::Grant>& grants,
                          WebUiViewer viewer = true);
std::string web_ui_effective_access(const control::PolicyObject& object,
                                    const std::vector<control::EffectiveAccessEntry>& entries,
                                    WebUiViewer viewer = true);
std::string web_ui_search(const search::Query& query,
                          const search::QueryResult& result,
                          WebUiViewer viewer = true);
std::string web_ui_metering(const nlohmann::json& usage, WebUiViewer viewer = true);
std::string web_ui_transparency(std::string_view identity,
                                const nlohmann::json& scan,
                                WebUiViewer viewer = false);
std::string web_ui_audit(const std::vector<audit::Event>& events,
                         const audit::Filter& filter,
                         const nlohmann::json& log_configuration = nlohmann::json::object(),
                         WebUiViewer viewer = true);
std::string web_ui_exports(const control::Repository& repository,
                           const std::vector<control::ForgeRecord>& exports,
                           WebUiViewer viewer = true);
std::string web_ui_drive_folder(const control::Repository& repository,
                                std::string_view ref,
                                std::string_view path,
                                const std::vector<RepositoryTreeEntry>& entries,
                                const DriveFolderOptions& options,
                                WebUiViewer viewer);
std::string web_ui_drive_preview(const control::Repository& repository,
                                 std::string_view ref,
                                 const RepositoryFile& file,
                                 const DrivePreview& preview,
                                 WebUiViewer viewer);
std::string web_ui_drive_history(const control::Repository& repository,
                                 std::string_view ref,
                                 std::string_view path,
                                 const std::vector<nlohmann::json>& changes,
                                 WebUiViewer viewer);
std::string web_ui_drive_version_diff(const control::Repository& repository,
                                      std::string_view ref,
                                      std::string_view path,
                                      std::string_view left_revision,
                                      std::string_view right_revision,
                                      const DriveVersionDiff& difference,
                                      WebUiViewer viewer);
std::string web_ui_drive_conflict(const control::Repository& repository,
                                  std::string_view ref,
                                  const RepositoryConflict& conflict,
                                  const std::vector<RepositoryFile>& versions,
                                  const std::optional<DriveVersionDiff>& difference,
                                  WebUiViewer viewer);
std::string web_ui_drive_share(const control::Repository& repository,
                               std::string_view ref,
                               std::string_view path,
                               const std::vector<control::EffectiveAccessEntry>& access,
                               const std::vector<nlohmann::json>& links,
                               std::string_view created_link,
                               std::string_view error,
                               WebUiViewer viewer = true);
std::string web_ui_drive_share_link(const control::Repository& repository,
                                    std::string_view ref,
                                    std::string_view path,
                                    const nlohmann::json& link,
                                    std::string_view error,
                                    WebUiViewer viewer = true);
std::string web_ui_shared_with_me(const std::vector<nlohmann::json>& folders,
                                  std::string_view error,
                                  WebUiViewer viewer = true);
std::string web_ui_drive_link(std::string_view path,
                              const std::vector<RepositoryTreeEntry>& entries,
                              std::string_view error,
                              std::string_view token = {},
                              std::string_view ref = "main",
                              std::string_view root_path = {},
                              WebUiViewer viewer = false);
std::string web_ui_drive_link_preview(std::string_view token,
                                      std::string_view ref,
                                      const RepositoryFile& file,
                                      const DrivePreview& preview,
                                      WebUiViewer viewer = false);
std::string web_ui_model(const nlohmann::json& model, WebUiViewer viewer);

} // namespace lake::api

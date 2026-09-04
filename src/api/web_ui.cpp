#include "lake/api/web_ui.hpp"

#include <cctype>
#include <format>

namespace lake::api {

std::string web_ui_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&#39;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

std::string web_ui_stylesheet() {
    return R"css(:root{--ink:#1b1f24;--muted:#5b6570;--line:#d9dee3;--paper:#fafaf8;--accent:#0b6e99;--accent-ink:#fff}
*{box-sizing:border-box}body{margin:0;font:15px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Helvetica,Arial,sans-serif;color:var(--ink);background:var(--paper)}
header{display:flex;align-items:center;gap:1.5rem;padding:.75rem 1.5rem;border-bottom:1px solid var(--line);background:#fff}
header .brand{font-weight:700;letter-spacing:.02em;text-decoration:none;color:var(--ink)}header nav{display:flex;gap:1rem;flex:1}
header nav a{color:var(--muted);text-decoration:none}header nav a:hover,header nav a[aria-current]{color:var(--accent)}
header .meta,header .session{color:var(--muted);font-size:.9rem}header .session a{color:var(--accent);text-decoration:none}
main{max-width:64rem;margin:2rem auto;padding:0 1.5rem}.eyebrow{text-transform:uppercase;letter-spacing:.12em;font-size:.75rem;color:var(--muted);margin:0}
h1{margin:.25rem 0 .5rem;font-size:1.75rem}.sub{color:var(--muted);margin:0 0 1.5rem}.panel{background:#fff;border:1px solid var(--line);border-radius:8px;padding:1rem 1.25rem}
code{background:#eef1f4;padding:.1rem .3rem;border-radius:4px;font-size:.9em}table{width:100%;border-collapse:collapse;background:#fff;border:1px solid var(--line);border-radius:8px}
th,td{text-align:left;padding:.55rem .75rem;border-bottom:1px solid var(--line)}th{font-size:.8rem;text-transform:uppercase;letter-spacing:.06em;color:var(--muted)}
tr:last-child td{border-bottom:0}td a{color:var(--accent);text-decoration:none}.badge{display:inline-block;padding:.05rem .45rem;border-radius:999px;font-size:.75rem;border:1px solid var(--line);color:var(--muted)}
.empty{color:var(--muted);padding:1.5rem;text-align:center}form.login{display:grid;gap:.75rem;max-width:26rem}form.search{display:flex;gap:.5rem;margin:0 0 1.5rem}
form.login input,.filters input,form.search input{font:inherit;padding:.5rem .6rem;border:1px solid var(--line);border-radius:6px}form.search input{flex:1}form.login button,form.inline button,.filters button,form.search button{font:inherit;padding:.5rem .9rem;border:0;border-radius:6px;background:var(--accent);color:var(--accent-ink);cursor:pointer}
form.inline{display:inline}form.inline button{background:transparent;color:var(--accent);padding:0}.error{color:#a12a2a;margin:0 0 .5rem}dl{display:grid;grid-template-columns:max-content 1fr;gap:.35rem 1rem;margin:0}dt{color:var(--muted)}dd{margin:0}
.filters{display:flex;flex-wrap:wrap;gap:.75rem;align-items:end;margin:0 0 1.5rem}.filters label{display:grid;gap:.2rem;color:var(--muted);font-size:.8rem}
pre{white-space:pre-wrap;overflow-wrap:anywhere;background:#eef1f4;padding:1rem;border-radius:6px}
form.stack{display:grid;gap:.75rem;max-width:36rem}form.stack label{display:grid;gap:.25rem;color:var(--muted)}
form.stack input,form.stack textarea,form.stack select{font:inherit;padding:.5rem .6rem;border:1px solid var(--line);border-radius:6px;background:#fff;color:var(--ink)}
form.stack textarea{min-height:8rem}button.primary{font:inherit;padding:.5rem .9rem;border:0;border-radius:6px;background:var(--accent);color:var(--accent-ink);cursor:pointer;width:max-content}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(18rem,1fr));gap:1rem}.actions{display:flex;gap:1rem;align-items:center;flex-wrap:wrap}.notice{border-left:3px solid var(--accent);padding:.65rem .85rem;background:#eef7fb;margin:0 0 1rem}.muted{color:var(--muted)}
)css";
}

namespace {

std::string nav(bool signed_in) {
    std::string out = "<nav><a href=\"/\">Home</a><a href=\"/customers\">Customers</a><a "
                      "href=\"/organizations\">Organizations</a><a "
                      "href=\"/repositories\">Repositories</a><a href=\"/search\">Search</a>"
                      "<a href=\"/audit\">Audit</a>";
    if (signed_in) {
        out += "<a href=\"/sessions\">Sessions</a><a "
               "href=\"/passkeys\">Passkeys</a><a href=\"/tokens\">Tokens</a>";
    }
    out += "<a href=\"/v1/openapi.json\">API</a></nav>";
    if (signed_in) {
        out += "<span class=\"session\"><form class=\"inline\" method=\"post\" "
               "action=\"/logout\"><button type=\"submit\">Sign "
               "out</button></form></span>";
    } else {
        out += "<span class=\"session\"><a href=\"/sign-in\">Sign in</a> · <a "
               "href=\"/sign-up\">Sign up</a></span>";
    }
    return out;
}

std::string row_link(std::string_view href, std::string_view text) {
    return std::format("<a href=\"{}\">{}</a>", web_ui_escape(href), web_ui_escape(text));
}

std::string url_component(std::string_view value) {
    std::string encoded;
    for (const unsigned char byte : value) {
        if (std::isalnum(byte) || byte == '-' || byte == '_' || byte == '.') {
            encoded += static_cast<char>(byte);
        } else {
            encoded += std::format("%{:02X}", byte);
        }
    }
    return encoded;
}

std::string repository_name(const control::Repository& repository) {
    return repository.name.empty() ? repository.id : repository.name;
}

std::string error_markup(std::string_view error) {
    return error.empty() ? std::string()
                         : std::format("<p class=\"error\">{}</p>", web_ui_escape(error));
}

std::string state_badge(bool active) {
    return std::format("<span class=\"badge\">{}</span>", active ? "active" : "revoked");
}

} // namespace

std::string web_ui_shell(std::string_view title,
                         std::string_view meta,
                         std::string_view content,
                         bool signed_in) {
    return std::format("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                       "<meta name=\"viewport\" "
                       "content=\"width=device-width,initial-scale=1\"><title>{}</title>"
                       "<link rel=\"stylesheet\" href=\"/style.css\"></head><body><header><a "
                       "class=\"brand\" href=\"/\">lake</a>{}"
                       "<span class=\"meta\">{}</span></header><main>{}</main></body></html>",
                       web_ui_escape(title),
                       nav(signed_in),
                       web_ui_escape(meta),
                       content);
}

std::string web_ui_hello(std::string_view node_name, std::string_view version, bool signed_in) {
    const std::string content =
        std::format("<p class=\"eyebrow\">hello world</p><h1>hello from lake</h1>"
                    "<p class=\"sub\">liquid is the tool. lake is where it collects.</p>"
                    "<div class=\"panel\"><p>This node is <code>{}</code>, running lake "
                    "<code>{}</code>, serving over the same "
                    "Beast2 + Capy/Corosio stack as Zetta.</p><p>Explore <a "
                    "href=\"/customers\">customers</a> and "
                    "<a href=\"/repositories\">repositories</a> (sign in with the admin "
                    "token), or use the API: "
                    "<code>GET /v1/openapi.json</code>, MCP at <code>POST "
                    "/mcp</code>.</p></div>",
                    web_ui_escape(node_name),
                    web_ui_escape(version));
    return web_ui_shell(
        "lake — hello", std::format("{} · {}", node_name, version), content, signed_in);
}

std::string web_ui_login(std::string_view error) {
    std::string content = "<p class=\"eyebrow\">session</p><h1>Sign in</h1><p class=\"sub\">Paste "
                          "the Lake's admin token.</p>";
    if (!error.empty())
        content += std::format("<p class=\"error\">{}</p>", web_ui_escape(error));
    content += "<form class=\"login\" method=\"post\" action=\"/session\"><input "
               "type=\"password\" "
               "name=\"token\" "
               "placeholder=\"admin token\" autocomplete=\"current-password\" "
               "required><button "
               "type=\"submit\">Sign in</button></form>";
    return web_ui_shell("lake — sign in", "sign in", content, false);
}

std::string web_ui_customers(const std::vector<control::Customer>& customers) {
    std::string content = "<p class=\"eyebrow\">control plane</p><h1>Customers</h1><p "
                          "class=\"sub\">The tenants. "
                          "Each is an individual or an organization and owns its repositories.</p>";
    if (customers.empty()) {
        content += "<div class=\"panel empty\">No customers yet. <code>POST "
                   "/v1/create_customer</code> creates one.</div>";
    } else {
        content += "<table><thead><tr><th>Handle</th><th>Type</th><th>Identity</"
                   "th><th>Created</"
                   "th></tr></thead><tbody>";
        for (const auto& c : customers) {
            content += std::format(
                "<tr><td>{}</td><td><span "
                "class=\"badge\">{}</span></td><td><code>{}</code></td><td>{}</td></"
                "tr>",
                row_link("/repositories?customer_id=" + c.id, c.name),
                web_ui_escape(c.customer_type),
                web_ui_escape(c.customer_type == "individual" ? c.user_id : c.organization_id),
                web_ui_escape(c.created_at));
        }
        content += "</tbody></table>";
    }
    return web_ui_shell(
        "lake — customers", std::format("{} customers", customers.size()), content, true);
}

std::string web_ui_repositories(const std::vector<control::Repository>& repositories,
                                const std::map<std::string, control::Customer>& owners,
                                bool signed_in) {
    std::string content = "<p class=\"eyebrow\">control "
                          "plane</p><h1>Repositories</h1><p class=\"sub\">Every "
                          "repository, newest first, addressed as "
                          "&lt;customer&gt;/&lt;name&gt;.</p>";
    if (repositories.empty()) {
        content += "<div class=\"panel empty\">No repositories yet. <code>POST "
                   "/v1/create_repository</code> creates one.</div>";
    } else {
        content += "<table><thead><tr><th>Repository</th><th>Owner</th><th>Visibility</"
                   "th><th>Description</th><th>Created</th></tr></thead><tbody>";
        for (const auto& r : repositories) {
            const auto owner = owners.find(r.customer_id);
            const std::string owner_name =
                owner == owners.end() ? r.customer_id : owner->second.name;
            content +=
                std::format("<tr><td>{}</td><td>{}</td><td><span "
                            "class=\"badge\">{}</span></td><td>{}</td><td>{}</td></tr>",
                            row_link("/repository?id=" + r.id, owner_name + "/" + r.name),
                            row_link("/repositories?customer_id=" + r.customer_id, owner_name),
                            web_ui_escape(r.visibility),
                            web_ui_escape(r.description),
                            web_ui_escape(r.created_at));
        }
        content += "</tbody></table>";
    }
    return web_ui_shell("lake — repositories",
                        std::format("{} repositories", repositories.size()),
                        content,
                        signed_in);
}

std::string web_ui_repository(const control::Repository& repository,
                              const control::Customer* owner,
                              const std::vector<RepositoryContentRef>& refs,
                              bool signed_in) {
    const std::string owner_name = owner ? owner->name : repository.customer_id;
    std::string content = std::format(
        "<p class=\"eyebrow\">repository</p><h1>{}/{}</h1><p "
        "class=\"sub\">{}</p><div "
        "class=\"panel\"><dl>"
        "<dt>id</dt><dd><code>{}</code></dd><dt>owner</dt><dd>{}</"
        "dd><dt>visibility</dt><dd><span "
        "class=\"badge\">{}</span></dd>"
        "<dt>created</dt><dd>{}</dd><dt>updated</dt><dd>{}</dd><dt>API</"
        "dt><dd><code>GET "
        "/v1/get_repository?id={}</code></dd></dl></div>",
        web_ui_escape(owner_name),
        web_ui_escape(repository.name),
        web_ui_escape(repository.description.empty() ? "No description." : repository.description),
        web_ui_escape(repository.id),
        row_link("/repositories?customer_id=" + repository.customer_id, owner_name),
        web_ui_escape(repository.visibility),
        web_ui_escape(repository.created_at),
        web_ui_escape(repository.updated_at),
        web_ui_escape(repository.id));
    if (!repository.deleted_at.empty()) {
        content += std::format("<div class=\"panel\"><strong>Tombstoned.</strong> "
                               "Deleted {}. Collection may "
                               "reclaim unreferenced content after {}.</div>",
                               web_ui_escape(repository.deleted_at),
                               web_ui_escape(repository.retention_until));
    }
    content += "<div class=\"panel\"><h2>Repository data</h2><p>"
               "Browse the content stored by libliquid.</p>";
    if (refs.empty()) {
        content += "<p>No refs are published.</p>";
    } else {
        content += "<ul>";
        for (const RepositoryContentRef& ref : refs) {
            content += std::format(
                "<li>{} <code>{}</code></li>",
                row_link("/repository/tree?id=" + repository.id + "&ref=" + url_component(ref.name),
                         ref.name),
                web_ui_escape(ref.change));
        }
        content += "</ul>";
    }
    content += std::format("<p>{} · {} · {}</p></div>",
                           row_link("/changes?repository_id=" + repository.id, "Change log"),
                           row_link("/conflicts?repository_id=" + repository.id, "Conflicts"),
                           row_link("/repository/access?id=" + repository.id, "Effective access"));
    return web_ui_shell("lake — " + owner_name + "/" + repository.name,
                        owner_name + "/" + repository.name,
                        content,
                        signed_in);
}

std::string web_ui_repository_tree(const control::Repository& repository,
                                   std::string_view ref,
                                   const std::vector<RepositoryTreeEntry>& entries,
                                   bool signed_in) {
    std::string content = std::format("<p class=\"eyebrow\">repository tree</p><h1>{}</h1>"
                                      "<p class=\"sub\">Ref <code>{}</code>.</p>",
                                      web_ui_escape(repository_name(repository)),
                                      web_ui_escape(ref));
    if (entries.empty()) {
        content += "<div class=\"panel empty\">This tree has no files.</div>";
    } else {
        content += "<table><thead><tr><th>Path</th><th>Kind</th><th>Blob</th><th>Mode</th>"
                   "</tr></thead><tbody>";
        for (const RepositoryTreeEntry& entry : entries) {
            const std::string path = "/file?repository_id=" + repository.id +
                                     "&ref=" + url_component(ref) +
                                     "&path=" + url_component(entry.path);
            const std::string displayed =
                entry.kind == "blob" ? row_link(path, entry.path) : web_ui_escape(entry.path);
            content += std::format("<tr><td>{}</td><td>{}</td><td><code>{}</code></td>"
                                   "<td><code>{:o}</code></td></tr>",
                                   displayed,
                                   web_ui_escape(entry.kind),
                                   web_ui_escape(entry.blob),
                                   entry.mode);
        }
        content += "</tbody></table>";
    }
    content +=
        std::format("<p>{}</p>", row_link("/repository?id=" + repository.id, "Back to repository"));
    return web_ui_shell("lake — tree", repository_name(repository), content, signed_in);
}

std::string web_ui_repository_file(const control::Repository& repository,
                                   const RepositoryFile& file,
                                   bool signed_in) {
    const std::string content =
        std::format("<p class=\"eyebrow\">file</p><h1>{}</h1><p class=\"sub\">{}</p>"
                    "<div class=\"panel\"><dl><dt>blob</dt><dd><code>{}</code></dd><dt>size</dt>"
                    "<dd>{} bytes</dd></dl></div><pre>{}</pre><p>{}</p>",
                    web_ui_escape(file.path),
                    web_ui_escape(repository_name(repository)),
                    web_ui_escape(file.blob),
                    file.size,
                    web_ui_escape(file.contents),
                    row_link("/repository?id=" + repository.id, "Back to repository"));
    return web_ui_shell("lake — file", file.path, content, signed_in);
}

std::string web_ui_repository_log(const control::Repository& repository,
                                  const std::vector<RepositoryChange>& changes,
                                  bool signed_in) {
    std::string content = std::format("<p class=\"eyebrow\">repository history</p><h1>Change "
                                      "log</h1><p class=\"sub\">{}</p>",
                                      web_ui_escape(repository_name(repository)));
    if (changes.empty()) {
        content += "<div class=\"panel empty\">No changes have been cut.</div>";
    } else {
        content += "<table><thead><tr><th>Change</th><th>Message</th><th>Author</th><th>When</th>"
                   "</tr></thead><tbody>";
        for (const RepositoryChange& change : changes) {
            content +=
                std::format("<tr><td>{}</td><td>{}</td><td><code>{}</code></td><td>{}</td></tr>",
                            row_link("/change?repository_id=" + repository.id +
                                         "&id=" + url_component(change.id),
                                     change.id),
                            web_ui_escape(change.message),
                            web_ui_escape(change.author),
                            web_ui_escape(change.occurred_at));
        }
        content += "</tbody></table>";
    }
    return web_ui_shell("lake — changes", repository_name(repository), content, signed_in);
}

std::string web_ui_change(const control::Repository& repository,
                          const RepositoryChange& change,
                          bool signed_in) {
    std::string parents;
    for (const std::string& parent : change.parents) {
        if (!parents.empty()) {
            parents += ", ";
        }
        parents += parent;
    }
    const std::string content = std::format(
        "<p class=\"eyebrow\">change</p><h1>{}</h1><p class=\"sub\">{}</p>"
        "<div class=\"panel\"><dl><dt>author</dt><dd><code>{}</code></dd><dt>created</dt>"
        "<dd>{}</dd><dt>parents</dt><dd><code>{}</code></dd></dl></div>"
        "<div class=\"panel\"><h2>Structural diff</h2><p>Structural diff rendering arrives "
        "with liquid #166.</p></div>",
        web_ui_escape(change.id),
        web_ui_escape(change.message),
        web_ui_escape(change.author),
        web_ui_escape(change.occurred_at),
        web_ui_escape(parents));
    return web_ui_shell("lake — change", repository_name(repository), content, signed_in);
}

std::string web_ui_repository_conflicts(const control::Repository& repository,
                                        const std::vector<RepositoryConflict>& conflicts,
                                        bool signed_in) {
    std::string content = std::format("<p class=\"eyebrow\">repository conflicts</p><h1>"
                                      "Conflicts</h1><p class=\"sub\">{}</p>",
                                      web_ui_escape(repository_name(repository)));
    if (conflicts.empty()) {
        content += "<div class=\"panel empty\">No unresolved conflict values.</div>";
    } else {
        content += "<table><thead><tr><th>Path</th><th>Conflicting blobs</th></tr></thead><tbody>";
        for (const RepositoryConflict& conflict : conflicts) {
            std::string blobs;
            for (const std::string& blob : conflict.blobs) {
                if (!blobs.empty()) {
                    blobs += ", ";
                }
                blobs += blob;
            }
            content += std::format("<tr><td><code>{}</code></td><td><code>{}</code></td></tr>",
                                   web_ui_escape(conflict.path),
                                   web_ui_escape(blobs));
        }
        content += "</tbody></table>";
    }
    return web_ui_shell("lake — conflicts", repository_name(repository), content, signed_in);
}

std::string
web_ui_artifact(std::string_view repository_id, std::string_view result_id, bool signed_in) {
    const std::string content = std::format(
        "<p class=\"eyebrow\">artifact</p><h1>{}</h1><p class=\"sub\">Repository "
        "<code>{}</code>.</p><div class=\"panel\"><h2>Attestations</h2><p>Artifact result, "
        "provenance, reproducibility, and SBOM rendering arrive with liquid #166.</p></div>",
        web_ui_escape(result_id),
        web_ui_escape(repository_id));
    return web_ui_shell("lake — artifact", repository_id, content, signed_in);
}

std::string web_ui_organizations(const std::vector<control::Organization>& organizations) {
    std::string content = "<p class=\"eyebrow\">identity</p><h1>Organizations</h1>"
                          "<p class=\"sub\">Organizations, teams, members, and delegated "
                          "administration.</p>";
    if (organizations.empty()) {
        content += "<div class=\"panel empty\">No organizations yet. "
                   "<code>POST /v1/create_organization</code> creates one.</div>";
    } else {
        content += "<table><thead><tr><th>Organization</th><th>TAAP identity</th>"
                   "<th>Administration</th><th>Created</th></tr></thead><tbody>";
        for (const auto& organization : organizations) {
            const std::string organization_query = "organization_id=" + organization.id;
            content += std::format(
                "<tr><td>{}</td><td><code>{}</code></td><td>{} · {} · "
                "{} · {}</td><td>{}</td></tr>",
                web_ui_escape(organization.display_name.empty() ? organization.id
                                                                : organization.display_name),
                web_ui_escape(organization.taap_id.empty() ? "not assigned" : organization.taap_id),
                row_link("/teams?" + organization_query, "teams"),
                row_link("/members?" + organization_query, "members"),
                row_link("/invitations?" + organization_query, "invitations"),
                row_link("/organization/access?id=" + organization.id, "effective access"),
                web_ui_escape(organization.created_at));
        }
        content += "</tbody></table>";
    }
    return web_ui_shell("lake — organizations",
                        std::format("{} organizations", organizations.size()),
                        content,
                        true);
}

std::string web_ui_teams(std::string_view organization_id,
                         const std::vector<control::Team>& teams) {
    std::string content = "<p class=\"eyebrow\">organization</p><h1>Teams</h1>"
                          "<p class=\"sub\">Roles on an organization flow to its teams unless "
                          "inheritance is cut.</p>";
    if (teams.empty()) {
        content += "<div class=\"panel empty\">No teams yet. <code>POST "
                   "/v1/create_team</code> "
                   "creates one.</div>";
    } else {
        content += "<table><thead><tr><th>Team</th><th>Name</th><th>Members</"
                   "th><th>Created</th>"
                   "</tr></thead><tbody>";
        for (const auto& team : teams) {
            content += std::format(
                "<tr><td>{}</td><td><code>{}</code></td><td>{} · {}</td><td>{}</td></tr>",
                web_ui_escape(team.display_name.empty() ? team.name : team.display_name),
                web_ui_escape(team.name),
                row_link("/members?organization_id=" + team.organization_id + "&team_id=" + team.id,
                         "members"),
                row_link("/team/access?id=" + team.id, "effective access"),
                web_ui_escape(team.created_at));
        }
        content += "</tbody></table>";
    }
    return web_ui_shell("lake — teams", web_ui_escape(organization_id), content, true);
}

std::string web_ui_memberships(std::string_view organization_id,
                               std::string_view team_id,
                               const std::vector<control::Membership>& memberships) {
    std::string content = "<p class=\"eyebrow\">authorization</p><h1>Members</h1>"
                          "<p class=\"sub\">Every role is backed by an "
                          "org_membership/1 attestation.</p>";
    if (memberships.empty()) {
        content += "<div class=\"panel empty\">No memberships yet. "
                   "<code>POST /v1/create_membership</code> creates one.</div>";
    } else {
        content += "<table><thead><tr><th>Principal</th><th>Type</th><th>Role</th>"
                   "<th>Attestation</th><th>Granted by</th></tr></thead><tbody>";
        for (const auto& membership : memberships) {
            content += std::format("<tr><td><code>{}</code></td><td>{}</td><td><span "
                                   "class=\"badge\">{}</span>"
                                   "</td><td><code>{}</code></td><td><code>{}</code></td></tr>",
                                   web_ui_escape(membership.principal_taap_id),
                                   web_ui_escape(membership.principal_type),
                                   web_ui_escape(membership.role),
                                   web_ui_escape(membership.attestation_digest),
                                   web_ui_escape(membership.granted_by_taap_id));
        }
        content += "</tbody></table>";
    }
    const std::string scope = team_id.empty() ? std::string(organization_id) : std::string(team_id);
    return web_ui_shell("lake — members", scope, content, true);
}

std::string web_ui_invitations(std::string_view organization_id,
                               std::string_view team_id,
                               const std::vector<control::Invitation>& invitations) {
    std::string content = "<p class=\"eyebrow\">organization</p><h1>Invitations</h1>"
                          "<p class=\"sub\">Email and handle invitations expire and become "
                          "attested memberships.</p>";
    if (invitations.empty()) {
        content += "<div class=\"panel empty\">No invitations yet. "
                   "<code>POST /v1/create_invitation</code> creates one.</div>";
    } else {
        content += "<table><thead><tr><th>Invitee</th><th>Role</th><th>Status</"
                   "th><th>Expires</th>"
                   "</tr></thead><tbody>";
        for (const auto& invitation : invitations) {
            content += std::format("<tr><td>{}: {}</td><td><span class=\"badge\">{}</span></td>"
                                   "<td>{}</td><td>{}</td></tr>",
                                   web_ui_escape(invitation.invitee_type),
                                   web_ui_escape(invitation.invitee),
                                   web_ui_escape(invitation.role),
                                   web_ui_escape(invitation.status),
                                   web_ui_escape(invitation.expires_at));
        }
        content += "</tbody></table>";
    }
    const std::string scope = team_id.empty() ? std::string(organization_id) : std::string(team_id);
    return web_ui_shell("lake — invitations", scope, content, true);
}

std::string web_ui_grants(const control::PolicyObject& object,
                          const std::vector<control::Grant>& grants) {
    std::string content = "<p class=\"eyebrow\">policy</p><h1>Direct grants</h1>"
                          "<p class=\"sub\">Attestation-backed roles directly "
                          "attached to this object.</p>";
    if (grants.empty()) {
        content += "<div class=\"panel empty\">No direct grants. <code>POST "
                   "/v1/create_grant</code> "
                   "creates one.</div>";
    } else {
        content += "<table><thead><tr><th>Principal</th><th>Role</th><th>Attestation</th>"
                   "<th>Granted by</th></tr></thead><tbody>";
        for (const auto& grant : grants) {
            content +=
                std::format("<tr><td><code>{}</code></td><td><span class=\"badge\">{}</span></td>"
                            "<td><code>{}</code></td><td><code>{}</code></td></tr>",
                            web_ui_escape(grant.principal_taap_id),
                            web_ui_escape(grant.role),
                            web_ui_escape(grant.attestation_digest),
                            web_ui_escape(grant.granted_by_taap_id));
        }
        content += "</tbody></table>";
    }
    content += std::format("<p>{}</p>",
                           row_link("/access?object_id=" + object.id, "View effective access"));
    return web_ui_shell(
        "lake — grants", object.object_type + ":" + object.external_id, content, true);
}

std::string web_ui_effective_access(const control::PolicyObject& object,
                                    const std::vector<control::EffectiveAccessEntry>& entries) {
    std::string content = "<p class=\"eyebrow\">sharing</p><h1>Effective access</h1>"
                          "<p class=\"sub\">Who can access this object, what they can do, and "
                          "where it comes from.</p>";
    if (entries.empty()) {
        content += "<div class=\"panel empty\">No effective grants.</div>";
    } else {
        content += "<table><thead><tr><th>Principal</th><th>Role</th><th>Actions</th>"
                   "<th>Source</th></tr></thead><tbody>";
        for (const auto& entry : entries) {
            std::string action_list;
            for (const auto& action : entry.actions) {
                if (!action_list.empty()) {
                    action_list += ", ";
                }
                action_list += action;
            }
            content +=
                std::format("<tr><td><code>{}</code></td><td><span class=\"badge\">{}</span></td>"
                            "<td>{}</td><td>{} <code>{}</code></td></tr>",
                            web_ui_escape(entry.principal_taap_id),
                            web_ui_escape(entry.role),
                            web_ui_escape(action_list),
                            entry.inherited ? "inherited from" : "direct on",
                            web_ui_escape(entry.source_object_id));
        }
        content += "</tbody></table>";
    }
    return web_ui_shell(
        "lake — effective access", object.object_type + ":" + object.external_id, content, true);
}

std::string web_ui_search(std::string_view query, const std::vector<search::Hit>& hits) {
    std::string content = "<p class=\"eyebrow\">search</p><h1>Search</h1>"
                          "<p class=\"sub\">Ranked text, quoted substring, or /regex/ search "
                          "across repositories you may read.</p>"
                          "<form class=\"search\" method=\"get\" action=\"/search\"><input "
                          "name=\"query\" value=\"" +
                          web_ui_escape(query) +
                          "\" placeholder=\"Search repositories\" required><button "
                          "type=\"submit\">Search</button></form>";
    if (query.empty()) {
        content += "<div class=\"panel empty\">Enter a query to search published "
                   "changes.</div>";
    } else if (hits.empty()) {
        content += "<div class=\"panel empty\">No readable matches.</div>";
    } else {
        content += "<table><thead><tr><th>Repository</th><th>Path</th><th>Change</"
                   "th><th>Snippet</th></tr></thead><tbody>";
        for (const search::Hit& hit : hits) {
            content +=
                std::format("<tr><td>{}/{}</td><td><code>{}</code></td><td><code>{}</code></td>"
                            "<td>{}</td></tr>",
                            web_ui_escape(hit.customer),
                            web_ui_escape(hit.repository),
                            web_ui_escape(hit.path),
                            web_ui_escape(hit.change),
                            web_ui_escape(hit.snippet));
        }
        content += "</tbody></table>";
    }
    return web_ui_shell(
        "lake — search", query.empty() ? "search" : std::string(query), content, true);
}

std::string web_ui_audit(const std::vector<audit::Event>& events, const audit::Filter& filter) {
    std::string content = "<p class=\"eyebrow\">control plane</p><h1>Audit log</h1><p "
                          "class=\"sub\">Signed, "
                          "hash-linked events. Use the filters to narrow the evidence view.</p>"
                          "<form class=\"filters\" method=\"get\" action=\"/audit\">"
                          "<label>Principal<input name=\"principal\" value=\"" +
                          web_ui_escape(filter.principal) +
                          "\"></label>"
                          "<label>Object<input name=\"object\" value=\"" +
                          web_ui_escape(filter.object) +
                          "\"></label>"
                          "<label>Outcome<input name=\"outcome\" value=\"" +
                          web_ui_escape(filter.outcome) +
                          "\"></label>"
                          "<label>From<input name=\"from\" value=\"" +
                          web_ui_escape(filter.from) +
                          "\"></label>"
                          "<label>To<input name=\"to\" value=\"" +
                          web_ui_escape(filter.to) +
                          "\"></label>"
                          "<button type=\"submit\">Filter</button></form>";
    if (events.empty()) {
        content += "<div class=\"panel empty\">No audit events match these filters.</div>";
    } else {
        content += "<table><thead><tr><th>When</th><th>Principal</th><th>Object</"
                   "th><th>Action</th>"
                   "<th>Outcome</th><th>Digest</th></tr></thead><tbody>";
        for (const audit::Event& event : events) {
            content += std::format("<tr><td>{}</td><td><code>{}</code></td><td><code>{}</code></td>"
                                   "<td>{}</td><td><span class=\"badge\">{}</span></td>"
                                   "<td><code>{}</code></td></tr>",
                                   web_ui_escape(event.occurred_at),
                                   web_ui_escape(event.draft.principal),
                                   web_ui_escape(event.draft.object),
                                   web_ui_escape(event.draft.action),
                                   web_ui_escape(event.draft.outcome),
                                   web_ui_escape(event.digest));
        }
        content += "</tbody></table>";
    }
    content += "<p class=\"sub\">API: <code>GET /v1/audit</code>, <code>GET "
               "/v1/export_audit</code>, "
               "and <code>GET /v1/verify_audit</code>.</p>";
    return web_ui_shell("lake — audit", std::format("{} events", events.size()), content, true);
}

std::string web_ui_sign_up(std::string_view error) {
    std::string content = "<p class=\"eyebrow\">identity</p><h1>Create an account</h1>"
                          "<p class=\"sub\">Bind a password credential to your client-held TAAP "
                          "identity.</p>";
    content += error_markup(error);
    content +=
        R"html(<form class="stack" method="post" action="/identity/sign-up">
<label>Email<input type="email" name="email" autocomplete="email" required></label>
<label>Display name<input type="text" name="display_name" autocomplete="name"></label>
<label>Password<input type="password" name="password" autocomplete="new-password" minlength="12" required></label>
<label>Device name<input type="text" name="device_name" value="browser" required></label>
<label>TAAP/1 identity document<textarea name="identity_document" spellcheck="false" required></textarea></label>
<button class="primary" type="submit">Create account</button></form>
<p class="muted">Already registered? <a href="/sign-in">Sign in</a>.</p>)html";
    return web_ui_shell("lake — sign up", "identity", content, false);
}

std::string web_ui_sign_in(std::string_view error) {
    std::string content = "<p class=\"eyebrow\">identity</p><h1>Sign in</h1>"
                          "<p class=\"sub\">Use a password, passkey, or "
                          "configured social provider.</p>";
    content += error_markup(error);
    content += R"html(<div class="grid"><div class="panel"><h2>Password</h2>
<form class="stack" method="post" action="/identity/sign-in">
<label>Email<input type="email" name="email" autocomplete="username" required></label>
<label>Password<input type="password" name="password" autocomplete="current-password" required></label>
<label>Device name<input type="text" name="device_name" value="browser" required></label>
<button class="primary" type="submit">Sign in</button></form>
<p><a href="/mfa">Verify TOTP</a> · <a href="/passkey-sign-in">Use a passkey</a></p></div>
<div class="panel"><h2>OAuth</h2><form class="stack" method="post" action="/identity/oauth">
<label>Provider<input type="text" name="provider" placeholder="github or google" required></label>
<label>Device name<input type="text" name="device_name" value="browser" required></label>
<label>TAAP/1 identity document for first sign-in<textarea name="identity_document" spellcheck="false"></textarea></label>
<button class="primary" type="submit">Continue to provider</button></form></div></div>
<p class="muted"><a href="/sign-up">Create an account</a> · <a href="/login">Operator sign-in</a></p>)html";
    return web_ui_shell("lake — sign in", "identity", content, false);
}

std::string web_ui_mfa(std::string_view error,
                       std::string_view otpauth_uri,
                       const std::vector<std::string>& recovery_codes,
                       bool signed_in) {
    std::string content = "<p class=\"eyebrow\">identity</p><h1>Multi-factor authentication</h1>";
    content += error_markup(error);
    if (signed_in) {
        content += "<p class=\"sub\">Enroll or replace a TOTP authenticator for password "
                   "sign-in. A new enrollment replaces existing recovery codes.</p>";
        if (!otpauth_uri.empty()) {
            content += std::format(
                "<p class=\"notice\">Save this authenticator URI now: <code>{}</code></p>",
                web_ui_escape(otpauth_uri));
            content += "<h2>Recovery codes</h2><p>Save these now. Each code works once.</p><ul>";
            for (const std::string& code : recovery_codes) {
                content += std::format("<li><code>{}</code></li>", web_ui_escape(code));
            }
            content += "</ul>";
        }
        content +=
            R"html(<form class="stack" method="post" action="/identity/mfa/enroll">
<button class="primary" type="submit">Enroll or replace MFA</button></form>
<h2>Disable MFA</h2>
<form class="stack" method="post" action="/identity/mfa/disable">
<label>Current TOTP or recovery code<input type="text" name="code" autocomplete="one-time-code" required></label>
<button type="submit">Disable MFA</button></form>)html";
    } else {
        content +=
            R"html(<p class="sub">Verify your password with a current TOTP or unused recovery code.</p>
<form class="stack" method="post" action="/identity/mfa/verify">
<label>Email<input type="email" name="email" autocomplete="username" required></label>
<label>Password<input type="password" name="password" autocomplete="current-password" required></label>
<label>TOTP or recovery code<input type="text" name="code" autocomplete="one-time-code" required></label>
<label>Device name<input type="text" name="device_name" value="browser" required></label>
<button class="primary" type="submit">Verify and sign in</button></form>)html";
    }
    return web_ui_shell("lake — MFA", "identity", content, signed_in);
}

std::string web_ui_passkeys(std::string_view error) {
    std::string content = "<p class=\"eyebrow\">identity</p><h1>Passkeys</h1>"
                          "<p class=\"sub\">Register an attestation-less passkey "
                          "for this account.</p>";
    content += error_markup(error);
    content +=
        R"html(<div class="panel"><label>Passkey name <input id="passkey-name" type="text" value="passkey"></label>
<div class="actions"><button class="primary" id="register-passkey" type="button">Register passkey</button><span id="passkey-status" class="muted"></span></div></div>
<script>(()=>{const b=s=>Uint8Array.from(atob(s.replace(/-/g,'+').replace(/_/g,'/')+'==='.slice((s.length+3)%4)),c=>c.charCodeAt(0));const e=a=>btoa(String.fromCharCode(...new Uint8Array(a))).replace(/\+/g,'-').replace(/\//g,'_').replace(/=+$/,'');const q=(u,o)=>fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)}).then(r=>r.json());document.getElementById('register-passkey').onclick=async()=>{const s=document.getElementById('passkey-status');try{const a=await q('/v1/begin_passkey_registration',{});if(!a.ok)throw Error(a.error.message);const o=a.value.public_key,c=o.challenge;o.challenge=b(o.challenge);o.user.id=b(o.user.id);const k=await navigator.credentials.create({publicKey:o});const z=await q('/v1/register_passkey',{challenge:c,client_data_json:e(k.response.clientDataJSON),attestation_object:e(k.response.attestationObject),name:document.getElementById('passkey-name').value});if(!z.ok)throw Error(z.error.message);s.textContent='Passkey registered.'}catch(x){s.textContent=x.message}}})();</script>)html";
    return web_ui_shell("lake — passkeys", "identity", content, true);
}

std::string web_ui_passkey_sign_in(std::string_view error) {
    std::string content = "<p class=\"eyebrow\">identity</p><h1>Sign in with a passkey</h1>";
    content += error_markup(error);
    content +=
        R"html(<div class="panel"><label>Device name <input id="device-name" type="text" value="browser"></label>
<div class="actions"><button class="primary" id="use-passkey" type="button">Use passkey</button><span id="passkey-status" class="muted"></span></div></div>
<script>(()=>{const b=s=>Uint8Array.from(atob(s.replace(/-/g,'+').replace(/_/g,'/')+'==='.slice((s.length+3)%4)),c=>c.charCodeAt(0));const e=a=>btoa(String.fromCharCode(...new Uint8Array(a))).replace(/\+/g,'-').replace(/\//g,'_').replace(/=+$/,'');const q=(u,o)=>fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)}).then(r=>r.json());document.getElementById('use-passkey').onclick=async()=>{const s=document.getElementById('passkey-status');try{const a=await q('/v1/begin_passkey_sign_in',{});if(!a.ok)throw Error(a.error.message);const o=a.value.public_key,c=o.challenge;o.challenge=b(o.challenge);const k=await navigator.credentials.get({publicKey:o});const z=await q('/v1/sign_in_passkey',{challenge:c,credential_id:e(k.rawId),client_data_json:e(k.response.clientDataJSON),authenticator_data:e(k.response.authenticatorData),signature:e(k.response.signature),device_name:document.getElementById('device-name').value});if(!z.ok)throw Error(z.error.message);location='/sessions'}catch(x){s.textContent=x.message}}})();</script>)html";
    return web_ui_shell("lake — passkey sign in", "identity", content, false);
}

std::string web_ui_sessions(const std::vector<control::Session>& sessions,
                            const std::vector<control::Device>& devices,
                            std::string_view error) {
    std::string content = "<p class=\"eyebrow\">identity</p><h1>Sessions and devices</h1>"
                          "<p class=\"sub\">Revocation takes effect on the next request.</p>";
    content += error_markup(error);
    content += "<h2>Sessions</h2><table><thead><tr><th>Kind</th><th>Name</"
               "th><th>Expires</th><th>Status</th><th></th></tr></thead><tbody>";
    for (const control::Session& session : sessions) {
        content += std::format("<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td><form "
                               "class=\"inline\" method=\"post\" "
                               "action=\"/identity/session/revoke\"><input type=\"hidden\" "
                               "name=\"session_id\" value=\"{}\"><button "
                               "type=\"submit\">Revoke</button></form></td></tr>",
                               web_ui_escape(session.kind),
                               web_ui_escape(session.name),
                               web_ui_escape(session.expires_at),
                               state_badge(session.revoked_at.empty()),
                               web_ui_escape(session.id));
    }
    if (sessions.empty())
        content += "<tr><td colspan=\"5\" class=\"empty\">No sessions.</td></tr>";
    content += "</tbody></table><h2>Devices</h2><table><thead><tr><th>Name</"
               "th><th>Created</th><th>Status</th><th></th></tr></thead><tbody>";
    for (const control::Device& device : devices) {
        content += std::format("<tr><td>{}</td><td>{}</td><td>{}</td><td><form class=\"inline\" "
                               "method=\"post\" action=\"/identity/device/revoke\"><input "
                               "type=\"hidden\" name=\"device_id\" value=\"{}\"><button "
                               "type=\"submit\">Revoke</button></form></td></tr>",
                               web_ui_escape(device.name),
                               web_ui_escape(device.created_at),
                               state_badge(device.revoked_at.empty()),
                               web_ui_escape(device.id));
    }
    if (devices.empty())
        content += "<tr><td colspan=\"4\" class=\"empty\">No devices.</td></tr>";
    content += "</tbody></table>";
    return web_ui_shell("lake — sessions", "identity", content, true);
}

std::string web_ui_tokens(const std::vector<control::Session>& sessions,
                          std::string_view issued_secret,
                          std::string_view error) {
    std::string content = "<p class=\"eyebrow\">identity</p><h1>API tokens</h1>"
                          "<p class=\"sub\">Create named, scoped sessions and "
                          "revoke them here.</p>";
    content += error_markup(error);
    if (!issued_secret.empty())
        content += std::format("<p class=\"notice\">Copy this token now: <code>{}</code></p>",
                               web_ui_escape(issued_secret));
    content +=
        R"html(<div class="panel"><form class="stack" method="post" action="/identity/token/create">
<label>Name<input type="text" name="name" required></label><label>Capability scopes, comma separated<input type="text" name="scopes" required></label>
<label>Expiry in seconds<input type="number" name="expires_in_seconds" min="1" value="2592000"></label>
<label><span><input type="checkbox" name="static" value="true"> Static exception</span></label><label>Reason for a static token<input type="text" name="reason"></label>
<button class="primary" type="submit">Create token</button></form></div><h2>Existing tokens</h2>
<table><thead><tr><th>Name</th><th>Kind</th><th>Scopes</th><th>Expires</th><th>Status</th><th></th></tr></thead><tbody>)html";
    std::size_t count = 0;
    for (const control::Session& session : sessions) {
        if (session.kind != "api_token" && session.kind != "static")
            continue;
        ++count;
        std::string scopes;
        for (const std::string& scope : session.scopes) {
            if (!scopes.empty())
                scopes += ", ";
            scopes += scope;
        }
        content +=
            std::format("<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td><form "
                        "class=\"inline\" method=\"post\" "
                        "action=\"/identity/session/revoke\"><input type=\"hidden\" "
                        "name=\"session_id\" value=\"{}\"><input type=\"hidden\" "
                        "name=\"return_to\" value=\"/tokens\"><button "
                        "type=\"submit\">Revoke</button></form></td></tr>",
                        web_ui_escape(session.name),
                        web_ui_escape(session.kind),
                        web_ui_escape(scopes),
                        web_ui_escape(session.expires_at),
                        state_badge(session.revoked_at.empty()),
                        web_ui_escape(session.id));
    }
    if (count == 0)
        content += "<tr><td colspan=\"6\" class=\"empty\">No API tokens.</td></tr>";
    content += "</tbody></table>";
    return web_ui_shell("lake — tokens", "identity", content, true);
}

} // namespace lake::api

#include "lake/api/web_ui.hpp"

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
)css";
}

namespace {

std::string nav(bool signed_in) {
    std::string out =
        "<nav><a href=\"/\">Home</a><a href=\"/customers\">Customers</a><a "
        "href=\"/organizations\">Organizations</a><a href=\"/repositories\">Repositories</a>"
        "<a href=\"/search\">Search</a><a href=\"/audit\">Audit</a>"
        "<a href=\"/v1/openapi.json\">API</a></nav>";
    if (signed_in) {
        out += "<span class=\"session\"><form class=\"inline\" method=\"post\" "
               "action=\"/logout\"><button type=\"submit\">Sign out</button></form></span>";
    } else {
        out += "<span class=\"session\"><a href=\"/login\">Sign in</a></span>";
    }
    return out;
}

std::string row_link(std::string_view href, std::string_view text) {
    return std::format("<a href=\"{}\">{}</a>", web_ui_escape(href), web_ui_escape(text));
}

} // namespace

std::string web_ui_shell(std::string_view title, std::string_view meta, std::string_view content,
                         bool signed_in) {
    return std::format(
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>{}</title>"
        "<link rel=\"stylesheet\" href=\"/style.css\"></head><body><header><a class=\"brand\" "
        "href=\"/\">lake</a>{}"
        "<span class=\"meta\">{}</span></header><main>{}</main></body></html>",
        web_ui_escape(title), nav(signed_in), web_ui_escape(meta), content);
}

std::string web_ui_hello(std::string_view node_name, std::string_view version, bool signed_in) {
    const std::string content =
        std::format("<p class=\"eyebrow\">hello world</p><h1>hello from lake</h1>"
                    "<p class=\"sub\">liquid is the tool. lake is where it collects.</p>"
                    "<div class=\"panel\"><p>This node is <code>{}</code>, running lake "
                    "<code>{}</code>, serving over the same "
                    "Beast2 + Capy/Corosio stack as Zetta.</p><p>Explore <a "
                    "href=\"/customers\">customers</a> and "
                    "<a href=\"/repositories\">repositories</a> (sign in with the admin token), or "
                    "use the API: "
                    "<code>GET /v1/openapi.json</code>, MCP at <code>POST /mcp</code>.</p></div>",
                    web_ui_escape(node_name), web_ui_escape(version));
    return web_ui_shell("lake — hello", std::format("{} · {}", node_name, version), content,
                        signed_in);
}

std::string web_ui_login(std::string_view error) {
    std::string content = "<p class=\"eyebrow\">session</p><h1>Sign in</h1><p class=\"sub\">Paste "
                          "the Lake's admin token.</p>";
    if (!error.empty())
        content += std::format("<p class=\"error\">{}</p>", web_ui_escape(error));
    content += "<form class=\"login\" method=\"post\" action=\"/session\"><input type=\"password\" "
               "name=\"token\" "
               "placeholder=\"admin token\" autocomplete=\"current-password\" required><button "
               "type=\"submit\">Sign in</button></form>";
    return web_ui_shell("lake — sign in", "sign in", content, false);
}

std::string web_ui_customers(const std::vector<control::Customer> &customers) {
    std::string content =
        "<p class=\"eyebrow\">control plane</p><h1>Customers</h1><p class=\"sub\">The tenants. "
        "Each is an individual or an organization and owns its repositories.</p>";
    if (customers.empty()) {
        content += "<div class=\"panel empty\">No customers yet. <code>POST "
                   "/v1/create_customer</code> creates one.</div>";
    } else {
        content += "<table><thead><tr><th>Handle</th><th>Type</th><th>Identity</th><th>Created</"
                   "th></tr></thead><tbody>";
        for (const auto &c : customers) {
            content += std::format(
                "<tr><td>{}</td><td><span "
                "class=\"badge\">{}</span></td><td><code>{}</code></td><td>{}</td></tr>",
                row_link("/repositories?customer_id=" + c.id, c.name),
                web_ui_escape(c.customer_type),
                web_ui_escape(c.customer_type == "individual" ? c.user_id : c.organization_id),
                web_ui_escape(c.created_at));
        }
        content += "</tbody></table>";
    }
    return web_ui_shell("lake — customers", std::format("{} customers", customers.size()), content,
                        true);
}

std::string web_ui_repositories(const std::vector<control::Repository> &repositories,
                                const std::map<std::string, control::Customer> &owners) {
    std::string content =
        "<p class=\"eyebrow\">control plane</p><h1>Repositories</h1><p class=\"sub\">Every "
        "repository, newest first, addressed as &lt;customer&gt;/&lt;name&gt;.</p>";
    if (repositories.empty()) {
        content += "<div class=\"panel empty\">No repositories yet. <code>POST "
                   "/v1/create_repository</code> creates one.</div>";
    } else {
        content += "<table><thead><tr><th>Repository</th><th>Owner</th><th>Visibility</"
                   "th><th>Description</th><th>Created</th></tr></thead><tbody>";
        for (const auto &r : repositories) {
            const auto owner = owners.find(r.customer_id);
            const std::string owner_name =
                owner == owners.end() ? r.customer_id : owner->second.name;
            content +=
                std::format("<tr><td>{}</td><td>{}</td><td><span "
                            "class=\"badge\">{}</span></td><td>{}</td><td>{}</td></tr>",
                            row_link("/repository?id=" + r.id, owner_name + "/" + r.name),
                            row_link("/repositories?customer_id=" + r.customer_id, owner_name),
                            web_ui_escape(r.visibility), web_ui_escape(r.description),
                            web_ui_escape(r.created_at));
        }
        content += "</tbody></table>";
    }
    return web_ui_shell("lake — repositories", std::format("{} repositories", repositories.size()),
                        content, true);
}

std::string web_ui_repository(const control::Repository &repository,
                              const control::Customer *owner) {
    const std::string owner_name = owner ? owner->name : repository.customer_id;
    const std::string content = std::format(
        "<p class=\"eyebrow\">repository</p><h1>{}/{}</h1><p class=\"sub\">{}</p><div "
        "class=\"panel\"><dl>"
        "<dt>id</dt><dd><code>{}</code></dd><dt>owner</dt><dd>{}</dd><dt>visibility</dt><dd><span "
        "class=\"badge\">{}</span></dd>"
        "<dt>created</dt><dd>{}</dd><dt>updated</dt><dd>{}</dd><dt>API</dt><dd><code>GET "
        "/v1/get_repository?id={}</code></dd></dl></div>",
        web_ui_escape(owner_name), web_ui_escape(repository.name),
        web_ui_escape(repository.description.empty() ? "No description." : repository.description),
        web_ui_escape(repository.id),
        row_link("/repositories?customer_id=" + repository.customer_id, owner_name),
        web_ui_escape(repository.visibility), web_ui_escape(repository.created_at),
        web_ui_escape(repository.updated_at), web_ui_escape(repository.id));
    return web_ui_shell("lake — " + owner_name + "/" + repository.name,
                        owner_name + "/" + repository.name, content, true);
}

std::string web_ui_organizations(const std::vector<control::Organization> &organizations) {
    std::string content =
        "<p class=\"eyebrow\">identity</p><h1>Organizations</h1>"
        "<p class=\"sub\">Organizations, teams, members, and delegated administration.</p>";
    if (organizations.empty()) {
        content += "<div class=\"panel empty\">No organizations yet. "
                   "<code>POST /v1/create_organization</code> creates one.</div>";
    } else {
        content += "<table><thead><tr><th>Organization</th><th>TAAP identity</th>"
                   "<th>Administration</th><th>Created</th></tr></thead><tbody>";
        for (const auto &organization : organizations) {
            const std::string organization_query = "organization_id=" + organization.id;
            content += std::format(
                "<tr><td>{}</td><td><code>{}</code></td><td>{} · {} · {}</td><td>{}</td></tr>",
                web_ui_escape(organization.display_name.empty() ? organization.id
                                                                : organization.display_name),
                web_ui_escape(organization.taap_id.empty() ? "not assigned" : organization.taap_id),
                row_link("/teams?" + organization_query, "teams"),
                row_link("/members?" + organization_query, "members"),
                row_link("/invitations?" + organization_query, "invitations"),
                web_ui_escape(organization.created_at));
        }
        content += "</tbody></table>";
    }
    return web_ui_shell("lake — organizations",
                        std::format("{} organizations", organizations.size()), content, true);
}

std::string web_ui_teams(std::string_view organization_id,
                         const std::vector<control::Team> &teams) {
    std::string content = "<p class=\"eyebrow\">organization</p><h1>Teams</h1>"
                          "<p class=\"sub\">Roles on an organization flow to its teams unless "
                          "inheritance is cut.</p>";
    if (teams.empty()) {
        content += "<div class=\"panel empty\">No teams yet. <code>POST /v1/create_team</code> "
                   "creates one.</div>";
    } else {
        content += "<table><thead><tr><th>Team</th><th>Name</th><th>Members</th><th>Created</th>"
                   "</tr></thead><tbody>";
        for (const auto &team : teams) {
            content += std::format(
                "<tr><td>{}</td><td><code>{}</code></td><td>{}</td><td>{}</td></tr>",
                web_ui_escape(team.display_name.empty() ? team.name : team.display_name),
                web_ui_escape(team.name),
                row_link("/members?organization_id=" + team.organization_id + "&team_id=" + team.id,
                         "members"),
                web_ui_escape(team.created_at));
        }
        content += "</tbody></table>";
    }
    return web_ui_shell("lake — teams", web_ui_escape(organization_id), content, true);
}

std::string web_ui_memberships(std::string_view organization_id, std::string_view team_id,
                               const std::vector<control::Membership> &memberships) {
    std::string content =
        "<p class=\"eyebrow\">authorization</p><h1>Members</h1>"
        "<p class=\"sub\">Every role is backed by an org_membership/1 attestation.</p>";
    if (memberships.empty()) {
        content += "<div class=\"panel empty\">No memberships yet. "
                   "<code>POST /v1/create_membership</code> creates one.</div>";
    } else {
        content += "<table><thead><tr><th>Principal</th><th>Type</th><th>Role</th>"
                   "<th>Attestation</th><th>Granted by</th></tr></thead><tbody>";
        for (const auto &membership : memberships) {
            content += std::format(
                "<tr><td><code>{}</code></td><td>{}</td><td><span class=\"badge\">{}</span>"
                "</td><td><code>{}</code></td><td><code>{}</code></td></tr>",
                web_ui_escape(membership.principal_taap_id),
                web_ui_escape(membership.principal_type), web_ui_escape(membership.role),
                web_ui_escape(membership.attestation_digest),
                web_ui_escape(membership.granted_by_taap_id));
        }
        content += "</tbody></table>";
    }
    const std::string scope = team_id.empty() ? std::string(organization_id) : std::string(team_id);
    return web_ui_shell("lake — members", scope, content, true);
}

std::string web_ui_invitations(std::string_view organization_id, std::string_view team_id,
                               const std::vector<control::Invitation> &invitations) {
    std::string content =
        "<p class=\"eyebrow\">organization</p><h1>Invitations</h1>"
        "<p class=\"sub\">Email and handle invitations expire and become attested memberships.</p>";
    if (invitations.empty()) {
        content += "<div class=\"panel empty\">No invitations yet. "
                   "<code>POST /v1/create_invitation</code> creates one.</div>";
    } else {
        content += "<table><thead><tr><th>Invitee</th><th>Role</th><th>Status</th><th>Expires</th>"
                   "</tr></thead><tbody>";
        for (const auto &invitation : invitations) {
            content +=
                std::format("<tr><td>{}: {}</td><td><span class=\"badge\">{}</span></td>"
                            "<td>{}</td><td>{}</td></tr>",
                            web_ui_escape(invitation.invitee_type),
                            web_ui_escape(invitation.invitee), web_ui_escape(invitation.role),
                            web_ui_escape(invitation.status), web_ui_escape(invitation.expires_at));
        }
        content += "</tbody></table>";
    }
    const std::string scope = team_id.empty() ? std::string(organization_id) : std::string(team_id);
    return web_ui_shell("lake — invitations", scope, content, true);
}

std::string web_ui_grants(const control::PolicyObject &object,
                          const std::vector<control::Grant> &grants) {
    std::string content =
        "<p class=\"eyebrow\">policy</p><h1>Direct grants</h1>"
        "<p class=\"sub\">Attestation-backed roles directly attached to this object.</p>";
    if (grants.empty()) {
        content +=
            "<div class=\"panel empty\">No direct grants. <code>POST /v1/create_grant</code> "
            "creates one.</div>";
    } else {
        content += "<table><thead><tr><th>Principal</th><th>Role</th><th>Attestation</th>"
                   "<th>Granted by</th></tr></thead><tbody>";
        for (const auto &grant : grants) {
            content += std::format(
                "<tr><td><code>{}</code></td><td><span class=\"badge\">{}</span></td>"
                "<td><code>{}</code></td><td><code>{}</code></td></tr>",
                web_ui_escape(grant.principal_taap_id), web_ui_escape(grant.role),
                web_ui_escape(grant.attestation_digest), web_ui_escape(grant.granted_by_taap_id));
        }
        content += "</tbody></table>";
    }
    content += std::format("<p>{}</p>",
                           row_link("/access?object_id=" + object.id, "View effective access"));
    return web_ui_shell("lake — grants", object.object_type + ":" + object.external_id, content,
                        true);
}

std::string web_ui_effective_access(const control::PolicyObject &object,
                                    const std::vector<control::EffectiveAccessEntry> &entries) {
    std::string content = "<p class=\"eyebrow\">sharing</p><h1>Effective access</h1>"
                          "<p class=\"sub\">Who can access this object, what they can do, and "
                          "where it comes from.</p>";
    if (entries.empty()) {
        content += "<div class=\"panel empty\">No effective grants.</div>";
    } else {
        content += "<table><thead><tr><th>Principal</th><th>Role</th><th>Actions</th>"
                   "<th>Source</th></tr></thead><tbody>";
        for (const auto &entry : entries) {
            std::string action_list;
            for (const auto &action : entry.actions) {
                if (!action_list.empty()) {
                    action_list += ", ";
                }
                action_list += action;
            }
            content += std::format(
                "<tr><td><code>{}</code></td><td><span class=\"badge\">{}</span></td>"
                "<td>{}</td><td>{} <code>{}</code></td></tr>",
                web_ui_escape(entry.principal_taap_id), web_ui_escape(entry.role),
                web_ui_escape(action_list), entry.inherited ? "inherited from" : "direct on",
                web_ui_escape(entry.source_object_id));
        }
        content += "</tbody></table>";
    }
    return web_ui_shell("lake — effective access", object.object_type + ":" + object.external_id,
                        content, true);
}

std::string web_ui_search(std::string_view query, const std::vector<search::Hit> &hits) {
    std::string content =
        "<p class=\"eyebrow\">search</p><h1>Search</h1>"
        "<p class=\"sub\">Ranked text, quoted substring, or /regex/ search across repositories you may read.</p>"
        "<form class=\"search\" method=\"get\" action=\"/search\"><input name=\"query\" value=\"" +
        web_ui_escape(query) +
        "\" placeholder=\"Search repositories\" required><button type=\"submit\">Search</button></form>";
    if (query.empty()) {
        content += "<div class=\"panel empty\">Enter a query to search published changes.</div>";
    } else if (hits.empty()) {
        content += "<div class=\"panel empty\">No readable matches.</div>";
    } else {
        content += "<table><thead><tr><th>Repository</th><th>Path</th><th>Change</th><th>Snippet</th>"
                   "</tr></thead><tbody>";
        for (const search::Hit &hit : hits) {
            content += std::format(
                "<tr><td>{}/{}</td><td><code>{}</code></td><td><code>{}</code></td><td>{}</td></tr>",
                web_ui_escape(hit.customer), web_ui_escape(hit.repository), web_ui_escape(hit.path),
                web_ui_escape(hit.change), web_ui_escape(hit.snippet));
        }
        content += "</tbody></table>";
    }
    return web_ui_shell("lake — search", query.empty() ? "search" : std::string(query), content, true);
}

std::string web_ui_audit(const std::vector<audit::Event> &events, const audit::Filter &filter) {
    std::string content =
        "<p class=\"eyebrow\">control plane</p><h1>Audit log</h1><p class=\"sub\">Signed, "
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
        content += "<table><thead><tr><th>When</th><th>Principal</th><th>Object</th><th>Action</th>"
                   "<th>Outcome</th><th>Digest</th></tr></thead><tbody>";
        for (const audit::Event &event : events) {
            content += std::format("<tr><td>{}</td><td><code>{}</code></td><td><code>{}</code></td>"
                                   "<td>{}</td><td><span class=\"badge\">{}</span></td>"
                                   "<td><code>{}</code></td></tr>",
                                   web_ui_escape(event.occurred_at),
                                   web_ui_escape(event.draft.principal),
                                   web_ui_escape(event.draft.object),
                                   web_ui_escape(event.draft.action),
                                   web_ui_escape(event.draft.outcome), web_ui_escape(event.digest));
        }
        content += "</tbody></table>";
    }
    content += "<p class=\"sub\">API: <code>GET /v1/audit</code>, <code>GET /v1/export_audit</code>, "
               "and <code>GET /v1/verify_audit</code>.</p>";
    return web_ui_shell("lake — audit", std::format("{} events", events.size()), content, true);
}

} // namespace lake::api

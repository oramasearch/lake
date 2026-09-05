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
    static constexpr std::string_view tokens =
#include "web_ui_tokens.inc"
        ;
    return std::string(tokens) +
           R"css(:root{--ink:#1b1f24;--muted:#5b6570;--line:#d9dee3;--paper:#fafaf8;--accent:#0b6e99;--accent-ink:#fff}
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
.image-preview{display:block;max-width:100%;max-height:40rem;margin:auto}.table-wrap{overflow:auto}.file-preview{max-height:40rem;overflow:auto}
.diff-entry{margin-top:1rem}.diff-entry h2{font-size:1rem;margin:0 0 .75rem}.diff-entry .badge{margin-left:.35rem}.diff-hunk{margin:.75rem 0 0}.diff-hunk span{display:block}.diff-remove{color:#8b1a1a}.diff-add{color:#176b35}
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
                      "href=\"/applications\">Applications</a><a "
                      "href=\"/repositories\">Repositories</a><a href=\"/search\">Search</a>"
                      "<a href=\"/metering\">Metering</a>"
                      "<a href=\"/secret-mounts\">Secrets</a>"
                      "<a href=\"/transparency\">Transparency</a><a href=\"/audit\">Audit</a>"
                      "<a href=\"/sso\">SSO</a><a href=\"/exports\">Exports</a>";
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

std::string joined(const std::vector<std::string>& values) {
    std::string output;
    for (const std::string& value : values) {
        if (!output.empty()) {
            output += ", ";
        }
        output += value;
    }
    return output;
}

std::string mode_text(const std::optional<std::uint32_t>& mode) {
    return mode ? std::format("{:o}", *mode) : "—";
}

std::string parent_path(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? std::string{} : std::string(path.substr(0, slash));
}

bool hidden_reformatting_entry(const RepositoryDiffEntry& entry) {
    return entry.changes.size() == 1 && entry.changes.front() == "modified" && entry.line_hunks &&
           entry.line_hunks->empty();
}

std::string theme_color(const control::Application& application,
                        std::string_view key,
                        std::string_view fallback) {
    const std::string value = application.theme.value(std::string(key), std::string(fallback));
    if ((value.size() == 4 || value.size() == 7) && value.front() == '#' &&
        std::ranges::all_of(value.substr(1), [](unsigned char character) {
            return std::isxdigit(character) != 0;
        })) {
        return value;
    }
    return std::string(fallback);
}

std::string application_shell(const control::Application& application,
                              std::string_view title,
                              std::string_view content) {
    const std::string accent = theme_color(application, "accent_color", "#0b6e99");
    const std::string background = theme_color(application, "background_color", "#fafaf8");
    std::string html =
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>" +
        web_ui_escape(title) +
        "</title><link rel=\"stylesheet\" href=\"/style.css\"><style>:root{--accent:" + accent +
        ";--paper:" + background +
        "}</style></head><body><header><span "
        "class=\"brand\">" +
        web_ui_escape(application.name) + "</span></header><main>" + std::string(content) +
        "</main></body></html>";
    return html;
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
    const std::string drive_ref = refs.empty() ? "main" : refs.front().name;
    content += std::format(
        "<p>{} · {} · {} · {} · {}</p></div>",
        row_link("/drive?repository_id=" + repository.id + "&ref=" + url_component(drive_ref),
                 "Drive"),
        row_link("/changes?repository_id=" + repository.id, "Change log"),
        row_link("/artifacts?repository_id=" + repository.id, "Artifacts"),
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
                          const std::vector<RepositoryDiffEntry>& differences,
                          bool signed_in) {
    std::string parents;
    for (const std::string& parent : change.parents) {
        if (!parents.empty()) {
            parents += ", ";
        }
        parents += parent;
    }
    std::string content = std::format(
        "<p class=\"eyebrow\">change</p><h1>{}</h1><p class=\"sub\">{}</p>"
        "<div class=\"panel\"><dl><dt>author</dt><dd><code>{}</code></dd><dt>created</dt>"
        "<dd>{}</dd><dt>parents</dt><dd><code>{}</code></dd></dl></div>",
        web_ui_escape(change.id),
        web_ui_escape(change.message),
        web_ui_escape(change.author),
        web_ui_escape(change.occurred_at),
        web_ui_escape(parents));
    std::size_t visible = 0;
    for (const RepositoryDiffEntry& entry : differences) {
        if (hidden_reformatting_entry(entry)) {
            continue;
        }
        ++visible;
        std::string badges;
        for (const std::string& status : entry.changes) {
            badges += std::format("<span class=\"badge\">{}</span>", web_ui_escape(status));
        }
        content += std::format("<section class=\"panel diff-entry\"><h2><code>{}</code>{}</h2><dl>"
                               "<dt>kind</dt><dd>{}</dd><dt>old path</dt><dd><code>{}</code></dd>"
                               "<dt>mode</dt><dd><code>{} → {}</code></dd><dt>blobs</dt>"
                               "<dd><code>{} → {}</code></dd></dl>",
                               web_ui_escape(entry.path),
                               badges,
                               web_ui_escape(entry.kind),
                               web_ui_escape(entry.old_path.empty() ? "—" : entry.old_path),
                               mode_text(entry.old_mode),
                               mode_text(entry.new_mode),
                               web_ui_escape(entry.old_blob.empty() ? "—" : entry.old_blob),
                               web_ui_escape(entry.new_blob.empty() ? "—" : entry.new_blob));
        if (!entry.old_conflicts.empty() || !entry.new_conflicts.empty()) {
            content += std::format("<div class=\"notice\"><strong>Conflict values</strong><br>old: "
                                   "<code>{}</code><br>new: <code>{}</code></div>",
                                   web_ui_escape(joined(entry.old_conflicts)),
                                   web_ui_escape(joined(entry.new_conflicts)));
        }
        if (entry.line_hunks) {
            for (const RepositoryLineHunk& hunk : *entry.line_hunks) {
                content += std::format("<pre class=\"diff-hunk\"><span>@@ -{},{} +{},{} @@</span>",
                                       hunk.old_line,
                                       hunk.old_count,
                                       hunk.new_line,
                                       hunk.new_count);
                for (const std::string& line : hunk.removed_lines) {
                    content +=
                        std::format("<span class=\"diff-remove\">-{}</span>", web_ui_escape(line));
                }
                for (const std::string& line : hunk.added_lines) {
                    content +=
                        std::format("<span class=\"diff-add\">+{}</span>", web_ui_escape(line));
                }
                content += "</pre>";
            }
        }
        content += "</section>";
    }
    if (visible == 0) {
        content += "<div class=\"panel empty diff-entry\">No changes to display.</div>";
    }
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

std::string web_ui_artifacts(const control::Repository& repository,
                             const RepositoryArtifactPage& page,
                             const RepositoryArtifactFilter& filter,
                             bool signed_in) {
    std::string content = std::format(
        "<p class=\"eyebrow\">repository artifacts</p><h1>Build results</h1>"
        "<p class=\"sub\">{}</p><form class=\"filters\" method=\"get\" action=\"/artifacts\" "
        "aria-label=\"Filter build results\"><input type=\"hidden\" name=\"repository_id\" "
        "value=\"{}\"><label>Action<input name=\"action\" value=\"{}\"></label>"
        "<label>Output path<input name=\"output\" value=\"{}\"></label>"
        "<label>Content digest<input name=\"content\" value=\"{}\"></label>"
        "<label>Page size<input name=\"limit\" type=\"number\" min=\"1\" max=\"1000\" "
        "value=\"{}\"></label><button type=\"submit\">Filter</button></form>",
        web_ui_escape(repository_name(repository)),
        web_ui_escape(repository.id),
        web_ui_escape(filter.action),
        web_ui_escape(filter.output),
        web_ui_escape(filter.content),
        filter.limit);
    if (page.items.empty()) {
        content += "<div class=\"panel empty\">No build results match these filters.</div>";
    } else {
        content += "<table><thead><tr><th>Result</th><th>Action</th><th>Outputs</th>"
                   "<th>Cache</th><th>Observed</th></tr></thead><tbody>";
        for (const RepositoryArtifact& artifact : page.items) {
            std::vector<std::string> paths;
            paths.reserve(artifact.outputs.size());
            for (const RepositoryArtifactOutput& output : artifact.outputs) {
                paths.push_back(output.path);
            }
            content +=
                std::format("<tr><td>{}</td><td><code>{}</code></td><td>{}</td><td><span "
                            "class=\"badge\">{}</span></td><td>{}</td></tr>",
                            row_link("/artifact?repository_id=" + repository.id +
                                         "&id=" + url_component(artifact.id),
                                     artifact.id),
                            web_ui_escape(artifact.action),
                            web_ui_escape(joined(paths)),
                            web_ui_escape(artifact.divergent ? "divergent" : artifact.cache_status),
                            web_ui_escape(artifact.observed_at));
        }
        content += "</tbody></table>";
    }
    if (!page.done && !page.next_cursor.empty()) {
        std::string next = "/artifacts?repository_id=" + repository.id +
                           "&cursor=" + url_component(page.next_cursor) +
                           "&limit=" + std::to_string(filter.limit);
        if (!filter.action.empty()) {
            next += "&action=" + url_component(filter.action);
        }
        if (!filter.output.empty()) {
            next += "&output=" + url_component(filter.output);
        }
        if (!filter.content.empty()) {
            next += "&content=" + url_component(filter.content);
        }
        content += std::format("<p>{}</p>", row_link(next, "Next page"));
    }
    content +=
        std::format("<p>{}</p>", row_link("/repository?id=" + repository.id, "Back to repository"));
    return web_ui_shell("lake — artifacts", repository_name(repository), content, signed_in);
}

std::string web_ui_artifact(const control::Repository& repository,
                            const RepositoryArtifact& artifact,
                            bool signed_in) {
    std::string content =
        std::format("<p class=\"eyebrow\">artifact result</p><h1>{}</h1><p class=\"sub\">{}</p>"
                    "<div class=\"panel\"><dl><dt>action</dt><dd><code>{}</code></dd>"
                    "<dt>exit</dt><dd>{}</dd><dt>cache</dt><dd><span class=\"badge\">{}</span></dd>"
                    "<dt>divergent</dt><dd>{}</dd><dt>worker</dt><dd>{}</dd>"
                    "<dt>observed</dt><dd>{}</dd><dt>started</dt><dd>{}</dd>"
                    "<dt>finished</dt><dd>{}</dd></dl></div>",
                    web_ui_escape(artifact.id),
                    web_ui_escape(repository_name(repository)),
                    web_ui_escape(artifact.action),
                    artifact.exit_code,
                    web_ui_escape(artifact.cache_status),
                    artifact.divergent ? "yes" : "no",
                    web_ui_escape(artifact.worker),
                    web_ui_escape(artifact.observed_at),
                    web_ui_escape(artifact.started_at.empty() ? "—" : artifact.started_at),
                    web_ui_escape(artifact.finished_at.empty() ? "—" : artifact.finished_at));
    content += "<section class=\"panel diff-entry\"><h2>Outputs</h2>";
    if (artifact.outputs.empty()) {
        content += "<p class=\"muted\">No outputs.</p>";
    } else {
        content += "<table><thead><tr><th>Path</th><th>Mode</th><th>Blob</th></tr></thead><tbody>";
        for (const RepositoryArtifactOutput& output : artifact.outputs) {
            content += std::format("<tr><td><code>{}</code></td><td><code>{:o}</code></td>"
                                   "<td><code>{}</code></td></tr>",
                                   web_ui_escape(output.path),
                                   output.mode,
                                   web_ui_escape(output.blob));
        }
        content += "</tbody></table>";
    }
    content += "</section>";
    content += std::format("<section class=\"panel diff-entry\"><h2>Timing</h2><dl>"
                           "<dt>queued</dt><dd>{} ms</dd><dt>sandbox</dt><dd>{} ms</dd>"
                           "<dt>execute</dt><dd>{} ms</dd><dt>hash</dt><dd>{} ms</dd>"
                           "<dt>store</dt><dd>{} ms</dd></dl></section>",
                           artifact.timing.queued_ms,
                           artifact.timing.sandbox_ms,
                           artifact.timing.exec_ms,
                           artifact.timing.hash_ms,
                           artifact.timing.store_ms);
    content += "<section class=\"panel diff-entry\"><h2>Attestations</h2>";
    if (artifact.attestations.empty()) {
        content += "<p class=\"muted\">No attestations.</p>";
    } else {
        content += "<table><thead><tr><th>Profile</th><th>Subject</th><th>Issuer</th>"
                   "<th>Verification</th></tr></thead><tbody>";
        for (const RepositoryArtifactAttestation& attestation : artifact.attestations) {
            const std::string verification =
                attestation.verified
                    ? "verified"
                    : "unverified" +
                          (attestation.reason.empty() ? std::string() : ": " + attestation.reason);
            content +=
                std::format("<tr><td><span class=\"badge\">{}</span><br><code>{}</code></td>"
                            "<td><code>{}</code></td><td><code>{}</code></td><td>{}</td></tr>",
                            web_ui_escape(attestation.profile),
                            web_ui_escape(attestation.id),
                            web_ui_escape(attestation.subject),
                            web_ui_escape(attestation.issuer),
                            web_ui_escape(verification));
        }
        content += "</tbody></table>";
    }
    content += "</section><section class=\"panel diff-entry\"><h2>Trace</h2>";
    if (!artifact.trace) {
        content += "<p class=\"muted\">No retained trace contains this result.</p>";
    } else {
        content +=
            std::format("<dl><dt>trace</dt><dd><code>{}</code></dd><dt>wall time</dt><dd>{} ms</dd>"
                        "<dt>actions</dt><dd>{}</dd><dt>dependencies</dt><dd>{}</dd>"
                        "<dt>cache</dt><dd>{} hits, {} misses</dd></dl>",
                        web_ui_escape(artifact.trace->id),
                        artifact.trace->wall_ms,
                        artifact.trace->actions,
                        artifact.trace->dependencies,
                        artifact.trace->cache_hits,
                        artifact.trace->cache_misses);
    }
    content += std::format("</section><p>{} · {}</p>",
                           row_link("/artifacts?repository_id=" + repository.id, "All artifacts"),
                           row_link("/repository?id=" + repository.id, "Back to repository"));
    return web_ui_shell("lake — artifact", repository_name(repository), content, signed_in);
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
                   "<th>Administration</th><th>SSO</th><th>Created</th></tr></thead><tbody>";
        for (const auto& organization : organizations) {
            const std::string organization_query = "organization_id=" + organization.id;
            content += std::format(
                "<tr><td>{}</td><td><code>{}</code></td><td>{} · {} · "
                "{} · {}</td><td>{}</td><td>{}</td></tr>",
                web_ui_escape(organization.display_name.empty() ? organization.id
                                                                : organization.display_name),
                web_ui_escape(organization.taap_id.empty() ? "not assigned" : organization.taap_id),
                row_link("/teams?" + organization_query, "teams"),
                row_link("/members?" + organization_query, "members"),
                row_link("/invitations?" + organization_query, "invitations"),
                row_link("/organization/access?id=" + organization.id, "effective access"),
                row_link("/sso?" + organization_query, "SSO"),
                web_ui_escape(organization.created_at));
        }
        content += "</tbody></table>";
    }
    return web_ui_shell("lake — organizations",
                        std::format("{} organizations", organizations.size()),
                        content,
                        true);
}

std::string web_ui_sso(std::string_view organization_id,
                       const std::vector<control::SsoProvider>& providers,
                       const std::vector<control::FederationIssuer>& issuers,
                       const std::vector<control::ProvisioningLog>& logs) {
    std::string content = "<p class=\"eyebrow\">organization identity</p><h1>Enterprise SSO</h1>"
                          "<p class=\"sub\">Provider configuration, federation policy, and SCIM "
                          "provisioning evidence.</p>";
    content += "<div class=\"panel\"><p>Configure providers with <code>POST "
               "/v1/register_sso_provider</code>, then validate them with <code>POST "
               "/v1/test_sso_provider</code>. Provider secrets and SCIM tokens are never shown "
               "here.</p></div>";
    content += "<h2>Providers</h2>";
    if (providers.empty()) {
        content += "<div class=\"panel empty\">No SSO providers are configured.</div>";
    } else {
        content += "<table><thead><tr><th>Name</th><th>Protocol</th><th>Issuer</th>"
                   "<th>JIT</th><th>Tested</th></tr></thead><tbody>";
        for (const control::SsoProvider& provider : providers) {
            content += std::format(
                "<tr><td><code>{}</code></td><td>{}</td><td><code>{}</code></td>"
                "<td>{}</td><td>{}</td></tr>",
                web_ui_escape(provider.name),
                web_ui_escape(provider.protocol),
                web_ui_escape(provider.issuer),
                provider.jit_provisioning ? "enabled" : "disabled",
                web_ui_escape(provider.tested_at.empty() ? "not tested" : provider.tested_at));
        }
        content += "</tbody></table>";
    }
    content += "<h2>Allowed issuer claims</h2>";
    if (issuers.empty()) {
        content += "<div class=\"panel empty\">No issuer claims are allowed yet.</div>";
    } else {
        content += "<table><thead><tr><th>Issuer</th><th>Claim</th></tr></thead><tbody>";
        for (const control::FederationIssuer& issuer : issuers) {
            content += std::format("<tr><td><code>{}</code></td><td><code>{}</code></td></tr>",
                                   web_ui_escape(issuer.issuer),
                                   web_ui_escape(issuer.claim));
        }
        content += "</tbody></table>";
    }
    content += "<h2>Provisioning log</h2>";
    if (logs.empty()) {
        content +=
            "<div class=\"panel empty\">No SCIM provisioning events for this provider.</div>";
    } else {
        content += "<table><thead><tr><th>When</th><th>Resource</th><th>Operation</th>"
                   "<th>External id</th><th>Outcome</th></tr></thead><tbody>";
        for (const control::ProvisioningLog& log : logs) {
            content += std::format("<tr><td>{}</td><td>{}</td><td>{}</td><td><code>{}</code></td>"
                                   "<td>{}</td></tr>",
                                   web_ui_escape(log.created_at),
                                   web_ui_escape(log.resource),
                                   web_ui_escape(log.operation),
                                   web_ui_escape(log.external_id),
                                   web_ui_escape(log.outcome));
        }
        content += "</tbody></table>";
    }
    return web_ui_shell(
        "lake — enterprise SSO", "organization " + std::string(organization_id), content, true);
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

std::string web_ui_search(const search::Query& query, const search::QueryResult& result) {
    std::string content = "<p class=\"eyebrow\">search</p><h1>Search</h1>"
                          "<p class=\"sub\">Ranked text, quoted substring, or /regex/ search "
                          "across repositories you may read. A tag uses its immutable snapshot; "
                          "a change or complete history uses scan mode.</p>"
                          "<form class=\"filters\" method=\"get\" action=\"/search\">"
                          "<label>Query<input name=\"query\" value=\"" +
                          web_ui_escape(query.text) +
                          "\" placeholder=\"Search repositories\" required></label>"
                          "<label>Customer<input name=\"customer\" value=\"" +
                          web_ui_escape(query.customer) +
                          "\"></label><label>Repository<input name=\"repository\" value=\"" +
                          web_ui_escape(query.repository) +
                          "\"></label><label>Branch or tag<input name=\"ref\" value=\"" +
                          web_ui_escape(query.ref) +
                          "\"></label><label>Exact change<input name=\"change\" value=\"" +
                          web_ui_escape(query.change) +
                          "\"></label><label><input type=\"checkbox\" name=\"history\" " +
                          (query.history ? "checked " : "") +
                          "value=\"1\"> Scan complete history</label><button type=\"submit\">"
                          "Search</button></form>";
    if (query.text.empty()) {
        content += "<div class=\"panel empty\">Enter a query to search published "
                   "changes.</div>";
    } else {
        content += std::format("<p>Query mode: <span class=\"badge\">{}</span>",
                               web_ui_escape(search::to_string(result.mode)));
        if (result.mode == search::QueryMode::Scan) {
            content += std::format(" · {} changes examined", result.scanned_changes);
        }
        content += ".</p>";
    }
    if (!query.text.empty() && result.items.empty()) {
        content += "<div class=\"panel empty\">No readable matches.</div>";
    } else if (!result.items.empty()) {
        content += "<table><thead><tr><th>Repository</th><th>Path</th><th>Change</"
                   "th><th>Snippet</th></tr></thead><tbody>";
        for (const search::Hit& hit : result.items) {
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
    return web_ui_shell("lake — search", query.text.empty() ? "search" : query.text, content, true);
}

std::string web_ui_metering(const nlohmann::json& usage) {
    const std::string customer_id = usage.value("customer_id", "");
    std::string content =
        "<p class=\"eyebrow\">economics</p><h1>Metering</h1>"
        "<p class=\"sub\">Hourly unique stored bytes, transfer, build compute, and configured "
        "pricing.</p><form class=\"filters\" method=\"get\" action=\"/metering\">"
        "<label>Customer<input name=\"customer_id\" value=\"" +
        web_ui_escape(customer_id) +
        "\" required></label><button type=\"submit\">Show usage</button></form>";
    if (customer_id.empty()) {
        content += "<div class=\"panel empty\">Choose a customer.</div>";
        return web_ui_shell("lake — metering", "metering", content, true);
    }
    if (usage.value("over_quota", false)) {
        content +=
            std::format("<p class=\"notice\">{}</p>", web_ui_escape(usage.value("warning", "")));
    }
    content += std::format("<div class=\"panel\"><dl><dt>Storage quota</dt><dd>{} bytes</dd>"
                           "<dt>Charge</dt><dd>{} micro-{}</dd></dl></div>",
                           usage.value("quota_bytes", std::uint64_t{0}),
                           usage.value("cost_micros", std::uint64_t{0}),
                           web_ui_escape(usage.value("currency", "USD")));
    const auto items = usage.value("items", nlohmann::json::array());
    if (items.empty()) {
        content += "<div class=\"panel empty\">No hourly usage.</div>";
    } else {
        content += "<table><thead><tr><th>Hour</th><th>Stored</th><th>In</th><th>Out</th>"
                   "<th>Compute</th><th>Charge</th></tr></thead><tbody>";
        for (const nlohmann::json& row : items) {
            content += std::format(
                "<tr><td><code>{}</code></td><td>{}</td><td>{}</td><td>{}</td><td>{}s</td>"
                "<td>{}</td></tr>",
                web_ui_escape(row.value("hour", "")),
                row.value("stored_bytes", std::uint64_t{0}),
                row.value("transfer_in_bytes", std::uint64_t{0}),
                row.value("transfer_out_bytes", std::uint64_t{0}),
                row.value("compute_seconds", std::uint64_t{0}),
                row.value("cost_micros", std::uint64_t{0}));
        }
        content += "</tbody></table>";
    }
    return web_ui_shell("lake — metering", customer_id, content, true);
}

std::string web_ui_audit(const std::vector<audit::Event>& events,
                         const audit::Filter& filter,
                         const nlohmann::json& log_configuration) {
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
    if (!log_configuration.empty()) {
        content += std::format(
            "<div class=\"panel\"><h2>Transparency log</h2><p class=\"sub\">Effective mode: "
            "<code>{}</code>; witness quorum: {} of {}{}.</p><p class=\"sub\">API: "
            "<code>GET /v1/transparency_log_configuration</code>.</p></div>",
            web_ui_escape(log_configuration.value("mode", "")),
            log_configuration.value("witness_threshold", std::size_t{0}),
            log_configuration.value("witnesses", nlohmann::json::array()).size(),
            log_configuration.contains("aggregator_endpoint") ? "; aggregation endpoint configured"
                                                              : "");
    }
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

std::string web_ui_transparency(std::string_view identity, const nlohmann::json& scan) {
    std::string content =
        "<p class=\"eyebrow\">public evidence</p><h1>Transparency monitor</h1>"
        "<p class=\"sub\">Scan checkpoint-bound bulk tiles for every TAAP attestation that "
        "names an identity.</p><form class=\"search\" method=\"get\" action=\"/transparency\">"
        "<input name=\"identity\" aria-label=\"TAAP identity\" placeholder=\"taap:1:…\" value=\"" +
        web_ui_escape(identity) + "\"><button type=\"submit\">Scan</button></form>";
    if (identity.empty()) {
        content += "<div class=\"panel empty\">Enter a TAAP identity to verify the current "
                   "checkpoint and scan its immutable tiles.</div>";
    } else {
        content += std::format(
            "<div class=\"panel\"><dl><dt>Checkpoint</dt><dd>{} entries</dd>"
            "<dt>Root</dt><dd><code>{}</code></dd><dt>Tiles read</dt><dd>{}</dd>"
            "<dt>Scan</dt><dd>{:.3f} ms; verified</dd><dt>Matches</dt><dd>{}</dd></dl></div>",
            scan.value("tree_size", std::uint64_t{0}),
            web_ui_escape(scan.value("root_hash", "")),
            scan.value("tiles_scanned", std::uint64_t{0}),
            scan.value("duration_ms", 0.0),
            scan.value("matches", nlohmann::json::array()).size());
        const nlohmann::json matches = scan.value("matches", nlohmann::json::array());
        if (matches.empty()) {
            content += "<div class=\"panel empty\">No attestations name this identity.</div>";
        } else {
            content += "<div class=\"table-wrap\"><table><thead><tr><th>Index</th><th>Profile</th>"
                       "<th>Issuer</th><th>Subject</th></tr></thead><tbody>";
            for (const nlohmann::json& match : matches) {
                content +=
                    std::format("<tr><td>{}</td><td><code>{}</code></td><td><code>{}</code></td>"
                                "<td><code>{}</code></td></tr>",
                                match.value("index", std::uint64_t{0}),
                                web_ui_escape(match.value("profile", "")),
                                web_ui_escape(match.value("issuer", "")),
                                web_ui_escape(match.value("subject", "")));
            }
            content += "</tbody></table></div>";
        }
    }
    content += "<p class=\"sub\">API: <code>GET /v1/transparency_identity_scan</code>, "
               "<code>GET /v1/transparency_tile</code>, and "
               "<code>GET /v1/transparency_checkpoint</code>.</p>";
    return web_ui_shell("lake — transparency", "public monitor", content, false);
}

std::string web_ui_secret_mounts(const std::vector<control::SecretMount>& mounts) {
    std::string content =
        "<p class=\"eyebrow\">control plane</p><h1>Secret mounts</h1>"
        "<p class=\"sub\">Organization-scoped static secrets and named encryption keys. "
        "Key material remains inside lake.</p>";
    if (mounts.empty()) {
        content += "<div class=\"panel empty\">No readable mounts. <code>POST "
                   "/v1/create_secret_mount</code> creates one.</div>";
    } else {
        content += "<table><thead><tr><th>Name</th><th>Organization</th><th>Customer</th>"
                   "<th>Description</th><th>Created</th><th></th></tr></thead><tbody>";
        for (const control::SecretMount& mount : mounts) {
            const std::string query = "mount_id=" + mount.id;
            content += std::format("<tr><td>{}</td><td><code>{}</code></td>"
                                   "<td><code>{}</code></td><td>{}</td><td>{}</td><td>{} · "
                                   "{}</td></tr>",
                                   web_ui_escape(mount.name),
                                   web_ui_escape(mount.organization_id),
                                   web_ui_escape(mount.customer_id),
                                   web_ui_escape(mount.description),
                                   web_ui_escape(mount.created_at),
                                   row_link("/secrets?" + query, "secrets"),
                                   row_link("/encryption-keys?" + query, "keys"));
        }
        content += "</tbody></table>";
    }
    return web_ui_shell(
        "lake — secret mounts", std::format("{} mounts", mounts.size()), content, true);
}

std::string web_ui_secrets(const control::SecretMount& mount,
                           std::string_view name,
                           const control::SecretMetadata* metadata) {
    std::string content =
        "<p class=\"eyebrow\">static secrets</p><h1>" + web_ui_escape(mount.name) +
        "</h1><p class=\"sub\">Inspect version metadata without rendering secret values. "
        "Writes, reads, deletion and destruction use the capability API.</p>"
        "<form class=\"filters\" method=\"get\" action=\"/secrets\">"
        "<input type=\"hidden\" name=\"mount_id\" value=\"" +
        web_ui_escape(mount.id) + "\"><label>Secret name<input name=\"name\" value=\"" +
        web_ui_escape(name) +
        "\" required></label><button type=\"submit\">Inspect history</button></form>";
    if (name.empty()) {
        content += "<div class=\"panel empty\">Enter a secret name to inspect its metadata.</div>";
    } else if (metadata == nullptr) {
        content += "<div class=\"panel empty\">No metadata is available.</div>";
    } else {
        content += std::format("<div class=\"panel\"><dl><dt>Name</dt><dd><code>{}</code></dd>"
                               "<dt>Current version</dt><dd>{}</dd><dt>Custom metadata</dt>"
                               "<dd><code>{}</code></dd></dl></div>",
                               web_ui_escape(metadata->name),
                               metadata->current_version,
                               web_ui_escape(metadata->custom_metadata.dump()));
        content += "<h2>Version history</h2><table><thead><tr><th>Version</th><th>Created</th>"
                   "<th>Deleted</th><th>Destroyed</th></tr></thead><tbody>";
        for (const control::SecretVersionMetadata& version : metadata->versions) {
            content += std::format(
                "<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>",
                version.version,
                web_ui_escape(version.created_at),
                web_ui_escape(version.deleted_at.empty() ? "—" : version.deleted_at),
                web_ui_escape(version.destroyed_at.empty() ? "—" : version.destroyed_at));
        }
        content += "</tbody></table>";
    }
    content += "<p class=\"sub\"><a href=\"/encryption-keys?mount_id=" + web_ui_escape(mount.id) +
               "\">Named encryption keys</a> · API: <code>POST /v1/put_secret</code>, "
               "<code>GET /v1/read_secret</code>, <code>GET /v1/secret_metadata</code>.</p>";
    return web_ui_shell("lake — secrets", mount.name, content, true);
}

std::string web_ui_encryption_keys(const control::SecretMount& mount,
                                   const std::vector<control::EncryptionKey>& keys) {
    std::string content = "<p class=\"eyebrow\">encryption service</p><h1>" +
                          web_ui_escape(mount.name) +
                          " keys</h1><p class=\"sub\">Named, versioned keys for encryption, "
                          "signing, HMAC and derivation. Key material is never returned.</p>";
    if (keys.empty()) {
        content += "<div class=\"panel empty\">No keys yet. <code>POST "
                   "/v1/create_encryption_key</code> creates one.</div>";
    } else {
        content += "<table><thead><tr><th>Name</th><th>Current version</th><th>Derived from</th>"
                   "<th>Created</th><th>Updated</th></tr></thead><tbody>";
        for (const control::EncryptionKey& key : keys) {
            content += std::format("<tr><td><code>{}</code></td><td>{}</td><td><code>{}</code>"
                                   "</td><td>{}</td><td>{}</td></tr>",
                                   web_ui_escape(key.name),
                                   key.current_version,
                                   web_ui_escape(key.derived_from.empty() ? "—" : key.derived_from),
                                   web_ui_escape(key.created_at),
                                   web_ui_escape(key.updated_at));
        }
        content += "</tbody></table>";
    }
    content += "<p class=\"sub\"><a href=\"/secrets?mount_id=" + web_ui_escape(mount.id) +
               "\">Static secrets</a> · API: <code>POST /v1/encrypt</code>, "
               "<code>POST /v1/decrypt</code>, <code>POST /v1/rewrap</code>.</p>";
    return web_ui_shell(
        "lake — encryption keys", std::format("{} keys", keys.size()), content, true);
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

std::string web_ui_application_sign_up(const control::Application& application,
                                       std::string_view error) {
    const std::string base = "/apps/" + web_ui_escape(application.id);
    std::string content = "<p class=\"eyebrow\">secure sign-up</p><h1>Create your account</h1>";
    content += error_markup(error);
    content += "<form class=\"stack\" method=\"post\" action=\"" + base +
               "/identity/sign-up\"><label>Email<input type=\"email\" name=\"email\" "
               "autocomplete=\"email\" required></label><label>Display name<input type=\"text\" "
               "name=\"display_name\" autocomplete=\"name\"></label><label>Password<input "
               "type=\"password\" name=\"password\" autocomplete=\"new-password\" minlength=\"12\" "
               "required></label><label>Device name<input type=\"text\" name=\"device_name\" "
               "value=\"browser\" required></label><label>TAAP/1 identity document<textarea "
               "name=\"identity_document\" spellcheck=\"false\" required></textarea></label>"
               "<button class=\"primary\" type=\"submit\">Create account</button></form><p "
               "class=\"muted\">Already registered? <a href=\"" +
               base + "/sign-in\">Sign in</a>.</p>";
    return application_shell(application, application.name + " — sign up", content);
}

std::string web_ui_application_sign_in(const control::Application& application,
                                       std::string_view error) {
    const std::string base = "/apps/" + web_ui_escape(application.id);
    std::string content = "<p class=\"eyebrow\">secure sign-in</p><h1>Sign in</h1>";
    content += error_markup(error);
    content +=
        "<form class=\"stack\" method=\"post\" action=\"" + base +
        "/identity/sign-in\"><label>Email<input type=\"email\" name=\"email\" "
        "autocomplete=\"username\" required></label><label>Password<input type=\"password\" "
        "name=\"password\" autocomplete=\"current-password\" required></label><label>Device "
        "name<input type=\"text\" name=\"device_name\" value=\"browser\" required></label>"
        "<button class=\"primary\" type=\"submit\">Sign in</button></form><p><a href=\"" +
        base + "/mfa\">Verify TOTP</a> · <a href=\"" + base +
        "/passkey-sign-in\">Use a passkey</a></p><h2>OAuth</h2><form class=\"stack\" "
        "method=\"post\" "
        "action=\"" +
        base +
        "/identity/oauth\"><label>Provider<input type=\"text\" name=\"provider\" required></label>"
        "<label>Device name<input type=\"text\" name=\"device_name\" value=\"browser\" "
        "required></label>"
        "<label>TAAP/1 identity document for first sign-in<textarea "
        "name=\"identity_document\"></textarea>"
        "</label><button class=\"primary\" type=\"submit\">Continue to provider</button></form><p "
        "class=\"muted\"><a href=\"" +
        base + "/sign-up\">Create an account</a></p>";
    return application_shell(application, application.name + " — sign in", content);
}

std::string web_ui_application_mfa(const control::Application& application,
                                   std::string_view error) {
    const std::string base = "/apps/" + web_ui_escape(application.id);
    std::string content = "<p class=\"eyebrow\">secure sign-in</p><h1>Verify MFA</h1>";
    content += error_markup(error);
    content +=
        "<form class=\"stack\" method=\"post\" action=\"" + base +
        "/identity/mfa/verify\"><label>Email<input type=\"email\" name=\"email\" "
        "autocomplete=\"username\" required></label><label>Password<input type=\"password\" "
        "name=\"password\" autocomplete=\"current-password\" required></label><label>TOTP or "
        "recovery code<input type=\"text\" name=\"code\" autocomplete=\"one-time-code\" "
        "required></label><label>Device name<input type=\"text\" name=\"device_name\" "
        "value=\"browser\" required></label><button class=\"primary\" type=\"submit\">Verify "
        "and sign in</button></form>";
    return application_shell(application, application.name + " — MFA", content);
}

std::string web_ui_application_passkey_sign_in(const control::Application& application,
                                               std::string_view error) {
    const std::string base = "/apps/" + web_ui_escape(application.id);
    const std::string redirect = application.redirect_uris.empty()
                                     ? base + "/sign-in"
                                     : web_ui_escape(application.redirect_uris.front());
    std::string content = "<p class=\"eyebrow\">secure sign-in</p><h1>Use a passkey</h1>";
    content += error_markup(error);
    content +=
        "<div class=\"panel\"><label>Device name <input id=\"device-name\" type=\"text\" "
        "value=\"browser\"></label><a id=\"signed-in\" href=\"" +
        redirect +
        "\" hidden></a><div class=\"actions\"><button class=\"primary\" "
        "id=\"use-passkey\" type=\"button\">Use passkey</button><span id=\"passkey-status\" "
        "class=\"muted\"></span></div></div><script>(()=>{const b=s=>Uint8Array.from(atob(s"
        ".replace(/-/g,'+').replace(/_/g,'/')+'==='.slice((s.length+3)%4)),c=>c.charCodeAt(0));"
        "const e=a=>btoa(String.fromCharCode(...new Uint8Array(a))).replace(/\\+/g,'-').replace("
        "/\\//g,'_').replace(/=+$/,'');const q=(u,o)=>fetch(u,{method:'POST',headers:{'Content-"
        "Type':'application/"
        "json'},body:JSON.stringify(o)}).then(r=>r.json());document.getElementById("
        "'use-passkey').onclick=async()=>{const "
        "s=document.getElementById('passkey-status');try{const "
        "a=await q('" +
        base +
        "/identity/passkey/begin',{});if(!a.ok)throw Error(a.error.message);const "
        "o=a.value.public_key,"
        "c=o.challenge;o.challenge=b(o.challenge);const k=await "
        "navigator.credentials.get({publicKey:o}"
        ");const z=await q('" +
        base +
        "/identity/passkey/"
        "sign-in',{challenge:c,credential_id:e(k.rawId),client_data_json:e(k.response"
        ".clientDataJSON),authenticator_data:e(k.response.authenticatorData),signature:e(k."
        "response."
        "signature),device_name:document.getElementById('device-name').value});if(!z.ok)throw "
        "Error(z."
        "error.message);const d=document.getElementById('signed-in').href;location=d+(d.includes("
        "'#')?'&':'#')+'access_token='+encodeURIComponent(z.value.access_token)+'&token_type="
        "Bearer&expires_in='+z.value.expires_in}catch(x){s.textContent=x.message}}})();</script>";
    return application_shell(application, application.name + " — passkey", content);
}

std::string web_ui_applications(const std::vector<control::Application>& applications,
                                const std::vector<control::Customer>& customers,
                                std::string_view error) {
    std::string content = "<p class=\"eyebrow\">auth as a service</p><h1>Applications</h1>";
    content += error_markup(error);
    content +=
        "<div class=\"grid\"><div><table><thead><tr><th>Name</th><th>Pool</th><th>Domain</th>"
        "</tr></thead><tbody>";
    for (const auto& application : applications) {
        content += "<tr><td><a href=\"/application?id=" + web_ui_escape(application.id) + "\">" +
                   web_ui_escape(application.name) + "</a></td><td><code>" +
                   web_ui_escape(application.pool_id) + "</code></td><td>" +
                   web_ui_escape(application.custom_sign_in_domain.empty()
                                     ? "lake domain"
                                     : application.custom_sign_in_domain) +
                   "</td></tr>";
    }
    if (applications.empty()) {
        content += "<tr><td colspan=\"3\" class=\"empty\">No applications.</td></tr>";
    }
    content += "</tbody></table></div><div class=\"panel\"><h2>Register application</h2><form "
               "class=\"stack\" method=\"post\" action=\"/applications\"><label>Customer<select "
               "name=\"customer_id\" required>";
    for (const auto& customer : customers) {
        content += "<option value=\"" + web_ui_escape(customer.id) + "\">" +
                   web_ui_escape(customer.name) + "</option>";
    }
    content +=
        "</select></label><label>Name<input name=\"name\" required></label><label>Allowed origins "
        "(one per line)<textarea name=\"allowed_origins\" "
        "required></textarea></label><label>Redirect "
        "URIs (one per line)<textarea name=\"redirect_uris\"></textarea></label><label>Custom "
        "sign-in "
        "domain<input name=\"custom_sign_in_domain\"></label><label>Accent color<input "
        "name=\"accent_color\" value=\"#0b6e99\"></label><button class=\"primary\" type=\"submit\">"
        "Register</button></form></div></div>";
    return web_ui_shell(
        "lake — applications", std::format("{} applications", applications.size()), content, true);
}

std::string web_ui_application(const control::Application& application) {
    const std::string id = web_ui_escape(application.id);
    std::string content =
        "<p class=\"eyebrow\">application</p><h1>" + web_ui_escape(application.name) +
        "</h1><div class=\"panel\"><dl><dt>ID</dt><dd><code>" + id +
        "</code></dd><dt>Pool</dt><dd><code>" + web_ui_escape(application.pool_id) +
        "</code></dd><dt>Origins</dt><dd>" + web_ui_escape(joined(application.allowed_origins)) +
        "</dd><dt>Redirect URIs</dt><dd>" + web_ui_escape(joined(application.redirect_uris)) +
        "</dd><dt>Custom domain</dt><dd>" +
        web_ui_escape(
            application.custom_sign_in_domain.empty() ? "—" : application.custom_sign_in_domain) +
        "</dd></dl></div><p><a href=\"/application/users?id=" + id +
        "\">Users</a> · <a href=\"/application/organizations?id=" + id +
        "\">Organizations</a> · <a href=\"/application/events?id=" + id +
        "\">Webhook events</a> · <a href=\"/apps/" + id + "/sign-in\">Hosted sign-in</a></p>";
    return web_ui_shell("lake — " + application.name, application.pool_id, content, true);
}

std::string web_ui_application_users(const control::Application& application,
                                     const std::vector<control::ApplicationUser>& users) {
    std::string content = "<p class=\"eyebrow\">application users</p><h1>" +
                          web_ui_escape(application.name) +
                          " users</h1><table><thead><tr><th>Email</th>"
                          "<th>Name</th><th>Status</th><th></th></tr></thead><tbody>";
    for (const auto& user : users) {
        content +=
            "<tr><td>" + web_ui_escape(user.email) + "</td><td>" +
            web_ui_escape(user.display_name) + "</td><td>" + web_ui_escape(user.status) +
            "</td><td><form class=\"inline\" method=\"post\" action=\"/application/users/suspend\">"
            "<input type=\"hidden\" name=\"application_id\" value=\"" +
            web_ui_escape(application.id) + "\"><input type=\"hidden\" name=\"user_id\" value=\"" +
            web_ui_escape(user.user_id) +
            "\"><button type=\"submit\">Suspend</button></form></td></tr>";
    }
    content += "</tbody></table>";
    return web_ui_shell(
        "lake — application users", std::format("{} users", users.size()), content, true);
}

std::string web_ui_application_organizations(
    const control::Application& application,
    const std::vector<control::ApplicationOrganization>& organizations) {
    std::string content =
        "<p class=\"eyebrow\">application organizations</p><h1>" + web_ui_escape(application.name) +
        " organizations</h1><table><thead><tr><th>Name</th><th>TAAP identity</th><th>Updated</th>"
        "<th></th></tr></thead><tbody>";
    for (const auto& organization : organizations) {
        content +=
            "<tr><td>" + web_ui_escape(organization.name) + "</td><td><code>" +
            web_ui_escape(organization.taap_id) + "</code></td><td>" +
            web_ui_escape(organization.updated_at) +
            "</td><td><form class=\"inline\" method=\"post\" action=\"/application/organizations/"
            "delete\"><input type=\"hidden\" name=\"application_id\" value=\"" +
            web_ui_escape(application.id) +
            "\"><input type=\"hidden\" name=\"organization_id\" value=\"" +
            web_ui_escape(organization.id) +
            "\"><button type=\"submit\">Delete</button></form></td></tr>";
    }
    content +=
        "</tbody></table><div class=\"panel\"><h2>Create organization</h2><form class=\"stack\" "
        "method=\"post\" action=\"/application/organizations\"><input type=\"hidden\" "
        "name=\"application_id\" value=\"" +
        web_ui_escape(application.id) +
        "\"><label>Name<input name=\"name\" required></label><label>TAAP identity<input "
        "name=\"taap_id\" required></label><button class=\"primary\" type=\"submit\">Create"
        "</button></form></div>";
    return web_ui_shell("lake — application organizations",
                        std::format("{} organizations", organizations.size()),
                        content,
                        true);
}

std::string web_ui_application_events(const control::Application& application,
                                      const std::vector<control::ApplicationWebhookEvent>& events,
                                      std::string_view webhook_secret) {
    std::string content = "<p class=\"eyebrow\">application webhooks</p><h1>" +
                          web_ui_escape(application.name) + " event log</h1>";
    if (!webhook_secret.empty()) {
        content += "<p class=\"notice\">Save this webhook secret now: <code>" +
                   web_ui_escape(webhook_secret) + "</code></p>";
    }
    content += "<table><thead><tr><th>Event</th><th>ID</th><th>Created</th></tr></thead><tbody>";
    for (const auto& event : events) {
        content += "<tr><td>" + web_ui_escape(event.event) + "</td><td><code>" +
                   web_ui_escape(event.id) + "</code></td><td>" + web_ui_escape(event.created_at) +
                   "</td></tr>";
    }
    content +=
        "</tbody></table><div class=\"panel\"><h2>Create webhook</h2><form class=\"stack\" "
        "method=\"post\" action=\"/application/webhooks\"><input type=\"hidden\" "
        "name=\"application_id\" value=\"" +
        web_ui_escape(application.id) +
        "\"><label>HTTPS URL<input type=\"url\" name=\"url\" required></label><label>Events "
        "(comma-separated)<input name=\"events\" value=\"user.created,user.updated,user.deleted,"
        "organization.created,organization.updated,organization.deleted,session.started,mfa."
        "changed\" "
        "required></label><button class=\"primary\" type=\"submit\">Create</button></form></div>";
    return web_ui_shell(
        "lake — application events", std::format("{} events", events.size()), content, true);
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

std::string web_ui_exports(const control::Repository& repository,
                           const std::vector<control::ForgeRecord>& exports) {
    std::string content = std::format(
        "<p class=\"eyebrow\">portability</p><h1>Repository export</h1><p class=\"sub\">"
        "Download a signed archive for <code>{}</code>.</p><div class=\"panel\"><form "
        "class=\"stack\" method=\"get\" action=\"/exports/download\"><input type=\"hidden\" "
        "name=\"repository_id\" value=\"{}\"><label><span><input type=\"checkbox\" "
        "name=\"include_git\" value=\"true\"> Include Git projection</span></label><button "
        "class=\"primary\" type=\"submit\">Request and download export</button></form></div>",
        web_ui_escape(repository_name(repository)),
        web_ui_escape(repository.id));
    content += "<h2>Prior exports</h2><table><thead><tr><th>Created</th><th>Git</th>"
               "<th>Manifest</th></tr></thead><tbody>";
    for (const control::ForgeRecord& record : exports) {
        content +=
            std::format("<tr><td>{}</td><td>{}</td><td><code>{}</code></td></tr>",
                        web_ui_escape(record.created_at),
                        record.value.value("include_git", false) ? "requested" : "omitted",
                        web_ui_escape(record.value.value("manifest", nlohmann::json::object())
                                          .value("format", "lake_export_manifest/1")));
    }
    if (exports.empty()) {
        content += "<tr><td colspan=\"3\" class=\"empty\">No exports requested.</td></tr>";
    }
    content += "</tbody></table>";
    return web_ui_shell("lake — exports", repository_name(repository), content, true);
}

std::string web_ui_drive_folder(const control::Repository& repository,
                                std::string_view ref,
                                std::string_view path,
                                const std::vector<RepositoryTreeEntry>& entries,
                                bool signed_in) {
    const std::string base = "/drive?repository_id=" + url_component(repository.id) +
                             "&ref=" + url_component(ref) + "&path=";
    std::string content = std::format(
        "<p class=\"eyebrow\">drive</p><h1>{}</h1><p class=\"sub\">Folder <code>{}</code> "
        "at <code>{}</code>. The same content remains a liquid repository.</p><div "
        "class=\"actions\"><a href=\"/drive/history?repository_id={}&ref={}&path={}\">History"
        "</a><a href=\"/drive/share?repository_id={}&ref={}&path={}\">Share</a><a "
        "href=\"/conflicts?repository_id={}\">Conflicts</a></div>",
        web_ui_escape(repository_name(repository)),
        web_ui_escape(path.empty() ? "/" : path),
        web_ui_escape(ref),
        url_component(repository.id),
        url_component(ref),
        url_component(path),
        url_component(repository.id),
        url_component(ref),
        url_component(path),
        url_component(repository.id));
    content +=
        "<table><thead><tr><th>Name</th><th>Kind</th><th>Size identity</th></tr></thead><tbody>";
    for (const RepositoryTreeEntry& entry : entries) {
        const std::size_t slash = entry.path.find_last_of('/');
        const std::string name =
            slash == std::string::npos ? entry.path : entry.path.substr(slash + 1);
        const std::string href =
            entry.kind == "tree"
                ? base + url_component(entry.path)
                : "/drive/preview?repository_id=" + url_component(repository.id) +
                      "&ref=" + url_component(ref) + "&path=" + url_component(entry.path);
        content += std::format("<tr><td>{}</td><td><span class=\"badge\">{}</span></td>"
                               "<td><code>{}</code></td></tr>",
                               row_link(href, name),
                               web_ui_escape(entry.kind),
                               web_ui_escape(entry.blob));
    }
    if (entries.empty()) {
        content += "<tr><td colspan=\"3\" class=\"empty\">This folder is empty.</td></tr>";
    }
    content += "</tbody></table>";
    return web_ui_shell("lake — drive", repository_name(repository), content, signed_in);
}

std::string web_ui_drive_preview(const control::Repository& repository,
                                 std::string_view ref,
                                 const RepositoryFile& file,
                                 const DrivePreview& preview,
                                 bool signed_in) {
    const std::string parent = parent_path(file.path);
    const std::string content = std::format(
        "<p class=\"eyebrow\">drive preview</p><h1>{}</h1><p class=\"sub\"><span "
        "class=\"badge\">{}</span> {} bytes · <a href=\"/drive?repository_id={}&ref={}&path={}\">"
        "Back to folder</a></p><div class=\"panel\">{}</div>",
        web_ui_escape(file.path),
        web_ui_escape(preview.format),
        file.size,
        url_component(repository.id),
        url_component(ref),
        url_component(parent),
        preview.html);
    return web_ui_shell("lake — preview", repository_name(repository), content, signed_in);
}

std::string web_ui_drive_history(const control::Repository& repository,
                                 std::string_view ref,
                                 std::string_view path,
                                 const std::vector<nlohmann::json>& changes,
                                 bool signed_in) {
    std::string content = std::format(
        "<p class=\"eyebrow\">drive history</p><h1>{}</h1><p class=\"sub\">Every version is "
        "a history query. Restoring writes a new change and does not rewrite prior versions.</p>",
        web_ui_escape(path));
    content += "<table><thead><tr><th>When</th><th>Change</th><th>What changed</th><th></th>"
               "</tr></thead><tbody>";
    for (const nlohmann::json& change : changes) {
        std::vector<std::string> kinds = change.value("changes", std::vector<std::string>{});
        content += std::format(
            "<tr><td>{}</td><td><code>{}</code><br>{}</td><td>{}</td><td><form "
            "class=\"inline\" method=\"post\" action=\"/drive/history/restore\"><input "
            "type=\"hidden\" name=\"repository_id\" value=\"{}\"><input type=\"hidden\" "
            "name=\"ref\" value=\"{}\"><input type=\"hidden\" name=\"path\" value=\"{}\">"
            "<input type=\"hidden\" name=\"change_id\" value=\"{}\"><button type=\"submit\">"
            "Restore</button></form></td></tr>",
            web_ui_escape(change.value("occurred_at", "")),
            web_ui_escape(change.value("id", "")),
            web_ui_escape(change.value("message", "")),
            web_ui_escape(joined(kinds)),
            web_ui_escape(repository.id),
            web_ui_escape(ref),
            web_ui_escape(path),
            web_ui_escape(change.value("id", "")));
    }
    if (changes.empty()) {
        content += "<tr><td colspan=\"4\" class=\"empty\">No changes touch this path.</td></tr>";
    }
    content += "</tbody></table>";
    return web_ui_shell("lake — drive history", repository_name(repository), content, signed_in);
}

std::string web_ui_drive_share(const control::Repository& repository,
                               std::string_view ref,
                               std::string_view path,
                               const std::vector<control::EffectiveAccessEntry>& access,
                               const std::vector<nlohmann::json>& links,
                               std::string_view created_link,
                               std::string_view error) {
    std::string content = std::format(
        "<p class=\"eyebrow\">drive sharing</p><h1>Share {}</h1><p class=\"sub\">Before sharing, "
        "review who can access this folder, what they can do, whether access is inherited, and "
        "whether the recipient gets a live object, a continuing link, or an independent copy.</p>",
        web_ui_escape(path));
    content += error_markup(error);
    if (!created_link.empty()) {
        content += "<p class=\"notice\">Copy this link now: <code>" + web_ui_escape(created_link) +
                   "</code></p>";
    }
    content += "<h2>Effective access</h2><table><thead><tr><th>Who</th><th>Role</th><th>Can do</th>"
               "<th>Source</th></tr></thead><tbody>";
    for (const control::EffectiveAccessEntry& entry : access) {
        content += std::format("<tr><td><code>{}</code></td><td>{}</td><td>{}</td><td>{}</td></tr>",
                               web_ui_escape(entry.principal_taap_id),
                               web_ui_escape(entry.role),
                               web_ui_escape(joined(entry.actions)),
                               entry.inherited ? "inherited" : "direct");
    }
    if (access.empty()) {
        content += "<tr><td colspan=\"4\" class=\"empty\">No grants.</td></tr>";
    }
    content += "</tbody></table><div class=\"grid\"><section class=\"panel\"><h2>Live object</h2>"
               "<p>Updates, moves, and revocation continue to apply.</p><form class=\"stack\" "
               "method=\"post\" action=\"/drive/share/direct\">";
    const std::string hidden = std::format(
        "<input type=\"hidden\" name=\"repository_id\" value=\"{}\"><input type=\"hidden\" "
        "name=\"ref\" value=\"{}\"><input type=\"hidden\" name=\"path\" value=\"{}\">",
        web_ui_escape(repository.id),
        web_ui_escape(ref),
        web_ui_escape(path));
    content += hidden;
    content +=
        "<label>Recipient TAAP identity<input name=\"principal_taap_id\" required></label>"
        "<label>Role<select name=\"role\"><option>read</option><option>write</option>"
        "<option>share</option></select></label><label>Attestation digest<input "
        "name=\"attestation_digest\" required></label><button class=\"primary\" "
        "type=\"submit\">Share live folder</button></form></section><section class=\"panel\">"
        "<h2>Link</h2><p>Scope, role, expiry, and revocation continue to apply.</p><form "
        "class=\"stack\" method=\"post\" action=\"/drive/share/link\">" +
        hidden +
        "<label>Role<select name=\"role\"><option>read</option><option>write</option>"
        "</select></label><label>Expires at (UTC)<input name=\"expires_at\" "
        "placeholder=\"2030-01-01T00:00:00Z\" required></label><button class=\"primary\" "
        "type=\"submit\">Create link</button></form></section><section class=\"panel\">"
        "<h2>Independent copy</h2><p>Downloaded bytes stop being a live object. They cannot "
        "be updated or recalled by changing access here.</p></section></div>";
    content += "<h2>Share links</h2><table><thead><tr><th>Role</th><th>Expires</th><th>Status</th>"
               "<th></th></tr></thead><tbody>";
    for (const nlohmann::json& link : links) {
        content += std::format(
            "<tr><td>{}</td><td>{}</td><td>{}</td><td><form class=\"inline\" method=\"post\" "
            "action=\"/drive/share/revoke\"><input type=\"hidden\" name=\"link_id\" "
            "value=\"{}\"><input type=\"hidden\" name=\"repository_id\" value=\"{}\"><input "
            "type=\"hidden\" name=\"ref\" value=\"{}\"><input type=\"hidden\" name=\"path\" "
            "value=\"{}\"><button type=\"submit\">Revoke</button></form></td></tr>",
            web_ui_escape(link.value("role", "")),
            web_ui_escape(link.value("expires_at", "")),
            link.value("active", false) ? "active" : "expired or revoked",
            web_ui_escape(link.value("id", "")),
            web_ui_escape(repository.id),
            web_ui_escape(ref),
            web_ui_escape(path));
    }
    if (links.empty()) {
        content += "<tr><td colspan=\"4\" class=\"empty\">No links.</td></tr>";
    }
    content += "</tbody></table>";
    return web_ui_shell("lake — drive sharing", repository_name(repository), content, true);
}

std::string web_ui_drive_link(std::string_view path,
                              const std::vector<RepositoryTreeEntry>& entries,
                              std::string_view error) {
    std::string content = "<p class=\"eyebrow\">shared drive folder</p><h1>" + web_ui_escape(path) +
                          "</h1>" + error_markup(error);
    content += "<table><thead><tr><th>Name</th><th>Kind</th></tr></thead><tbody>";
    for (const RepositoryTreeEntry& entry : entries) {
        content += std::format("<tr><td>{}</td><td>{}</td></tr>",
                               web_ui_escape(entry.path),
                               web_ui_escape(entry.kind));
    }
    if (entries.empty() && error.empty()) {
        content += "<tr><td colspan=\"2\" class=\"empty\">This folder is empty.</td></tr>";
    }
    content += "</tbody></table>";
    return web_ui_shell("lake — shared drive folder", "share link", content, false);
}

std::string web_ui_model(const nlohmann::json& model, bool signed_in) {
    const nlohmann::json& storage = model.at("storage");
    std::string content =
        "<p class=\"eyebrow\">model</p><h1>" + web_ui_escape(model.value("modelId", "")) +
        "</h1><p class=\"sub\">Revision <code>" + web_ui_escape(model.value("sha", "")) +
        "</code></p><div class=\"grid\"><section class=\"panel\"><h2>Storage</h2><dl>";
    content += "<dt>Logical bytes</dt><dd>" +
               std::to_string(storage.value("logical_bytes", std::uint64_t{0})) + "</dd>";
    content += "<dt>Unique bytes</dt><dd>" +
               std::to_string(storage.value("unique_bytes", std::uint64_t{0})) + "</dd>";
    content += "<dt>Deduplicated bytes</dt><dd>" +
               std::to_string(storage.value("deduplicated_bytes", std::uint64_t{0})) + "</dd>";
    content += "<dt>Deduplication ratio</dt><dd>" +
               std::to_string(storage.value("dedup_ratio", 0.0)) + "</dd>";
    content += "</dl></section><section class=\"panel\"><h2>Training provenance</h2>";
    const nlohmann::json& provenance = model.at("training_provenance");
    if (provenance.is_object()) {
        content += provenance.value("verified", false)
                       ? "<p><span class=\"badge\">verified</span></p>"
                       : "<p><span class=\"badge\">not verified</span></p>";
        content += "<dl><dt>Dataset</dt><dd><code>" +
                   web_ui_escape(provenance.value("dataset_repository_id", "")) + "</code></dd>";
        content += "<dt>Dataset revision</dt><dd><code>" +
                   web_ui_escape(provenance.value("dataset_revision", "")) + "</code></dd>";
        content += "<dt>Code revision</dt><dd><code>" +
                   web_ui_escape(provenance.value("code_revision", "")) + "</code></dd>";
        content += "<dt>Run</dt><dd><code>" + web_ui_escape(provenance.value("invocation_id", "")) +
                   "</code></dd>";
        content += "<dt>Runner</dt><dd><code>" + web_ui_escape(provenance.value("runner", "")) +
                   "</code></dd>";
        content += "<dt>Builder</dt><dd><code>" + web_ui_escape(provenance.value("builder", "")) +
                   "</code></dd>";
        content += "<dt>Environment digest</dt><dd><code>" +
                   web_ui_escape(provenance.value("environment_digest", "")) + "</code></dd>";
        content += "<dt>Parameters</dt><dd><code>" +
                   web_ui_escape(provenance.value("parameters", nlohmann::json::array()).dump()) +
                   "</code></dd>";
        content += "<dt>Environment</dt><dd><code>" +
                   web_ui_escape(provenance.value("environment", nlohmann::json::object()).dump()) +
                   "</code></dd>";
        content += "<dt>Model attestation</dt><dd><code>" +
                   web_ui_escape(provenance.value("model_attestation", "")) + "</code></dd>";
        content += "<dt>Run attestation</dt><dd><code>" +
                   web_ui_escape(provenance.value("run_attestation", "")) + "</code></dd>";
        if (!provenance.value("reason", "").empty()) {
            content +=
                "<dt>Reason</dt><dd>" + web_ui_escape(provenance.value("reason", "")) + "</dd>";
        }
        content += "</dl>";
    } else {
        content += "<p class=\"empty\">No training provenance is attached.</p>";
    }
    content += "</section></div><h2>Files</h2><table><thead><tr><th>Path</th><th>Size</th>"
               "<th>Stored</th><th>Saved</th></tr></thead><tbody>";
    for (const nlohmann::json& file : model.at("siblings")) {
        content += std::format("<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>",
                               web_ui_escape(file.value("rfilename", "")),
                               file.value("size", std::uint64_t{0}),
                               file.value("storedSize", std::uint64_t{0}),
                               file.value("deduplicatedBytes", std::uint64_t{0}));
    }
    if (model.at("siblings").empty()) {
        content += "<tr><td colspan=\"4\" class=\"empty\">No files.</td></tr>";
    }
    content += "</tbody></table><h2>Revisions</h2><table><thead><tr><th>Revision</th>"
               "<th>Logical</th><th>Unique</th><th>Saved</th></tr></thead><tbody>";
    for (const nlohmann::json& revision : storage.at("revisions")) {
        content += std::format("<tr><td><code>{}</code></td><td>{}</td><td>{}</td><td>{}</td></tr>",
                               web_ui_escape(revision.value("revision", "")),
                               revision.value("logical_bytes", std::uint64_t{0}),
                               revision.value("unique_bytes", std::uint64_t{0}),
                               revision.value("deduplicated_bytes", std::uint64_t{0}));
    }
    if (storage.at("revisions").empty()) {
        content += "<tr><td colspan=\"4\" class=\"empty\">No revisions.</td></tr>";
    }
    content += "</tbody></table>";
    return web_ui_shell("lake — model", model.value("modelId", ""), content, signed_in);
}

} // namespace lake::api

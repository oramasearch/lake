#include "lake/api/web_ui.hpp"

#include "lake/api/capabilities.hpp"
#include "lake/api/markdown.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include <liquid/hash.hpp>

#include "lake/api/pipeline.hpp"
#include "lake/control/store.hpp"
#include "modules/frontends/common/syntax.hpp"

namespace lake::api {

bool workspace_page_path(std::string_view path) {
    if (path == "/workspaces" || path == "/workspaces/create" || path == "/workspaces/delete") {
        return false;
    }
    for (const std::string_view prefix : {"/design", "/train", "/ship", "/ask", "/workspace",
                                           "/workspaces"}) {
        if (path == prefix || (path.starts_with(prefix) && path.size() > prefix.size() &&
                               path[prefix.size()] == '/')) {
            return path != "/workspace/select";
        }
    }
    return false;
}

std::string workspace_path_id(std::string_view path) {
    if (!path.starts_with("/w/")) {
        return {};
    }
    const auto end = path.find('/', 3);
    if (end == std::string_view::npos) {
        return {};
    }
    const std::string id(path.substr(3, end - 3));
    if (id.size() != 36 || std::ranges::any_of(id, [](unsigned char c) {
            return !std::isxdigit(c) && c != '-';
        })) {
        return {};
    }
    return id;
}

std::string workspace_local_path(std::string_view path) {
    const auto id = workspace_path_id(path);
    return std::string(id.empty() ? path : path.substr(3 + id.size()));
}

namespace {

std::string workspace_document_links(std::string html, std::string_view prefix) {
    const std::string_view input(html);
    std::string output;
    std::size_t copied = 0;
    bool in_script = false;
    // Rewrite URL attributes and inline navigation scripts, never hidden form values.
    for (std::size_t pos = 0; pos < input.size(); ++pos) {
        if (input[pos] == '<') {
            if (input.substr(pos).starts_with("<script")) {
                in_script = true;
            } else if (input.substr(pos).starts_with("</script")) {
                in_script = false;
            }
        }
        if ((input[pos] != '\'' && input[pos] != '"') || pos + 1 == input.size() ||
            input[pos + 1] != '/') {
            continue;
        }
        const std::string_view leading = input.substr(0, pos);
        if (!in_script && !leading.ends_with("href=") && !leading.ends_with("action=") &&
            !leading.ends_with("src=")) {
            continue;
        }
        const auto end = input.find_first_of("?\"'# <", pos + 1);
        const auto path = input.substr(pos + 1, end == std::string_view::npos
                                                   ? std::string_view::npos
                                                   : end - pos - 1);
        if (workspace_page_path(path)) {
            if (copied == 0) {
                output.reserve(input.size());
            }
            output.append(input.substr(copied, pos + 1 - copied));
            output.append(prefix);
            copied = pos + 1;
        }
    }
    if (copied == 0) {
        return html;
    }
    output.append(input.substr(copied));
    return output;
}

std::string workspace_form_fields(std::string html, std::string_view workspace_id) {
    const std::string_view input(html);
    const std::string field = "<input type=\"hidden\" name=\"workspace_id\" value=\"" +
                             std::string(workspace_id) + "\">";
    std::string output;
    std::size_t copied = 0;
    std::size_t pos = 0;
    while ((pos = input.find("<form", pos)) != std::string_view::npos) {
        const auto open = input.find('>', pos);
        const auto close = input.find("</form>", open);
        if (open == std::string::npos || close == std::string::npos) {
            break;
        }
        const auto body = input.substr(open + 1, close - open - 1);
        if (!body.contains("name=\"workspace_id\"")) {
            if (copied == 0) {
                output.reserve(input.size());
            }
            output.append(input.substr(copied, open + 1 - copied));
            output.append(field);
            copied = open + 1;
        }
        pos = close + 7;
    }
    if (copied == 0) {
        return html;
    }
    output.append(input.substr(copied));
    return output;
}

} // namespace

std::string web_ui_workspace_document(std::string html, std::string_view workspace_id) {
    if (workspace_path_id("/w/" + std::string(workspace_id) + "/").empty()) {
        return html;
    }
    const std::string prefix = "/w/" + std::string(workspace_id);
    return workspace_form_fields(workspace_document_links(std::move(html), prefix), workspace_id);
}

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

namespace {

std::string shell_argument(std::string_view value) {
    std::string quoted = "'";
    for (const char character : value) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    quoted += '\'';
    return quoted;
}

std::string markdown_slug(std::string_view heading) {
    std::string slug;
    bool pending_dash = false;
    for (const unsigned char character : heading) {
        if (std::isalnum(character)) {
            if (pending_dash && !slug.empty()) {
                slug += '-';
            }
            slug += static_cast<char>(std::tolower(character));
            pending_dash = false;
        } else if (!slug.empty()) {
            pending_dash = true;
        }
    }
    return slug.empty() ? "section" : slug;
}

} // namespace

std::string web_ui_markdown(std::string_view markdown) {
    // One renderer for READMEs and the Docs section (lake/api/markdown.hpp).
    return "<article class=\"markdown-body\">" + markdown_to_html(markdown) + "</article>";
}

std::string web_ui_hashed_asset_url(std::string_view path, std::string_view contents) {
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(contents.data()),
                                           contents.size());
    const liquid::result<liquid::digest> digest = liquid::hash(liquid::algorithm::sha256, bytes);
    if (!digest) {
        return {};
    }
    constexpr std::string_view hex = "0123456789abcdef";
    std::string hash;
    hash.reserve(16);
    for (std::size_t index = 0; index < 8; ++index) {
        const unsigned char value = std::to_integer<unsigned char>(digest->bytes[index]);
        hash += hex[value >> 4];
        hash += hex[value & 0x0f];
    }
    const std::size_t slash = path.rfind('/');
    const std::size_t extension = path.rfind('.');
    if (extension == std::string_view::npos ||
        (slash != std::string_view::npos && extension < slash)) {
        return std::string(path) + '-' + hash;
    }
    return std::string(path.substr(0, extension)) + '-' + hash +
           std::string(path.substr(extension));
}

std::span<const std::string_view> web_ui_font_names() {
    static constexpr std::array names{
        std::string_view{"ibm-plex-sans-400.woff2"},
        std::string_view{"ibm-plex-sans-500.woff2"},
        std::string_view{"ibm-plex-sans-600.woff2"},
        std::string_view{"ibm-plex-mono-400.woff2"},
        std::string_view{"ibm-plex-mono-500.woff2"},
    };
    return names;
}

std::string_view web_ui_font_url(std::string_view name) {
    static const std::map<std::string, std::string, std::less<>> urls = [] {
        std::map<std::string, std::string, std::less<>> result;
        for (const std::string_view font_name : web_ui_font_names()) {
            const std::string path = "/fonts/" + std::string(font_name);
            result.emplace(font_name, web_ui_hashed_asset_url(path, web_ui_font(font_name)));
        }
        return result;
    }();
    const auto found = urls.find(name);
    return found == urls.end() ? std::string_view{} : std::string_view(found->second);
}

std::string_view web_ui_stylesheet() {
    static constexpr std::string_view tokens =
#include "web_ui_tokens.inc"
        ;
    static constexpr std::string_view styles =
#include "web_ui_styles.inc"
        ;
    static const std::string stylesheet = [] {
        std::string result = std::string(tokens) + std::string(styles);
        for (const std::string_view name : web_ui_font_names()) {
            const std::string plain_url = "/fonts/" + std::string(name);
            const std::string_view hashed_url = web_ui_font_url(name);
            std::size_t offset = 0;
            while ((offset = result.find(plain_url, offset)) != std::string::npos) {
                result.replace(offset, plain_url.size(), hashed_url);
                offset += hashed_url.size();
            }
        }
        return result;
    }();
    return stylesheet;
}

std::string_view web_ui_stylesheet_url() {
    static const std::string url = web_ui_hashed_asset_url("/style.css", web_ui_stylesheet());
    return url;
}

std::string_view web_ui_asset_cache_control(std::string_view path) {
    if (path == web_ui_stylesheet_url()) {
        return "public, max-age=31536000, immutable";
    }
    for (const std::string_view name : web_ui_font_names()) {
        if (path == web_ui_font_url(name)) {
            return "public, max-age=31536000, immutable";
        }
    }
    for (const std::string_view name : web_ui_experiment_asset_names()) {
        if (path == web_ui_experiment_asset_url(name)) {
            return "public, max-age=31536000, immutable";
        }
    }
    return "no-cache";
}

std::string web_ui_icon(std::string_view name) {
    std::string_view body;
    if (name == "lake") {
        body = R"(<path d="M4 3h8v2H6v6h6v2H4z"/><path d="M8 7h4v2H8z"/>)";
    } else if (name == "home") {
        body =
            R"(<path d="M8 1.4 1.5 7v7.5h5V10h3v4.5h5V7L8 1.4Zm4.5 11.1h-1V8h-7v4.5h-1V7.9L8 4l4.5 3.9v4.6Z"/>)";
    } else if (name == "repository") {
        body =
            R"(<path d="M3 1h9.5A1.5 1.5 0 0 1 14 2.5V14H3a2 2 0 0 1-2-2V3a2 2 0 0 1 2-2Zm0 2v7h9V3H3Zm0 9a.5.5 0 0 0 0 1h9v-1H3Z"/>)";
    } else if (name == "search") {
        body =
            R"(<path d="m14.7 13.3-3.1-3.1a5.5 5.5 0 1 0-1.4 1.4l3.1 3.1 1.4-1.4ZM3 7.5A4.5 4.5 0 1 1 12 7.5a4.5 4.5 0 0 1-9 0Z"/>)";
    } else if (name == "customers") {
        body =
            R"(<path d="M8 8a3 3 0 1 0 0-6 3 3 0 0 0 0 6Zm0-2a1 1 0 1 1 0-2 1 1 0 0 1 0 2Zm0 3c-3.3 0-6 1.8-6 4v1h12v-1c0-2.2-2.7-4-6-4Zm-3.5 3c.7-.6 2-1 3.5-1s2.8.4 3.5 1h-7Z"/>)";
    } else if (name == "organization") {
        body =
            R"(<path d="M2 1h9v4h3v10H2V1Zm2 2v10h2v-2h2v2h1V3H4Zm7 4v6h1V7h-1Z"/><path d="M5 5h1v1H5zm2 0h1v1H7zM5 8h1v1H5zm2 0h1v1H7z"/>)";
    } else if (name == "metering") {
        body = R"(<path d="M2 2h12v12H2V2Zm2 2v8h8V4H4Zm1 6h1V8H5v2Zm2 0h1V5H7v5Zm2 0h1V7H9v3Z"/>)";
    } else if (name == "secrets") {
        body =
            R"(<path d="M4 7V5a4 4 0 0 1 8 0v2h2v8H2V7h2Zm2 0h4V5a2 2 0 0 0-4 0v2Zm-2 2v4h8V9H4Z"/>)";
    } else if (name == "audit") {
        body =
            R"(<path d="M3 1h7l3 3v11H3V1Zm2 2v10h6V5H9V3H5Zm1 5 1.4 1.4L10.8 6 12 7.2 7.4 12 4.8 9.4 6 8Z"/>)";
    } else if (name == "identity") {
        body =
            R"(<path d="M8 1a4 4 0 1 0 0 8 4 4 0 0 0 0-8Zm0 2a2 2 0 1 1 0 4 2 2 0 0 1 0-4Zm0 7c-3.3 0-6 1.8-6 4v1h12v-1c0-2.2-2.7-4-6-4Zm-3.5 3c.7-.6 2-1 3.5-1s2.8.4 3.5 1h-7Z"/>)";
    } else if (name == "api") {
        body =
            R"(<path d="M1 2h14v12H1V2Zm2 2v8h10V4H3Zm1 2 2 2-2 2 1.4 1.4L8.8 8 5.4 4.6 4 6Zm5 4h3V8H9v2Z"/>)";
    } else if (name == "agent") {
        // A speech bubble with a face: an agent you can talk to.
        body =
            R"(<path d="M2 2h12v9H7l-3 3v-3H2V2Zm2 2v5h8V4H4Z"/><path d="M6 5.5h1v1.5H6zm3 0h1v1.5H9z"/>)";
    } else if (name == "streams") {
        // Three ordered messages moving through a stream.
        body =
            R"(<path d="M1 3h9v2H1V3Zm0 4h12v2H1V7Zm0 4h9v2H1v-2Z"/><path d="m10 1 4 3-4 3V1Zm3 8 3 3-3 3V9Z"/>)";
    } else if (name == "docs") {
        // An open book: the Docs section.
        body =
            R"(<path d="M1 3c2.5-1 5-1 7 .5C10 2 12.5 2 15 3v10c-2.5-1-5-1-7 .5C6 12 3.5 12 1 13V3Zm2 1.3v6.5c1.4-.3 2.8-.2 4 .4V4.7C5.8 4 4.4 4 3 4.3Zm10 0c-1.4-.3-2.8-.3-4 .4v6.5c1.2-.6 2.6-.7 4-.4V4.3Z"/>)";
    } else if (name == "theme") {
        body = R"(<path d="M8 1a7 7 0 1 0 7 7 7 7 0 0 0-7-7Zm0 12V3a5 5 0 0 1 0 10Z"/>)";
    } else {
        return {};
    }
    return std::format("<svg class=\"icon\" viewBox=\"0 0 16 16\" aria-hidden=\"true\" "
                       "focusable=\"false\">{}</svg>",
                       body);
}

namespace {

constexpr std::string_view theme_preference_script =
    R"(<script id="theme-preference">(()=>{try{const t=localStorage.getItem('lake-theme');if(t==='light'||t==='dark')document.documentElement.dataset.theme=t;}catch{}})();</script>)";
constexpr std::string_view theme_toggle_script =
    R"(<script id="theme-toggle-script">(()=>{const b=document.querySelector('[data-theme-toggle]');if(!b)return;const current=()=>document.documentElement.dataset.theme||(matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light');const label=()=>{const t=current();b.setAttribute('aria-label',`Use ${t==='dark'?'light':'dark'} theme`);b.setAttribute('aria-pressed',String(t==='dark'));};b.addEventListener('click',()=>{const t=current()==='dark'?'light':'dark';document.documentElement.dataset.theme=t;try{localStorage.setItem('lake-theme',t);}catch{}label();});label();})();</script>)";

std::string stylesheet_link() {
    return std::format("<link rel=\"stylesheet\" href=\"{}\">", web_ui_stylesheet_url());
}

// The routes each navigation item owns. A request path matches an entry exactly or as a parent
// ("/drive" owns "/drive/preview"). Repository pages, including a repository's own environments
// and logs, belong to Repositories; the cross-repository list belongs to Deployments.
std::span<const std::string_view> navigation_routes(std::string_view key) {
    static constexpr std::string_view home[] = {"/"};
    static constexpr std::string_view repositories[] = {
        "/repositories", "/repository",  "/file",     "/changes",  "/change",
        "/issues",       "/issue",       "/releases", "/webhooks", "/protection",
        "/conflicts",    "/artifacts",   "/artifact", "/exports",  "/drive",
        "/environments", "/environment", "/logs",     "/models",   "/datasets"};
    static constexpr std::string_view shared[] = {"/shared-with-me", "/share"};
    static constexpr std::string_view search[] = {"/search"};
    static constexpr std::string_view deployments[] = {"/deployments"};
    // Each Design page owns its own routes; Projects keeps the bare /design and its detail pages.
    static constexpr std::string_view projects[] = {
        "/design/projects", "/design/project", "/design/pipeline"};
    static constexpr std::string_view opportunities[] = {"/design/opportunities", "/design/discover"};
    static constexpr std::string_view solutions[] = {"/design/solutions", "/design/solution"};
    static constexpr std::string_view methodologies[] = {"/design/methodologies",
                                                         "/design/methodology"};
    static constexpr std::string_view agent_library[] = {"/design/agent-library"};
    static constexpr std::string_view agents[] = {"/train/agents", "/train/agent"};
    static constexpr std::string_view data[] = {"/train/data", "/train/dataset", "/train/query"};
    static constexpr std::string_view runs[] = {"/train/runs", "/train/run"};
    static constexpr std::string_view experiments[] = {
        "/train/experiments", "/train/experiment", "/train/experiment/run",
        "/train/experiments/compare"};
    static constexpr std::string_view streams[] = {"/train/streams", "/train/stream"};
    static constexpr std::string_view models[] = {"/ship/models"};
    static constexpr std::string_view indexes[] = {"/ship/indexes", "/ship/index"};
    static constexpr std::string_view packages[] = {"/ship/packages"};
    static constexpr std::string_view apps[] = {"/ship/apps", "/ship/app"};
    static constexpr std::string_view services[] = {"/services"};
    static constexpr std::string_view workspaces[] = {"/workspaces"};
    static constexpr std::string_view activity[] = {"/activity"};
    static constexpr std::string_view customers[] = {"/customers"};
    static constexpr std::string_view organizations[] = {"/organizations",
                                                         "/organization",
                                                         "/teams",
                                                         "/team",
                                                         "/members",
                                                         "/member",
                                                         "/invitations",
                                                         "/grants",
                                                         "/access",
                                                         "/sso"};
    static constexpr std::string_view applications[] = {"/applications", "/application"};
    static constexpr std::string_view metering[] = {"/metering"};
    static constexpr std::string_view secrets[] = {
        "/secret-mounts", "/secrets", "/encryption-keys"};
    static constexpr std::string_view audit[] = {"/audit"};
    static constexpr std::string_view mcp[] = {"/developer/mcp"};
    if (key == "home")
        return home;
    if (key == "repositories")
        return repositories;
    if (key == "shared with me")
        return shared;
    if (key == "search")
        return search;
    if (key == "deployments")
        return deployments;
    if (key == "projects")
        return projects;
    if (key == "opportunities")
        return opportunities;
    if (key == "solutions")
        return solutions;
    if (key == "methodologies")
        return methodologies;
    if (key == "agent library")
        return agent_library;
    if (key == "agents")
        return agents;
    if (key == "data")
        return data;
    if (key == "runs")
        return runs;
    if (key == "experiments")
        return experiments;
    if (key == "streams")
        return streams;
    if (key == "models")
        return models;
    if (key == "indexes")
        return indexes;
    if (key == "services")
        return services;
    if (key == "workspaces") {
        return workspaces;
    }
    if (key == "packages")
        return packages;
    if (key == "apps")
        return apps;
    if (key == "activity")
        return activity;
    if (key == "customers")
        return customers;
    if (key == "organizations")
        return organizations;
    if (key == "applications")
        return applications;
    if (key == "metering")
        return metering;
    if (key == "secret")
        return secrets;
    if (key == "audit")
        return audit;
    if (key == "mcp")
        return mcp;
    return {};
}

bool route_owns(std::string_view path, std::string_view route) {
    if (route == "/") {
        return path == "/";
    }
    // The bare Design stage link lands on Projects.
    if (route == "/design/projects" && path == "/design") {
        return true;
    }
    return path == route ||
           (path.starts_with(route) && path.size() > route.size() && path[route.size()] == '/');
}

// Fallback for renderers called without a request path: the page title decides.
bool title_is_current(std::string_view title, std::string_view key) {
    if (key == "home") {
        return title == "lake — design, train, and ship AI";
    }
    if (key == "repositories") {
        return title == "lake — repositories" || title == "lake — tree" || title == "lake — file" ||
               title == "lake — changes" || title == "lake — change" ||
               title == "lake — artifacts" || title == "lake — artifact" ||
               title == "lake — conflicts" || title == "lake — exports" ||
               title == "lake — drive" || title == "lake — preview" ||
               title == "lake — drive history" || title == "lake — drive version diff" ||
               title == "lake — drive conflict" || title == "lake — drive sharing" ||
               title == "lake — shared drive folder" ||
               title.starts_with("lake — ") && title.find('/') != std::string_view::npos;
    }
    if (key == "organizations") {
        return title == "lake — organizations" || title == "lake — teams" ||
               title == "lake — members" || title == "lake — invitations" ||
               title == "lake — grants" || title == "lake — effective access" ||
               title == "lake — enterprise SSO";
    }
    if (key == "secret") {
        return title == "lake — secret mounts" || title == "lake — secrets" ||
               title == "lake — encryption keys";
    }
    if (key == "mcp") {
        return title == "lake — MCP";
    }
    if (key == "applications") {
        return title == "lake — applications" || title == "lake — application users" ||
               title == "lake — application organizations" ||
               title == "lake — application events" || title.starts_with("lake — application:");
    }
    return title == std::format("lake — {}", key);
}

bool navigation_is_current(const WebUiViewer& viewer,
                           std::string_view title,
                           std::string_view key) {
    if (viewer.path.empty()) {
        return title_is_current(title, key);
    }
    for (const std::string_view route : navigation_routes(key)) {
        if (route_owns(workspace_local_path(viewer.path), route)) {
            return true;
        }
    }
    return false;
}

std::string nav_link(std::string_view href,
                     std::string_view label,
                     std::string_view icon,
                     std::string_view key,
                     const WebUiViewer& viewer,
                     std::string_view title) {
    return std::format("<a class=\"nav-link\" href=\"{}\"{}>{}<span>{}</span></a>",
                       href,
                       navigation_is_current(viewer, title, key) ? " aria-current=\"page\"" : "",
                       web_ui_icon(icon),
                       label);
}

// One sidebar link: the group heading it sits under ("" for the top links), where it goes, its
// label and icon, and the key navigation_is_current matches the page against.
struct NavItem {
    std::string_view group, href, label, icon, key;
};

// The sidebar follows docs/design/cloud.md: the viewer's repositories first, then the sections
// that act on them. A section appears only when it has a page the viewer can open; Design and
// Train join when their pages exist. Account pages live in the top-bar account menu.
// Labels are Title Case (docs/ui/design-system.md, Capitalization). The breadcrumb reads the same
// list (docs/ui/breadcrumb.md), so the two never disagree.
std::vector<NavItem> nav_items(const WebUiViewer& viewer) {
    std::vector<NavItem> items{{"", "/", "Home", "home", "home"},
                               {"",
                                "/repositories",
                                viewer.signed_in ? "Repositories" : "Public Repositories",
                                "repository",
                                "repositories"}};
    if (viewer.signed_in) {
        if (!viewer.administrator) {
            items.push_back(
                {"", "/shared-with-me", "Shared with Me", "repository", "shared with me"});
        }
        items.push_back({"", "/search", "Search", "search", "search"});
        items.push_back({"Design", "/design/projects", "Projects", "audit", "projects"});
        items.push_back(
            {"Design", "/design/opportunities", "Opportunities", "search", "opportunities"});
        items.push_back({"Design", "/design/solutions", "Solutions", "audit", "solutions"});
        items.push_back(
            {"Design", "/design/methodologies", "Methodologies", "metering", "methodologies"});
        items.push_back({"Design", "/design/agent-library", "Agent Library", "agent", "agent library"});
        items.push_back({"Train", "/train/agents", "Agents", "agent", "agents"});
        items.push_back({"Train", "/train/data", "Data", "metering", "data"});
        items.push_back({"Train", "/train/runs", "Runs", "metering", "runs"});
        items.push_back({"Train", "/train/experiments", "Experiments", "metering", "experiments"});
        items.push_back({"Train", "/train/streams", "Streams", "streams", "streams"});
        items.push_back({"Ship", "/ship/models", "Models", "api", "models"});
        items.push_back({"Ship", "/ship/indexes", "Search Indexes", "search", "indexes"});
        items.push_back({"Ship", "/ship/packages", "Packages", "repository", "packages"});
        items.push_back({"Ship", "/ship/apps", "Apps", "api", "apps"});
        items.push_back({"Ship", "/deployments", "Deployments", "api", "deployments"});
        items.push_back({"Ship", "/services", "Services", "api", "services"});
        items.push_back({"Ship", "/workspaces", "Workspaces", "api", "workspaces"});
    }
    // A signed-in account's own organizations, secrets, usage and activity. Operators reach the
    // instance-wide versions under Administration instead.
    if (viewer.signed_in && !viewer.administrator) {
        items.push_back(
            {"Organization", "/organizations", "Organizations", "organization", "organizations"});
        items.push_back({"Organization", "/secret-mounts", "Secrets", "secrets", "secret"});
        items.push_back({"Organization", "/metering", "Usage", "metering", "metering"});
        items.push_back({"Organization", "/activity", "Activity", "audit", "activity"});
    }
    if (viewer.administrator) {
        items.push_back({"Administration", "/customers", "Customers", "customers", "customers"});
        items.push_back(
            {"Administration", "/organizations", "Organizations", "organization", "organizations"});
        items.push_back({"Administration", "/applications", "Applications", "api", "applications"});
        items.push_back({"Administration", "/metering", "Metering", "metering", "metering"});
        items.push_back({"Administration", "/secret-mounts", "Secrets", "secrets", "secret"});
        items.push_back({"Administration", "/audit", "Audit", "audit", "audit"});
    }
    if (viewer.signed_in) {
        items.push_back({"Developer", "/v1/openapi.json", "API", "api", "api"});
        items.push_back({"Developer", "/developer/mcp", "MCP", "api", "mcp"});
    }
    // The Docs section (lake#252), last under Developer, is for everyone, signed in or not.
    items.push_back({"Developer", "/docs", "Docs", "docs", "docs"});
    return items;
}

std::string nav(const WebUiViewer& viewer, std::string_view title) {
    std::string out = "<nav class=\"primary-nav\" aria-label=\"Primary\">";
    std::string_view group;
    for (const NavItem& item : nav_items(viewer)) {
        if (item.group != group && !item.group.empty()) {
            out += std::format("<p class=\"nav-label\">{}</p>", item.group);
        }
        group = item.group;
        out += nav_link(item.href, item.label, item.icon, item.key, viewer, title);
    }
    out += "</nav>";
    return out;
}

// The breadcrumb a page gets without building its own (docs/ui/breadcrumb.md): the sidebar item it
// belongs to, under its group, then `meta` as the page's own level when it adds something. Account
// pages (their `meta` is "identity") sit under Account. A page outside the sidebar shows `meta`
// alone.
std::vector<WebUiCrumb>
default_trail(const WebUiViewer& viewer, std::string_view title, std::string_view meta) {
    std::vector<WebUiCrumb> trail;
    const std::vector<NavItem> items = nav_items(viewer);
    for (const NavItem& item : items) {
        if (!navigation_is_current(viewer, title, item.key)) {
            continue;
        }
        if (!item.group.empty()) {
            const auto first = std::ranges::find(items, item.group, &NavItem::group);
            trail.push_back({std::string(item.group), std::string(first->href), {}});
        }
        trail.push_back({std::string(item.label), std::string(item.href), {}});
        break;
    }
    const auto same = [](std::string_view a, std::string_view b) {
        return std::ranges::equal(a, b, [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) ==
                   std::tolower(static_cast<unsigned char>(y));
        });
    };
    if (meta == "identity" && viewer.signed_in && !viewer.administrator) {
        trail.push_back({"Account", "/settings", {}});
        constexpr std::string_view prefix = "lake — ";
        if (title.starts_with(prefix)) {
            std::string page(title.substr(prefix.size()));
            if (!page.empty()) {
                page[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(page[0])));
            }
            trail.push_back({page, {}, {}});
        }
        return trail;
    }
    if (!meta.empty() && meta != "identity" &&
        (trail.empty() || (!same(meta, trail.back().label) &&
                           !(trail.size() > 1 && same(meta, trail.front().label))))) {
        trail.push_back(web_ui_entity_crumb(meta));
    }
    return trail;
}

// Signed out: sign in and create account. Signed in: an account menu that needs no script
// (<details>), holding the person's own pages and sign out. Operators have no personal pages.
std::string url_component(std::string_view value);

std::string session_control(const WebUiViewer& viewer) {
    static constexpr std::string_view sign_out =
        "<form class=\"inline\" method=\"post\" action=\"/logout\"><button "
        "type=\"submit\">Sign out</button></form>";
    if (!viewer.signed_in) {
        return "<span class=\"session\"><a href=\"/sign-in\">Sign in</a><a "
               "href=\"/sign-up\">Create account</a></span>";
    }
    if (viewer.administrator) {
        return std::format("<span class=\"session\">{}</span>", sign_out);
    }
    // Ask Orama (docs/api/ask-orama.md): a panel beside every page (lake#258), told the whole page
    // it was opened from, path and query. No script: the toggle is a <details>, the panel a frame of
    // the Ask page in its embedded form, and a link opens it full size.
    const std::string from = (viewer.path.empty() ? std::string("/") : viewer.path) +
                             (viewer.query.empty() ? std::string() : "?" + viewer.query);
    const std::string ask = std::format(
        "<details class=\"ask-dock\"><summary class=\"button ask-orama\">{}Ask Orama</summary>"
        "<div class=\"ask-dock-panel\"><iframe src=\"/ask?embed=1&amp;from={}\" title=\"Ask Orama\" "
        "loading=\"lazy\"></iframe><p><a href=\"/ask?from={}\">Open Ask Orama in a full page</a></p>"
        "</div></details>",
        web_ui_icon("agent"),
        url_component(from),
        url_component(from));
    return ask +
           std::format("<details class=\"account-menu\"><summary>{}<span>Account</span></summary>"
                       "<div class=\"account-menu-panel\"><a href=\"/settings\">Settings</a>"
                       "<a href=\"/sessions\">Sessions</a>{}"
                       "<a href=\"/tokens\">Tokens</a>{}</div></details>",
                       web_ui_icon("identity"),
                       viewer.clerk ? "" : "<a href=\"/passkeys\">Passkeys</a>",
                       sign_out);
}

std::string row_link(std::string_view href, std::string_view text) {
    return std::format("<a href=\"{}\">{}</a>", web_ui_escape(href), web_ui_escape(text));
}

std::string agent_events_link(std::string_view customer_id, std::string_view task_id) {
    if (customer_id.empty() || task_id.empty()) {
        return {};
    }
    return "<p class=\"sub\">" +
           row_link("/train/stream?customer_id=" + url_component(customer_id) +
                        "&zetta=workspace&topic=agent-events&task=" + url_component(task_id),
                    "View this turn in Streams") +
           "</p>";
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

std::string
forge_tabs(const control::Repository& repository, std::string_view current, WebUiViewer viewer) {
    const std::string id = url_component(repository.id);
    const std::vector<std::tuple<std::string, std::string, std::string>> tabs{
        {"Files", "/drive?repository_id=" + id, "files"},
        {"Code", "/repository?id=" + id, "code"},
        {"Changes", "/changes?repository_id=" + id, "changes"},
        {"Issues", "/issues?repository_id=" + id, "issues"},
        {"Releases", "/releases?repository_id=" + id, "releases"},
        {"Artifacts", "/artifacts?repository_id=" + id, "artifacts"},
        {"Deployments", "/environments?repository_id=" + id, "platform"},
        {"Webhooks", "/webhooks?repository_id=" + id, "webhooks"},
        {"Protection", "/protection?repository_id=" + id, "protection"},
    };
    std::string output = "<nav class=\"tabs forge-tabs\" aria-label=\"Repository\">";
    for (const auto& [label, href, key] : tabs) {
        if (!viewer.signed_in && (key == "files" || key == "platform" || key == "webhooks")) {
            continue;
        }
        // The protection page includes the instance audit capability.
        if (key == "protection" && !viewer.administrator) {
            continue;
        }
        output += std::format("<a href=\"{}\"{}>{}</a>",
                              web_ui_escape(href),
                              key == current ? " aria-current=\"page\"" : "",
                              label);
    }
    output += "</nav>";
    return output;
}

std::string abbreviated_identifier(std::string_view value) {
    if (value.size() <= 20) {
        return std::string(value);
    }
    return std::string(value.substr(0, 12)) + "…" + std::string(value.substr(value.size() - 7));
}

std::string identifier_markup(std::string_view value) {
    return std::format(
        "<code class=\"identifier\" title=\"{}\" data-full-identifier=\"{}\">{}</code>",
        web_ui_escape(value),
        web_ui_escape(value),
        web_ui_escape(abbreviated_identifier(value)));
}

std::string identifier_list_markup(const std::vector<std::string>& values) {
    if (values.empty()) {
        return "<span class=\"muted\">None</span>";
    }
    std::string output = "<span class=\"identity-list\">";
    for (const std::string& value : values) {
        output += identifier_markup(value);
    }
    output += "</span>";
    return output;
}

std::string file_language(std::string_view path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos) {
        return "Other";
    }
    const std::string_view extension = path.substr(dot);
    if (extension == ".c" || extension == ".h") {
        return "C";
    }
    if (extension == ".cc" || extension == ".cpp" || extension == ".cxx" || extension == ".hh" ||
        extension == ".hpp" || extension == ".hxx") {
        return "C++";
    }
    if (extension == ".md" || extension == ".markdown") {
        return "Markdown";
    }
    if (extension == ".json") {
        return "JSON";
    }
    if (extension == ".js" || extension == ".mjs") {
        return "JavaScript";
    }
    if (extension == ".ts" || extension == ".tsx") {
        return "TypeScript";
    }
    if (extension == ".py") {
        return "Python";
    }
    if (extension == ".rs") {
        return "Rust";
    }
    return "Other";
}

std::string byte_count(std::uint64_t bytes) {
    if (bytes < 1024) {
        return std::format("{} B", bytes);
    }
    if (bytes < 1024 * 1024) {
        return std::format("{:.1f} KiB", static_cast<double>(bytes) / 1024.0);
    }
    return std::format("{:.1f} MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

std::string path_breadcrumbs(const control::Repository& repository,
                             std::string_view ref,
                             std::string_view path) {
    std::string output = "<nav class=\"breadcrumbs\" aria-label=\"Breadcrumb\">";
    output += row_link("/repository?id=" + repository.id, repository_name(repository));
    output += "<span aria-hidden=\"true\">/</span>";
    output += row_link("/repository/tree?id=" + repository.id + "&ref=" + url_component(ref), ref);
    std::string accumulated;
    std::size_t offset = 0;
    while (offset < path.size()) {
        const std::size_t slash = path.find('/', offset);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        const std::string_view part = path.substr(offset, end - offset);
        if (!accumulated.empty()) {
            accumulated += '/';
        }
        accumulated += part;
        output += "<span aria-hidden=\"true\">/</span>";
        if (end == path.size()) {
            output += "<span aria-current=\"page\">" + web_ui_escape(part) + "</span>";
        } else {
            output += row_link("/repository/tree?id=" + repository.id +
                                   "&ref=" + url_component(ref) + "#" + url_component(accumulated),
                               part);
        }
        offset = end + 1;
    }
    output += "</nav>";
    return output;
}

std::optional<liquid_frontend::syntax_language> syntax_language(std::string_view path) {
    const std::size_t dot = path.find_last_of('.');
    const std::string_view extension =
        dot == std::string_view::npos ? std::string_view{} : path.substr(dot);
    if (extension == ".c" || extension == ".h") {
        return liquid_frontend::syntax_language::c;
    }
    if (extension == ".cc" || extension == ".cpp" || extension == ".cxx" || extension == ".hh" ||
        extension == ".hpp" || extension == ".hxx") {
        return liquid_frontend::syntax_language::cpp;
    }
    return std::nullopt;
}

bool source_keyword(std::string_view token) {
    static const std::set<std::string_view> keywords{
        "alignas",   "alignof",  "auto",     "bool",      "break",    "case",    "catch",
        "char",      "class",    "const",    "constexpr", "continue", "default", "delete",
        "do",        "double",   "else",     "enum",      "explicit", "extern",  "false",
        "float",     "for",      "friend",   "if",        "inline",   "int",     "long",
        "namespace", "new",      "noexcept", "nullptr",   "operator", "private", "protected",
        "public",    "return",   "short",    "signed",    "sizeof",   "static",  "struct",
        "switch",    "template", "this",     "throw",     "true",     "try",     "typedef",
        "typename",  "union",    "unsigned", "using",     "virtual",  "void",    "volatile",
        "while"};
    return keywords.contains(token);
}

std::string highlight_source_line(std::string_view line) {
    std::string output;
    std::size_t offset = 0;
    while (offset < line.size()) {
        if (offset + 1 < line.size() && line[offset] == '/' && line[offset + 1] == '/') {
            output +=
                "<span class=\"syntax-comment\">" + web_ui_escape(line.substr(offset)) + "</span>";
            break;
        }
        if (line[offset] == '"' || line[offset] == '\'') {
            const char quote = line[offset];
            std::size_t end = offset + 1;
            bool escaped = false;
            while (end < line.size()) {
                if (!escaped && line[end] == quote) {
                    ++end;
                    break;
                }
                escaped = !escaped && line[end] == '\\';
                if (line[end] != '\\') {
                    escaped = false;
                }
                ++end;
            }
            output += "<span class=\"syntax-string\">" +
                      web_ui_escape(line.substr(offset, end - offset)) + "</span>";
            offset = end;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(line[offset])) || line[offset] == '_') {
            std::size_t end = offset + 1;
            while (end < line.size() &&
                   (std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '_')) {
                ++end;
            }
            const std::string_view token = line.substr(offset, end - offset);
            if (source_keyword(token)) {
                output += "<span class=\"syntax-keyword\">" + web_ui_escape(token) + "</span>";
            } else {
                output += web_ui_escape(token);
            }
            offset = end;
            continue;
        }
        output += web_ui_escape(line.substr(offset, 1));
        ++offset;
    }
    return output;
}

const RepositoryBlameLine* blame_for_line(const std::vector<RepositoryBlameLine>& blame,
                                          std::uint64_t line) {
    const auto found = std::ranges::find(blame, line, &RepositoryBlameLine::line);
    return found == blame.end() ? nullptr : &*found;
}

std::string source_view(const RepositoryFile& file, const std::vector<RepositoryBlameLine>& blame) {
    std::set<std::uint64_t> syntax_lines;
    const auto language = syntax_language(file.path);
    if (language) {
        const auto* first = reinterpret_cast<const std::uint8_t*>(file.contents.data());
        const auto units = liquid_frontend::parse_units(
            *language, std::span<const std::uint8_t>{first, file.contents.size()});
        for (const liquid_frontend::syntax_unit& unit : units) {
            for (std::uint64_t line = unit.start.line; line <= unit.end.line; ++line) {
                syntax_lines.insert(line);
            }
        }
    }
    std::istringstream input{file.contents};
    std::string line;
    std::string output = std::format(
        "<div class=\"source-view{}\" data-language=\"{}\"><table><thead class=\"sr-only\">"
        "<tr><th>Blame</th><th>Line</th><th>Source</th></tr></thead><tbody>",
        language ? " syntax-highlighted" : " plain-text",
        language ? file_language(file.path) : "Plain text");
    std::uint64_t number = 1;
    while (std::getline(input, line)) {
        const RepositoryBlameLine* line_blame = blame_for_line(blame, number);
        output += std::format(
            "<tr id=\"L{}\" data-line=\"{}\"{}><td class=\"blame-cell\">{}{}{}</td>"
            "<td class=\"line-number\"><a href=\"#L{}\" data-line-range=\"L{}-L{}\" "
            "aria-label=\"Line {}\">{}</a></td><td class=\"source-line\"><code>{}</code></td>"
            "</tr>",
            number,
            number,
            syntax_lines.contains(number) ? " class=\"syntax-unit\"" : "",
            line_blame ? identifier_markup(line_blame->change) : "",
            line_blame ? "<span>" + web_ui_escape(line_blame->author) + "</span>" : "",
            line_blame ? "<time>" + web_ui_escape(line_blame->occurred_at) + "</time>" : "",
            number,
            number,
            number,
            number,
            number,
            language ? highlight_source_line(line) : web_ui_escape(line));
        ++number;
    }
    if (number == 1) {
        output += "<tr><td colspan=\"3\" class=\"empty\">This file is empty.</td></tr>";
    }
    output += "</tbody></table></div>";
    return output;
}

std::string error_markup(std::string_view error) {
    return error.empty() ? std::string()
                         : std::format("<p class=\"error\">{}</p>", web_ui_escape(error));
}

std::string badge_markup(std::string_view value) {
    std::string_view modifier;
    if (value == "active" || value == "success" || value == "verified" || value == "hit" ||
        value == "added") {
        modifier = " status-success";
    } else if (value == "revoked" || value == "denied" || value == "failed" ||
               value == "divergent" || value == "removed" || value == "conflicted") {
        modifier = " status-danger";
    } else if (value == "pending" || value == "warning" || value == "miss") {
        modifier = " status-warning";
    } else if (value == "model" || value == "agent" || value == "modified") {
        modifier = " status-ai";
    }
    return std::format("<span class=\"badge{}\">{}</span>", modifier, web_ui_escape(value));
}

std::string state_badge(bool active) {
    return badge_markup(active ? "active" : "revoked");
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

std::string path_name(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::string
drive_query(const control::Repository& repository, std::string_view ref, std::string_view path) {
    return "repository_id=" + url_component(repository.id) + "&ref=" + url_component(ref) +
           "&path=" + url_component(path);
}

std::string drive_breadcrumbs(const control::Repository& repository,
                              std::string_view ref,
                              std::string_view path) {
    std::string output = "<nav class=\"breadcrumbs\" aria-label=\"Breadcrumb\">";
    output += row_link("/drive?" + drive_query(repository, ref, ""), repository_name(repository));
    std::size_t offset = 0;
    while (offset < path.size()) {
        const std::size_t slash = path.find('/', offset);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        const std::string prefix(path.substr(0, end));
        output += "<span aria-hidden=\"true\">/</span>";
        if (end == path.size()) {
            output += "<span aria-current=\"page\">" +
                      web_ui_escape(path.substr(offset, end - offset)) + "</span>";
        } else {
            output += row_link("/drive?" + drive_query(repository, ref, prefix),
                               path.substr(offset, end - offset));
        }
        if (slash == std::string_view::npos) {
            break;
        }
        offset = slash + 1;
    }
    output += "</nav>";
    return output;
}

std::string display_size(std::uint64_t size) {
    static constexpr std::array<std::string_view, 4> units{"B", "KiB", "MiB", "GiB"};
    double value = static_cast<double>(size);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 ? std::format("{} {}", size, units[unit])
                     : std::format("{:.1f} {}", value, units[unit]);
}

std::string drive_diff_markup(const DriveVersionDiff& difference) {
    std::string output = "<pre class=\"diff-hunk drive-version-diff\">";
    for (const DriveDiffLine& line : difference.lines) {
        const std::string marker = line.kind == "removed" ? "-" : line.kind == "added" ? "+" : " ";
        const std::string modifier = line.kind == "removed" ? " class=\"diff-remove\""
                                     : line.kind == "added" ? " class=\"diff-add\""
                                                            : "";
        output += "<span" + modifier + ">" + marker + web_ui_escape(line.text) + "</span>";
    }
    output += "</pre>";
    return output;
}

std::string drive_hidden_context(const control::Repository& repository,
                                 std::string_view ref,
                                 std::string_view path) {
    return std::format(
        "<input type=\"hidden\" name=\"repository_id\" value=\"{}\"><input type=\"hidden\" "
        "name=\"ref\" value=\"{}\"><input type=\"hidden\" name=\"path\" value=\"{}\">",
        web_ui_escape(repository.id),
        web_ui_escape(ref),
        web_ui_escape(path));
}

std::string share_link_state(const nlohmann::json& link) {
    const auto revoked_at = link.find("revoked_at");
    if (revoked_at != link.end() && revoked_at->is_string() && !revoked_at->empty()) {
        return "revoked";
    }
    return link.value("active", false) ? "active" : "expired";
}

std::string share_link_actions(std::string_view role) {
    if (role == "write") {
        return "Read; write scope is available to capability-aware clients";
    }
    return "Browse and download";
}

std::string drive_share_link_url(const control::Repository& repository,
                                 std::string_view ref,
                                 std::string_view path,
                                 std::string_view link_id) {
    return "/drive/share/link?repository_id=" + url_component(repository.id) +
           "&ref=" + url_component(ref) + "&path=" + url_component(path) +
           "&link_id=" + url_component(link_id);
}

std::string drive_entry_actions(const control::Repository& repository,
                                std::string_view ref,
                                const RepositoryTreeEntry& entry,
                                bool signed_in) {
    const std::string query = drive_query(repository, ref, entry.path);
    std::string output = "<div class=\"drive-item-actions\">";
    output += row_link("/drive/history?" + query, "History");
    if (!signed_in) {
        return output + "</div>";
    }
    output += "<details><summary>Manage</summary><div class=\"drive-manage\">";
    output += "<form method=\"post\" action=\"/drive/rename\" class=\"stack\">";
    output += drive_hidden_context(repository, ref, entry.path);
    output += "<label>New name<input name=\"name\" required value=\"" +
              web_ui_escape(path_name(entry.path)) +
              "\"></label><button type=\"submit\">Rename</button>"
              "</form>";
    output += "<form method=\"post\" action=\"/drive/move\" class=\"stack\">";
    output += drive_hidden_context(repository, ref, entry.path);
    output +=
        "<label>Destination folder<input name=\"destination_folder\" placeholder=\"team/archive\">"
        "</label><button type=\"submit\">Move</button></form>";
    output += "<form method=\"post\" action=\"/drive/delete\" class=\"inline\">";
    output += drive_hidden_context(repository, ref, entry.path);
    output +=
        "<button class=\"danger\" type=\"submit\">Delete</button></form></div></details></div>";
    return output;
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
    std::string html = "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                       "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                       "<meta name=\"color-scheme\" content=\"light dark\">"
                       "<title>" +
                       web_ui_escape(title) + "</title>" + std::string(theme_preference_script) +
                       stylesheet_link() + "<style>:root{--accent:" + accent +
                       ";--paper:" + background + ";--app-accent:" + accent +
                       ";--app-background:" + background +
                       "}</style></head><body class=\"hosted-shell\">"
                       "<a class=\"skip-link\" href=\"#main-content\">Skip to content</a>"
                       "<header class=\"hosted-header\"><span class=\"hosted-brand\"><span "
                       "class=\"hosted-brand-mark\" aria-hidden=\"true\"></span>" +
                       web_ui_escape(application.name) +
                       "</span><button class=\"theme-toggle\" type=\"button\" data-theme-toggle "
                       "aria-label=\"Toggle theme\">" +
                       web_ui_icon("theme") +
                       "<span class=\"sr-only\">Toggle theme</span></button></header>"
                       "<main class=\"hosted-main\" id=\"main-content\" tabindex=\"-1\"><div "
                       "class=\"hosted-card\">" +
                       std::string(content) + "</div></main>" + std::string(theme_toggle_script) +
                       "</body></html>";
    return html;
}

} // namespace

WebUiCrumb web_ui_entity_crumb(std::string_view value, std::string href) {
    const bool hex_like = value.size() >= 16 && std::ranges::all_of(value, [](char c) {
                              return std::isxdigit(static_cast<unsigned char>(c)) || c == '-';
                          });
    if (hex_like) {
        return {std::string(value.substr(0, 8)) + "…", std::move(href), std::string(value)};
    }
    return {std::string(value), std::move(href), {}};
}

std::string web_ui_breadcrumb(const std::vector<WebUiCrumb>& trail) {
    if (trail.empty()) {
        return {};
    }
    std::string out = "<nav class=\"breadcrumbs topbar-trail\" aria-label=\"Breadcrumb\"><ol>";
    for (std::size_t i = 0; i < trail.size(); ++i) {
        const WebUiCrumb& crumb = trail[i];
        const bool last = i + 1 == trail.size();
        const std::string hover = crumb.full.empty()
                                      ? std::string()
                                      : std::format(" title=\"{}\"", web_ui_escape(crumb.full));
        if (last) {
            out += std::format("<li><span aria-current=\"page\"{}>{}</span></li>",
                               hover,
                               web_ui_escape(crumb.label));
        } else if (crumb.href.empty()) {
            out += std::format("<li><span{}>{}</span></li>", hover, web_ui_escape(crumb.label));
        } else {
            out += std::format("<li><a href=\"{}\"{}>{}</a></li>",
                               web_ui_escape(crumb.href),
                               hover,
                               web_ui_escape(crumb.label));
        }
    }
    return out + "</ol></nav>";
}

std::string web_ui_shell(std::string_view title,
                         std::string_view meta,
                         std::string_view content,
                         WebUiViewer viewer) {
    return web_ui_shell(title, default_trail(viewer, title, meta), content, viewer);
}

std::string web_ui_shell(std::string_view title,
                         const std::vector<WebUiCrumb>& trail,
                         std::string_view content,
                         WebUiViewer viewer) {
    std::string selection;
    if (viewer.signed_in && !viewer.spaces.empty()) {
        selection = "<form method=\"get\" action=\"/workspace/select\" class=\"filters\">"
                    "<input type=\"hidden\" name=\"page\" value=\"" +
                    web_ui_escape(workspace_local_path(viewer.path)) + "\">"
                    "<input type=\"hidden\" name=\"page_query\" value=\"" + web_ui_escape(viewer.query) + "\">"
                    "<label>Space<select name=\"customer_id\" "
                    "onchange=\"this.form.elements.workspace_id.value='';this.form.submit()\">";
        for (const auto& [id, name] : viewer.spaces) {
            selection += "<option value=\"" + web_ui_escape(id) + "\"" +
                (id == viewer.customer_id ? " selected" : "") + ">" + web_ui_escape(name) + "</option>";
        }
        selection += "</select></label><label>Workspace<select name=\"workspace_id\" required>";
        if (viewer.workspace_id.empty()) {
            selection += "<option value=\"\">Choose a workspace</option>";
        }
        for (const auto& [id, name] : viewer.workspaces) {
            selection += "<option value=\"" + web_ui_escape(id) + "\"" +
                (id == viewer.workspace_id ? " selected" : "") + ">" + web_ui_escape(name) + "</option>";
        }
        selection += "</select></label><button type=\"submit\">Switch</button></form>"
                     "<a href=\"/workspaces?customer_id=" + web_ui_escape(viewer.customer_id) +
                     "\">New workspace</a>";
    }
    auto html = std::format(
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta name=\"color-scheme\" content=\"light dark\"><title>{}</title>{}{}</head><body>"
        "<a class=\"skip-link\" href=\"#main-content\">Skip to content</a>"
        "<div class=\"app-shell\"><aside class=\"sidebar\"><a class=\"brand\" href=\"/\">"
        "<span class=\"brand-mark\">{}</span><span>lake</span></a>{}"
        "</aside>"
        "<div class=\"app-frame\"><header class=\"topbar\"><div class=\"topbar-context\">{}</div>"
        "<div class=\"topbar-actions\"><button class=\"theme-toggle\" type=\"button\" "
        "data-theme-toggle aria-label=\"Toggle theme\">{}<span class=\"sr-only\">Toggle "
        "theme</span>"
        "</button>{}</div></header><main id=\"main-content\" tabindex=\"-1\">"
        "<div class=\"page-container\">{}</div></main></div></div>{}</body></html>",
        web_ui_escape(title),
        theme_preference_script,
        stylesheet_link(),
        web_ui_icon("lake"),
        nav(viewer, title),
        selection + web_ui_breadcrumb(trail),
        web_ui_icon("theme"),
        session_control(viewer),
        content,
        theme_toggle_script);
    return web_ui_workspace_document(std::move(html), viewer.workspace_id);
}

std::string web_ui_error(std::string_view code, std::string_view message, WebUiViewer viewer) {
    const std::string content = std::format(
        "<div class=\"state-page error-state\" role=\"alert\"><p class=\"eyebrow\">{}</p>"
        "<h1>We Could Not Load This Page</h1><p class=\"sub\">{}</p>"
        "<p><a class=\"button primary\" href=\"/repositories\">Back to repositories</a></p></div>",
        web_ui_escape(code),
        web_ui_escape(message));
    return web_ui_shell("lake — error", code, content, viewer);
}

std::string web_ui_loading(std::string_view subject, WebUiViewer viewer) {
    const std::string content =
        std::format("<div class=\"state-page loading-state\" role=\"status\" aria-live=\"polite\" "
                    "aria-busy=\"true\"><p class=\"eyebrow\">loading</p><h1>{}</h1>"
                    "<p class=\"sub\">Loading.</p><div class=\"loading-bars\" "
                    "aria-hidden=\"true\"><span></span><span></span><span></span></div></div>",
                    web_ui_escape(subject));
    return web_ui_shell("lake — loading", subject, content, viewer);
}

std::string web_ui_home(WebUiViewer viewer) {
    std::string content =
        "<p class=\"eyebrow\">lake by Orama</p>"
        "<h1>Design, train, and ship AI on your own data.</h1>"
        "<p class=\"sub\">Load your data into your own workspace, train models on it, and call "
        "them from your code, in Orama's cloud or your own.</p>";
    if (viewer.signed_in) {
        content += "<p class=\"actions\"><a class=\"button button-primary\" href=\"/docs\">"
                   "Getting started</a> <a class=\"button\" href=\"/train/data\">Load your data</a>"
                   "</p>";
    } else {
        content += "<p class=\"actions\"><a class=\"button button-primary\" "
                   "href=\"/sign-up\">Create account</a> <a class=\"button\" "
                   "href=\"/sign-in\">Sign in</a> <a class=\"button\" href=\"/docs\">Read the docs"
                   "</a></p>";
    }
    content += "<div class=\"grid\">"
               "<section class=\"card\"><h3>Design</h3>"
               "<p>Start from the business question and the decision it should improve. A "
               "project turns it into a pipeline you can read, change and approve.</p>"
               + std::string(viewer.signed_in ? "<p><a href=\"/design/projects\">Projects</a></p>" : "") +
               "</section>"
               "<section class=\"card\"><h3>Train</h3>"
               "<p>Your data lives in your workspace: upload a file, connect a database or "
               "generate a dataset, then train on it and read what each run found.</p>"
               + std::string(viewer.signed_in ? "<p><a href=\"/train/data\">Data</a> · <a "
                                                "href=\"/train/runs\">Runs</a></p>"
                                              : "") +
               "</section>"
               "<section class=\"card\"><h3>Ship</h3>"
               "<p>Publish a model and call it from your code, publish a search index, or run "
               "databases for your team. Files and code live in repositories.</p>"
               + std::string(viewer.signed_in ? "<p><a href=\"/ship/models\">Models</a> · <a "
                                                "href=\"/repositories\">Repositories</a></p>"
                                              : "");
    if (viewer.signed_in && !viewer.administrator) {
        content += "<p><a href=\"/tokens\">Create a token</a></p>";
    }
    content += "</section></div>";
    return web_ui_shell("lake — design, train, and ship AI", "Welcome to lake", content, viewer);
}

std::string web_ui_clerk_sign_in(const ClerkConfig& clerk,
                                 std::string_view mode,
                                 std::string_view error,
                                 bool signed_out,
                                 WebUiViewer viewer) {
    const bool sign_up = mode == "sign-up";
    std::string content = std::format(
        "<p class=\"eyebrow\">lake by Orama</p><h1>{}</h1><p class=\"sub\">{}</p>",
        sign_up ? "Create Your Account" : "Sign In",
        sign_up ? "Accounts are added by invitation while lake is in development. Ask the "
                  "person who invited you, or sign in if you already have access."
                : "Sign in with the email address you were invited with.");
    if (!error.empty()) {
        content += std::format("<p class=\"error\" role=\"alert\">{}</p>", web_ui_escape(error));
    }
    content += "<div id=\"clerk-mount\"></div>"
               "<p id=\"clerk-status\" class=\"muted\" role=\"status\" aria-live=\"polite\"></p>"
               "<noscript><p class=\"error\">Sign-in needs JavaScript.</p></noscript>";
    // clerk-js comes from the instance's own Frontend API host, the address Clerk documents for a
    // script-tag installation. After Clerk signs the person in, the page posts the `lake` template
    // token once as an ordinary form; the server verifies it and answers with a redirect that sets
    // lake_session. Nothing else in lake reads Clerk state.
    content += std::format(
        "<script async crossorigin=\"anonymous\" data-clerk-publishable-key=\"{0}\" "
        "src=\"https://{1}/npm/@clerk/clerk-js@5/dist/clerk.browser.js\"></script>"
        "<script id=\"clerk-sign-in\">window.addEventListener('load',async()=>{{"
        "const s=document.getElementById('clerk-status');"
        "try{{await window.Clerk.load();"
        "if({2}){{await window.Clerk.signOut();history.replaceState(null,'',location.pathname);}}"
        "let busy=false;const exchange=async()=>{{if(busy||!window.Clerk.session)return;busy=true;"
        "s.textContent='Signing you in.';"
        "const token=await window.Clerk.session.getToken({{template:'{3}'}});"
        "const f=document.createElement('form');f.method='post';f.action='/identity/clerk-session';"
        "for(const[k,v]of[['token',token],['device_name','browser']]){{"
        "const i=document.createElement('input');i.type='hidden';i.name=k;i.value=v;f.append(i);}}"
        "document.body.append(f);f.submit();}};"
        "if(window.Clerk.session){{await exchange();return;}}"
        "window.Clerk.addListener(({{session}})=>{{if(session)exchange();}});"
        "const m=document.getElementById('clerk-mount');"
        "{4}}}catch(e){{s.textContent='Sign-in could not load: '+e.message;}}}});</script>",
        web_ui_escape(clerk.publishable_key),
        web_ui_escape(clerk.frontend_api),
        signed_out ? "true" : "false",
        clerk_token_template,
        sign_up
            ? "window.Clerk.mountSignUp(m,{signInUrl:'/sign-in',forceRedirectUrl:'/sign-in'});"
            : "window.Clerk.mountSignIn(m,{signUpUrl:'/sign-up',forceRedirectUrl:'/sign-in'});");
    return web_ui_shell(sign_up ? "lake — create account" : "lake — sign in",
                        sign_up ? "Create account" : "Sign in",
                        content,
                        viewer);
}

std::string web_ui_login(std::string_view error, WebUiViewer viewer) {
    std::string content =
        "<p class=\"eyebrow\">Operator access</p><h1>Operator Sign In</h1>"
        "<p class=\"sub\">Use your operator credential to manage this Lake. "
        "For your own repositories, <a href=\"/sign-in\">sign in to your account</a>.</p>";
    if (!error.empty())
        content += std::format("<p class=\"error\">{}</p>", web_ui_escape(error));
    content += "<form class=\"login\" method=\"post\" action=\"/session\"><label>Operator "
               "credential<input "
               "type=\"password\" "
               "name=\"token\" "
               "placeholder=\"Operator credential\" autocomplete=\"current-password\" "
               "required></label><button "
               "type=\"submit\">Sign in</button></form>";
    return web_ui_shell("lake — operator sign in", "Operator access", content, viewer);
}

std::string web_ui_customers(const std::vector<control::Customer>& customers, WebUiViewer viewer) {
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
        "lake — customers", std::format("{} customers", customers.size()), content, viewer);
}

std::string web_ui_repositories(const std::vector<control::Repository>& repositories,
                                const std::map<std::string, control::Customer>& owners,
                                WebUiViewer viewer) {
    std::string content =
        viewer.signed_in
            ? "<p class=\"eyebrow\">Your work</p><h1>Repositories</h1>"
              "<p class=\"sub\">Repositories you can access, newest first.</p>"
            : "<p class=\"eyebrow\">Explore</p><h1>Public Repositories</h1>"
              "<p class=\"sub\">Browse code, files, and history shared publicly on this Lake.</p>";
    if (repositories.empty()) {
        content +=
            viewer.signed_in
                ? "<div class=\"panel empty\">No repositories yet. Create your first repository "
                  "to store files and review changes.</div>"
                : "<div class=\"panel empty\">No public repositories yet. "
                  "<a href=\"/sign-up\">Create an account</a> to start your own.</div>";
    } else {
        content += "<table><thead><tr><th>Repository</th><th>Owner</th><th>Visibility</"
                   "th><th>Description</th><th>Created</th></tr></thead><tbody>";
        for (const auto& r : repositories) {
            const auto owner = owners.find(r.customer_id);
            const std::string owner_name =
                owner == owners.end() ? r.customer_id : owner->second.name;
            const bool yours = owner != owners.end() && !viewer.user_id.empty() &&
                               owner->second.user_id == viewer.user_id;
            content += std::format("<tr><td>{}</td><td>{}</td><td><span "
                                   "class=\"badge\">{}</span></td><td>{}</td><td>{}</td></tr>",
                                   row_link("/repository?id=" + r.id, owner_name + "/" + r.name),
                                   row_link("/repositories?customer_id=" + r.customer_id,
                                            yours ? owner_name + " (you)" : owner_name),
                                   web_ui_escape(r.visibility),
                                   web_ui_escape(r.description),
                                   web_ui_escape(r.created_at));
        }
        content += "</tbody></table>";
    }
    if (viewer.signed_in && !owners.empty()) {
        content +=
            "<section class=\"panel\"><h2>Create a Repository</h2>"
            "<p>Your personal space owns your repositories. lake gave it a generated handle when "
            "you first signed in; it is marked <em>your personal space</em> below.</p>"
            "<form class=\"stack\" method=\"post\" action=\"/repository/create\">"
            "<label>Space<select name=\"customer_id\" required>";
        for (const auto& [id, owner] : owners) {
            const bool yours = !viewer.user_id.empty() && owner.user_id == viewer.user_id;
            content += "<option value=\"" + web_ui_escape(id) + "\"" + (yours ? " selected" : "") +
                       ">" + web_ui_escape(owner.name) + (yours ? " (your personal space)" : "") +
                       "</option>";
        }
        content +=
            "</select></label><label>Repository name<input name=\"name\" required "
            "pattern=\"[a-z0-9]([a-z0-9\\-]{0,254}[a-z0-9])?\" "
            "placeholder=\"my-first-repository\"></label>"
            "<p class=\"muted\">Use lowercase letters, numbers, and inner hyphens.</p>"
            "<label>Description<input name=\"description\"></label>"
            "<p>New repositories are private.</p>"
            "<button class=\"primary\" type=\"submit\">Create repository</button></form></section>";
    }
    return web_ui_shell("lake — repositories",
                        std::format("{} repositories", repositories.size()),
                        content,
                        viewer);
}

std::string web_ui_repository(const control::Repository& repository,
                              const control::Customer* owner,
                              const RepositoryHomeData& data,
                              WebUiViewer viewer) {
    const std::string owner_name = owner ? owner->name : repository.customer_id;
    const std::string default_ref = data.refs.empty() ? "main" : data.refs.front().name;
    std::string content = std::format(
        "<div class=\"forge-heading\"><div><p class=\"eyebrow\">repository</p>"
        "<h1>{}/{}</h1><p class=\"sub\">{}</p></div><span class=\"badge\">{}</span></div>{}"
        "<div class=\"repository-layout\"><div class=\"repository-main\">",
        web_ui_escape(owner_name),
        web_ui_escape(repository.name),
        web_ui_escape(repository.description.empty() ? "No description." : repository.description),
        web_ui_escape(repository.visibility),
        forge_tabs(repository, "code", viewer));
    if (!repository.deleted_at.empty()) {
        content += std::format("<div class=\"notice status-danger\"><strong>Tombstoned.</strong> "
                               "Deleted {}. Collection may reclaim unreferenced content after "
                               "{}.</div>",
                               web_ui_escape(repository.deleted_at),
                               web_ui_escape(repository.retention_until));
    }
    if (viewer.signed_in && repository.deleted_at.empty()) {
        content += std::format(
            "<section class=\"panel\"><div class=\"section-heading\"><div><h2>Build image</h2>"
            "<p class=\"muted\">Queue an in-cluster build from this repository.</p></div>"
            "<form method=\"post\" action=\"/repository/build\">"
            "<input type=\"hidden\" name=\"customer_id\" value=\"{}\">"
            "<input type=\"hidden\" name=\"repository\" value=\"{}\">"
            "<input type=\"hidden\" name=\"revision\" value=\"{}\">"
            "<input type=\"hidden\" name=\"target\" value=\"{}:latest\">"
            "<button class=\"primary\" type=\"submit\">Build image</button></form></div></section>",
            web_ui_escape(repository.customer_id),
            web_ui_escape(repository.id),
            web_ui_escape(default_ref),
            web_ui_escape(repository.name));
    }
    content +=
        "<section class=\"panel repository-browser\"><div class=\"section-heading\">"
        "<div><h2>Files</h2><p class=\"muted\">The published tree at <code>" +
        web_ui_escape(default_ref) + "</code>.</p></div>" +
        row_link("/repository/tree?id=" + repository.id + "&ref=" + url_component(default_ref),
                 "Browse tree") +
        "</div>";
    if (data.tree.empty()) {
        content += "<div class=\"empty compact-empty\">No files are published on this ref.</div>";
    } else {
        content += "<details class=\"tree-group\" open><summary>Repository root</summary>"
                   "<ul class=\"forge-tree\">";
        const std::size_t visible = std::min<std::size_t>(data.tree.size(), 12);
        for (std::size_t index = 0; index < visible; ++index) {
            const RepositoryTreeEntry& entry = data.tree[index];
            const std::string href = entry.kind == "blob"
                                         ? "/file?repository_id=" + repository.id +
                                               "&ref=" + url_component(default_ref) +
                                               "&path=" + url_component(entry.path)
                                         : "/repository/tree?id=" + repository.id +
                                               "&ref=" + url_component(default_ref) + "#" +
                                               url_component(entry.path);
            content += std::format("<li><span class=\"tree-kind\" aria-hidden=\"true\">{}</span>{}"
                                   "<span class=\"tree-size\">{}</span></li>",
                                   entry.kind == "blob" ? "·" : "›",
                                   row_link(href, entry.path),
                                   entry.kind == "blob" ? byte_count(entry.size) : "directory");
        }
        content += "</ul></details>";
    }
    content += "</section><section class=\"panel recent-changes\"><div class=\"section-heading\">"
               "<h2>Recent Changes</h2>" +
               row_link("/changes?repository_id=" + repository.id, "All changes") + "</div>";
    if (data.changes.empty()) {
        content += "<div class=\"empty compact-empty\">No changes have been cut.</div>";
    } else {
        content += "<ol class=\"change-list\">";
        const std::size_t visible = std::min<std::size_t>(data.changes.size(), 5);
        for (std::size_t index = 0; index < visible; ++index) {
            const RepositoryChange& change = data.changes[index];
            content += std::format(
                "<li><div><strong>{}</strong><span class=\"muted\">{} · {}</span></div>{}</li>",
                row_link("/change?repository_id=" + repository.id +
                             "&id=" + url_component(change.id),
                         change.message.empty() ? "Untitled change" : change.message),
                web_ui_escape(change.author),
                web_ui_escape(change.occurred_at),
                identifier_markup(change.id));
        }
        content += "</ol>";
    }
    content += "</section><section class=\"panel readme-panel\"><div class=\"section-heading\">"
               "<h2>README</h2></div>";
    if (data.readme) {
        content += web_ui_markdown(data.readme->contents);
    } else {
        content += "<div class=\"empty compact-empty\">Add a README.md to explain this "
                   "repository.</div>";
    }
    content += "</section></div><aside class=\"repository-sidebar\" aria-label=\"Repository "
               "details\"><section class=\"panel\"><h2>Clone and Push</h2>";
    if (data.external_origin.empty()) {
        content += "<p class=\"muted\">An administrator must set "
                   "<code>LAKE_EXTERNAL_ORIGIN</code> or <code>LAKE_PUBLIC_HOST</code> before "
                   "clone and push commands are available.</p>";
    } else {
        const std::string url =
            shell_argument(data.external_origin + "/v1/repositories/" + repository.id);
        const std::string directory = shell_argument("./" + repository.name);
        content +=
            "<p class=\"muted\">Create an account token on this instance under "
            "<a href=\"/tokens\">Tokens</a> and export it as <code>LIQUID_TOKEN</code> "
            "in your terminal. Use a token issued by this instance only.</p>"
            "<p>First checkout into a new directory:</p><pre><code>" +
            web_ui_escape("liquid clone " + url + " " + directory + "\ncd " + directory) +
            "</code></pre><p>From that checkout, after capturing and naming your changes:</p>"
            "<pre><code>" +
            web_ui_escape("liquid push " + url) + "</code></pre>";
    }
    content += "</section>";
    content += "<section class=\"panel\"><h2>References</h2>";
    if (data.refs.empty()) {
        content += "<p class=\"muted\">No references are published.</p>";
    } else {
        content += "<ul class=\"detail-list\">";
        for (std::size_t index = 0; index < data.refs.size(); ++index) {
            const RepositoryContentRef& ref = data.refs[index];
            content += std::format(
                "<li><div>{}{}</div>{}</li>",
                row_link("/repository/tree?id=" + repository.id + "&ref=" + url_component(ref.name),
                         ref.name),
                index == 0 ? " <span class=\"badge\">default</span>" : "",
                identifier_markup(ref.change));
        }
        content += "</ul>";
    }
    if (!data.releases.empty()) {
        content += "<h3>Releases</h3><ul class=\"detail-list\">";
        for (const control::ForgeRecord& release : data.releases) {
            content += std::format("<li><strong>{}</strong>{}</li>",
                                   web_ui_escape(release.value.value("tag", "untagged")),
                                   identifier_markup(release.value.value("change_id", "")));
        }
        content += "</ul>";
    }
    content += "</section><section class=\"panel\"><h2>Languages</h2>";
    std::map<std::string, std::uint64_t> language_bytes;
    std::uint64_t total_bytes = 0;
    for (const RepositoryTreeEntry& entry : data.tree) {
        if (entry.kind != "blob") {
            continue;
        }
        language_bytes[file_language(entry.path)] += entry.size;
        total_bytes += entry.size;
    }
    if (language_bytes.empty()) {
        content += "<p class=\"muted\">No indexed file sizes yet.</p>";
    } else {
        content += "<ul class=\"language-summary\">";
        for (const auto& [language, bytes] : language_bytes) {
            content += std::format("<li><span>{}</span><strong>{}</strong></li>",
                                   web_ui_escape(language),
                                   byte_count(bytes));
        }
        content += std::format("</ul><p class=\"muted\">{} indexed across {} files.</p>",
                               byte_count(total_bytes),
                               std::ranges::count_if(data.tree, [](const auto& entry) {
                                   return entry.kind == "blob";
                               }));
    }
    content += "</section><section class=\"panel\"><h2>Effective Access</h2>";
    if (data.access.empty()) {
        content += "<p class=\"muted\">No direct or inherited grants are visible.</p>";
    } else {
        content += "<ul class=\"detail-list\">";
        for (const control::EffectiveAccessEntry& entry : data.access) {
            content += std::format("<li>{}{}</li>",
                                   identifier_markup(entry.principal_taap_id),
                                   badge_markup(entry.role));
        }
        content += "</ul>";
    }
    content += std::format(
        "<p>{}</p></section><section class=\"panel repository-meta\"><h2>About</h2><dl>"
        "<dt>Identifier</dt><dd>{}</dd><dt>Owner</dt><dd>{}</dd><dt>Created</dt><dd>{}</dd>"
        "<dt>Updated</dt><dd>{}</dd></dl></section></aside></div>",
        viewer.signed_in ? row_link("/repository/access?id=" + repository.id, "View access details")
                         : "",
        identifier_markup(repository.id),
        row_link("/repositories?customer_id=" + repository.customer_id, owner_name),
        web_ui_escape(repository.created_at),
        web_ui_escape(repository.updated_at));
    return web_ui_shell("lake — " + owner_name + "/" + repository.name,
                        owner_name + "/" + repository.name,
                        content,
                        viewer);
}

std::string web_ui_repository(const control::Repository& repository,
                              const control::Customer* owner,
                              const std::vector<RepositoryContentRef>& refs,
                              WebUiViewer viewer) {
    RepositoryHomeData data;
    data.refs = refs;
    return web_ui_repository(repository, owner, data, viewer);
}

namespace {

std::string platform_text(const nlohmann::json& value, std::string_view key) {
    const auto found = value.find(key);
    if (found == value.end() || found->is_null()) {
        return {};
    }
    return found->is_string() ? found->get<std::string>() : found->dump();
}

std::string platform_stale_banner(std::string_view stale_since) {
    if (stale_since.empty()) {
        return {};
    }
    return std::format(
        "<div class=\"notice status-warning\" role=\"status\"><strong>Platform data is "
        "stale.</strong> The operator control plane has been unreachable since {}. Writes are "
        "disabled.</div>",
        web_ui_escape(stale_since));
}

std::string short_image(std::string_view image) {
    const std::size_t digest = image.find("sha256:");
    if (digest == std::string_view::npos) {
        return abbreviated_identifier(image);
    }
    const std::size_t end = std::min(image.size(), digest + 19);
    return std::string(image.substr(digest, end - digest));
}

std::string platform_url(std::string_view url) {
    if (!url.starts_with("https://") && !url.starts_with("http://")) {
        return web_ui_escape(url);
    }
    return row_link(url, url);
}

std::string platform_heading(const control::Repository& repository,
                             std::string_view eyebrow,
                             std::string_view title,
                             std::string_view current,
                             WebUiViewer viewer) {
    return std::format("<p class=\"eyebrow\">{}</p><h1>{}</h1>{}",
                       web_ui_escape(eyebrow),
                       web_ui_escape(title),
                       forge_tabs(repository, current, viewer));
}

} // namespace

namespace {

// A repository page's breadcrumb (docs/ui/breadcrumb.md): the sidebar's Repositories, the
// repository (linking to its home), then what this page is.
std::string repository_shell(std::string_view title,
                             const control::Repository& repository,
                             std::string_view page,
                             std::string_view content,
                             WebUiViewer viewer) {
    std::vector<WebUiCrumb> trail = default_trail(viewer, title, "");
    trail.push_back(web_ui_entity_crumb(repository_name(repository),
                                        "/repository?id=" + url_component(repository.id)));
    trail.push_back({std::string(page), {}, {}});
    return web_ui_shell(title, trail, content, viewer);
}

struct WorkspaceSection {
    std::string_view key, verb, title, path, sub;
};

// The Design, Train and Ship pages, in navigation order. `path` is the list page a detail page
// belongs to.
constexpr std::array<WorkspaceSection, 23> workspace_sections{{
    {"workspaces", "Ship", "Workspaces", "/workspaces", "Isolated environments for your space."},
    {"projects",
     "Design",
     "Projects",
     "/design/projects",
     "A project starts from the business question: the decision it should improve and how you "
     "will measure it. Its pipelines are built from that intent."},
    {"discover",
     "Design",
     "Opportunities",
     "/design/opportunities",
     "What your data could be used for. Auto-Suggest reads the data your workspace can reach and "
     "recommends what to build, from Orama's solutions catalogue; it never changes your data. "
     "Turn an Opportunity into a project, or dismiss it."},
    {"solutions",
     "Design",
     "Solutions",
     "/design/solutions",
     "Orama's solutions catalogue: each module answers one business problem end to end, from the "
     "methodologies it composes to the decision it drives and how its return is measured. Start "
     "one and it becomes a project in your workspace."},
    {"solution", "Design", "Solution", "/design/solutions", ""},
    {"methodologies",
     "Design",
     "Methodologies",
     "/design/methodologies",
     "The modelling methods your workspace's engine knows: what each needs, what it makes, how it "
     "is measured, and whether it runs here today. Solutions compose them."},
    {"methodology", "Design", "Methodology", "/design/methodologies", ""},
    {"library",
     "Design",
     "Agent Library",
     "/design/agent-library",
     "Agents Orama provides, each for one job. Use them where they appear, or ask Orama and it "
     "brings in the right one."},
    {"project", "Design", "Project", "/design/projects", ""},
    {"pipeline",
     "Design",
     "Pipeline",
     "/design/projects",
     "The steps Zetta runs, how data flows between them, and what the last run did. Change it "
     "here; a changed pipeline is approved again before it runs."},
    {"agents",
     "Train",
     "Agents",
     "/train/agents",
     "Build agents that work for you, run them, and see what they did. Talk to any of them in "
     "Ask Orama."},
    {"agent", "Train", "Agent", "/train/agents", ""},
    {"data",
     "Train",
     "Data",
     "/train/data",
     "The connections and datasets your workspace trains on."},
    {"dataset", "Train", "Dataset", "/train/data", ""},
    {"query",
     "Train",
     "Query",
     "/train/data",
     "Read-only SQL over your workspace's data: one SELECT or WITH query at a time."},
    {"runs",
     "Train",
     "Runs",
     "/train/runs",
     "Every pipeline run: what it trained, its status, and its report."},
    {"run", "Train", "Run", "/train/runs", ""},
    {"models",
     "Ship",
     "Models",
     "/ship/models",
     "Trained models, and whether each is published as a prediction endpoint."},
    {"indexes",
     "Ship",
     "Search Indexes",
     "/ship/indexes",
     "Search indexes over your data, and whether each is published for queries."},
    {"index",
     "Ship",
     "Search Index",
     "/ship/indexes",
     "Search it here, publish it, and call it from your own code with a key that can only "
     "search."},
    {"apps",
     "Ship",
     "Apps",
     "/ship/apps",
     "Create, deploy and operate customer applications with live rollout status."},
    {"app", "Ship", "App", "/ship/apps", ""},
    {"services",
     "Ship",
     "Services",
     "/services",
     "The databases, streams and engines this space runs on lake: Postgres, Zetta, Kafka, "
     "ClickHouse, TiDB, Cassandra and Valkey, each its own, with its data kept apart from every "
     "other customer's. Your workspace is the Zetta that Design, Train and Ship use."},
}};

const WorkspaceSection& workspace_section(std::string_view key) {
    for (const WorkspaceSection& section : workspace_sections) {
        if (section.key == key) {
            return section;
        }
    }
    return workspace_sections.front();
}

std::string json_text(const nlohmann::json& value, std::string_view key) {
    const auto found = value.find(std::string(key));
    if (found == value.end() || found->is_null()) {
        return {};
    }
    return found->is_string() ? found->get<std::string>() : found->dump();
}

std::string customer_query(std::string_view customer_id) {
    return "customer_id=" + url_component(customer_id);
}

std::string hidden(std::string_view name, std::string_view value) {
    return std::format("<input type=\"hidden\" name=\"{}\" value=\"{}\">",
                       web_ui_escape(name),
                       web_ui_escape(value));
}

std::string
workspace_table(const nlohmann::json& rows,
                std::initializer_list<std::pair<std::string_view, std::string_view>> columns) {
    std::string output = "<table><thead><tr>";
    for (const auto& [key, label] : columns) {
        output += "<th>" + web_ui_escape(label) + "</th>";
    }
    output += "</tr></thead><tbody>";
    for (const nlohmann::json& row : rows) {
        output += "<tr>";
        for (const auto& [key, label] : columns) {
            output += "<td>" + web_ui_escape(json_text(row, key)) + "</td>";
        }
        output += "</tr>";
    }
    return output + "</tbody></table>";
}

// Lifecycle actions live on the control-plane page, including failed provisioning requests.
std::string workspace_delete_form(const WorkspacePage& page,
                                  std::string_view heading,
                                  std::string_view explanation,
                                  std::string_view button) {
    return "<section class=\"panel\"><h2>" + web_ui_escape(heading) + "</h2><p>" +
        web_ui_escape(explanation) + "</p><a href=\"/workspaces?customer_id=" +
        web_ui_escape(page.customer_id) + "\">" + web_ui_escape(button) + "</a></section>";
}

std::string workspace_gate(const WorkspacePage& page, const WorkspaceSection& section) {
    const nlohmann::json& state = page.state;
    if (state.value("requested", false)) {
        const std::string phase = state.value("phase", std::string());
        if (phase == "Failed") {
            const std::string message = state.value("message", std::string());
            // The operator refuses a new workspace once the Lake holds spec.workspaces.
            // maxWorkspaces of them, and says so as "workspace quota reached (<n>)".
            const bool quota = message.starts_with("workspace quota reached");
            return std::format(
                       "<div class=\"panel notice status-danger\"><h2>Your Workspace Could Not "
                       "Be Prepared</h2><p>{}</p></div>",
                       quota ? "This lake already runs as many workspaces as it allows (" +
                                   web_ui_escape(message) +
                                   "). An operator can raise the limit; yours starts as soon as "
                                   "there is room, or cancel the request below."
                             : web_ui_escape(message)) +
                   workspace_delete_form(page,
                                         "Cancel This Workspace",
                                         "Withdraw the request so you can start again later.",
                                         "Cancel workspace");
        }
        return std::format(
            "<div class=\"panel\" role=\"status\"><h2>Preparing Your Workspace</h2><p>lake asked "
            "for a Zetta cluster of your own, where your projects, data and models live apart "
            "from every other customer's. {}</p><p class=\"muted\">This page checks again every "
            "15 seconds.</p><meta http-equiv=\"refresh\" content=\"15\"></div>",
            phase.empty() ? "It has not been picked up yet."
                          : "Status: " + web_ui_escape(phase) + ".");
    }
    return std::format(
        "<div class=\"panel\"><h2>Start Your Workspace</h2><p>Design, Train and Ship run on a "
        "Zetta cluster of your own, so your projects, data and models are never shared with "
        "another customer. It takes a minute or two to prepare.</p>"
        "<form method=\"post\" action=\"/workspace/start\">{}{}"
        "<button class=\"button button-primary\" type=\"submit\">Start your workspace</button>"
        "</form></div>",
        hidden("customer_id", page.customer_id),
        hidden("return", section.path));
}

// Seconds since a UTC timestamp, ISO-8601 or Postgres text ("2026-09-26T03:04:00Z",
// "2026-09-26 03:04:00.12+00"; fractions ignored), or -1.
long seconds_since(std::string_view timestamp) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    const std::string text(timestamp.substr(0, 19));
    if (std::sscanf(text.c_str(),
                    "%d-%d-%d%*c%d:%d:%d",
                    &year,
                    &month,
                    &day,
                    &hour,
                    &minute,
                    &second) != 6) {
        return -1;
    }
    const std::chrono::sys_days date{std::chrono::year{year} / month / day};
    const auto at = date + std::chrono::hours{hour} + std::chrono::minutes{minute} +
                    std::chrono::seconds{second};
    const auto elapsed =
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now() - at);
    return elapsed.count() < 0 ? 0 : static_cast<long>(elapsed.count());
}

// "40 seconds", "2 minutes", "7 minutes": a duration as a person says it.
std::string spoken_duration(long seconds) {
    if (seconds < 90) {
        return std::format("{} seconds", seconds);
    }
    return std::format("{} minutes", (seconds + 30) / 60);
}

// What a starting service is waiting for (the operator's message, shown above this) and how long
// its kind usually takes, so nobody wonders whether a Cassandra still starting is stuck.
std::string service_start_hint(const nlohmann::json& service, std::string_view phase) {
    if (phase != "Waiting for the operator" && phase != "Provisioning" && phase != "Connecting") {
        return {};
    }
    const control::ServiceKind* kind = control::find_service_kind(json_text(service, "kind"));
    if (kind == nullptr) {
        return {};
    }
    const long elapsed = seconds_since(json_text(service, "created_at"));
    std::string hint = "Usually ready in about " + spoken_duration(kind->ready_seconds);
    if (elapsed >= 0) {
        hint += "; started " + spoken_duration(elapsed) + " ago";
        if (elapsed > 2 * kind->ready_seconds) {
            hint += ". This is taking longer than usual; the message above says what it is "
                    "waiting for";
        }
    }
    return "<br><span class=\"muted\">" + web_ui_escape(hint) + ".</span>";
}

std::string service_phase(const nlohmann::json& service) {
    const std::string phase = json_text(service, "phase");
    if (json_text(service, "desired") == "absent") {
        return phase.empty() || phase == "Ready" ? "Deleting" : phase;
    }
    if (phase == "Ready" && !service.value("connected", true)) {
        return "Connecting";
    }
    return phase.empty() ? "Waiting for the operator" : phase;
}

// Services: every Zetta and Postgres cluster of the customer (docs/api/services.md). Never shows a
// credential except the one Postgres connection the viewer asked for, on the response to that post.
std::string services_content(const WorkspacePage& page) {
    // Zetta services first (the workspace is one), then the rest in the order lake lists them.
    nlohmann::json services = nlohmann::json::array();
    const nlohmann::json listed = page.data.value("list_services", nlohmann::json::object())
                                      .value("services", nlohmann::json::array());
    for (const bool zetta : {true, false}) {
        for (const nlohmann::json& service : listed) {
            if ((json_text(service, "kind") == "zetta") == zetta) {
                services.push_back(service);
            }
        }
    }
    std::string out;
    bool settling = false;
    if (const auto connection = page.data.find("service_connection");
        connection != page.data.end()) {
        out += std::format(
            "<section class=\"panel notice\" role=\"status\"><h2>Connection to {}</h2><p>Reachable "
            "inside the lake cluster, from your other services. Copy it now; it is not shown "
            "again unless you ask.</p>{}</section>",
            web_ui_escape(json_text(*connection, "name")),
            [&] {
                std::string rows = "<dl>";
                for (const auto& [key, value] : connection->items()) {
                    if (key == "name") {
                        continue;
                    }
                    rows +=
                        "<dt>" + web_ui_escape(key) + "</dt><dd><code>" +
                        web_ui_escape(value.is_string() ? value.get<std::string>() : value.dump()) +
                        "</code></dd>";
                }
                return rows + "</dl>";
            }());
    }
    if (services.empty()) {
        out += "<div class=\"panel empty\">No services yet.</div>";
    } else {
        out += "<table><thead><tr><th>Name</th><th>Kind</th><th>Size</th><th>Status</th>"
               "<th>Address</th><th></th></tr></thead><tbody>";
        for (const nlohmann::json& service : services) {
            const std::string id = json_text(service, "id");
            const std::string name = json_text(service, "name");
            const std::string kind = json_text(service, "kind");
            const std::string phase = service_phase(service);
            const bool deleting = json_text(service, "desired") == "absent";
            settling = settling || (phase != "Ready" && phase != "Failed");
            std::string actions;
            if (!deleting && kind != "zetta" && phase == "Ready") {
                actions += std::format("<form method=\"post\" action=\"/services/connection\">{}{}"
                                       "<button type=\"submit\">Show connection</button></form>",
                                       hidden("customer_id", page.customer_id),
                                       hidden("service_id", id));
            }
            std::string used_by;
            for (const nlohmann::json& connection :
                 page.data.value("list_connections", nlohmann::json::object())
                     .value("connections", nlohmann::json::array())) {
                if (json_text(connection, "name") == name) {
                    used_by =
                        "<p class=\"notice status-warning\">Train → Data's connection <code>" +
                        web_ui_escape(name) + "</code> uses it and stops working.</p>";
                }
            }
            if (!deleting) {
                actions += std::format(
                    "<details><summary>Delete</summary><form class=\"stack\" method=\"post\" "
                    "action=\"/services/delete\">{}{}{}<label>Type <code>{}</code> to delete it "
                    "and all of its data, which cannot be recovered<input name=\"confirm\" "
                    "required autocomplete=\"off\"></label><button class=\"button button-danger\" "
                    "type=\"submit\">Delete {}</button></form></details>",
                    hidden("customer_id", page.customer_id),
                    hidden("service_id", id),
                    used_by,
                    web_ui_escape(name),
                    web_ui_escape(name));
            }
            out += std::format(
                "<tr><td>{}{}</td><td>{}</td><td>{} × {} GiB</td><td>{}{}</td><td><code>{}</code>"
                "</td><td>{}</td></tr>",
                web_ui_escape(name),
                service.value("default", false) ? " <span class=\"muted\">(workspace)</span>" : "",
                [&] {
                    const control::ServiceKind* known = control::find_service_kind(kind);
                    return known == nullptr
                               ? web_ui_escape(kind)
                               : std::string(known->label);
                }(),
                web_ui_escape(json_text(service, "instances")),
                web_ui_escape(json_text(service, "storage_gi")),
                web_ui_escape(phase),
                (json_text(service, "message").empty()
                     ? std::string()
                     : "<br><span class=\"muted\">" + web_ui_escape(json_text(service, "message")) +
                           "</span>") +
                    (deleting ? std::string() : service_start_hint(service, phase)),
                web_ui_escape(json_text(service, "endpoint")),
                actions);
        }
        out += "</tbody></table>";
    }
    if (settling) {
        out += "<p class=\"muted\">This page checks again every 3 seconds while a service is "
               "changing.</p><meta http-equiv=\"refresh\" content=\"3\">";
    }
    out += std::format(
        "<section class=\"panel\"><h2>New Service</h2><form class=\"stack\" method=\"post\" "
        "action=\"/services/create\">{}<label>Kind<select name=\"kind\">{}</select>"
        "</label><label>Name<input name=\"name\" required pattern=\"[a-z][a-z0-9\\-]{{0,30}}\" "
        "placeholder=\"crm\"></label><label>Instances<select name=\"instances\"><option>1"
        "</option><option>2</option><option>3</option></select></label><label>Storage (GiB)"
        "<input name=\"storage_gi\" type=\"number\" min=\"1\" max=\"100\" value=\"10\">"
        "</label><p class=\"muted\">{}Each kind's usual start time is next to it.</p><button "
        "class=\"button button-primary\" type=\"submit\">Create service</button></form>"
        "</section>",
        hidden("customer_id", page.customer_id),
        [] {
            std::string options;
            for (const control::ServiceKind& kind : control::service_kinds) {
                options += std::format("<option value=\"{}\"{}>{} — ready in about {}</option>",
                                       kind.key,
                                       kind.key == "zetta" ? " selected" : "",
                                       kind.label,
                                       spoken_duration(kind.ready_seconds));
            }
            return options;
        }(),
        [&] {
            // No limit (0, the default) says nothing; a configured one is stated.
            const std::string limit =
                json_text(page.data.value("list_services", nlohmann::json::object()), "limit");
            return limit.empty() || limit == "0"
                       ? std::string()
                       : "This space may run " + web_ui_escape(limit) + " services at once. ";
        }());
    return out;
}

// "Volume" and "cluster disk" from the provider table's "Volume — cluster disk"; the raw keys for a
// pair lake does not know.
std::pair<std::string, std::string> store_kind_words(std::string_view type,
                                                     std::string_view provider) {
    const control::StoreProvider* known = control::find_store_provider(type, provider);
    if (known == nullptr) {
        return {std::string(type), std::string(provider)};
    }
    constexpr std::string_view dash = " — ";
    const std::size_t at = known->label.find(dash);
    if (at == std::string_view::npos) {
        return {std::string(known->label), std::string(provider)};
    }
    return {std::string(known->label.substr(0, at)),
            std::string(known->label.substr(at + dash.size()))};
}

std::string store_phase(const nlohmann::json& store) {
    const std::string phase = json_text(store, "phase");
    if (json_text(store, "desired") == "absent") {
        return phase.empty() || phase == "Ready" ? "Deleting" : phase;
    }
    if (phase == "Ready" && !store.value("connected", true)) {
        return "Connecting";
    }
    return phase.empty() ? "Waiting for the operator" : phase;
}

// Train → Data → Storage (docs/api/storage.md, lake#196): the customer's stores, how to reach a
// Ready one, and a new one. Shown whether or not the workspace is ready; stores do not need it.
std::string storage_content(const WorkspacePage& page) {
    const nlohmann::json stores = page.data.value("list_stores", nlohmann::json::object())
                                      .value("stores", nlohmann::json::array());
    std::string out = "<section class=\"panel\" id=\"storage\"><h2>Storage</h2><p "
                      "class=\"muted\">Buckets, filesystems and volumes for your services. "
                      "Private to your space.</p>";
    bool settling = false;
    if (stores.empty()) {
        out += "<p>No storage yet.</p>";
    } else {
        out += "<table><thead><tr><th>Name</th><th>Type</th><th>Provider</th><th>Size</th>"
               "<th>Status</th><th>Endpoint</th><th></th></tr></thead><tbody>";
        for (const nlohmann::json& store : stores) {
            const std::string id = json_text(store, "id");
            const std::string name = json_text(store, "name");
            const std::string phase = store_phase(store);
            const bool deleting = json_text(store, "desired") == "absent";
            settling = settling || (phase != "Ready" && phase != "Failed");
            const auto [type, provider] =
                store_kind_words(json_text(store, "type"), json_text(store, "provider"));
            std::string actions;
            if (!deleting && phase == "Ready") {
                actions +=
                    std::format("<form method=\"post\" action=\"/train/data/store-connection\">{}{}"
                                "<button type=\"submit\">Show connection</button></form>",
                                hidden("customer_id", page.customer_id),
                                hidden("store_id", id));
            }
            if (!deleting) {
                actions += std::format(
                    "<details><summary>Delete</summary><form class=\"stack\" method=\"post\" "
                    "action=\"/train/data/store-delete\">{}{}<p class=\"notice "
                    "status-warning\">Deleting a store deletes the data in it, which cannot be "
                    "recovered.</p><label>Type <code>{}</code> to delete it<input "
                    "name=\"confirm\" required autocomplete=\"off\"></label><button "
                    "class=\"button button-danger\" type=\"submit\">Delete {}</button></form>"
                    "</details>",
                    hidden("customer_id", page.customer_id),
                    hidden("store_id", id),
                    web_ui_escape(name),
                    web_ui_escape(name));
            }
            out += std::format(
                "<tr><td>{}</td><td>{}</td><td>{}</td><td>{} GiB</td><td>{}{}</td><td><code>{}"
                "</code></td><td>{}</td></tr>",
                web_ui_escape(name),
                web_ui_escape(type),
                web_ui_escape(provider),
                web_ui_escape(json_text(store, "size_gi")),
                web_ui_escape(phase),
                json_text(store, "message").empty()
                    ? std::string()
                    : "<br><span class=\"muted\">" + web_ui_escape(json_text(store, "message")) +
                          "</span>",
                web_ui_escape(json_text(store, "endpoint")),
                actions);
        }
        out += "</tbody></table>";
    }
    if (settling) {
        out += "<p class=\"muted\">This page checks again every 3 seconds while storage is "
               "changing.</p><meta http-equiv=\"refresh\" content=\"3\">";
    }
    // Only what can be created today; the rest is one plain line, without internal plans
    // (lake#257).
    std::string options;
    bool chosen = false;
    bool more = false;
    for (const control::StoreProvider& entry : control::store_providers) {
        if (!entry.available) {
            more = true;
            continue;
        }
        options += std::format("<option value=\"{}:{}\"{}>{}</option>",
                               entry.type,
                               entry.provider,
                               chosen ? "" : " selected",
                               web_ui_escape(std::string(entry.label)));
        chosen = true;
    }
    const std::string later = more ? "<small>Buckets (Cloudflare R2, AWS S3, Backblaze B2, "
                                     "Azure, Google Cloud) and shared filesystems are coming "
                                     "later.</small>"
                                   : "";
    out += std::format(
        "<h3>New Storage</h3><form class=\"stack\" method=\"post\" "
        "action=\"/train/data/store-create\">{}<label>Type and provider<select "
        "name=\"store\">{}</select>{}</label><label>Name<input name=\"name\" required "
        "pattern=\"[a-z][a-z0-9\\-]{{0,30}}\" placeholder=\"uploads\"></label><label>Size (GiB)"
        "<input name=\"size_gi\" type=\"number\" min=\"1\" max=\"100\" value=\"10\"></label>"
        "<button class=\"button button-primary\" type=\"submit\">Create storage</button></form>"
        "</section>",
        hidden("customer_id", page.customer_id),
        options,
        later);
    return out;
}

// How to reach one store, on the response to Show connection only; never stored.
std::string store_connection_notice(const WorkspacePage& page) {
    const auto connection = page.data.find("store_connection");
    if (connection == page.data.end()) {
        return {};
    }
    std::string rows = "<dl>";
    for (const auto& [key, value] : connection->items()) {
        if (key == "name") {
            continue;
        }
        rows += "<dt>" + web_ui_escape(key) + "</dt><dd><code>" +
                web_ui_escape(value.is_string() ? value.get<std::string>() : value.dump()) +
                "</code></dd>";
    }
    return std::format(
        "<section class=\"panel notice\" role=\"status\"><h2>Connection to {}</h2><p>What a "
        "service in your space mounts or calls to use it. It is not shown again unless you "
        "ask.</p>{}</dl></section>",
        web_ui_escape(json_text(*connection, "name")),
        rows);
}

// A value as table text: strings bare, numbers trimmed, everything else as compact JSON.
std::string cell_text(const nlohmann::json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_float()) {
        return std::format("{:.4g}", value.get<double>());
    }
    return value.is_null() ? std::string() : value.dump();
}

// A list of objects as a table, columns from the first row's keys.
std::string object_table(const nlohmann::json& rows) {
    if (!rows.is_array() || rows.empty()) {
        return {};
    }
    if (!rows.front().is_object()) {
        std::string out = "<ul>";
        for (const nlohmann::json& row : rows) {
            out += "<li>" + web_ui_escape(cell_text(row)) + "</li>";
        }
        return out + "</ul>";
    }
    std::vector<std::string> columns;
    for (const auto& [key, value] : rows.front().items()) {
        columns.push_back(key);
    }
    std::string out = "<table><thead><tr>";
    for (const std::string& column : columns) {
        out += "<th>" + web_ui_escape(column) + "</th>";
    }
    out += "</tr></thead><tbody>";
    for (const nlohmann::json& row : rows) {
        out += "<tr>";
        for (const std::string& column : columns) {
            out += "<td>" +
                   web_ui_escape(row.is_object() && row.contains(column) ? cell_text(row.at(column))
                                                                         : std::string()) +
                   "</td>";
        }
        out += "</tr>";
    }
    return out + "</tbody></table>";
}

// get_run_report as a reader wants it: what the model learned from, what it leans on, who is at
// risk, and what not to believe — the raw document stays one click away.
std::string run_report_panel(const nlohmann::json& report) {
    std::string out = "<section class=\"panel\"><h2>Report</h2>";
    if (const auto dataset = report.find("dataset");
        dataset != report.end() && dataset->is_object()) {
        out += "<h3>What It Learned From</h3><dl>";
        for (const auto& [key, value] : dataset->items()) {
            const std::string label = key == "source"     ? std::string("trained on")
                                      : key == "not_used" ? std::string("columns not used")
                                                          : key;
            out += "<dt>" + web_ui_escape(label) + "</dt><dd>" +
                   web_ui_escape(value.is_array()
                                     ? [&] {
                                           std::string joined;
                                           for (const auto& item : value) {
                                               joined += (joined.empty() ? "" : ", ") + cell_text(item);
                                           }
                                           return joined;
                                       }()
                                     : cell_text(value)) +
                   "</dd>";
        }
        out += "</dl>";
    }
    const auto section = [&](const char* key, const char* heading, const char* note) {
        const auto found = report.find(key);
        if (found == report.end() || !found->is_array() || found->empty()) {
            return std::string();
        }
        return std::format("<h3>{}</h3>{}{}",
                           heading,
                           note[0] ? std::format("<p class=\"muted\">{}</p>", note) : "",
                           object_table(*found));
    };
    out += section(
        "drivers", "What the Model Leans On", "A weight is what the model relied on, not a cause.");
    out += section("at_risk", "Most at Risk", "");
    out += section("caveats", "Do Not Read These as Results", "");
    out += section("missing", "What This Run Cannot Tell You", "");
    return out + std::format("<details><summary>The raw report</summary><pre>{}</pre></details>"
                             "</section>",
                             web_ui_escape(report.dump(2)));
}

// get_run's logs, outputs and lineage, and the spans that mention the run.
std::string run_details_panel(const nlohmann::json& run,
                              const nlohmann::json& traces,
                              std::string_view run_id) {
    std::string out;
    if (const auto lineage = run.find("lineage"); lineage != run.end() && lineage->is_object()) {
        out += "<section class=\"panel\"><h2>Lineage</h2><dl>";
        for (const auto& [key, value] : lineage->items()) {
            std::string text;
            if (value.is_array()) {
                for (const auto& item : value) {
                    text += (text.empty() ? "" : " → ") + cell_text(item);
                }
            } else {
                text = cell_text(value);
            }
            out += "<dt>" + web_ui_escape(key) + "</dt><dd>" + web_ui_escape(text) + "</dd>";
        }
        out += "</dl></section>";
    }
    if (const auto outputs = run.find("outputs"); outputs != run.end() && !outputs->empty()) {
        out += "<section class=\"panel\"><h2>Outputs</h2>" + object_table(*outputs) + "</section>";
    }
    if (const auto logs = run.find("logs"); logs != run.end() && !logs->empty()) {
        out += "<section class=\"panel\"><h2>Log</h2>" + object_table(*logs) + "</section>";
    }
    // The trace: spans whose attributes name this run, longest bar scaled to the widest.
    std::vector<nlohmann::json> spans;
    double longest = 0;
    for (const nlohmann::json& span : traces.value("spans", nlohmann::json::array())) {
        bool mentions = false;
        for (const auto& [key, value] :
             span.value("attributes", nlohmann::json::object()).items()) {
            mentions = mentions || (value.is_string() && value.get<std::string>() == run_id);
        }
        if (mentions) {
            spans.push_back(span);
            longest = std::max(longest, span.value("duration_ms", 0.0));
        }
    }
    if (!spans.empty()) {
        out += "<section class=\"panel\"><h2>Trace</h2><table><thead><tr><th>Span</th><th>Time"
               "</th><th></th></tr></thead><tbody>";
        for (const nlohmann::json& span : spans) {
            const double duration = span.value("duration_ms", 0.0);
            out +=
                std::format("<tr><td>{}</td><td>{:.1f} ms</td><td><span class=\"waterfall-bar{}\" "
                            "style=\"width:{}px\"></span></td></tr>",
                            web_ui_escape(json_text(span, "name")),
                            duration,
                            span.value("ok", true) ? "" : " failed",
                            longest > 0 ? static_cast<int>(4 + 240 * duration / longest) : 4);
        }
        out += "</tbody></table></section>";
    }
    return out;
}

std::string select_options(std::span<const std::string_view> values, std::string_view chosen) {
    std::string out;
    for (const std::string_view value : values) {
        out += std::format(
            "<option value=\"{}\"{}>{}</option>", value, value == chosen ? " selected" : "", value);
    }
    return out;
}

// Gate checks, from gate or from deploy's gate: every check with its status and reason.
std::string gate_panel(const nlohmann::json& gate, std::string_view heading) {
    const std::string verdict = json_text(gate, "verdict");
    const bool blocked = gate.value("block_deploy", false);
    std::string out =
        std::format("<section class=\"panel notice{}\" role=\"status\"><h2>{}</h2><p><span "
                    "class=\"badge {}\">{}</span> {}</p>",
                    blocked ? " status-danger" : "",
                    web_ui_escape(heading),
                    verdict == "pass"   ? "status-success"
                    : verdict == "warn" ? "status-warning"
                                        : "status-danger",
                    web_ui_escape(verdict.empty() ? (blocked ? "fail" : "pass") : verdict),
                    blocked ? "It cannot run or deploy until the failing checks pass."
                            : "It may run and deploy.");
    const nlohmann::json checks = gate.value("checks", nlohmann::json::array());
    if (!checks.empty()) {
        out += "<table class=\"gate-checks\"><thead><tr><th>Check</th><th>Status</th><th>Why"
               "</th></tr></thead><tbody>";
        for (const nlohmann::json& check : checks) {
            const std::string status = json_text(check, "status");
            out += std::format(
                "<tr><td>{}</td><td><span class=\"badge {}\">{}</span></td><td>{}</td></tr>",
                web_ui_escape(json_text(check, "name")),
                status == "pass"   ? "status-success"
                : status == "warn" ? "status-warning"
                                   : "status-danger",
                web_ui_escape(status),
                web_ui_escape(json_text(check, "reason")));
        }
        out += "</tbody></table>";
    }
    return out + "</section>";
}

// The pipeline page (docs/design/zetta-parity.md): see, inspect, change, govern and follow one
// Zetta execution graph.
// Compatibility for records created before names were denormalized onto runs and models.
std::string project_called(const nlohmann::json& data, const std::string& id) {
    const nlohmann::json projects =
        data.value("names", nlohmann::json::object()).value("projects", nlohmann::json::object());
    return projects.contains(id) ? cell_text(projects[id]) : std::string();
}
nlohmann::json graph_called(const nlohmann::json& data, const std::string& id) {
    const nlohmann::json graphs =
        data.value("names", nlohmann::json::object()).value("graphs", nlohmann::json::object());
    return graphs.contains(id) ? graphs[id] : nlohmann::json::object();
}
// A pipeline and its project as links, or "a deleted project" when lake no longer has it.
std::string run_subject(const std::string& query, const nlohmann::json& data,
                        const std::string& graph_id, const std::string& project_id,
                        const std::string& pipeline_name, const std::string& project_name) {
    nlohmann::json graph = pipeline_name.empty()
                               ? graph_called(data, graph_id)
                               : nlohmann::json{{"name", pipeline_name}, {"project", project_name}};
    if (graph.empty() && !data.contains("names")) {
        // Names were not loaded: show what the run carries.
        graph = {{"name", graph_id}, {"project", project_id}};
    }
    if (graph.empty()) {
        return "<span class=\"muted\">a pipeline of a deleted project</span>";
    }
    const std::string project = json_text(graph, "project");
    return row_link("/design/pipeline?" + query + "&project=" + url_component(project) +
                        "&graph=" + url_component(json_text(graph, "name")),
                    json_text(graph, "name")) +
           " <span class=\"muted\">in</span> " +
           row_link("/design/project?" + query + "&name=" + url_component(project), project);
}
std::string when(std::string_view timestamp) {
    // 2026-09-27T13:29:22.572Z → 2026-09-27 13:29 UTC
    if (timestamp.size() < 16) {
        return std::string(timestamp);
    }
    return std::string(timestamp.substr(0, 10)) + " " + std::string(timestamp.substr(11, 5)) +
           " UTC";
}

std::string pipeline_content(const WorkspacePage& page) {
    namespace pl = lake::api::pipeline;
    const std::string query = customer_query(page.customer_id);
    const auto parsed = pl::from_json(page.data.value("get_graph", nlohmann::json::object()));
    if (!parsed || !page.data.contains("get_graph")) {
        return "<div class=\"panel empty\">This pipeline could not be read from the workspace."
               "</div>";
    }
    const pl::Graph& graph = *parsed;
    const std::string base = "/design/pipeline?" + query +
                             "&project=" + url_component(page.project) +
                             "&graph=" + url_component(graph.name);
    const auto common = [&] {
        return hidden("customer_id", page.customer_id) + hidden("project", page.project) +
               hidden("graph", graph.name);
    };

    // This pipeline's runs, newest first, and what the latest one did to each step.
    std::vector<nlohmann::json> runs;
    for (const nlohmann::json& run : page.data.value("list_runs", nlohmann::json::object())
                                         .value("runs", nlohmann::json::array())) {
        if (json_text(run, "graph") == graph.name || json_text(run, "graph") == graph.graph_id) {
            runs.push_back(run);
        }
    }
    std::ranges::sort(runs, [](const nlohmann::json& a, const nlohmann::json& b) {
        return json_text(a, "created") > json_text(b, "created");
    });
    std::map<std::string, std::string> status;
    const nlohmann::json latest = page.data.value("latest_run", nlohmann::json::object());
    for (const nlohmann::json& step : latest.value("lineage", nlohmann::json::object())
                                          .value("steps", nlohmann::json::array())) {
        if (step.is_string()) {
            status[step.get<std::string>()] = json_text(latest, "status");
        }
    }

    const bool approved = graph.approval == "approved";
    std::string out = std::format(
        "<p class=\"sub\">Project {} · <span class=\"badge {}\">{}</span>{} · {} steps · {} "
        "edges</p>",
        row_link("/design/project?" + query + "&name=" + url_component(page.project), page.project),
        approved ? "status-success" : "status-warning",
        approved ? "approved" : "proposed — approve before it runs",
        // approved_by is Zetta's principal id (lake's own key), meaningless to a person; lake's
        // audit log says who approved. Show when.
        graph.approved_at.empty() ? std::string()
                                  : " " + web_ui_escape(graph.approved_at.substr(0, 16)) + " UTC",
        graph.steps.size(),
        graph.edges.size());
    // What this pipeline trains (lake#289, zetta#1061): an engine with the methodology registry
    // says which methodology owns its model step, and whether it can run.
    if (const nlohmann::json read = page.data.value("get_graph", nlohmann::json::object());
        read.contains("runnable")) {
        const std::string methodology = json_text(read, "methodology");
        if (read.value("runnable", false)) {
            out += "<p class=\"sub\">Trains with <span class=\"badge status-success\">" +
                   web_ui_escape(methodology.empty() ? std::string("its model step") : methodology) +
                   "</span></p>";
        } else {
            out += "<section class=\"panel notice\" role=\"status\"><h2>This Pipeline Trains "
                   "Nothing Yet</h2><p>" +
                   (methodology.empty()
                        ? std::string("It has no model step the engine can run, so a run only "
                                      "passes through its steps.")
                        : "It needs " + web_ui_escape(methodology) +
                              ", which this workspace's engine does not run yet.") +
                   " Build a pipeline with a methodology that runs, or ask the Project Agent "
                   "below to change it.</p></section>";
        }
    }
    if (const auto gate = page.data.find("gate"); gate != page.data.end()) {
        out += gate_panel(*gate, "Checks");
    }
    if (const auto deployed = page.data.find("deploy"); deployed != page.data.end()) {
        out += std::format("<section class=\"panel notice\" role=\"status\"><h2>Deployed</h2>"
                           "<p>Deployment <code>{}</code> to <code>{}</code>.</p></section>",
                           web_ui_escape(json_text(*deployed, "deployment_id")),
                           web_ui_escape(json_text(*deployed, "target")));
        out += gate_panel(deployed->value("gate", nlohmann::json::object()), "Its Checks");
    }

    // A change the Project Agent proposed (lake#249): what it would do, the graph after, and Apply.
    if (const auto proposed = page.data.find("proposal");
        proposed != page.data.end() && proposed->is_object()) {
        const nlohmann::json& proposal = *proposed;
        std::string steps, problems;
        for (const nlohmann::json& step : proposal.value("steps", nlohmann::json::array())) {
            steps += "<li>" + web_ui_escape(cell_text(step)) + "</li>";
        }
        for (const nlohmann::json& problem : proposal.value("problems", nlohmann::json::array())) {
            problems += "<li>" + web_ui_escape(cell_text(problem)) + "</li>";
        }
        const nlohmann::json run = proposal.value("run", nlohmann::json::object());
        std::string settings;
        if (!json_text(run, "dataset").empty()) {
            settings += "<li>Train on <code>" + web_ui_escape(json_text(run, "dataset")) +
                        "</code></li>";
        }
        if (run.contains("holdout") && run["holdout"].is_number()) {
            settings += std::format("<li>Hold out {:.0f}% of rows for measuring</li>",
                                    run["holdout"].get<double>() * 100);
        }
        nlohmann::json after = proposal.value("after", nlohmann::json::object());
        if (after.is_object()) {
            after["name"] = graph.name;
        }
        const auto drawn = pl::from_json(after);
        const bool applicable = problems.empty() && !steps.empty();
        out += std::format(
            "<section class=\"panel notice\" id=\"proposal\"><h2>Proposed by the Project Agent"
            "</h2><p>{}</p><p class=\"sub\">Nothing has changed yet. Applying saves these edits; "
            "the pipeline then needs approval again before it runs.</p>{}{}{}{}"
            "<div class=\"actions\">{}<a class=\"button\" href=\"{}\">Discard</a></div></section>",
            web_ui_escape(json_text(proposal, "summary")),
            steps.empty() ? std::string("<p>No edits to the graph.</p>")
                          : "<h3>Edits</h3><ol>" + steps + "</ol>",
            settings.empty() ? std::string() : "<h3>Run settings</h3><ul>" + settings + "</ul>",
            problems.empty() ? std::string()
                             : "<h3>Why it cannot be applied</h3><ul>" + problems + "</ul>",
            drawn && !drawn->steps.empty()
                ? "<h3>After</h3><div class=\"pipeline-canvas\">" +
                      pl::render_svg(*drawn, {}, "", base + "&step=") + "</div>"
                : std::string(),
            applicable
                ? std::format("<form class=\"inline\" method=\"post\" "
                              "action=\"/design/pipeline/apply\">{}{}{}{}<button class=\"button "
                              "button-primary\" type=\"submit\">Apply</button></form>",
                              common(),
                              hidden("edits", proposal.value("edits", nlohmann::json::array()).dump()),
                              hidden("dataset", json_text(run, "dataset")),
                              hidden("holdout",
                                     run.contains("holdout") && run["holdout"].is_number()
                                         ? std::format("{}", run["holdout"].get<double>())
                                         : std::string()))
                : std::string(),
            base);
    }

    if (page.data.value("project_agent_working", false)) {
        out += "<section class=\"panel notice\" id=\"proposal\" role=\"status\"><p><strong>The "
               "Project Agent is working on it…</strong> It reads your data and prepares the "
               "edits; nothing changes until you apply them. This page checks again every 3 "
               "seconds.</p><p>" +
               web_ui_escape(json_text(page.data, "project_agent_message")) +
               "</p><meta http-equiv=\"refresh\" content=\"3\"></section>";
    } else if (page.data.value("project_agent_reply", false)) {
        out +=
            "<section class=\"panel notice\" id=\"proposal\"><h2>Project Agent Question</h2>"
            "<div class=\"ask-reply\">" +
            web_ui_markdown(json_text(page.data, "project_agent_error")) +
            "</div><h3>Reply</h3><form class=\"stack\" method=\"post\" "
            "action=\"/design/pipeline/ask\">" +
            common() + hidden("task_id", json_text(page.data, "project_agent_task_id")) +
            "<label>Reply<textarea name=\"reply\" rows=\"3\" required></textarea></label>"
            "<button class=\"button button-primary\" type=\"submit\">Reply to the Project "
            "Agent</button></form></section>";
    } else if (!json_text(page.data, "project_agent_error").empty()) {
        out += "<p class=\"error\" role=\"alert\" id=\"proposal\">The Project Agent did not propose "
               "a change: " +
               web_ui_escape(json_text(page.data, "project_agent_error")) + "</p>";
    }

    // Ask the Project Agent for a change instead of editing by hand (lake#249).
    out += std::format(
        "<section class=\"panel\"><h2>Ask the Project Agent</h2><form class=\"stack\" "
        "method=\"post\" action=\"/design/pipeline/ask\">{}<label>What should change?<textarea "
        "name=\"request\" rows=\"3\" required placeholder=\"Train on ux_review_churn only and hold "
        "out 20% of rows\"></textarea></label><button class=\"button button-primary\" "
        "type=\"submit\">Propose a change</button></form><p class=\"muted\">It reads your data and "
        "proposes edits for you to review; nothing changes until you apply them.</p></section>",
        common());

    // See it.
    out += "<section class=\"panel\"><h2>Graph</h2>";
    out += graph.steps.empty()
               ? std::string("<p>No steps yet. Add the first one below.</p>")
               : "<div class=\"pipeline-canvas\">" +
                     pl::render_svg(graph, status, page.step, base + "&step=") + "</div>";
    out += "<p class=\"muted\">Solid arrows carry data; dashed arrows only order steps. Select a "
           "step to inspect or change it.</p></section>";

    // Inspect and change the selected step.
    if (const pl::Step* step = graph.step(page.step); step != nullptr) {
        std::string incoming, outgoing;
        for (const pl::Edge& edge : graph.edges) {
            if (edge.to == step->id)
                incoming += "<li>" +
                            row_link(base + "&step=" + url_component(edge.from), edge.from) +
                            " <span class=\"muted\">" + web_ui_escape(edge.kind) + "</span></li>";
            if (edge.from == step->id)
                outgoing += "<li>" + row_link(base + "&step=" + url_component(edge.to), edge.to) +
                            " <span class=\"muted\">" + web_ui_escape(edge.kind) + "</span></li>";
        }
        std::string targets;
        for (const pl::Step& other : graph.steps) {
            if (other.id != step->id)
                targets += std::format("<option value=\"{}\">{} ({})</option>",
                                       web_ui_escape(other.id),
                                       web_ui_escape(other.name),
                                       web_ui_escape(other.id));
        }
        out += std::format(
            "<section class=\"panel\"><h2>Step {}</h2><dl><dt>Id</dt><dd><code>{}</code></dd>"
            "<dt>Kind</dt><dd>{}</dd>{}<dt>Last run</dt><dd>{}</dd><dt>From</dt><dd><ul>{}</ul></dd>"
            "<dt>To</dt><dd><ul>{}</ul></dd></dl><div class=\"pipeline-edit\">"
            "<form class=\"stack\" method=\"post\" action=\"/design/pipeline/edit\">{}{}{}<label>"
            "Name<input name=\"name\" required value=\"{}\"></label><button type=\"submit\">"
            "Rename step</button></form>"
            "<form class=\"stack\" method=\"post\" action=\"/design/pipeline/edit\">{}{}{}<label>"
            "Kind<select name=\"kind\">{}</select></label><button type=\"submit\">Change kind"
            "</button></form>"
            "<form class=\"stack\" method=\"post\" action=\"/design/pipeline/edit\">{}{}{}<label>"
            "Then<select name=\"to\">{}</select></label><label>Carrying<select name=\"kind\">{}"
            "</select></label><button type=\"submit\">Connect</button></form>"
            "<form class=\"stack\" method=\"post\" action=\"/design/pipeline/edit\">{}{}{}<p>"
            "Removing a step removes its edges too.</p><button class=\"button button-danger\" "
            "type=\"submit\">Remove step</button></form></div></section>",
            web_ui_escape(step->name),
            web_ui_escape(step->id),
            web_ui_escape(step->kind),
            [&] {
                std::string settings;
                for (const auto& [key, value] : step->attributes.items()) {
                    settings += "<dt>" + web_ui_escape(key) + "</dt><dd><code>" +
                                web_ui_escape(value.is_string() ? value.get<std::string>()
                                                                : value.dump()) +
                                "</code></dd>";
                }
                return settings;
            }(),
            web_ui_escape(status.contains(step->id) ? status.at(step->id) : "not in the last run"),
            incoming.empty() ? "<li class=\"muted\">nothing</li>" : incoming,
            outgoing.empty() ? "<li class=\"muted\">nothing</li>" : outgoing,
            common(),
            hidden("op", "rename_step"),
            hidden("step", step->id),
            web_ui_escape(step->name),
            common(),
            hidden("op", "kind_step"),
            hidden("step", step->id),
            select_options(pl::step_kinds, step->kind),
            common(),
            hidden("op", "add_edge"),
            hidden("from", step->id),
            targets,
            select_options(pl::edge_kinds, "data_flow"),
            common(),
            hidden("op", "remove_step"),
            hidden("step", step->id));
    }

    // Change the pipeline as a whole.
    std::string step_options;
    for (const pl::Step& step : graph.steps) {
        step_options += std::format("<option value=\"{}\">{} ({})</option>",
                                    web_ui_escape(step.id),
                                    web_ui_escape(step.name),
                                    web_ui_escape(step.id));
    }
    std::string edge_rows;
    for (const pl::Edge& edge : graph.edges) {
        edge_rows += std::format(
            "<tr><td>{}</td><td>{}</td><td>{}</td><td><form class=\"inline\" method=\"post\" "
            "action=\"/design/pipeline/edit\">{}{}{}{}<button type=\"submit\">Remove</button>"
            "</form></td></tr>",
            row_link(base + "&step=" + url_component(edge.from), edge.from),
            row_link(base + "&step=" + url_component(edge.to), edge.to),
            web_ui_escape(edge.kind),
            common(),
            hidden("op", "remove_edge"),
            hidden("from", edge.from),
            hidden("to", edge.to));
    }
    out += std::format(
        "<section class=\"panel\"><h2>Change the Pipeline</h2><div class=\"pipeline-edit\">"
        "<form class=\"stack\" method=\"post\" action=\"/design/pipeline/edit\">{}{}<label>Kind"
        "<select name=\"kind\">{}</select></label><label>Name<input name=\"name\" "
        "placeholder=\"Score accounts\"></label><button class=\"button button-primary\" "
        "type=\"submit\">Add step</button></form>"
        "<form class=\"stack\" method=\"post\" action=\"/design/pipeline/edit\">{}{}<label>From"
        "<select name=\"from\">{}</select></label><label>To<select name=\"to\">{}</select>"
        "</label><label>Carrying<select name=\"kind\">{}</select></label><button "
        "type=\"submit\">Add edge</button></form>"
        "<form class=\"stack\" method=\"post\" action=\"/design/pipeline/edit\">{}{}<label>Name"
        "<input name=\"name\" required value=\"{}\"></label><button type=\"submit\">Rename "
        "pipeline</button></form></div>{}</section>",
        common(),
        hidden("op", "add_step"),
        select_options(pl::step_kinds, "model"),
        common(),
        hidden("op", "add_edge"),
        step_options,
        step_options,
        select_options(pl::edge_kinds, "data_flow"),
        common(),
        hidden("op", "rename"),
        web_ui_escape(graph.name),
        edge_rows.empty() ? std::string()
                          : "<h3>Edges</h3><table><thead><tr><th>From</th><th>To</th><th>Carrying"
                            "</th><th></th></tr></thead><tbody>" +
                                edge_rows + "</tbody></table>");

    // Govern it: approve, check, run, deploy.
    std::string datasets;
    for (const nlohmann::json& dataset : page.data.value("list_datasets", nlohmann::json::object())
                                             .value("datasets", nlohmann::json::array())) {
        const std::string name = json_text(dataset, "name");
        const std::string database = json_text(dataset, "database");
        const std::string path = database.empty() ? name : database + "." + name;
        datasets += std::format("<option value=\"{}\"{}>{}</option>",
                                web_ui_escape(path),
                                path == json_text(page.data, "run_dataset") ? " selected" : "",
                                web_ui_escape(path));
    }
    // A real hold-out by default, so a run reports a held-out score (lake#254), and a model name
    // that is this pipeline's own, never a constant every pipeline shares.
    const std::string holdout = json_text(page.data, "run_holdout").empty()
                                    ? std::string("0.2")
                                    : json_text(page.data, "run_holdout");
    std::string model_name;
    for (const char c : page.project + "_" + graph.name) {
        model_name += std::isalnum(static_cast<unsigned char>(c)) ? static_cast<char>(std::tolower(
                                                                        static_cast<unsigned char>(c)))
                                                                  : '_';
    }
    out += std::format(
        "<section class=\"panel\" id=\"run\"><h2>Approve, Check, Run and Deploy</h2><div "
        "class=\"pipeline-edit\">{}"
        "<form class=\"stack\" method=\"post\" action=\"/design/pipeline/gate\">{}<p>Run Zetta's "
        "correctness checks without running anything.</p><button type=\"submit\">Check</button>"
        "</form>"
        "<form class=\"stack\" method=\"post\" action=\"/design/pipeline/run\">{}<label>Train on"
        "<select name=\"dataset_path\"><option value=\"\">the pipeline's own source</option>{}"
        "</select></label><label>Hold out for measuring<input name=\"holdout_fraction\" "
        "type=\"number\" min=\"0\" max=\"0.9\" step=\"0.05\" value=\"{}\"><small>The share of "
        "rows kept out of training to measure the model; 0 measures nothing.</small></label>"
        "<label>Model name<input name=\"model_name\" value=\"{}\"></label><button "
        "class=\"button button-primary\" type=\"submit\"{}>Check and run</button></form>"
        "<form class=\"stack\" method=\"post\" action=\"/design/pipeline/deploy\">{}<p>Place it "
        "in the workspace behind its checks.</p><button type=\"submit\"{}>Deploy</button></form>"
        "</div></section>",
        approved ? std::string()
                 : std::format("<form class=\"stack\" method=\"post\" "
                               "action=\"/design/pipeline/approve\">{}<p>Zetta verifies and runs "
                               "only an approved pipeline.</p><button class=\"button "
                               "button-primary\" type=\"submit\">Approve</button></form>",
                               common()),
        common(),
        common(),
        datasets,
        web_ui_escape(holdout),
        web_ui_escape(model_name),
        approved ? "" : " disabled",
        common(),
        approved ? "" : " disabled");

    // Follow it.
    out += "<section class=\"panel\"><h2>Runs</h2>";
    if (runs.empty()) {
        out += "<p>No runs of this pipeline yet.</p>";
    } else {
        out += "<table><thead><tr><th>Run</th><th>Status</th><th>Started</th><th>Outputs</th>"
               "</tr></thead><tbody>";
        for (const nlohmann::json& run : runs) {
            const std::string id = json_text(run, "id");
            out += std::format("<tr><td>{}</td><td><span class=\"badge\">{}</span></td><td>{}"
                               "</td><td>{}</td></tr>",
                               row_link("/train/run?" + query + "&id=" + url_component(id), id),
                               web_ui_escape(json_text(run, "status")),
                               web_ui_escape(json_text(run, "created")),
                               web_ui_escape(json_text(run, "output_count")));
        }
        out += "</tbody></table>";
    }
    return out + "</section>";
}

// One search index (#187): what it covers, a search box, publish and unpublish, and how to call it
// from outside with a key scoped to search_index.
std::string index_content(const WorkspacePage& page) {
    const std::string query = customer_query(page.customer_id);
    const nlohmann::json index = page.data.value("get_index", nlohmann::json::object());
    if (index.empty()) {
        return "<div class=\"panel empty\">This index could not be read from the workspace.</div>";
    }
    const bool published = index.value("exposed", false);
    std::string fields, field_options = "<option value=\"\">all fields</option>";
    for (const nlohmann::json& field : index.value("fields", nlohmann::json::array())) {
        fields += (fields.empty() ? "" : ", ") + cell_text(field);
        field_options +=
            std::format("<option{}>{}</option>",
                        cell_text(field) == json_text(page.data, "search_field") ? " selected" : "",
                        web_ui_escape(cell_text(field)));
    }
    std::string out = std::format(
        "<section class=\"panel\"><dl><dt>Table</dt><dd>{}</dd><dt>Kind</dt><dd>{}</dd><dt>Backend"
        "</dt><dd>{}</dd><dt>Fields</dt><dd>{}</dd><dt>Created</dt><dd>{}</dd><dt>Published</"
        "dt><dd>"
        "<span class=\"badge {}\">{}</span></dd></dl><form class=\"inline\" method=\"post\" "
        "action=\"/ship/index/publish\">{}{}{}<button type=\"submit\"{}>{}</button></form>"
        "</section>",
        web_ui_escape(json_text(index, "table")),
        web_ui_escape(json_text(index, "kind")),
        web_ui_escape(json_text(index, "backend")),
        web_ui_escape(fields),
        web_ui_escape(json_text(index, "created")),
        published ? "status-success" : "",
        published ? "published" : "not published",
        hidden("customer_id", page.customer_id),
        hidden("index", page.subject),
        hidden("publish", published ? "0" : "1"),
        published ? "" : " class=\"button button-primary\"",
        published ? "Unpublish" : "Publish");

    // Try a search.
    const std::string asked = json_text(page.data, "search_query");
    out += std::format(
        "<section class=\"panel\"><h2>Try a Search</h2><form class=\"stack\" method=\"get\" "
        "action=\"/ship/index\">{}{}<label>Search for<input name=\"q\" required value=\"{}\" "
        "placeholder=\"renewal risk\"></label><label>In<select name=\"field\">{}</select>"
        "</label><label>Hits<input name=\"top_k\" type=\"number\" min=\"1\" max=\"100\" "
        "value=\"{}\"></label><button class=\"button button-primary\" type=\"submit\">Search"
        "</button></form>",
        hidden("customer_id", page.customer_id),
        hidden("id", page.subject),
        web_ui_escape(asked),
        field_options,
        page.data.contains("search_top_k") ? cell_text(page.data.at("search_top_k"))
                                           : std::string("10"));
    if (const auto results = page.data.find("search"); results != page.data.end()) {
        const nlohmann::json hits = results->value("hits", nlohmann::json::array());
        out += std::format("<p class=\"muted\">{} hits in {} ms.</p>",
                           hits.size(),
                           cell_text(results->value("duration_ms", nlohmann::json(0))));
        out += hits.empty() ? std::string("<p>Nothing matched.</p>")
                            : "<table><thead><tr><th>#</th><th>Document</th><th>Score</th></tr>"
                              "</thead><tbody>" +
                                  [&] {
                                      std::string rows;
                                      int rank = 0;
                                      for (const nlohmann::json& hit : hits) {
                                          rows += std::format(
                                              "<tr><td>{}</td><td><code>{}</code></td><td>{}</td>"
                                              "</tr>",
                                              ++rank,
                                              web_ui_escape(json_text(hit, "id")),
                                              cell_text(hit.value("score", nlohmann::json(0))));
                                      }
                                      return rows;
                                  }() +
                                  "</tbody></table>";
    }
    out += "</section>";

    // Call it from outside.
    const std::string origin = page.origin.empty() ? "https://lake.oramadev.net" : page.origin;
    const std::string url = std::format("{}/v1/search_index?customer_id={}&index={}&query=",
                                        origin,
                                        url_component(page.customer_id),
                                        url_component(page.subject));
    if (!published) {
        out += "<section class=\"panel\"><h2>Call It from Your Code</h2><p>Publish the index "
               "first: only published indexes answer outside calls.</p></section>";
        return out;
    }
    out += std::format(
        "<section class=\"panel\"><h2>Call It from Your Code</h2><ol><li><p>Create a key that "
        "can only search published indexes, and copy it; it is shown once.</p><form "
        "class=\"inline\" method=\"post\" action=\"/identity/token/create\">{}{}<button "
        "type=\"submit\">Create a search key</button></form></li><li><p>Call lake with it. The "
        "search runs in your own workspace; lake checks the key and records every "
        "query.</p><pre><code>curl -s -H \"Authorization: Bearer $LAKE_SEARCH_KEY\" \\\n  "
        "\"{}renewal%20risk&amp;top_k=10\"</code></pre><pre><code>// JavaScript\nconst res = await "
        "fetch(\"{}\" + encodeURIComponent(\"renewal risk\"), {{\n  headers: {{ Authorization: "
        "`Bearer ${{process.env.LAKE_SEARCH_KEY}}` }},\n}});\nconst {{ value }} = await "
        "res.json(); "
        "// value.hits: [{{ id, score }}]</code></pre><pre><code># Python\nimport os, requests\n"
        "r = requests.get(\"{}/v1/search_index\", params={{\"customer_id\": \"{}\", \"index\": "
        "\"{}\", \"query\": \"renewal risk\"}},\n    headers={{\"Authorization\": f\"Bearer "
        "{{os.environ['LAKE_SEARCH_KEY']}}\"}})\nhits = "
        "r.json()[\"value\"][\"hits\"]</code></pre></li>"
        "<li><p>From an AI agent, the same search is the MCP tool <code>search_index</code> at "
        "<code>{}/mcp</code>.</p></li></ol><p class=\"muted\">Unpublishing the index stops every "
        "outside call at once; the key keeps working for other published indexes until you revoke "
        "it on <a href=\"/tokens\">Tokens</a>. A key lasts as long as the session that made "
        "it, at most 30 days.</p></section>",
        hidden("name", "search " + json_text(index, "table")),
        hidden("scopes", "search_index"),
        web_ui_escape(url),
        web_ui_escape(url),
        web_ui_escape(origin),
        web_ui_escape(page.customer_id),
        web_ui_escape(page.subject),
        web_ui_escape(origin));
    return out;
}

// Studio's skills (orama-design src/studio/studioAgents.js): granted, never assumed.
constexpr std::array<std::pair<std::string_view, std::string_view>, 5> agent_skills{{
    {"search", "Search data"},
    {"generate-data", "Generate a dataset"},
    {"train", "Train a Model"},
    {"summarize", "Summarize and explain"},
    {"call-api", "Call the API"},
}};

std::string agent_skill_labels(const nlohmann::json& skills) {
    std::string labels;
    for (const nlohmann::json& skill : skills) {
        if (!skill.is_string()) {
            continue;
        }
        std::string label = skill.get<std::string>();
        for (const auto& [key, text] : agent_skills) {
            if (key == label) {
                label = std::string(text);
            }
        }
        labels += (labels.empty() ? "" : ", ") + label;
    }
    return labels.empty() ? std::string("—") : labels;
}

std::string agent_engagement(const nlohmann::json& agent) {
    return json_text(agent, "trigger").empty() ? "Conversation" : "Conversation and unattended";
}

std::string agent_model_label(const nlohmann::json& agent) {
    const std::string name = json_text(agent, "model_name");
    return name.empty() ? json_text(agent, "model") : name;
}

// Every specialized agent Orama ships (docs/api/train-agents.md, Agent Library). Shown whether or
// not the workspace is ready: the library is lake's, not the workspace's.
std::string agent_library_content(const WorkspacePage& page) {
    const nlohmann::json agents = page.data.value("list_agent_library", nlohmann::json::object())
                                      .value("agents", nlohmann::json::array());
    std::string out =
        "<section class=\"panel\" id=\"agent-library\"><h2>Agent Library</h2><p class=\"sub\">"
        "Agents Orama provides, each for one job. Use them where they appear below, or just ask "
        "Orama and it brings in the right one.</p>";
    if (agents.empty()) {
        return out + "<p>The Agent Library is not available.</p></section>";
    }
    out += "<table><thead><tr><th>Agent</th><th>What it does for you</th><th>Where to use it</th>"
           "<th></th></tr></thead><tbody>";
    for (const nlohmann::json& agent : agents) {
        const bool available = json_text(agent, "status") == "Available";
        out += std::format(
            "<tr><td><strong>{}</strong></td><td>{}</td><td>{}</td><td><span class=\"badge{}\">{}"
            "</span></td></tr>",
            web_ui_escape(json_text(agent, "name")),
            web_ui_escape(json_text(agent, "purpose")),
            web_ui_escape(json_text(agent, "used_in")),
            available ? " status-success" : "",
            available                                     ? std::string("Available")
            : json_text(agent, "runs_as") == "offline" ? std::string("Not yet available to chat")
                                                         : std::string("Not available here"));
    }
    return out + "</tbody></table></section>";
}

std::string agent_create_form(const WorkspacePage& page) {
    std::string skills;
    for (const auto& [key, label] : agent_skills) {
        skills += std::format("<label><input type=\"checkbox\" name=\"skills\" value=\"{}\"> {}"
                              "</label>",
                              key,
                              label);
    }
    return std::format(
        "<section class=\"panel\" id=\"create-agent\"><h2>Create an Agent</h2><p><a "
        "class=\"button\" href=\"/train/agents/new?{}\">Build it with the Agent Factory</a> "
        "or fill in the form below.</p><p class=\"sub\">"
        "An agent does one job for you. Say what the job is, how it should work, what success "
        "looks like and what it may use. Without a schedule it answers only when you talk to it; "
        "with one it also runs on its own.</p><form "
        "class=\"stack\" method=\"post\" action=\"/train/agents/create\">{}<label>Name<input "
        "name=\"name\" required maxlength=\"80\" placeholder=\"Renewal watcher\"></label><label>"
        "What it's for<input name=\"purpose\" required placeholder=\"Flag renewals at risk a month "
        "ahead\"></label><label>Where it works<select name=\"work_area\"><option "
        "value=\"workspace\">Anywhere in this workspace</option><option value=\"project\">One "
        "project</option><option value=\"dataset\">Specific datasets</option></select></label>"
        "<label>Which project or datasets (if not the whole workspace)<input "
        "name=\"work_area_detail\" placeholder=\"churn\"></label><label>Instructions<small>How it "
        "should work, and what it must never do.</small><textarea "
        "name=\"manifesto\" rows=\"3\" placeholder=\"Answer from company data only. Say when "
        "evidence is missing rather than estimating.\"></textarea></label><label>What success looks "
        "like<textarea name=\"goal\" rows=\"2\" placeholder=\"Every at-risk renewal is flagged a "
        "month ahead.\"></textarea></label><fieldset><legend>What it may do</legend>{}</fieldset>"
        "<label>Model<select name=\"model\"><option value=\"base\">Orama base model, inside your "
        "workspace</option><option value=\"frontier\">A frontier model (OpenAI), run by lake"
        "</option></select></label><label>Runs on its own<small>Leave empty to use it only in "
        "conversation.</small><input name=\"trigger\" placeholder=\"every Monday at 09:00\">"
        "</label><button class=\"button button-primary\" type=\"submit\">Create "
        "agent</button></form></section>",
        customer_query(page.customer_id),
        hidden("customer_id", page.customer_id),
        skills);
}

std::string agents_content(const WorkspacePage& page) {
    const std::string query = customer_query(page.customer_id);
    const nlohmann::json listed = page.data.value("list_agents", nlohmann::json::object())
                                      .value("agents", nlohmann::json::array());
    std::string out = "<section class=\"panel\" id=\"your-agents\"><h2>Your Agents</h2>";
    std::size_t shown = 0;
    std::string rows;
    for (const nlohmann::json& agent : listed) {
        if (json_text(agent, "kind") == "orama") {
            continue;
        }
        ++shown;
        const std::string id = json_text(agent, "id");
        const std::string status = json_text(agent, "status");
        rows += std::format(
            "<tr><td><span class=\"agent-cell\">{}<small>{}</small></span></td><td><span "
            "class=\"badge\">{}</span></td><td>{}</td><td><code>{}</code></td><td><span "
            "class=\"badge\">{}</span></td></tr>",
            row_link("/train/agent?" + query + "&id=" + url_component(id),
                     json_text(agent, "name")),
            web_ui_escape(json_text(agent, "purpose")),
            agent_engagement(agent),
            web_ui_escape(agent_skill_labels(agent.value("skills", nlohmann::json::array()))),
            web_ui_escape(agent_model_label(agent)),
            web_ui_escape(status.empty() ? "idle" : status));
    }
    if (shown == 0) {
        out += "<p>No agents yet. Create one below.</p>";
    } else {
        out += "<table><thead><tr><th>Agent</th><th>Engagement</th><th>Skills</th><th>Model</th>"
               "<th>Status</th></tr></thead><tbody>" +
               rows + "</tbody></table>";
    }
    return out + "</section>" + agent_create_form(page);
}

std::string agent_content(const WorkspacePage& page) {
    const std::string query = customer_query(page.customer_id);
    const nlohmann::json agent = page.data.value("get_agent", nlohmann::json::object());
    if (agent.empty()) {
        return "<div class=\"panel empty\">No such agent in this workspace.</div>";
    }
    const std::string runtime = json_text(agent, "runtime");
    const nlohmann::json area = agent.value("work_area", nlohmann::json::object());
    std::string area_text = json_text(area, "kind") == "project"   ? "A single project"
                            : json_text(area, "kind") == "dataset" ? "Specific data"
                                                                   : "This workspace";
    if (!json_text(area, "detail").empty()) {
        area_text += ": " + json_text(area, "detail");
    }
    const auto row = [](std::string_view label, std::string_view value) {
        return std::format("<dt>{}</dt><dd>{}</dd>",
                           label,
                           web_ui_escape(value.empty() ? std::string_view("—") : value));
    };
    std::string out = "<section class=\"panel\"><h2>About</h2><dl>";
    out += row("Purpose", json_text(agent, "purpose"));
    out += row("Engagement", agent_engagement(agent));
    out += row("Work area", area_text);
    out += row("What it may do", agent_skill_labels(agent.value("skills", nlohmann::json::array())));
    out += row("Model", agent_model_label(agent));
    out += row("Runs",
               runtime == "python"    ? "On agent-runtime"
               : runtime == "service" ? "As its own service"
                                      : "In your workspace");
    out += row("Instructions", json_text(agent, "manifesto"));
    out += row("What success looks like", json_text(agent, "goal"));
    out += row("Runs on its own", json_text(agent, "trigger"));
    out += row("Status", json_text(agent, "status"));
    out += std::format("</dl><p><a class=\"button\" href=\"/ask?{}&agent={}\">Talk to this "
                       "agent in Ask Orama</a></p></section>",
                       query,
                       url_component(json_text(agent, "id")));

    const nlohmann::json pipelines = agent.value("pipelines", nlohmann::json::array());
    const auto grant_form = [&](std::string_view action,
                                std::string_view project,
                                std::string_view graph,
                                std::string_view label) {
        return std::format("<form class=\"inline\" method=\"post\" action=\"{}\">{}{}{}{}"
                           "<button type=\"submit\">{}</button></form>",
                           action,
                           hidden("customer_id", page.customer_id),
                           hidden("id", json_text(agent, "id")),
                           hidden("project", project),
                           hidden("graph", graph),
                           label);
    };
    out += "<section class=\"panel\" id=\"agent-pipelines\"><h2>Pipelines</h2><p "
           "class=\"sub\">An approved Pipeline granted here becomes one of this agent's tools: it "
           "checks the correctness gate, runs it and reads the report. The grant keeps the "
           "approved version.</p>";
    std::set<std::string> granted;
    if (pipelines.empty()) {
        out += "<p>No Pipelines granted.</p>";
    } else {
        out += "<ul>";
        for (const nlohmann::json& grant : pipelines) {
            granted.insert(json_text(grant, "project") + "/" + json_text(grant, "graph"));
            out += std::format("<li>{} in {} <small>approved {}</small> {}</li>",
                               row_link("/design/pipeline?" + query + "&project=" +
                                            url_component(json_text(grant, "project")) +
                                            "&graph=" + url_component(json_text(grant, "graph")),
                                        json_text(grant, "graph")),
                               web_ui_escape(json_text(grant, "project")),
                               web_ui_escape(json_text(grant, "version")),
                               grant_form("/train/agent/revoke",
                                          json_text(grant, "project"),
                                          json_text(grant, "graph"),
                                          "Revoke"));
        }
        out += "</ul>";
    }
    std::string offers;
    for (const nlohmann::json& candidate :
         page.data.value("approved_pipelines", nlohmann::json::array())) {
        const std::string key =
            json_text(candidate, "project") + "/" + json_text(candidate, "graph");
        if (granted.contains(key)) {
            continue;
        }
        offers += std::format("<li>{} in {} {}</li>",
                              web_ui_escape(json_text(candidate, "graph")),
                              web_ui_escape(json_text(candidate, "project")),
                              grant_form("/train/agent/grant",
                                         json_text(candidate, "project"),
                                         json_text(candidate, "graph"),
                                         "Grant"));
    }
    if (!offers.empty()) {
        out += "<h3>Approved Pipelines You Can Grant</h3><ul>" + offers + "</ul>";
    }
    out += "</section>";

    if (runtime == "python") {
        out += "<section class=\"panel\" id=\"agent-schedules\"><h2>Schedules</h2><p "
               "class=\"sub\">Each schedule is its own Kubernetes CronJob: every tick runs this "
               "agent once, as you, with what you ask it here.</p>";
        const nlohmann::json schedules =
            page.data.value("list_agent_schedules", nlohmann::json::object())
                .value("schedules", nlohmann::json::array());
        if (schedules.empty()) {
            out += "<p>No schedules. It runs when you run it.</p>";
        } else {
            out += "<table><thead><tr><th>When</th><th>Cron</th><th>Asked</th><th>Status</th>"
                   "<th>Last fired</th><th></th></tr></thead><tbody>";
            for (const nlohmann::json& schedule : schedules) {
                const std::string phase = json_text(schedule, "phase");
                const nlohmann::json asked = schedule.value("input", nlohmann::json());
                out += std::format(
                    "<tr><td>{}</td><td><code>{}</code> <small>{}</small></td><td>{}</td><td><span "
                    "class=\"badge\">{}</span> <small>{}</small></td><td>{}</td><td><form "
                    "class=\"inline\" method=\"post\" action=\"/train/agent/unschedule\">{}{}{}"
                    "<button type=\"submit\">Remove</button></form></td></tr>",
                    web_ui_escape(json_text(schedule, "description")),
                    web_ui_escape(json_text(schedule, "cron")),
                    web_ui_escape(json_text(schedule, "time_zone")),
                    web_ui_escape(asked.is_string() ? asked.get<std::string>()
                                                    : std::string()),
                    web_ui_escape(phase.empty() ? std::string("Pending") : phase),
                    web_ui_escape(json_text(schedule, "message")),
                    web_ui_escape(json_text(schedule, "last_schedule_at")),
                    hidden("customer_id", page.customer_id),
                    hidden("id", json_text(agent, "id")),
                    hidden("schedule_id", json_text(schedule, "id")));
            }
            out += "</tbody></table>";
        }
        out += std::format(
            "<form class=\"stack\" method=\"post\" action=\"/train/agent/schedule\">{}{}<label>"
            "When<input name=\"when\" required placeholder=\"every monday at 09:00, or 0 9 * * "
            "1\"></label><label>Ask it<input name=\"input\" placeholder=\"What should each run "
            "do?\"></label><button class=\"button\" type=\"submit\">Add schedule</button>"
            "</form></section>",
            hidden("customer_id", page.customer_id),
            hidden("id", json_text(agent, "id")));
    }

    out += "<section class=\"panel\" id=\"agent-runs\"><h2>Unattended Runs</h2>";
    const nlohmann::json launches = page.data.value("list_agent_launches", nlohmann::json::object())
                                        .value("launches", nlohmann::json::array());
    bool changing = false;
    if (launches.empty()) {
        out += json_text(agent, "trigger").empty()
                   ? "<p>This agent only talks: it has no trigger.</p>"
                   : "<p>No unattended runs yet. Its trigger runs it.</p>";
    } else {
        out += "<table><thead><tr><th>Started</th><th>How</th><th>Mode</th><th>Status</th>"
               "<th>Result</th><th></th></tr></thead><tbody>";
        for (const nlohmann::json& launch : launches) {
            const std::string phase = json_text(launch, "phase");
            const bool present = json_text(launch, "desired") == "present";
            const bool live = present && phase != "Succeeded" && phase != "Failed";
            changing = changing || (live && phase != "Ready");
            const nlohmann::json result = launch.value("result", nlohmann::json());
            std::string answer = result.is_object() ? json_text(result, "reply") : "";
            if (answer.empty() && result.is_object()) {
                answer = json_text(result, "error");
            }
            const std::string status = !present ? std::string("Stopped")
                                                : (phase.empty() ? std::string("Pending") : phase);
            std::string stop;
            // A scheduled run belongs to its CronJob; removing the schedule stops the next ones.
            if (live && json_text(launch, "trigger") != "schedule") {
                stop = std::format("<form class=\"inline\" method=\"post\" "
                                   "action=\"/train/agent/stop\">{}{}{}<button "
                                   "type=\"submit\">Stop</button></form>",
                                   hidden("customer_id", page.customer_id),
                                   hidden("id", json_text(agent, "id")),
                                   hidden("launch_id", json_text(launch, "id")));
            }
            out += std::format("<tr><td>{}</td><td>{}</td><td>{}</td><td><span "
                               "class=\"badge\">{}</span> <small>{}</small></td><td>{}</td>"
                               "<td>{}</td></tr>",
                               web_ui_escape(json_text(launch, "created_at")),
                               web_ui_escape(json_text(launch, "trigger")),
                               web_ui_escape(json_text(launch, "mode")),
                               web_ui_escape(status),
                               web_ui_escape(json_text(launch, "message")),
                               web_ui_escape(answer.substr(0, 400)),
                               stop);
        }
        out += "</tbody></table>";
    }
    if (runtime == "python") {
        out += std::format(
            "<form class=\"stack\" method=\"post\" action=\"/train/agent/run\">{}{}<label>Run "
            "it once with<input name=\"input\" placeholder=\"What should it do this "
            "time?\"></label><button class=\"button button-primary\" type=\"submit\">Run "
            "now</button></form>",
            hidden("customer_id", page.customer_id),
            hidden("id", json_text(agent, "id")));
    }
    if (changing) {
        out += "<p class=\"sub\" role=\"status\">A run is starting. This page checks again "
               "every 5 seconds.<meta http-equiv=\"refresh\" content=\"5\"></p>";
    }
    out += "</section>";

    if (runtime.empty() || runtime == "native") {
        const nlohmann::json training =
            page.data.value("agent_training_status", nlohmann::json::object());
        if (!training.empty()) {
            out += std::format("<section class=\"panel\"><h2>Training</h2><p>{}</p></section>",
                               web_ui_escape(json_text(training, "reason")));
        }
    }
    out += std::format(
        "<section class=\"panel\"><h2>Delete Agent</h2><form class=\"stack\" "
        "method=\"post\" action=\"/train/agent/delete\">{}{}{}<label>Type <strong>{}</strong> to "
        "delete this agent<input name=\"confirm\" required autocomplete=\"off\"></label><button "
        "class=\"button button-danger\" type=\"submit\">Delete agent</button></form></section>",
        hidden("customer_id", page.customer_id),
        hidden("id", json_text(agent, "id")),
        hidden("name", json_text(agent, "name")),
        web_ui_escape(json_text(agent, "name")));
    return out;
}

// Design > Discover Opportunities (docs/api/discover.md, lake#237): Auto-Suggest's answer as
// ranked Opportunities, what it declined, and project drafts created only on confirmation.
std::string discover_content(const WorkspacePage& page) {
    // Design → Opportunities (lake#269): saved Opportunities, a scan that runs in the background,
    // and what was done with each.
    std::string out;
    const std::string query = customer_query(page.customer_id);
    const nlohmann::json library = page.data.value("list_agent_library", nlohmann::json::object());
    bool running = false;
    for (const nlohmann::json& agent : library.value("agents", nlohmann::json::array())) {
        if (json_text(agent, "key") == "agent-autosuggest") {
            running = agent.value("deployed", false);
        }
    }
    const nlohmann::json task = page.data.value("task", nlohmann::json::object());
    const std::string status = json_text(task, "status");
    const bool scanning = status == "pending" || status == "running";
    if (scanning) {
        out += "<section class=\"panel notice\" role=\"status\"><p><strong>Scanning your "
               "workspace…</strong> Auto-Suggest is reading your datasets and matching them to "
               "the solutions catalogue. You can leave this page; the Opportunities are kept "
               "when it finishes. This page checks again every 3 seconds.</p><meta "
               "http-equiv=\"refresh\" content=\"3\"></section>";
    } else if (status == "failed") {
        out += std::format("<p class=\"error\" role=\"alert\">The scan did not finish: {}</p>",
                           web_ui_escape(json_text(task, "message")));
    }
    const nlohmann::json rows = page.data.value("list_opportunities", nlohmann::json::object())
                                    .value("opportunities", nlohmann::json::array());
    // The newest scan's Opportunities are the recommendation; what earlier scans recommended and
    // nobody acted on is kept, collapsed, so each idea is not listed once per scan (lake#287).
    std::string last_scan, latest_task;
    for (const nlohmann::json& row : rows) {
        if (json_text(row, "created_at") > last_scan) {
            last_scan = json_text(row, "created_at");
            latest_task = json_text(row, "task_id");
        }
    }
    if (!running) {
        out += "<div class=\"panel empty\">Auto-Suggest is not running on this lake, so a new "
               "scan is not available.</div>";
    } else if (!scanning) {
        out += std::format(
            "<section class=\"panel\"><h2>Scan Your Workspace</h2><p>{}</p><form method=\"post\" "
            "action=\"/design/opportunities/scan\">{}<button class=\"button button-primary\" "
            "type=\"submit\">{}</button></form></section>",
            last_scan.empty()
                ? std::string("Auto-Suggest reads your workspace's datasets and their columns and "
                              "recommends what to build. It runs in the background and changes "
                              "nothing.")
                : "Last scanned " + web_ui_escape(last_scan.substr(0, 16)) +
                      " UTC. Scan again after your data changes; earlier Opportunities stay.",
            hidden("customer_id", page.customer_id),
            last_scan.empty() ? "Scan my workspace" : "Scan again");
    }
    // Whether the engine runs what each Opportunity needs (lake#289, zetta#1061). Absent on an
    // engine that predates the registry: then no badge.
    const nlohmann::json methodologies = page.data.value("list_methodologies", nlohmann::json());
    const auto runs_badge = [&](const nlohmann::json& row) -> std::string {
        const nlohmann::json entry = lake::api::find_methodology(
            methodologies, json_text(row, "methodology"), json_text(row, "solution"));
        if (!entry.is_object()) return {};
        if (entry.value("runnable", false)) {
            return "<p><span class=\"badge status-success\">Runs today: " +
                   web_ui_escape(json_text(entry, "name")) + " (" +
                   web_ui_escape(json_text(entry, "id")) + ")</span></p>";
        }
        return "<p><span class=\"badge status-warning\">Needs " +
               web_ui_escape(json_text(entry, "id")) + ", not supported yet" +
               (json_text(entry, "issue").empty()
                    ? std::string()
                    : " (" + web_ui_escape(json_text(entry, "issue")) + ")") +
               "</span></p>";
    };
    std::string fresh, earlier, made, later, dismissed;
    std::size_t earlier_count = 0;
    for (const nlohmann::json& row : rows) {
        const std::string state = json_text(row, "status");
        const std::string id = json_text(row, "id");
        const nlohmann::json detail = row.value("detail", nlohmann::json::object());
        const bool current = json_text(row, "task_id") == latest_task;
        if (state == "new") {
            earlier_count += current ? 0 : 1;
            (current ? fresh : earlier) += std::format(
                "<article class=\"panel ask-proposal\"><label class=\"opportunity-pick\"><input "
                "type=\"checkbox\" name=\"pick\" value=\"{}\"> <span>{}</span></label><p "
                "class=\"sub\">{}{}</p>"
                "{}{}{}<p><button type=\"submit\" formaction=\"/design/opportunities/dismiss\" "
                "formnovalidate name=\"opportunity_id\" value=\"{}\">Dismiss</button></p></article>",
                web_ui_escape(id),
                web_ui_escape(json_text(row, "intent")),
                json_text(row, "solution").empty()
                    ? std::string()
                    : "Catalogue solution " + web_ui_escape(json_text(row, "solution")) + " · ",
                web_ui_escape(json_text(row, "methodology")),
                runs_badge(row),
                // Auto-Suggest writes inline markdown (`table` names); the renderer escapes first.
                markdown_to_html(json_text(row, "why")),
                json_text(detail, "business_action").empty()
                    ? std::string()
                    : markdown_to_html("**What it changes:** " +
                                       json_text(detail, "business_action")),
                web_ui_escape(id));
        } else if (state == "project") {
            made += "<li>" + web_ui_escape(json_text(row, "intent")) + " → " +
                    row_link("/design/project?" + query + "&name=" +
                                 url_component(json_text(row, "project")),
                             json_text(row, "project")) +
                    "</li>";
        } else if (state == "not_yet" && current) {
            later += "<li><strong>" + web_ui_escape(json_text(row, "intent")) + "</strong>: " +
                     web_ui_escape(json_text(row, "why")) + "</li>";
        } else if (state == "dismissed") {
            dismissed += "<li>" + web_ui_escape(json_text(row, "intent")) + "</li>";
        }
    }
    if (rows.empty() && !scanning) {
        out += "<div class=\"panel empty\">No Opportunities yet. Scan your workspace to see what "
               "your data could be used for.</div>";
    }
    if (!fresh.empty() || !earlier.empty()) {
        out += std::format(
            "<section class=\"panel\"><h2>Recommended</h2><form class=\"stack\" method=\"post\" "
            "action=\"/design/opportunities/create\">{}{}{}<label><input type=\"checkbox\" "
            "name=\"confirm\" value=\"yes\" required> Create a project draft for each selected "
            "Opportunity. Nothing runs until you approve a plan.</label><button class=\"button "
            "button-primary\" type=\"submit\">Create project drafts</button></form></section>",
            hidden("customer_id", page.customer_id),
            fresh.empty() ? std::string("<p class=\"sub\">The latest scan recommended nothing "
                                        "new.</p>")
                          : fresh,
            earlier.empty()
                ? std::string()
                : std::format("<details class=\"earlier-scans\"><summary>Earlier scans ({})"
                              "</summary><p class=\"sub\">What earlier scans recommended and "
                              "nobody has acted on. The latest scan above replaces them; you can "
                              "still pick or dismiss one.</p>{}</details>",
                              earlier_count,
                              earlier));
    }
    if (!made.empty()) {
        out += "<section class=\"panel\"><h2>Made into Projects</h2><ul>" + made + "</ul></section>";
    }
    if (!later.empty()) {
        out += "<section class=\"panel\"><h2>Not Yet</h2><p class=\"sub\">What your data does not "
               "support yet, and why.</p><ul>" +
               later + "</ul></section>";
    }
    if (!dismissed.empty()) {
        out += "<details class=\"panel\"><summary>Dismissed</summary><ul>" + dismissed +
               "</ul></details>";
    }
    return out;
}

namespace {

// A registry list as text: ["M-1", "M-2"] → "M-1, M-2".
std::string joined(const nlohmann::json& list, std::string_view separator = ", ") {
    std::string out;
    for (const nlohmann::json& item : list.is_array() ? list : nlohmann::json::array()) {
        const std::string text = item.is_string() ? item.get<std::string>()
                                 : item.is_object() ? json_text(item, "role").empty()
                                                          ? json_text(item, "name")
                                                          : json_text(item, "role")
                                                    : item.dump();
        if (text.empty()) continue;
        if (!out.empty()) out += separator;
        out += text;
    }
    return out;
}

// "Churn & Retention" → "churn-retention": a default project name.
std::string slug_text(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

std::string runs_badge_of(const nlohmann::json& entry) {
    if (json_text(entry, "status") == "sketch") {
        return "<span class=\"badge\">In design</span>";
    }
    if (entry.value("runnable", false)) {
        return std::string("<span class=\"badge status-success\">") +
               (entry.value("partial", false) ? "Runs, in part" : "Runs today") + "</span>";
    }
    return "<span class=\"badge status-warning\">Not yet</span>";
}

bool run_is_active(const nlohmann::json& run) {
    const std::string status = json_text(run, "status");
    return status == "queued" || status == "pending" || status == "running";
}

std::string run_status_badge(const std::string& status) {
    const bool successful = status == "succeeded" || status == "pass" || status == "within bounds";
    const bool failed = status == "failed" || status == "fail" || status == "attention needed";
    const bool active =
        status == "warn" || status == "queued" || status == "pending" || status == "running";
    const std::string modifier = successful ? " status-success"
                                 : failed   ? " status-danger"
                                 : active   ? " status-warning"
                                            : "";
    return "<span class=\"badge" + modifier + "\">" +
           web_ui_escape(status.empty() ? "unknown" : status) + "</span>";
}

std::string solution_runs_content(const nlohmann::json& listing, const std::string& query) {
    const nlohmann::json runs = listing.value("runs", nlohmann::json::array());
    std::string out = "<section class=\"panel\"><h2>Runs</h2>";
    if (runs.empty()) {
        return out + "<p>No runs yet.</p></section>";
    }
    bool refresh = false;
    out += "<table><thead><tr><th>Run</th><th>Status</th><th>Stage Reached</th>"
           "<th>Started</th><th>Error</th><th></th></tr></thead><tbody>";
    for (const nlohmann::json& run : runs) {
        const std::string id = json_text(run, "id");
        const std::string status = json_text(run, "status");
        refresh = refresh || run_is_active(run);
        out += std::format(
            "<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>",
            identifier_markup(id),
            run_status_badge(status),
            web_ui_escape(json_text(run, "stage").empty() ? "—" : json_text(run, "stage")),
            web_ui_escape(when(json_text(run, "created"))),
            json_text(run, "error").empty()
                ? std::string("—")
                : "<span class=\"error\">" + web_ui_escape(json_text(run, "error")) + "</span>",
            row_link("/train/run?" + query + "&id=" + url_component(id), "Open full run"));
    }
    out += "</tbody></table>";
    if (refresh) {
        out += "<p class=\"muted\">This page checks again every 3 seconds while a run is "
               "active.</p><meta http-equiv=\"refresh\" content=\"3\">";
    }
    return out + "</section>";
}

std::string solution_state_summary(const nlohmann::json& solution) {
    const nlohmann::json runs = solution.value("runs", nlohmann::json::array());
    if (runs.empty()) {
        return "<p class=\"sub\"><strong>Created.</strong> The solution is ready to run.</p>";
    }
    const nlohmann::json& latest = runs.front();
    const std::string status = json_text(latest, "status");
    const std::string stage = json_text(latest, "stage");
    if (run_is_active(latest)) {
        return "<p class=\"sub\"><strong>Running.</strong> " +
               web_ui_escape(stage.empty() ? "Preparing the solution." : "Reached " + stage + ".") +
               "</p>";
    }
    if (status == "failed") {
        const std::string error = json_text(latest, "error");
        return "<p class=\"sub error\"><strong>Last run failed" +
               web_ui_escape(stage.empty() ? std::string() : " at " + stage) + ".</strong>" +
               (error.empty() ? std::string() : " " + web_ui_escape(error)) + "</p>";
    }
    if (status == "succeeded") {
        return "<p class=\"sub\"><strong>Delivering.</strong> The last run succeeded.</p>";
    }
    return "<p class=\"sub\"><strong>Created.</strong> No run has completed yet.</p>";
}

std::string json_disclosure(const nlohmann::json& value) {
    return "<details><summary>View JSON</summary><pre>" + web_ui_escape(value.dump(2)) +
           "</pre></details>";
}

std::string policy_summary(const nlohmann::json& policy, const nlohmann::json& config) {
    std::string out =
        "<dl><dt>Version</dt><dd>" + web_ui_escape(cell_text(policy.value("current_version", 0))) +
        "</dd><dt>Autonomy</dt><dd>" +
        web_ui_escape(json_text(config, "autonomy").empty() ? "—" : json_text(config, "autonomy")) +
        "</dd><dt>Score threshold</dt><dd>" +
        web_ui_escape(config.contains("threshold") ? cell_text(config["threshold"]) : "—") +
        "</dd></dl>";
    const nlohmann::json tiers = config.value("tiers", nlohmann::json::array());
    if (!tiers.empty()) {
        out += "<h3>Tiers and Thresholds</h3><table><thead><tr><th>Tier</th><th>Minimum "
               "Score</th><th>Action</th></tr></thead><tbody>";
        for (const nlohmann::json& tier : tiers) {
            out += std::format("<tr><td>{}</td><td>{}</td><td>{}</td></tr>",
                               web_ui_escape(json_text(tier, "name")),
                               web_ui_escape(cell_text(tier.value("minimum_score", 0.0))),
                               web_ui_escape(json_text(tier, "action")));
        }
        out += "</tbody></table>";
    }
    return out;
}

std::string count_text(std::uint64_t count) {
    std::string out = std::to_string(count);
    for (std::size_t position = out.size(); position > 3; position -= 3) {
        out.insert(position - 3, ",");
    }
    return out;
}

std::string experiment_summary(const nlohmann::json& experiment) {
    const nlohmann::json balance =
        experiment.contains("balance") && experiment["balance"].is_object()
            ? experiment["balance"]
            : nlohmann::json::object();
    nlohmann::json allocations = balance.value("allocations", nlohmann::json::array());
    if (!allocations.is_array()) {
        allocations = nlohmann::json::array();
    }
    if (allocations.empty() && experiment.contains("allocations") &&
        experiment["allocations"].is_array()) {
        allocations = experiment["allocations"];
    }
    if (allocations.empty() && experiment.contains("holdout_fraction")) {
        const double holdout = experiment.value("holdout_fraction", 0.0);
        allocations = nlohmann::json::array(
            {{{"name", "holdout"}, {"tier", "all"}, {"fraction", holdout}},
             {{"name", "delivery"}, {"tier", "all"}, {"fraction", 1.0 - holdout}}});
    }
    if (allocations.empty()) {
        return "<p>No allocations have been configured.</p>";
    }
    std::string out;
    if (experiment.contains("sizes") && experiment["sizes"].is_object()) {
        const nlohmann::json& sizes = experiment["sizes"];
        out += "<p><strong>" + count_text(sizes.value("assignments", std::uint64_t{0})) +
               " assignments.</strong> " +
               count_text(sizes.value("holdout_assignments", std::uint64_t{0})) +
               " holdout assignments.</p>";
    }
    out += "<table><thead><tr><th>Allocation</th><th>Tier</th><th>Share</th>"
           "<th>Assignments</th><th>Expected</th><th>Balanced</th></tr></thead><tbody>";
    for (const nlohmann::json& allocation : allocations) {
        const double fraction =
            allocation.value("configured_fraction", allocation.value("fraction", 0.0));
        out += std::format(
            "<tr><td>{}</td><td>{}</td><td>{:.1f}%</td><td>{}</td><td>{}</td><td>{}</td>"
            "</tr>",
            web_ui_escape(json_text(allocation, "name")),
            web_ui_escape(json_text(allocation, "tier")),
            fraction * 100.0,
            web_ui_escape(allocation.contains("assignments")
                              ? count_text(allocation["assignments"].get<std::uint64_t>())
                              : "—"),
            web_ui_escape(allocation.contains("expected") ? cell_text(allocation["expected"])
                                                          : "—"),
            allocation.value("balanced", false) ? "Yes" : "No");
    }
    return out + "</tbody></table>";
}

std::string monitor_summary(const nlohmann::json& monitor) {
    const nlohmann::json breaches = monitor.value("breaches", nlohmann::json::array());
    const bool calibration_breached =
        monitor.value("calibration", nlohmann::json::object()).value("breached", false);
    const bool healthy = breaches.empty() && !calibration_breached;
    std::string out =
        "<p>" + run_status_badge(healthy ? "within bounds" : "attention needed") +
        "</p><dl><dt>Input drift threshold</dt><dd>" +
        web_ui_escape(monitor.contains("psi_threshold") ? cell_text(monitor["psi_threshold"])
                                                        : "—") +
        "</dd><dt>Calibration error threshold</dt><dd>" +
        web_ui_escape(monitor.contains("calibration_error_threshold")
                          ? cell_text(monitor["calibration_error_threshold"])
                          : "—") +
        "</dd></dl>";
    const nlohmann::json guardrails = monitor.value("guardrails", nlohmann::json::array());
    if (!guardrails.empty()) {
        out += "<h3>Guardrails</h3><table><thead><tr><th>Guardrail</th><th>Observed</th>"
               "<th>Status</th></tr></thead><tbody>";
        for (const nlohmann::json& guardrail : guardrails) {
            const nlohmann::json bound = guardrail.value("bound", nlohmann::json::object());
            out += std::format(
                "<tr><td>{}</td><td>{}</td><td>{}</td></tr>",
                web_ui_escape(json_text(bound, "name")),
                web_ui_escape(guardrail.contains("value") ? cell_text(guardrail["value"])
                                                          : "Not observed"),
                run_status_badge(guardrail.value("breached", false) ? "failed" : "within bounds"));
        }
        out += "</tbody></table>";
    }
    return out;
}

bool certificate_check_applies(const nlohmann::json& check, const std::string& solution) {
    const std::string status = json_text(check, "status");
    const std::string reason = json_text(check, "reason");
    const std::string name = json_text(check, "name");
    if (status != "pass") {
        return true;
    }
    if (reason.starts_with("no bound ") || reason.starts_with("no step ") ||
        reason.starts_with("no ML step ") || reason.starts_with("no model step ")) {
        return false;
    }
    std::string prefix;
    for (const char character : solution) {
        if (std::isalnum(static_cast<unsigned char>(character))) {
            prefix += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
    }
    return name.find(prefix + "_") != std::string::npos || name.ends_with("/non_empty_graph") ||
           name == "non_empty_graph";
}

std::string certificate_check_table(const nlohmann::json& checks) {
    std::string out = "<table class=\"gate-checks\"><thead><tr><th>Check</th><th>Status</th>"
                      "<th>Why</th></tr></thead><tbody>";
    for (const nlohmann::json& check : checks) {
        out += std::format("<tr><td>{}</td><td>{}</td><td>{}</td></tr>",
                           web_ui_escape(json_text(check, "name")),
                           run_status_badge(json_text(check, "status")),
                           web_ui_escape(json_text(check, "reason")));
    }
    return out + "</tbody></table>";
}

std::string certificate_check_group(const std::string& name) {
    const std::size_t slash = name.find('/');
    if (slash != std::string::npos) {
        return name.substr(0, slash);
    }
    const std::size_t underscore = name.find('_');
    if (underscore > 1 && (name.front() == 's' || name.front() == 'b') &&
        std::ranges::all_of(name.substr(1, underscore - 1), [](const char character) {
            return std::isdigit(static_cast<unsigned char>(character));
        })) {
        return std::string(1, static_cast<char>(std::toupper(name.front()))) + "-" +
               name.substr(1, underscore - 1);
    }
    return "Pipeline";
}

std::string correctness_certificate(const nlohmann::json& certificate,
                                    const std::string& solution) {
    const std::string verdict = json_text(certificate, "verdict");
    std::string out = "<h3>Correctness Certificate</h3><p><strong>Verdict:</strong> " +
                      run_status_badge(verdict) + "</p>";
    std::map<std::string, nlohmann::json> groups;
    nlohmann::json hidden = nlohmann::json::array();
    for (const nlohmann::json& check : certificate.value("checks", nlohmann::json::array())) {
        if (!certificate_check_applies(check, solution)) {
            hidden.push_back(check);
            continue;
        }
        const std::string name = json_text(check, "name");
        groups[certificate_check_group(name)].push_back(check);
    }
    for (const auto& [group, checks] : groups) {
        out += "<details><summary>" + web_ui_escape(group) + " (" + std::to_string(checks.size()) +
               " checks)</summary>" + certificate_check_table(checks) + "</details>";
    }
    if (!hidden.empty()) {
        out += "<details><summary>Show all checks (" + std::to_string(hidden.size()) +
               " not applicable)</summary>" + certificate_check_table(hidden) + "</details>";
    }
    return out;
}

std::string roi_summary(const nlohmann::json& readout) {
    const nlohmann::json statement = readout.value("roi_statement", nlohmann::json::object());
    const nlohmann::json model = statement.value("model", nlohmann::json::object());
    std::string out;
    if (statement.value("claimable", false)) {
        out = std::format("<p><strong>{:.1f}% return on investment.</strong> Net value {} {} "
                          "from gross value {} {}.</p>",
                          statement.value("return_on_investment", 0.0) * 100.0,
                          web_ui_escape(json_text(model, "currency")),
                          web_ui_escape(cell_text(statement.value("net_value", 0.0))),
                          web_ui_escape(json_text(model, "currency")),
                          web_ui_escape(cell_text(statement.value("gross_value", 0.0))));
    } else {
        std::string reason = json_text(statement, "reason");
        if (reason.empty() && readout.contains("incrementality") &&
            readout["incrementality"].is_object()) {
            reason = json_text(readout["incrementality"], "reason");
        }
        out = "<p><strong>ROI cannot be claimed yet.</strong> " +
              web_ui_escape(reason.empty() ? "No measured reason is available." : reason) + "</p>";
    }
    if (readout.contains("correctness_certificate")) {
        out += correctness_certificate(readout["correctness_certificate"],
                                       json_text(readout, "solution"));
    }
    return out;
}

// Design → Methodologies (lake#291): the engine's methodology registry (zetta#1061).
std::string methodologies_content(const WorkspacePage& page) {
    const std::string query = customer_query(page.customer_id);
    const nlohmann::json listed = page.data.value("list_methodologies", nlohmann::json::object())
                                      .value("methodologies", nlohmann::json::array());
    const nlohmann::json solutions = page.data.value("list_solutions", nlohmann::json::object())
                                         .value("solutions", nlohmann::json::array());
    if (listed.empty()) {
        return "<div class=\"panel empty\">This workspace's engine does not list its methodologies "
               "yet. It runs tabular prediction (M-1).</div>";
    }
    const auto used_by = [&](const std::string& id) {
        std::string out;
        for (const nlohmann::json& solution : solutions) {
            for (const char* key : {"methodologies", "composition"}) {
                const nlohmann::json list = solution.value(key, nlohmann::json::array());
                bool found = false;
                for (const nlohmann::json& item : list.is_array() ? list : nlohmann::json::array()) {
                    const std::string text = item.is_string() ? item.get<std::string>()
                                             : !json_text(item, "methodology").empty()
                                                 ? json_text(item, "methodology")
                                                 : json_text(item, "id");
                    if (text == id) found = true;
                }
                if (found) {
                    if (!out.empty()) out += ", ";
                    out += row_link("/design/solution?" + query + "&id=" +
                                        url_component(json_text(solution, "id")),
                                    json_text(solution, "id") + " " + json_text(solution, "name"));
                    break;
                }
            }
        }
        return out;
    };
    if (page.section == "methodology") {
        for (const nlohmann::json& entry : listed) {
            if (json_text(entry, "id") != page.subject) continue;
            std::string out = std::format(
                "<p class=\"sub\">{} · {}</p><section class=\"panel\"><table><tbody>"
                "<tr><th>What it needs</th><td>{}</td></tr><tr><th>What it makes</th><td>{}</td></tr>"
                "<tr><th>How it is measured</th><td>{}</td></tr><tr><th>Model kinds</th><td>{}</td></tr>"
                "<tr><th>Solutions that use it</th><td>{}</td></tr>{}</tbody></table></section>",
                web_ui_escape(json_text(entry, "family")),
                runs_badge_of(entry),
                web_ui_escape(joined(entry.value("inputs", nlohmann::json::array()))),
                web_ui_escape(joined(entry.value("outputs", nlohmann::json::array()))),
                web_ui_escape(joined(entry.value("metrics", nlohmann::json::array()))),
                web_ui_escape(joined(entry.value("model_kinds", nlohmann::json::array()))),
                used_by(json_text(entry, "id")).empty() ? std::string("—")
                                                        : used_by(json_text(entry, "id")),
                entry.value("runnable", false) || json_text(entry, "issue").empty()
                    ? std::string()
                    : "<tr><th>Tracked in</th><td>" + web_ui_escape(json_text(entry, "issue")) +
                          "</td></tr>");
            if (entry.value("runnable", false)) {
                out += "<p><a class=\"button button-primary\" href=\"/design/projects?" + query +
                       "\">Build a pipeline with it</a> <span class=\"sub\">Open a project and "
                       "choose " + web_ui_escape(json_text(entry, "id")) +
                       " under Build a Pipeline.</span></p>";
            }
            return out;
        }
        return "<div class=\"panel empty\">No methodology " + web_ui_escape(page.subject) +
               ".</div>";
    }
    std::string out, family;
    for (const nlohmann::json& entry : listed) {
        if (json_text(entry, "family") != family) {
            if (!family.empty()) out += "</tbody></table></section>";
            family = json_text(entry, "family");
            out += "<section class=\"panel\"><h2>" + web_ui_escape(family) +
                   "</h2><table><thead><tr><th>Methodology</th><th>What it makes</th><th>Status</th>"
                   "</tr></thead><tbody>";
        }
        out += std::format("<tr><td>{}</td><td>{}</td><td>{}</td></tr>",
                           row_link("/design/methodology?" + query + "&id=" +
                                        url_component(json_text(entry, "id")),
                                    json_text(entry, "id") + " " + json_text(entry, "name")),
                           web_ui_escape(joined(entry.value("outputs", nlohmann::json::array()))),
                           runs_badge_of(entry));
    }
    if (!family.empty()) out += "</tbody></table></section>";
    return out;
}

// Design → Solutions (lake#290): the engine's solution registry (zetta#1102). Product copy only.
std::string solutions_content(const WorkspacePage& page) {
    const std::string query = customer_query(page.customer_id);
    const bool listed_any =
        page.data.value("list_solutions", nlohmann::json::object()).contains("solutions") ||
        page.data.value("get_solution", nlohmann::json::object()).contains("id") ||
        page.data.value("get_solution", nlohmann::json::object()).contains("solution");
    if (!listed_any) {
        return "<div class=\"panel empty\">This workspace's engine does not list solutions yet. "
               "The solutions catalogue arrives with the next engine update; meanwhile see "
               "<a href=\"/design/methodologies?" + query + "\">Methodologies</a>.</div>";
    }
    const auto methodology_links = [&](const nlohmann::json& list) {
        std::string out;
        for (const nlohmann::json& item : list.is_array() ? list : nlohmann::json::array()) {
            const std::string id = item.is_string()                          ? item.get<std::string>()
                                   : !json_text(item, "methodology").empty() ? json_text(item, "methodology")
                                                                             : json_text(item, "id");
            if (id.empty()) continue;
            if (!out.empty()) out += ", ";
            const int level = item.is_object() ? item.value("minimum_level", 1) : 1;
            out += row_link("/design/methodology?" + query + "&id=" + url_component(id), id) +
                   (level > 1 ? " <span class=\"sub\">(level " + std::to_string(level) + "+)</span>" : "");
        }
        return out.empty() ? std::string("—") : out;
    };
    if (page.section == "solution") {
        const nlohmann::json entry = page.data.value("get_solution", nlohmann::json::object());
        const nlohmann::json solution =
            entry.contains("solution") && entry["solution"].is_object() ? entry["solution"] : entry;
        if (json_text(solution, "id").empty()) {
            return "<div class=\"panel empty\">No solution " + web_ui_escape(page.subject) + ".</div>";
        }
        std::string levels;
        for (const nlohmann::json& level : solution.value("capability_levels", solution.value("levels", nlohmann::json::array()))) {
            levels += "<li><strong>" + web_ui_escape(json_text(level, "name").empty()
                                                         ? json_text(level, "id")
                                                         : json_text(level, "name")) +
                      "</strong> " + web_ui_escape(json_text(level, "description")) + "</li>";
        }
        std::string out = std::format(
            "<p class=\"sub\">{} · {}</p><section class=\"panel\"><p>{}</p><table><tbody>"
            "<tr><th>The problem</th><td>{}</td></tr><tr><th>Methodologies</th><td>{}</td></tr>"
            "<tr><th>How it acts</th><td>{}</td></tr><tr><th>Decision speed</th><td>{}</td></tr>"
            "<tr><th>What data it needs</th><td>{}</td></tr>{}</tbody></table>{}</section>",
            web_ui_escape(json_text(solution, "id")),
            runs_badge_of(solution),
            web_ui_escape(json_text(solution, "promise")),
            web_ui_escape(json_text(solution, "problem").empty() ? json_text(solution, "use_case")
                                                                 : json_text(solution, "problem")),
            methodology_links(solution.value("methodologies",
                                             solution.value("composition", nlohmann::json::array()))),
            web_ui_escape(joined(solution.value("action_patterns", nlohmann::json::array()))),
            web_ui_escape(json_text(solution, "latency").empty()
                              ? joined(solution.value("latency_classes", nlohmann::json::array()))
                              : json_text(solution, "latency")),
            web_ui_escape(joined(solution.value("required_data_roles", solution.value("data_roles", nlohmann::json::array())))),
            solution.value("runnable", false) || json_text(solution, "issue").empty()
                ? std::string()
                : "<tr><th>Tracked in</th><td>" + web_ui_escape(json_text(solution, "issue")) +
                      "</td></tr>",
            levels.empty() ? std::string() : "<h2>Levels</h2><ul>" + levels + "</ul>");
        out += solution_runs_content(page.data.value("list_runs", nlohmann::json::object()), query);
        const bool startable = solution.value("runnable", false) || json_text(solution, "status") == "preview";
        if (startable) {
            std::string options;
            for (const nlohmann::json& level : solution.value("capability_levels", solution.value("levels", nlohmann::json::array()))) {
                const std::string value = level.contains("level") && level["level"].is_number()
                                              ? std::to_string(level["level"].get<int>())
                                              : json_text(level, "id");
                options += "<option value=\"" + web_ui_escape(value) + "\">" +
                           web_ui_escape(json_text(level, "name").empty() ? json_text(level, "id")
                                                                         : json_text(level, "name")) +
                           "</option>";
            }
            std::string datasets;
            for (const nlohmann::json& dataset :
                 page.data.value("list_datasets", nlohmann::json::object())
                     .value("datasets", nlohmann::json::array())) {
                const std::string database = json_text(dataset, "database").empty()
                                                 ? std::string("datasets")
                                                 : json_text(dataset, "database");
                const std::string path = database + "." + json_text(dataset, "name");
                datasets += "<option value=\"" + web_ui_escape(path) + "\">" + web_ui_escape(path) +
                            "</option>";
            }
            out += std::format(
                "<section class=\"panel\"><h2>Start This Solution</h2><form class=\"stack\" "
                "method=\"post\" action=\"/design/solutions/start\">{}{}<label>Project name<input "
                "name=\"name\" required value=\"{}\"></label>{}<label>Dataset<select "
                "name=\"dataset\" required><option value=\"\" selected disabled>Choose a dataset"
                "</option>{}</select></label><fieldset><legend>Where Actions Go</legend><label>"
                "Delivery<select name=\"delivery_kind\" required><option value=\"\" selected "
                "disabled>Choose delivery</option><option value=\"webhook\">Webhook</option>"
                "<option value=\"salesforce\">Salesforce connector</option><option "
                "value=\"hubspot\">"
                "HubSpot connector</option></select></label><label>Webhook URL or connector "
                "endpoint"
                "<input name=\"delivery_endpoint\" required></label><label>Owner<input "
                "name=\"owner\" "
                "required></label></fieldset><fieldset><legend>Economics</legend><label>Value per "
                "outcome<input name=\"value_per_outcome\" type=\"number\" step=\"any\" required>"
                "</label><label>Investment<input name=\"investment\" type=\"number\" step=\"any\" "
                "min=\"0\" required></label><label>Currency<input name=\"currency\" required "
                "placeholder=\"USD\"></label><small>These are your inputs. lake does not supply "
                "defaults.</small></fieldset><button class=\"button "
                "button-primary\" type=\"submit\">Start</button></form></section>",
                hidden("customer_id", page.customer_id),
                hidden("solution", json_text(solution, "id")),
                web_ui_escape(slug_text(json_text(solution, "name"))),
                options.empty()
                    ? std::string("<label>Level<input name=\"level\" type=\"number\" min=\"1\" "
                                  "max=\"4\" required></label>")
                    : "<label>Level<select name=\"level\">" + options + "</select></label>",
                datasets);
        }
        return out;
    }
    const nlohmann::json listed = page.data.value("list_solutions", nlohmann::json::object())
                                      .value("solutions", nlohmann::json::array());
    std::string modules, sketches;
    for (const nlohmann::json& entry : listed) {
        const std::string row = std::format(
            "<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>",
            row_link("/design/solution?" + query + "&id=" + url_component(json_text(entry, "id")),
                     json_text(entry, "id") + " " + json_text(entry, "name")),
            web_ui_escape(json_text(entry, "promise")),
            methodology_links(entry.value("methodologies", entry.value("composition", nlohmann::json::array()))),
            runs_badge_of(entry));
        (json_text(entry, "status") == "sketch" ? sketches : modules) += row;
    }
    std::string out;
    const std::string head = "<table><thead><tr><th>Solution</th><th>What it does</th>"
                             "<th>Methodologies</th><th>Status</th></tr></thead><tbody>";
    if (!modules.empty()) out += "<section class=\"panel\"><h2>Modules</h2>" + head + modules + "</tbody></table></section>";
    if (!sketches.empty()) out += "<section class=\"panel\"><h2>In Design</h2>" + head + sketches + "</tbody></table></section>";
    return out.empty() ? "<div class=\"panel empty\">No solutions listed.</div>" : out;
}

nlohmann::json proposed_solution_foundation(const nlohmann::json& task) {
    const nlohmann::json answer = task.value("answer", nlohmann::json::object());
    const nlohmann::json result = answer.value("result", nlohmann::json::object());
    nlohmann::json proposal = result.value("foundation", nlohmann::json::object());
    if (!proposal.is_object()) {
        return nlohmann::json::object();
    }
    const nlohmann::json context = task.value("context", nlohmann::json::object());
    if (context.contains("delivery")) {
        proposal["delivery"] = context["delivery"];
    }
    if (context.contains("roi_model")) {
        proposal["roi_model"] = context["roi_model"];
    }
    nlohmann::json sources = proposal.value("sources", nlohmann::json::object());
    if (!sources.is_object()) {
        sources = nlohmann::json::object();
    }
    const std::string dataset = context.value("dataset", std::string());
    const std::size_t dot = dataset.find('.');
    if (!dataset.empty()) {
        sources["source"] = dot == std::string::npos ? "datasets" : dataset.substr(0, dot);
        sources["table"] = dot == std::string::npos ? dataset : dataset.substr(dot + 1);
    }
    proposal["sources"] = std::move(sources);
    return proposal;
}

nlohmann::json missing_solution_predicates(const nlohmann::json& task,
                                           const nlohmann::json& proposal) {
    nlohmann::json missing = proposal.value("missing_predicates", nlohmann::json::array());
    if (!missing.is_array()) {
        missing = nlohmann::json::array();
    }
    if (task.value("context", nlohmann::json::object()).value("solution", std::string()) != "S-1") {
        return missing;
    }
    const nlohmann::json predicates = proposal.value("predicates", nlohmann::json::object());
    for (const char* name : {"in_collections", "open_complaint", "vulnerable"}) {
        const bool named = predicates.is_object() && predicates.contains(name) &&
                           predicates.at(name).is_string() &&
                           !predicates.at(name).get_ref<const std::string&>().empty();
        if (!named && std::ranges::find(missing, nlohmann::json(name)) == missing.end()) {
            missing.push_back(name);
        }
    }
    return missing;
}

std::string solution_proposal_content(const WorkspacePage& page, const nlohmann::json& task) {
    const std::string status = json_text(task, "status");
    if (status == "pending" || status == "running") {
        return "<section class=\"panel notice\" role=\"status\"><h2>Foundation Proposal</h2>"
               "<p><strong>The Project Agent is reading the dataset…</strong> It is proposing "
               "the snapshot boundary, column roles, methodology inputs and policy predicates. "
               "Nothing is created until you confirm. This page checks again every 3 seconds."
               "</p><p>" +
               web_ui_escape(json_text(task, "message")) +
               "</p><meta http-equiv=\"refresh\" content=\"3\"></section>";
    }
    if (status == "failed") {
        return "<p class=\"error\" role=\"alert\">The Project Agent could not propose the "
               "foundation: " +
               web_ui_escape(json_text(task, "message")) + "</p>";
    }
    const nlohmann::json proposal = proposed_solution_foundation(task);
    if (proposal.empty()) {
        return status == "succeeded"
                   ? "<p class=\"error\" role=\"alert\">The Project Agent returned no foundation "
                     "proposal.</p>"
                   : std::string();
    }
    std::string missing;
    for (const nlohmann::json& name : missing_solution_predicates(task, proposal)) {
        if (!missing.empty()) {
            missing += ", ";
        }
        missing += name.is_string() ? name.get<std::string>() : name.dump();
    }
    std::string notice;
    if (!missing.empty()) {
        notice = "<p class=\"error\" role=\"alert\"><strong>Actions will remain blocked.</strong> "
                 "The dataset is missing these exclusion predicates: " +
                 web_ui_escape(missing) + ". They are not treated as false.</p>";
    }
    return std::format(
        "<section class=\"panel\"><h2>Foundation Proposal</h2><p>Review and edit the exact "
        "foundation sent to the engine. Delivery and economics are your inputs from Start.</p>{}"
        "<form class=\"stack\" method=\"post\" action=\"/design/solution/confirm\">{}{}{}"
        "<label>Foundation (JSON)<textarea name=\"foundation\" rows=\"28\" required>{}</textarea>"
        "</label><label><input type=\"checkbox\" name=\"confirm\" value=\"yes\" required> I "
        "reviewed this snapshot, these roles, predicates, delivery settings and economics.</label>"
        "<button class=\"button button-primary\" type=\"submit\">Confirm and create solution"
        "</button></form></section>",
        notice,
        hidden("customer_id", page.customer_id),
        hidden("project", page.subject),
        hidden("task_id", json_text(task, "task_id")),
        web_ui_escape(proposal.dump(2)));
}

bool solution_cell(const std::string& cell, const std::string& scope) {
    return !scope.empty() &&
           (cell == scope ||
            (cell.starts_with(scope) && cell.size() > scope.size() && cell[scope.size()] == '/'));
}

std::vector<const nlohmann::json*> solution_ledger_entries(const nlohmann::json& solution) {
    const nlohmann::json foundation = solution.value("foundation", nlohmann::json::object());
    const std::string scope = json_text(foundation, "scope");
    std::vector<const nlohmann::json*> scoped;
    if (!solution.contains("ledger") || !solution["ledger"].is_object() ||
        !solution["ledger"].contains("entries") || !solution["ledger"]["entries"].is_array()) {
        return scoped;
    }
    const nlohmann::json& entries = solution["ledger"]["entries"];
    for (const nlohmann::json& entry : entries) {
        if (json_text(entry, "kind") != "action" ||
            !solution_cell(json_text(entry, "cell"), scope)) {
            continue;
        }
        scoped.push_back(&entry);
    }
    std::ranges::sort(scoped, [](const nlohmann::json* left, const nlohmann::json* right) {
        return json_text(*left, "recorded_at") > json_text(*right, "recorded_at");
    });
    return scoped;
}

std::string outcome_form(const WorkspacePage& page, const nlohmann::json& entry) {
    return std::format(
        "<details><summary>Record outcome</summary><form class=\"stack\" method=\"post\" "
        "action=\"/design/solution/outcome\">{}{}{}<label>Outcome<input name=\"outcome\" "
        "required></label><label>Value<input name=\"value\" type=\"number\" step=\"any\" "
        "required></label><label>Currency<input name=\"currency\" required value=\"{}\">"
        "</label><label>Occurred at<input name=\"occurred_at\" required placeholder=\""
        "2026-09-28T12:00:00Z\"></label><button type=\"submit\">Record</button></form>"
        "</details>",
        hidden("customer_id", page.customer_id),
        hidden("project", page.subject),
        hidden("action_id", json_text(entry, "id")),
        web_ui_escape(json_text(entry, "currency")));
}

std::string approval_form(const WorkspacePage& page, const nlohmann::json& decision) {
    return std::format(
        "<form method=\"post\" action=\"/design/solution/approve\">{}{}{}"
        "<input type=\"hidden\" name=\"confirm\" value=\"yes\"><button type=\"submit\">"
        "Approve A1</button></form>",
        hidden("customer_id", page.customer_id),
        hidden("project", page.subject),
        hidden("decision_id", json_text(decision, "id")));
}

using DecisionLookup = std::map<std::string, const nlohmann::json*>;

DecisionLookup decision_lookup(const nlohmann::json& decisions) {
    std::map<std::string, const nlohmann::json*> decisions_by_id;
    if (decisions.is_array()) {
        for (const nlohmann::json& decision : decisions) {
            decisions_by_id[json_text(decision, "id")] = &decision;
        }
    }
    return decisions_by_id;
}

struct SolutionActionCounts {
    std::size_t decisions = 0;
    std::size_t actions = 0;
    std::size_t held = 0;
};

SolutionActionCounts solution_action_counts(const std::vector<const nlohmann::json*>& entries) {
    SolutionActionCounts counts;
    for (const nlohmann::json* entry : entries) {
        const std::string action = json_text(*entry, "action");
        if (action == "publish_driver_report") {
            continue;
        }
        ++counts.decisions;
        if (action == "no_contact") {
            ++counts.held;
        } else {
            ++counts.actions;
        }
    }
    return counts;
}

std::string latest_actions_content(const WorkspacePage& page,
                                   const std::vector<const nlohmann::json*>& entries) {
    std::string action_rows;
    std::size_t shown_actions = 0;
    for (const nlohmann::json* entry : entries) {
        const std::string action = json_text(*entry, "action");
        if (action == "no_contact" || action == "publish_driver_report") {
            continue;
        }
        action_rows += std::format(
            "<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}<br>{}</td><td>{}</td><td>{}"
            "</td></tr>",
            web_ui_escape(json_text(*entry, "subject")),
            web_ui_escape(action),
            web_ui_escape(json_text(*entry, "cell")),
            web_ui_escape(entry->contains("score") ? cell_text(entry->at("score")) : "—"),
            web_ui_escape(json_text(*entry, "model_version")),
            web_ui_escape(json_text(*entry, "policy_version")),
            web_ui_escape(json_text(*entry, "recorded_at")),
            outcome_form(page, *entry));
        if (++shown_actions == 10) {
            break;
        }
    }
    if (action_rows.empty()) {
        return "<h3>Latest Actions</h3><p>No delivered actions are recorded for this "
               "solution.</p>";
    }
    return "<h3>Latest Actions</h3><table><thead><tr><th>Entity</th><th>Action</th>"
           "<th>Cell</th><th>Score</th><th>Model / Policy</th><th>Recorded</th><th></th>"
           "</tr></thead><tbody>" +
           action_rows + "</tbody></table>";
}

bool pending_a1(const nlohmann::json& decision) {
    return json_text(decision, "autonomy") == "A1" &&
           json_text(decision, "status") == "pending_approval";
}

std::string
queue_row(const WorkspacePage& page, const nlohmann::json* entry, const nlohmann::json* decision) {
    const bool pending = decision != nullptr && pending_a1(*decision);
    const std::string subject =
        entry == nullptr ? json_text(*decision, "entity_id") : json_text(*entry, "subject");
    const std::string proposed =
        decision == nullptr ? "no_contact" : json_text(*decision, "action");
    const std::string cell =
        entry == nullptr ? json_text(*decision, "cell") : json_text(*entry, "cell");
    const nlohmann::json* scored = entry == nullptr ? decision : entry;
    const std::string score = scored->contains("score") ? cell_text(scored->at("score")) : "—";
    const std::string governance = pending ? "A1 · pending approval" : "Held · no action delivered";
    const std::string controls = pending ? approval_form(page, *decision) : "—";
    return std::format(
        "<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>",
        web_ui_escape(subject),
        web_ui_escape(proposed),
        web_ui_escape(cell),
        web_ui_escape(score),
        web_ui_escape(governance),
        controls);
}

std::string a1_queue_content(const WorkspacePage& page,
                             const std::vector<const nlohmann::json*>& entries,
                             const DecisionLookup& decisions_by_id) {
    std::map<std::string, const nlohmann::json*> held_by_decision;
    for (const nlohmann::json* entry : entries) {
        if (json_text(*entry, "action") == "no_contact") {
            held_by_decision[json_text(*entry, "decision")] = entry;
        }
    }

    std::string queue_rows;
    std::size_t shown_queue = 0;
    for (const auto& [id, decision] : decisions_by_id) {
        if (!pending_a1(*decision)) {
            continue;
        }
        const auto held = held_by_decision.find(id);
        queue_rows +=
            queue_row(page, held == held_by_decision.end() ? nullptr : held->second, decision);
        if (++shown_queue == 10) {
            break;
        }
    }
    for (const nlohmann::json* entry : entries) {
        if (shown_queue == 10 || json_text(*entry, "action") != "no_contact") {
            continue;
        }
        const auto decision = decisions_by_id.find(json_text(*entry, "decision"));
        if (decision != decisions_by_id.end() && pending_a1(*decision->second)) {
            continue;
        }
        queue_rows +=
            queue_row(page, entry, decision == decisions_by_id.end() ? nullptr : decision->second);
        ++shown_queue;
    }
    if (queue_rows.empty()) {
        return "<h3>A1 Queue</h3><p>No held or approval-pending decisions are recorded for this "
               "solution.</p>";
    }
    return "<h3>A1 Queue</h3><table><thead><tr><th>Entity</th><th>Proposed Action</th>"
           "<th>Cell</th><th>Score</th><th>Governance</th><th></th></tr></thead><tbody>" +
           queue_rows + "</tbody></table>";
}

std::string solution_decisions_content(const WorkspacePage& page, const nlohmann::json& solution) {
    const std::vector<const nlohmann::json*> entries = solution_ledger_entries(solution);
    const nlohmann::json decisions = solution.value("decisions", nlohmann::json::array());
    const DecisionLookup decisions_by_id = decision_lookup(decisions);
    const SolutionActionCounts counts = solution_action_counts(entries);
    if (counts.decisions == 0 && decisions_by_id.empty()) {
        return "<section class=\"panel\"><h2>Actions and A1 Queue</h2><p>No decisions have been "
               "produced yet. Run the solution to populate delivered, held and approval-pending "
               "actions.</p></section>";
    }
    std::string out =
        "<section class=\"panel\"><h2>Actions and A1 Queue</h2><h3>Decision and Action "
        "Counts</h3><dl><dt>Decisions</dt><dd>" +
        count_text(counts.decisions) + "</dd><dt>Actions</dt><dd>" + count_text(counts.actions) +
        "</dd><dt>Held</dt><dd>" + count_text(counts.held) + "</dd></dl>";
    out += latest_actions_content(page, entries);
    out += a1_queue_content(page, entries, decisions_by_id);
    return out +
           "<p class=\"sub\">Approvals change the decision state; they do not silently deliver "
           "an action.</p></section>";
}

std::string active_solution_content(const WorkspacePage& page,
                                    const nlohmann::json& solution,
                                    const nlohmann::json& task) {
    std::string stages;
    for (const nlohmann::json& stage : solution.value("stages", nlohmann::json::array())) {
        stages += std::format("<tr><td>{}</td><td>{}</td><td>{}</td></tr>",
                              web_ui_escape(json_text(stage, "stage")),
                              web_ui_escape(json_text(stage, "name")),
                              web_ui_escape(json_text(stage, "kind")));
    }
    const nlohmann::json proposal = proposed_solution_foundation(task);
    const nlohmann::json sources = solution.value("sources", nlohmann::json::object());
    const nlohmann::json missing = sources.contains("missing_policy_predicates")
                                       ? sources["missing_policy_predicates"]
                                       : missing_solution_predicates(task, proposal);
    std::string run;
    if (missing.empty()) {
        run = std::format(
            "<form class=\"stack\" method=\"post\" action=\"/design/solution/run\">{}{}{}"
            "<label>Delivery token (optional, used for this run only)<input type=\"password\" "
            "name=\"delivery_token\" autocomplete=\"off\"></label><button class=\"button "
            "button-primary\" type=\"submit\">Run</button></form>",
            hidden("customer_id", page.customer_id),
            hidden("project", page.subject),
            hidden("graph", json_text(solution, "graph")));
    } else {
        run = "<p class=\"error\" role=\"alert\">Run is blocked because an exclusion predicate "
              "is missing. Missing evidence is never treated as false.</p>";
    }
    std::string out =
        "<section class=\"panel\"><h2>Solution Stages</h2><table><thead><tr><th>Stage</th><th>Name"
        "</th><th>Kind</th></tr></thead><tbody>" +
        stages + "</tbody></table>" + run + "</section>";

    const nlohmann::json policy = solution.value("policy", nlohmann::json::object());
    nlohmann::json config = policy.value("config", nlohmann::json::object());
    const nlohmann::json versions = policy.value("versions", nlohmann::json::array());
    if (versions.is_array() && !versions.empty() && versions.back().is_object()) {
        config = versions.back().value("config", config);
    }
    if (!policy.empty()) {
        out += std::format(
            "<section class=\"panel\"><h2>Decision Policy</h2>{}{}<details><summary>"
            "Propose a New Policy Version</summary><form class=\"stack\" method=\"post\" action=\""
            "/design/solution/policy\">{}{}{}<label>Configuration (JSON)<textarea name=\"config\" "
            "rows=\"16\" required>{}</textarea></label><label>Reason<input name=\"reason\" "
            "required></label><label><input type=\"checkbox\" name=\"confirm\" value=\"yes\" "
            "required> I approve creating this policy version.</label><button type=\"submit\">"
            "Create policy version</button></form></details></section>",
            policy_summary(policy, config),
            json_disclosure(policy),
            hidden("customer_id", page.customer_id),
            hidden("project", page.subject),
            hidden("policy_id", json_text(policy, "id")),
            web_ui_escape(config.dump(2)));
    }
    if (solution.contains("experiment")) {
        out += "<section class=\"panel\"><h2>Experiment and Holdout</h2>" +
               experiment_summary(solution["experiment"]) +
               json_disclosure(solution["experiment"]) + "</section>";
    }
    out += solution_decisions_content(page, solution);
    if (solution.contains("monitor")) {
        out += "<section class=\"panel\"><h2>Monitor</h2>" + monitor_summary(solution["monitor"]) +
               json_disclosure(solution["monitor"]) + "</section>";
    }
    if (solution.contains("roi")) {
        out += "<section class=\"panel\"><h2>ROI Readout</h2>" + roi_summary(solution["roi"]) +
               json_disclosure(solution["roi"]) + "</section>";
    }
    return out;
}

} // namespace

std::string workspace_content(const WorkspacePage& page) {
    if (page.section == "methodologies" || page.section == "methodology") {
        return methodologies_content(page);
    }
    if (page.section == "solutions" || page.section == "solution") {
        return solutions_content(page);
    }
    if (page.section == "pipeline") {
        return pipeline_content(page);
    }
    if (page.section == "index") {
        return index_content(page);
    }
    if (page.section == "agents") {
        return agents_content(page);
    }
    if (page.section == "discover") {
        return discover_content(page);
    }
    if (page.section == "agent") {
        return agent_content(page);
    }
    const nlohmann::json& data = page.data;
    const std::string query = customer_query(page.customer_id);
    std::string out;
    if (page.section == "projects") {
        const nlohmann::json projects = data.value("list_projects", nlohmann::json::object())
                                            .value("projects", nlohmann::json::array());
        if (projects.empty()) {
            out += "<div class=\"panel empty\">No projects yet.</div>";
        } else {
            out += "<table><thead><tr><th>Project</th></tr></thead><tbody>";
            for (const nlohmann::json& name : projects) {
                const std::string text = name.is_string() ? name.get<std::string>() : name.dump();
                out += "<tr><td>" +
                       row_link("/design/project?" + query + "&name=" + url_component(text), text) +
                       "</td></tr>";
            }
            out += "</tbody></table>";
        }
        out += std::format("<p><a class=\"button\" href=\"/design/opportunities?{}\">See your "
                           "Opportunities</a> Not sure where to start? Auto-Suggest recommends "
                           "what your data could be used for.</p>",
                           query);
        out += std::format(
            "<section class=\"panel\"><h2>New Project</h2><form class=\"stack\" method=\"post\" "
            "action=\"/design/projects/create\">{}<label>Name<input name=\"name\" required "
            "pattern=\"[a-z0-9][a-z0-9_\\-]{{0,62}}\" placeholder=\"retention-review\"></label>"
            "<label>Description<input name=\"description\" placeholder=\"Which enterprise accounts "
            "will not renew?\"></label><label>The decision it should improve<textarea "
            "name=\"intent\" rows=\"3\" placeholder=\"Which accounts the team calls first each "
            "week, measured by accounts saved per quarter\"></textarea></label><button "
            "class=\"button button-primary\" type=\"submit\">Create project</button></form>"
            "</section>",
            hidden("customer_id", page.customer_id));
    } else if (page.section == "project") {
        const nlohmann::json project = data.value("get_project", nlohmann::json::object());
        const nlohmann::json graphs = project.value("graphs", nlohmann::json::array());
        const nlohmann::json solution_task = data.value("solution_task", nlohmann::json::object());
        const nlohmann::json solution_project =
            data.value("solution_project", nlohmann::json::object());
        if (!solution_project.empty()) {
            out += solution_state_summary(solution_project);
        } else {
            out += std::format("<p class=\"sub\">{}</p>",
                               web_ui_escape(json_text(project, "description").empty()
                                                 ? "No description."
                                                 : json_text(project, "description")));
        }
        if (!solution_project.empty()) {
            out += active_solution_content(page, solution_project, solution_task);
        } else if (!solution_task.empty()) {
            out += solution_proposal_content(page, solution_task);
        }
        out += solution_runs_content(data.value("list_runs", nlohmann::json::object()), query);
        // Made from an Opportunity: the Project Agent designs the pipeline in the background.
        const nlohmann::json build = data.value("build", nlohmann::json::object());
        const std::string build_status = json_text(build, "status");
        const bool designing = graphs.empty() && (build_status == "pending" ||
                                                  build_status == "Running");
        if (designing) {
            out += "<section class=\"panel notice\" role=\"status\"><p><strong>The Project Agent "
                   "is designing this project's pipeline…</strong> It reads your data and the "
                   "Opportunity this project came from; the pipeline appears here when it is "
                   "done. You can leave this page. This page checks again every 3 seconds.</p>"
                   "<p>" +
                   web_ui_escape(json_text(build, "message")) +
                   "</p><meta http-equiv=\"refresh\" content=\"3\"></section>";
        } else if (graphs.empty() && build_status == "Failed") {
            out += std::format("<p class=\"error\" role=\"alert\">The Project Agent could not "
                               "design this project's pipeline: {} Describe it below instead."
                               "</p>",
                               web_ui_escape(json_text(build, "message")));
        }
        out += "<section class=\"panel\"><h2>Pipelines</h2>";
        if (graphs.empty() && designing) {
            out += "<p>Designing…</p>";
        } else if (graphs.empty()) {
            out += "<p>No pipelines yet. Describe what the project should predict or answer, "
                   "and Zetta builds one.</p>";
        } else {
            out += "<table><thead><tr><th>Pipeline</th><th>Methodology</th><th>Approval</th>"
                   "<th></th></tr></thead><tbody>";
            for (const nlohmann::json& graph : graphs) {
                const std::string id = json_text(graph, "id");
                // Zetta verifies and runs only an approved plan; a person approves it here first.
                const bool approved = json_text(graph, "approval") == "approved";
                const std::string graph_name =
                    json_text(graph, "name").empty() ? id : json_text(graph, "name");
                // What it trains (lake#289): the engine reports each pipeline's methodology
                // and whether it can run; one that cannot trains nothing.
                std::string trains = "—";
                if (graph.contains("runnable")) {
                    const std::string methodology = json_text(graph, "methodology");
                    trains = graph.value("runnable", false)
                                 ? "<span class=\"badge status-success\">" +
                                       web_ui_escape(methodology.empty() ? "Runs" : methodology) +
                                       "</span>"
                                 : "<span class=\"badge status-warning\">Trains nothing yet" +
                                       (methodology.empty()
                                            ? std::string()
                                            : ": needs " + web_ui_escape(methodology)) +
                                       "</span>";
                }
                out += std::format(
                    "<tr><td>{}</td><td>{}</td><td>{}</td><td><form method=\"post\" action=\"{}\">{}{}{}"
                    "<button type=\"submit\">{}</button></form></td></tr>",
                    row_link("/design/pipeline?" + query +
                                 "&project=" + url_component(page.subject) +
                                 "&graph=" + url_component(graph_name),
                             graph_name),
                    trains,
                    web_ui_escape(json_text(graph, "approval")),
                    approved ? "/design/project/run" : "/design/project/approve",
                    hidden("customer_id", page.customer_id),
                    hidden("project", page.subject),
                    hidden("graph", id),
                    approved ? "Check and run" : "Approve");
            }
            out += "</tbody></table>";
        }
        out += std::format(
            "</section><section class=\"panel\"><h2>Build a Pipeline</h2><form class=\"stack\" "
            "method=\"post\" action=\"/design/project/compile\">{}{}<label>What should it predict "
            "or answer?<textarea name=\"intent\" rows=\"3\" required placeholder=\"Predict which "
            "accounts will churn in the next 90 days, with the reasons\"></textarea></label>"
            "<label>From data<select name=\"dataset\"><option value=\"\">Let the builder describe "
            "its sources</option>{}</select><small>Choose a dataset and the pipeline reads it, and "
            "its runs train on it.</small></label>{}"
            "<button class=\"button button-primary\" type=\"submit\">Build pipeline</button>"
            "</form></section>",
            hidden("customer_id", page.customer_id),
            hidden("project", page.subject),
            [&] {
                std::string options;
                for (const nlohmann::json& dataset :
                     data.value("list_datasets", nlohmann::json::object())
                         .value("datasets", nlohmann::json::array())) {
                    const std::string database = json_text(dataset, "database").empty()
                                                     ? std::string("datasets")
                                                     : json_text(dataset, "database");
                    const std::string path = database + "." + json_text(dataset, "name");
                    options += std::format("<option value=\"{}\">{}</option>",
                                           web_ui_escape(path),
                                           web_ui_escape(json_text(dataset, "name")));
                }
                return options;
            }(),
            // The methodologies the engine runs (lake#289); absent on an older engine.
            [&] {
                const nlohmann::json listed = data.value("list_methodologies", nlohmann::json::object())
                                                  .value("methodologies", nlohmann::json::array());
                std::string options;
                for (const nlohmann::json& entry : listed) {
                    if (!entry.value("runnable", false)) continue;
                    options += std::format("<option value=\"{}\">{} ({})</option>",
                                           web_ui_escape(json_text(entry, "id")),
                                           web_ui_escape(json_text(entry, "name")),
                                           web_ui_escape(json_text(entry, "id")));
                }
                if (options.empty()) return std::string();
                return "<label>Methodology<select name=\"methodology\"><option value=\"\">Let Zetta "
                       "choose from the description</option>" + options +
                       "</select><small>Only the methodologies this workspace's engine runs are "
                       "listed.</small></label>";
            }());
        // Other ways to make a pipeline (docs/design/zetta-parity.md).
        out += std::format(
            "<section class=\"panel\"><h2>Other Ways to Start a Pipeline</h2><div "
            "class=\"pipeline-edit\">"
            "<form class=\"stack\" method=\"post\" action=\"/design/project/new-pipeline\">{}{}"
            "<label>Name<input name=\"name\" required placeholder=\"weekly-retention\"></label>"
            "<p class=\"muted\">An empty pipeline you build step by step.</p><button "
            "type=\"submit\">Create</button></form>"
            "<form class=\"stack\" method=\"post\" action=\"/design/project/n8n\">{}{}<label>"
            "Name<input name=\"name\" placeholder=\"n8n workflow\"></label><label>n8n workflow "
            "export (JSON)<textarea name=\"content\" rows=\"5\" required></textarea></label>"
            "<button type=\"submit\">Import from n8n</button></form>"
            "<form class=\"stack\" method=\"post\" action=\"/design/project/python\">{}{}<label>"
            "Name<input name=\"name\" placeholder=\"python program\"></label><label>Python "
            "pipeline<textarea name=\"source\" rows=\"5\" required></textarea></label><p "
            "class=\"muted\">Zetta reads it and lowers it to a pipeline; the code never runs."
            "</p><button type=\"submit\">Build from Python</button></form></div></section>",
            hidden("customer_id", page.customer_id),
            hidden("project", page.subject),
            hidden("customer_id", page.customer_id),
            hidden("project", page.subject),
            hidden("customer_id", page.customer_id),
            hidden("project", page.subject));
        out += std::format(
            "<section class=\"panel\"><details><summary>Delete project</summary><form "
            "class=\"stack\" method=\"post\" action=\"/design/project/delete\">{}{}<p>Removes the "
            "project, its intent and its pipelines. Its runs and models stay "
            "listed.</p><label>Type "
            "<code>{}</code> to delete it<input name=\"confirm\" required autocomplete=\"off\">"
            "</label><button class=\"button button-danger\" type=\"submit\">Delete project"
            "</button></form></details></section>",
            hidden("customer_id", page.customer_id),
            hidden("project", page.subject),
            web_ui_escape(page.subject));
    } else if (page.section == "data") {
        const nlohmann::json connections = data.value("list_connections", nlohmann::json::object())
                                               .value("connections", nlohmann::json::array());
        const nlohmann::json datasets = data.value("list_datasets", nlohmann::json::object())
                                            .value("datasets", nlohmann::json::array());
        // Load data comes first (lake#256): the way in, open, then what is already here.
        std::string load;
        if (const auto loaded = data.find("loaded"); loaded != data.end()) {
            const std::string table = json_text(*loaded, "table");
            out += std::format(
                "<section class=\"panel notice\" role=\"status\"><p>Loaded <strong>{}</strong> rows "
                "into {}.</p></section>",
                web_ui_escape(json_text(*loaded, "rows")),
                row_link("/train/dataset?" + query + "&database=datasets&name=" +
                             url_component(table),
                         table));
        }
        const std::size_t load_at = out.size();
        out += "<section class=\"panel\"><h2>Connections</h2>";
        if (connections.empty()) {
            out += "<p>No connections yet. Add the database your data lives in below.</p>";
        } else {
            out += "<table><thead><tr><th>Name</th><th>Type</th><th>Where</th><th>From service"
                   "</th><th>Saved</th><th></th></tr></thead><tbody>";
            for (const nlohmann::json& connection : connections) {
                const nlohmann::json connector =
                    connection.value("connector", nlohmann::json::object());
                const std::string where =
                    json_text(connector, "type") == "postgres"
                        ? json_text(connector, "host") + "/" + json_text(connector, "database")
                        : json_text(connector, "account");
                std::string from_service = "—";
                for (const nlohmann::json& service :
                     data.value("list_services", nlohmann::json::object())
                         .value("services", nlohmann::json::array())) {
                    if (json_text(service, "name") == json_text(connection, "name") &&
                        !json_text(connector, "host").empty() &&
                        json_text(service, "endpoint").starts_with(json_text(connector, "host"))) {
                        from_service = row_link("/services?" + query, json_text(service, "name"));
                    }
                }
                out += std::format(
                    "<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td><form "
                    "method=\"post\" action=\"/train/data/disconnect\">{}{}<button "
                    "type=\"submit\">Remove</button></form></td></tr>",
                    web_ui_escape(json_text(connection, "name")),
                    web_ui_escape(json_text(connector, "type")),
                    web_ui_escape(where) + " · " +
                        row_link("/train/data?" + query +
                                     "&browse=" + url_component(json_text(connection, "name")),
                                 "Browse tables"),
                    from_service,
                    web_ui_escape(json_text(connection, "created")),
                    hidden("customer_id", page.customer_id),
                    hidden("name", json_text(connection, "name")));
            }
            out += "</tbody></table>";
        }
        // Browse one connection's tables (list_source_tables, lake#181).
        if (const auto browsed = data.find("list_source_tables"); browsed != data.end()) {
            out += std::format("<h3>Tables in {}</h3>", web_ui_escape(page.subject));
            std::string rows;
            for (const nlohmann::json& table : browsed->value("tables", nlohmann::json::array())) {
                std::string columns;
                for (const nlohmann::json& column :
                     table.value("columns", nlohmann::json::array())) {
                    columns += (columns.empty() ? "" : ", ") + json_text(column, "column") + " " +
                               json_text(column, "type");
                }
                rows += std::format(
                    "<tr><td>{}</td><td>{}</td><td>{}</td><td class=\"muted\">{}</td></tr>",
                    web_ui_escape(json_text(table, "schema")),
                    web_ui_escape(json_text(table, "table")),
                    web_ui_escape(json_text(table, "kind")),
                    web_ui_escape(columns));
            }
            out += rows.empty()
                       ? std::string("<p>No tables.</p>")
                       : "<table><thead><tr><th>Schema</th><th>Table</th><th>Kind</th>"
                         "<th>Columns</th></tr></thead><tbody>" +
                             rows +
                             "</tbody></table>"
                             "<p class=\"muted\">Pull one with <em>Pull a dataset</em> below.</p>";
        }
        // Connect one of the space's own services (lake#188): one click, no password typed or
        // shown.
        std::string service_rows;
        std::set<std::string> connected_names;
        for (const nlohmann::json& connection : connections) {
            connected_names.insert(json_text(connection, "name"));
        }
        for (const nlohmann::json& service : data.value("list_services", nlohmann::json::object())
                                                 .value("services", nlohmann::json::array())) {
            const std::string kind = json_text(service, "kind");
            if (json_text(service, "desired") == "absent" || service.value("default", false) ||
                kind == "zetta") {
                continue;
            }
            const std::string name = json_text(service, "name");
            const bool ready =
                json_text(service, "phase") == "Ready" && service.value("connected", true);
            std::string action;
            if (kind != "postgres") {
                action = "<span class=\"muted\">The workspace can't read " +
                         web_ui_escape(kind) + " yet; Postgres works today</span>";
            } else if (!ready) {
                action = "<span class=\"muted\">starting</span>";
            } else {
                action = std::format("<form class=\"inline\" method=\"post\" "
                                     "action=\"/train/data/connect-service\">{}{}<button "
                                     "type=\"submit\">{}</button></form>",
                                     hidden("customer_id", page.customer_id),
                                     hidden("service_id", json_text(service, "id")),
                                     connected_names.contains(name) ? "Refresh" : "Connect");
            }
            service_rows += std::format("<tr><td>{}</td><td>{}</td><td>{}</td></tr>",
                                        web_ui_escape(name),
                                        web_ui_escape(kind),
                                        action);
        }
        if (!service_rows.empty()) {
            out += "<h3>Connect One of Your Services</h3><p class=\"muted\">The workspace reaches "
                   "it inside the cluster, with the credential lake already holds; nothing to "
                   "type.</p><table><thead><tr><th>Service</th><th>Kind</th><th></th></tr></thead>"
                   "<tbody>" +
                   service_rows + "</tbody></table>";
        }
        // The credential goes to the workspace, which stores it and never returns it; lake keeps
        // no copy, and the field is never filled back in.
        out += std::format(
            "<details><summary>Add a connection</summary><form class=\"stack\" method=\"post\" "
            "action=\"/train/data/connect\" autocomplete=\"off\">{}<label>Name<input "
            "name=\"name\" required pattern=\"[A-Za-z0-9_\\-]{{1,64}}\" "
            "placeholder=\"warehouse\"></label><label>Type<select name=\"type\"><option "
            "value=\"postgres\">Postgres</option><option value=\"snowflake\">Snowflake</option>"
            "</select></label><fieldset><legend>Postgres</legend><label>Host<input "
            "name=\"host\" placeholder=\"db.example.com\"></label><label>Port<input name=\"port\" "
            "inputmode=\"numeric\" placeholder=\"5432\"></label><label>User<input "
            "name=\"user\"></label><label>SSL mode<select name=\"sslmode\"><option "
            "value=\"require\">require</option><option value=\"verify-full\">verify-full"
            "</option><option value=\"disable\">disable</option></select></label></fieldset>"
            "<fieldset><legend>Snowflake</legend><label>Account<input name=\"account\" "
            "placeholder=\"ab12345.us-east-1\"></label><label>Warehouse<input "
            "name=\"warehouse\"></label><label>Role<input name=\"role\"></label></fieldset>"
            "<label>Database<input name=\"database\"></label><label>Schema<input "
            "name=\"schema\"></label><label>Password or access token<input type=\"password\" "
            "name=\"token\" autocomplete=\"new-password\"></label><p class=\"muted\">Use a "
            "read-only account: only a single SELECT ever runs against it.</p><button "
            "class=\"button button-primary\" type=\"submit\">Save connection</button></form>"
            "</details>",
            hidden("customer_id", page.customer_id));
        out += "</section><section class=\"panel\"><h2>Datasets</h2>";
        if (datasets.empty()) {
            out += "<p>No datasets yet.</p>";
        } else {
            out += "<table><thead><tr><th>Name</th><th>Database</th></tr>"
                   "</thead><tbody>";
            for (const nlohmann::json& dataset : datasets) {
                const std::string name = json_text(dataset, "name");
                out += std::format("<tr><td>{}</td><td>{}</td></tr>",
                                   row_link("/train/dataset?" + query + "&database=" +
                                                url_component(json_text(dataset, "database")) +
                                                "&name=" + url_component(name),
                                            name),
                                   web_ui_escape(json_text(dataset, "database")));
            }
            out += "</tbody></table>";
        }
        out += "<p class=\"muted\">Open a dataset for its row count, size, format and sample.</p>";
        out +=
            std::format("<p>{}</p>", row_link("/train/query?" + query, "Query your data with SQL"));
        load += std::format(
            "<details open><summary>Upload a file</summary><form class=\"stack\" method=\"post\" "
            "action=\"/train/data/upload\" enctype=\"multipart/form-data\">{}<label>File (CSV or "
            "JSONL, up to 16 MiB)<input type=\"file\" name=\"file\" required "
            "accept=\".csv,.jsonl,.ndjson,text/csv\"></label><label>Format<select name=\"format\">"
            "<option value=\"\">From the file name</option><option value=\"csv\">CSV</option>"
            "<option value=\"jsonl\">JSONL</option></select></label><label>Dataset name<input "
            "name=\"table_name\" required pattern=\"[A-Za-z_][A-Za-z0-9_]{{0,62}}\" "
            "placeholder=\"accounts\"></label><button class=\"button button-primary\" "
            "type=\"submit\">Upload</button></form></details>",
            hidden("customer_id", page.customer_id));
        if (!connections.empty()) {
            std::string options;
            for (const nlohmann::json& connection : connections) {
                options += std::format("<option>{}</option>",
                                       web_ui_escape(json_text(connection, "name")));
            }
            load += std::format(
                "<details><summary>Pull a table from a connection</summary><form class=\"stack\" "
                "method=\"post\" action=\"/train/data/pull\">{}<label>From<select "
                "name=\"connection\">{}</select></label><label>Query<textarea name=\"query\" "
                "rows=\"4\" required placeholder=\"SELECT account_id, plan, seats, renewed FROM "
                "accounts\"></textarea></label><label>Dataset name<input name=\"table_name\" "
                "required pattern=\"[A-Za-z_][A-Za-z0-9_]{{0,62}}\" placeholder=\"accounts\">"
                "</label><label>At most this many rows<input name=\"max_rows\" "
                "inputmode=\"numeric\" placeholder=\"100000\"></label><button class=\"button "
                "button-primary\" type=\"submit\">Pull dataset</button></form></details>",
                hidden("customer_id", page.customer_id),
                options);
        }
        // Dataset generators (list_dataset_generators / generate_dataset, lake#181).
        const nlohmann::json generators =
            data.value("list_dataset_generators", nlohmann::json::object())
                .value("generators", nlohmann::json::array());
        if (!generators.empty()) {
            std::string options;
            for (const nlohmann::json& generator : generators) {
                options += std::format("<option value=\"{}\">{}</option>",
                                       web_ui_escape(json_text(generator, "kind")),
                                       web_ui_escape(json_text(generator, "title").empty()
                                                         ? json_text(generator, "kind")
                                                         : json_text(generator, "title")));
            }
            load += std::format(
                "<details><summary>Generate a dataset</summary><form class=\"stack\" "
                "method=\"post\" "
                "action=\"/train/data/generate\">{}<label>Generator<select "
                "name=\"kind\">{}</select>"
                "</label><label>Variants per subject<input name=\"variants_per_subject\" "
                "type=\"number\" min=\"1\" max=\"50\" placeholder=\"4\"></label><button "
                "class=\"button button-primary\" "
                "type=\"submit\">Generate</button></form></details>",
                hidden("customer_id", page.customer_id),
                options);
        }
        out += "</section>";
        out.insert(load_at,
                   "<section class=\"panel\" id=\"load-data\"><h2>Load Data</h2><p class=\"sub\">"
                   "Bring data into your workspace: upload a file, pull a table from a database "
                   "you connected below, or generate one. Each becomes a dataset you can train "
                   "on. <a href=\"/docs/load-data\">How loading data works</a></p>" +
                   load + "</section>");
    } else if (page.section == "dataset") {
        const nlohmann::json dataset = data.value("get_dataset", nlohmann::json::object());
        std::string columns;
        for (const nlohmann::json& column : dataset.value("columns", nlohmann::json::array())) {
            columns += (columns.empty() ? "" : ", ") + cell_text(column);
        }
        const nlohmann::json partitions = dataset.value("partitions", nlohmann::json::array());
        const std::string database = json_text(dataset, "database").empty()
                                         ? std::string("datasets")
                                         : json_text(dataset, "database");
        out += std::format(
            "<section class=\"panel\"><dl><dt>Database</dt><dd>{}</dd><dt>Format</dt><dd>{}</dd>"
            "<dt>Rows</dt><dd>{}</dd><dt>Bytes</dt><dd>{}</dd><dt>Columns</dt><dd>{}</dd>"
            "<dt>Partitions</dt><dd>{}</dd>{}</"
            "dl>"
            "<form class=\"inline\" method=\"post\" "
            "action=\"/train/dataset/promote\">{}{}{}<button "
            "type=\"submit\">Keep durable</button></form> <span class=\"muted\">A dataset pulled "
            "from a connection is a cache with one copy; this keeps more copies.</span> "
            "{}</section>",
            web_ui_escape(database),
            web_ui_escape(json_text(dataset, "format")),
            web_ui_escape(cell_text(dataset.value("rows",
                dataset.value("expected_rows", nlohmann::json(""))))),
            web_ui_escape(json_text(dataset, "size_bytes")),
            web_ui_escape(columns),
            partitions.size(),
            dataset.value("degraded", false)
                ? "<dt>Health</dt><dd class=\"error\">" +
                      web_ui_escape(json_text(dataset, "degraded_reason")) + "</dd>"
                : std::string(),
            hidden("customer_id", page.customer_id),
            hidden("database", database),
            hidden("dataset", page.subject),
            row_link(
                "/train/query?" + query + "&statement=" +
                    url_component("SELECT * FROM " + database + "." + page.subject),
                "Query it"));
        if (const auto sample = data.find("sample"); sample != data.end()) {
            out += "<section class=\"panel\"><h2>First Rows</h2>" +
                   object_table(sample->value("rows", nlohmann::json::array())) + "</section>";
        }
        if (!partitions.empty()) {
            // How the workspace stores it: for operators, not the first thing a person reads.
            out += "<details class=\"panel\"><summary>Storage details (" +
                   std::to_string(partitions.size()) + " partitions)</summary>" +
                   object_table(partitions) + "</details>";
        }
    } else if (page.section == "query") {
        const std::string statement = json_text(data, "statement");
        out += std::format(
            "<section class=\"panel\"><form class=\"stack\" method=\"get\" "
            "action=\"/train/query\">{}"
            "<label>Query<textarea name=\"statement\" rows=\"5\" required placeholder=\"SELECT plan, "
            "COUNT(*) FROM datasets.accounts GROUP BY plan\">{}</textarea></label><label>Database<input "
            "name=\"database\" value=\"{}\" placeholder=\"datasets\"></label><button "
            "class=\"button "
            "button-primary\" type=\"submit\">Run</button></form></section>",
            hidden("customer_id", page.customer_id),
            web_ui_escape(statement),
            web_ui_escape(json_text(data, "database")));
        if (const auto result = data.find("workspace_sql"); result != data.end()) {
            nlohmann::json rows = result->value("rows", nlohmann::json::array());
            const std::size_t total = rows.size();
            if (total > 500) {
                rows.erase(rows.begin() + 500, rows.end());
            }
            out += std::format("<section class=\"panel\"><h2>{} rows</h2>{}{}</section>",
                               total,
                               total > 500 ? "<p class=\"sub\">Showing the first 500.</p>" : "",
                               rows.empty() ? std::string("<p>No rows.</p>") : object_table(rows));
        }
    } else if (page.section == "runs") {
        const nlohmann::json runs = data.value("list_runs", nlohmann::json::object())
                                        .value("runs", nlohmann::json::array());
        if (runs.empty()) {
            out += "<div class=\"panel empty\">No runs yet. Run a pipeline from a project.</div>";
        } else {
            out += "<table><thead><tr><th>Pipeline</th><th>Status</th><th>Started</th><th></th>"
                   "</tr></thead><tbody>";
            for (const nlohmann::json& run : runs) {
                const std::string id = json_text(run, "id");
                const std::string status = json_text(run, "status");
                out += std::format(
                    "<tr><td>{}</td><td><span class=\"badge{}\">{}</span></td><td>{}</td><td>{}"
                    "</td></tr>",
                    run_subject(query,
                                data,
                                json_text(run, "graph"),
                                json_text(run, "project"),
                                json_text(run, "pipeline_name"),
                                json_text(run, "project_name")),
                    status == "succeeded" ? " status-success"
                    : status == "failed"  ? " status-danger"
                                          : "",
                    web_ui_escape(status),
                    web_ui_escape(when(json_text(run, "created"))),
                    row_link("/train/run?" + query + "&id=" + url_component(id),
                             status == "failed" ? "Why it failed" : "Report") + " · " +
                    row_link("/train/experiment/run?" + query + "&run=" + url_component(id),
                             "Charts"));
            }
            out += "</tbody></table>";
        }
    } else if (page.section == "run") {
        const nlohmann::json run = data.value("get_run", nlohmann::json::object());
        out += "<p>" + row_link("/train/experiment/run?" + query + "&run=" +
            url_component(json_text(run, "id")), "Training charts") + "</p>";
        out += "<p>" + row_link("/train/streams?" + query, "Workspace streams") + "</p>";
        const std::string status = json_text(run, "status");
        out += std::format("<p>Status: <span class=\"badge\">{}</span></p>",
                           web_ui_escape(status.empty() ? "unknown" : status));
        if (!json_text(run, "graph").empty()) {
            out += "<p class=\"sub\">" +
                   run_subject(query,
                               data,
                               json_text(run, "graph"),
                               json_text(run, "project"),
                               json_text(run, "pipeline_name"),
                               json_text(run, "project_name")) +
                   (json_text(run, "created").empty()
                        ? std::string()
                        : " · started " + web_ui_escape(when(json_text(run, "created")))) +
                   "</p>";
        }
        // What the run says about itself (lake#251, lake#253): a structural run trained
        // nothing; a connect step that was not read says so.
        std::string said;
        for (const nlohmann::json& log : run.value("logs", nlohmann::json::array())) {
            const std::string message = json_text(log, "message");
            if (message.starts_with("structural:")) {
                said += "<p><strong>Nothing was trained.</strong> " +
                        web_ui_escape(message.substr(12)) + ".</p>";
            } else if (message.find(" was not read: ") != std::string::npos) {
                said += "<p>" + web_ui_escape(message.substr(message.find(' ') + 1)) + ".</p>";
            }
        }
        if (!said.empty()) {
            out += "<section class=\"panel notice\" role=\"status\">" + said + "</section>";
        }
        if (data.contains("get_run_report")) {
            out += run_report_panel(data.at("get_run_report"));
        }
        out += run_details_panel(
            run, data.value("list_traces", nlohmann::json::object()), page.subject);
        if (data.contains("get_run_report")) {
            // nothing more: the report is shown
        } else if (status == "running" || status == "queued" || status == "pending") {
            out += "<div class=\"panel\" role=\"status\">The report appears when the run "
                   "finishes. This page checks again every 15 seconds.<meta "
                   "http-equiv=\"refresh\" content=\"15\"></div>";
        }
    } else if (page.section == "models") {
        const nlohmann::json models = data.value("list_models", nlohmann::json::object())
                                          .value("models", nlohmann::json::array());
        if (models.empty()) {
            out += "<div class=\"panel empty\">No models yet. Models appear here when a run "
                   "trains one.</div>";
        } else {
            const nlohmann::json details = data.value("model_details", nlohmann::json::object());
            const std::string origin =
                json_text(data, "origin").empty() ? "https://lake.oramadev.net" : json_text(data, "origin");
            out += "<table><thead><tr><th>Model</th><th>Project</th><th>Trained</th><th>Score</th>"
                   "<th>Published</th><th></th></tr></thead><tbody>";
            std::string calls;
            for (const nlohmann::json& model : models) {
                const std::string id = json_text(model, "id");
                const nlohmann::json detail = details.value(id, model);
                const bool published = detail.value("exposed", false);
                const nlohmann::json metrics = detail.value("metrics", nlohmann::json::object());
                // A held-out score when the run measured one; otherwise say it is in-sample.
                std::string score = "<span class=\"muted\">not measured</span>";
                for (const char* key : {"held_out_auc", "holdout_auc"}) {
                    if (metrics.contains(key) && metrics[key].is_number()) {
                        score = std::format("AUC {:.3f} <small>held out</small>",
                                            metrics[key].get<double>());
                        break;
                    }
                }
                if (score.find("held out") == std::string::npos && metrics.contains("in_sample_auc") &&
                    metrics["in_sample_auc"].is_number()) {
                    score = std::format("AUC {:.3f} <small class=\"muted\">in-sample only, not held "
                                        "out</small>",
                                        metrics["in_sample_auc"].get<double>());
                }
                const std::string project =
                    json_text(model, "project_name").empty()
                        ? project_called(data, json_text(model, "project"))
                        : json_text(model, "project_name");
                out += std::format(
                    "<tr><td><strong>{}</strong><br><small class=\"muted\"><code>{}</code></small>"
                    "</td><td>{}</td><td>{}</td><td>{}</td><td><span class=\"badge{}\">{}</span></td>"
                    "<td><form class=\"inline\" method=\"post\" action=\"/ship/models/publish\">{}{}"
                    "{}<button type=\"submit\"{}>{}</button></form></td></tr>",
                    web_ui_escape(json_text(model, "name")),
                    web_ui_escape(id.substr(0, 8)),
                    project.empty() ? std::string("<span class=\"muted\">a deleted project</span>")
                                    : row_link("/design/project?" + query + "&name=" +
                                                   url_component(project),
                                               project),
                    web_ui_escape(when(json_text(model, "created"))),
                    score,
                    published ? " status-success" : "",
                    published ? "published" : "not published",
                    hidden("customer_id", page.customer_id),
                    hidden("model", id),
                    hidden("publish", published ? "0" : "1"),
                    published ? "" : " class=\"button button-primary\"",
                    published ? "Unpublish" : "Publish");
                if (published) {
                    std::string features;
                    for (const nlohmann::json& feature :
                         detail.value("feature_schema", nlohmann::json::object())
                             .value("features", nlohmann::json::array())) {
                        features += std::string(features.empty() ? "" : ", ") + "\"" +
                                    cell_text(feature) + "\": 0";
                    }
                    calls += std::format(
                        "<h3>{} <small class=\"muted\"><code>{}</code></small></h3><pre><code>"
                        "curl -s {}/v1/predict \\\n  -H \"Authorization: Bearer $LAKE_KEY\" "
                        "-H 'Content-Type: application/json' \\\n  -d '{{\"customer_id\": \"{}\", "
                        "\"model\": \"{}\", \"rows\": [{{{}}}]}}'</code></pre>",
                        web_ui_escape(json_text(model, "name")),
                        web_ui_escape(id.substr(0, 8)),
                        web_ui_escape(origin),
                        web_ui_escape(page.customer_id),
                        web_ui_escape(id),
                        web_ui_escape(features));
                }
            }
            out += "</tbody></table>";
            out += std::format(
                "<section class=\"panel\" id=\"call-a-model\"><h2>Call a Model from Your Code</h2>"
                "{}<p>Each row you send is keyed by the model's features; the answer ranks the rows "
                "by risk with what drives each score. Use a lake key from "
                "<a href=\"/developer/mcp\">Developer → MCP</a>; from an AI agent it is the MCP tool "
                "<code>predict</code>. Unpublishing a model stops every outside call at once.</p>"
                "</section>",
                calls.empty() ? std::string("<p>Publish a model to call it from outside lake.</p>")
                               : calls);
        }
    } else if (page.section == "indexes") {
        const nlohmann::json indexes = data.value("list_indexes", nlohmann::json::object())
                                           .value("indexes", nlohmann::json::array());
        if (indexes.empty()) {
            out += "<div class=\"panel empty\">No search indexes yet.</div>";
        } else {
            out += "<table><thead><tr><th>Index</th><th>Table</th><th>Kind</th><th>Fields</th>"
                   "<th>Created</th><th></th></tr></thead><tbody>";
            for (const nlohmann::json& index : indexes) {
                const std::string id = json_text(index, "id");
                std::string fields;
                for (const nlohmann::json& field : index.value("fields", nlohmann::json::array())) {
                    fields += (fields.empty() ? "" : ", ") + cell_text(field);
                }
                out += std::format(
                    "<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>",
                    row_link("/ship/index?" + query + "&id=" + url_component(id), id),
                    web_ui_escape(json_text(index, "table")),
                    web_ui_escape(json_text(index, "kind")),
                    web_ui_escape(fields),
                    web_ui_escape(json_text(index, "created")),
                    row_link("/ship/index?" + query + "&id=" + url_component(id), "Search"));
            }
            out += "</tbody></table><p class=\"muted\">Open an index to search it, publish it, "
                   "and call it from your own code.</p>";
        }
        std::string tables;
        for (const nlohmann::json& dataset : data.value("list_datasets", nlohmann::json::object())
                                                 .value("datasets", nlohmann::json::array())) {
            tables += std::format("<option>{}</option>", web_ui_escape(json_text(dataset, "name")));
        }
        out += tables.empty()
                   ? std::string("<p class=\"muted\">To make an index, first add a dataset on "
                                 "<a href=\"/train/data\">Data</a>.</p>")
                   : std::format(
                         "<section class=\"panel\"><h2>New Index</h2><form class=\"stack\" "
                         "method=\"post\" action=\"/ship/indexes/create\">{}<label>Dataset"
                         "<select name=\"table\">{}</select></label><label>Fields to search, "
                         "comma separated<input name=\"fields\" placeholder=\"all columns\">"
                         "</label><p class=\"muted\">A full-text (BM25F) index over the dataset."
                         "</p><button class=\"button button-primary\" type=\"submit\">Create "
                         "index</button></form></section>",
                         hidden("customer_id", page.customer_id),
                         tables);
    }
    return out;
}

// The Design, Train and Ship breadcrumb (docs/ui/breadcrumb.md): the stage, the list a page belongs
// to, then the entities in the URL — a project and its pipeline, a dataset, a run, an index — by
// name, or by abbreviated id when the URL carries only an id. Every level keeps the space.
std::vector<WebUiCrumb> workspace_trail(const WorkspacePage& page,
                                        const WorkspaceSection& section) {
    const std::string space =
        page.customer_id.empty() ? std::string() : customer_query(page.customer_id);
    const auto link = [&](std::string_view path, std::string extra = {}) {
        std::string query = space;
        if (!extra.empty()) {
            query += (query.empty() ? "" : "&") + extra;
        }
        return std::string(path) + (query.empty() ? "" : "?" + query);
    };
    const auto list_title = [](std::string_view path) -> std::string_view {
        for (const WorkspaceSection& list : workspace_sections) {
            if (list.path == path && !list.sub.empty() && list.key != "query" &&
                list.key != "pipeline") {
                return list.title;
            }
        }
        return {};
    };
    std::vector<WebUiCrumb> trail;
    const std::string_view stage_path = section.verb == "Design"  ? "/design/projects"
                                        : section.verb == "Train" ? "/train/data"
                                                                  : "/ship/models";
    trail.push_back({std::string(section.verb), link(stage_path), {}});
    const std::string_view list = list_title(section.path);
    if (section.title == list) {
        trail.push_back({std::string(list), link(section.path), {}});
        if (section.key == "data" && !page.subject.empty()) {
            trail.push_back(web_ui_entity_crumb(page.subject));
        }
        return trail;
    }
    trail.push_back({std::string(list), link(section.path), {}});
    if (section.key == "pipeline") {
        if (!page.project.empty()) {
            trail.push_back(web_ui_entity_crumb(
                page.project, link("/design/project", "name=" + url_component(page.project))));
        }
        trail.push_back(web_ui_entity_crumb(page.subject.empty() ? "Pipeline" : page.subject));
    } else if (page.subject.empty()) {
        trail.push_back({std::string(section.title), {}, {}});
    } else {
        trail.push_back(web_ui_entity_crumb(page.subject));
    }
    return trail;
}

} // namespace

std::optional<std::string> web_ui_docs(std::string_view slug, WebUiViewer viewer) {
    const std::vector<WebUiGuidePage>& pages = web_ui_guide();
    if (pages.empty()) {
        return std::nullopt;
    }
    const auto found = slug.empty() ? pages.begin()
                                    : std::ranges::find(pages, slug, &WebUiGuidePage::slug);
    if (found == pages.end()) {
        return std::nullopt;
    }
    std::string contents;
    for (const WebUiGuidePage& page : pages) {
        const std::string href =
            page.slug == pages.front().slug ? std::string("/docs") : "/docs/" + std::string(page.slug);
        contents += std::format("<li><a href=\"{}\"{}>{}</a></li>",
                                href,
                                page.slug == found->slug ? " aria-current=\"page\"" : "",
                                web_ui_escape(page.title));
    }
    // The next guide in reading order, so the section reads front to back.
    std::string next;
    if (const auto after = std::next(found); after != pages.end()) {
        next = std::format("<p class=\"docs-next\">Next: <a href=\"/docs/{}\">{}</a></p>",
                           after->slug,
                           web_ui_escape(after->title));
    }
    const std::string content = std::format(
        "<p class=\"eyebrow\">Docs</p><div class=\"docs\"><nav class=\"docs-contents\" "
        "aria-label=\"Docs\"><ul>{}</ul></nav><article class=\"docs-page\">{}{}</article></div>",
        contents,
        markdown_to_html(found->markdown),
        next);
    return web_ui_shell("lake — " + std::string(found->title),
                        std::vector<WebUiCrumb>{{"Docs", "/docs"}, {std::string(found->title), ""}},
                        content,
                        viewer);
}

std::string workspaces_content(const WorkspacePage& page) {
    const std::string customer = web_ui_escape(page.customer_id);
    std::string content = "<section class=\"panel\"><h2>New workspace</h2>"
        "<form method=\"post\" action=\"/workspaces/create\">"
        "<input type=\"hidden\" name=\"customer_id\" value=\"" + customer + "\">"
        "<label>Name<input name=\"name\" required maxlength=\"128\"></label>"
        "<label>Environment<input name=\"environment\" value=\"dev\" required maxlength=\"47\" "
        "pattern=\"[a-z0-9]([a-z0-9-]*[a-z0-9])?\"></label>"
        "<p>Use dev, staging, prod or a lowercase DNS label.</p>"
        "<button type=\"submit\">New workspace</button></form></section>"
        "<div class=\"panel\"><table><thead><tr><th>Name</th><th>Environment</th>"
        "<th>Phase</th><th>Namespace</th><th>Created</th><th>Actions</th></tr></thead><tbody>";
    for (const auto& row : page.data.value("list_workspaces", nlohmann::json::object())
                                  .value("workspaces", nlohmann::json::array())) {
        const std::string id = web_ui_escape(row.value("id", ""));
        const std::string name = web_ui_escape(row.value("name", ""));
        content += "<tr><td><a href=\"/w/" + id + "/design/projects\">" + name + "</a></td>";
        for (const char* field : {"environment", "phase", "namespace", "created_at"}) {
            content += "<td>" + web_ui_escape(row.value(field, "")) + "</td>";
        }
        content += "<td>";
        if (row.value("desired", "") != "deleted") {
            content += "<details><summary>Delete</summary><p>Deletes the workspace and its data. Type " +
                name + " to confirm.</p><form method=\"post\" action=\"/workspaces/delete\">"
                "<input type=\"hidden\" name=\"customer_id\" value=\"" + customer + "\">"
                "<input type=\"hidden\" name=\"workspace_id\" value=\"" + id + "\">"
                "<label>Workspace name<input name=\"confirm\" required autocomplete=\"off\"></label>"
                "<button type=\"submit\">Delete workspace</button></form></details>";
        } else {
            content += "Deletion requested";
        }
        content += "</td></tr>";
    }
    return content + "</tbody></table></div>";
}

std::string web_ui_workspace(const WorkspacePage& page, WebUiViewer viewer) {
    const WorkspaceSection& section = workspace_section(page.section);
    std::string named =
        page.section == "agent"
            ? json_text(page.data.value("get_agent", nlohmann::json::object()), "name")
            : std::string();
    if (page.section == "run") {
        // "Run: <uuid>" told a person nothing (lake#255).
        const nlohmann::json run = page.data.value("get_run", nlohmann::json::object());
        const std::string pipeline =
            json_text(graph_called(page.data, json_text(run, "graph")), "name");
        if (!pipeline.empty()) {
            named = "of " + pipeline;
        }
    }
    const std::string subject = named.empty() ? page.subject : named;
    const std::string title =
        subject.empty()             ? std::string(section.title)
        : subject.starts_with("of ") ? std::string(section.title) + " " + subject
                                     : std::string(section.title) + ": " + subject;
    std::string content = std::format("<p class=\"eyebrow\">{}</p><h1>{}</h1>",
                                      web_ui_escape(section.verb),
                                      web_ui_escape(title));
    if (!section.sub.empty()) {
        content += std::format("<p class=\"sub\">{}</p>", web_ui_escape(section.sub));
    }
    if (!page.error.empty()) {
        content +=
            std::format("<p class=\"error\" role=\"alert\">{}</p>", web_ui_escape(page.error));
    }
    if (!page.unavailable.empty()) {
        content +=
            std::format("<div class=\"panel empty\">{}</div>", web_ui_escape(page.unavailable));
    } else if (page.section == "workspaces") {
        content += workspaces_content(page);
    } else if (page.section == "services") {
        content += services_content(page);
    } else if (page.section == "library") {
        // The Agent Library is lake's, not the workspace's: shown whether it is ready or not.
        content += agent_library_content(page);
    } else if (!page.state.value("ready", false)) {
        if (page.section == "data") {
            content += store_connection_notice(page);
        }
        content += workspace_gate(page, section);
    } else {
        if (page.section == "data") {
            content += store_connection_notice(page);
        }
        content += workspace_content(page);
    }
    if (page.unavailable.empty() && page.section == "data") {
        content += storage_content(page);
    }
    if (page.section == "agents") {
        content += agent_library_content(page);
    }
    return web_ui_shell("lake — " + title, workspace_trail(page, section), content, viewer);
}

std::string web_ui_packages(const nlohmann::json& catalog,
                            bool available,
                            std::string_view origin,
                            WebUiViewer viewer,
                            const nlohmann::json& builds,
                            std::string_view customer_id) {
    std::string content =
        "<p class=\"eyebrow\">Ship</p><h1>Packages</h1><p class=\"sub\">Container images, npm "
        "packages and Python distributions published to this lake. Private images are visible "
        "only to their space.</p>";
    if (!available) {
        content += "<div class=\"panel empty\">This lake has no registry storage configured.</div>";
        return web_ui_shell("lake — packages", "Packages", content, viewer);
    }
    struct Kind {
        std::string_view key, title, install;
    };
    const std::string host = std::string(origin).starts_with("https://")
                                 ? std::string(origin).substr(8)
                                 : std::string(origin);
    for (const Kind& kind : {Kind{"oci", "Container images", "docker pull {host}/{name}:{version}"},
                             Kind{"npm", "npm packages", "npm install {name} --registry {origin}"},
                             Kind{"python",
                                  "Python distributions",
                                  "pip install {name} --index-url {origin}/simple/"}}) {
        const nlohmann::json items = catalog.value(std::string(kind.key), nlohmann::json::array());
        content += std::format("<section class=\"panel\"><h2>{}</h2>", kind.title);
        if (items.empty()) {
            content += "<p class=\"muted\">None published.</p></section>";
            continue;
        }
        content += "<table><thead><tr><th>Name</th><th>Versions</th><th>Install</th></tr></thead>"
                   "<tbody>";
        for (const nlohmann::json& item : items) {
            const std::string name = json_text(item, "name");
            std::string versions;
            std::string latest;
            const auto listed = item.find("versions");
            const nlohmann::json versions_list =
                listed != item.end() && listed->is_array() ? *listed : nlohmann::json::array();
            for (const nlohmann::json& version : versions_list) {
                const std::string text = version.is_string() ? version.get<std::string>() : "";
                versions += (versions.empty() ? "" : ", ") + text;
                latest = text;
            }
            std::string install;
            if (!origin.empty()) {
                install = std::string(kind.install);
                for (const auto& [placeholder, value] :
                     std::array<std::pair<std::string_view, std::string>, 4>{
                         {{"{host}", host},
                          {"{origin}", std::string(origin)},
                          {"{name}", name},
                          {"{version}", latest.empty() ? "latest" : latest}}}) {
                    for (std::size_t at = install.find(placeholder); at != std::string::npos;
                         at = install.find(placeholder)) {
                        install.replace(at, placeholder.size(), value);
                    }
                }
            }
            content += std::format("<tr><td>{}</td><td>{}</td><td>{}</td></tr>",
                                   web_ui_escape(name),
                                   web_ui_escape(versions),
                                   install.empty() ? std::string("<span class=\"muted\">—</span>")
                                                   : "<code>" + web_ui_escape(install) + "</code>");
        }
        content += "</tbody></table></section>";
    }
    content += "<section class=\"panel\"><h2>Builds</h2>";
    if (builds.empty()) {
        content += "<p class=\"muted\">No image builds in this space.</p></section>";
    } else {
        content += "<table><thead><tr><th>Image</th><th>Phase</th><th>Digest</th><th>Log</th>"
                   "</tr></thead><tbody>";
        for (const nlohmann::json& build : builds) {
            // A build without an image yet has a null digest (lake#440): read every field as
            // text that may be absent or null, never as a required string.
            const std::string id = json_text(build, "id");
            content += std::format(
                "<tr data-build-id=\"{}\"><td><code>{}</code></td><td class=\"build-phase\">{}"
                "</td><td><code>{}</code></td><td><details><summary>Live log</summary>"
                "<pre class=\"build-log\"></pre></details><a href=\"/train/stream?customer_id={}"
                "&amp;zetta=workspace&amp;topic=build-events.{}\">Browse messages</a></td></tr>",
                web_ui_escape(id),
                web_ui_escape(json_text(build, "target")),
                web_ui_escape(json_text(build, "phase")),
                web_ui_escape(json_text(build, "digest")),
                url_component(customer_id), url_component(id));
        }
        content += "</tbody></table></section>";
        content += std::format(
            R"(<script>(() => {{
    const customer = {};
    const workspace = location.pathname.split('/')[2];
    for (const row of document.querySelectorAll('[data-build-id]')) {{
        let next = 1;
        const follow = async () => {{
            const query = new URLSearchParams({{
                customer_id: customer, workspace_id: workspace,
                build_id: row.dataset.buildId, from_sequence: String(next), wait_ms: '25000'
            }});
            try {{
                const response = await fetch('/v1/receive_build_events?' + query);
                const envelope = await response.json();
                if (envelope.ok) {{
                    for (const event of envelope.value.events) {{
                        next = Math.max(next, event.sequence + 1);
                        if (event.phase) row.querySelector('.build-phase').textContent = event.phase;
                        for (const line of event.lines || []) {{
                            row.querySelector('.build-log').textContent += line + '\n';
                        }}
                    }}
                    next = Math.max(next, envelope.value.next_sequence || next);
                }}
            }} catch (error) {{
                // A later poll retries a transient transport failure.
            }}
            setTimeout(follow, 250);
        }};
        follow();
    }}
}})();</script>)",
            nlohmann::json(std::string(customer_id)).dump());
    }
    return web_ui_shell("lake — packages", "Packages", content, viewer);
}

namespace {

std::string app_customer_selector(const std::vector<WorkspaceCustomer>& customers,
                                  std::string_view customer_id,
                                  std::string_view path) {
    return {};
}

std::string app_phase_badge(std::string_view phase, bool ready) {
    const std::string modifier = ready                ? " status-success"
                                 : phase == "Failed"  ? " status-danger"
                                 : phase == "Pending" ? " status-warning"
                                                      : "";
    return std::format("<span class=\"badge{}\" data-app-phase>{}</span>",
                       modifier,
                       web_ui_escape(phase.empty() ? "Pending" : phase));
}

std::string app_url(const nlohmann::json& app) {
    const std::string url = json_text(app, "url");
    if (url.empty()) {
        return "<span class=\"muted\">Not assigned</span>";
    }
    if (!url.starts_with("https://") && !url.starts_with("http://")) {
        return web_ui_escape(url);
    }
    return std::format(
        "<a href=\"{}\" rel=\"noreferrer\">{}</a>", web_ui_escape(url), web_ui_escape(url));
}

std::string app_bindings_summary(const nlohmann::json& app) {
    const nlohmann::json bindings = app.value("bindings", nlohmann::json::object());
    const bool zetta = bindings.contains("zetta") && !bindings.at("zetta").is_null();
    const std::size_t services = bindings.value("services", nlohmann::json::array()).size();
    if (!zetta && services == 0) {
        return "None";
    }
    return std::format(
        "{}Zetta, {} service{}", zetta ? "" : "No ", services, services == 1 ? "" : "s");
}

std::string app_hidden(std::string_view customer_id, std::string_view app_id) {
    return hidden("customer_id", customer_id) + hidden("app_id", app_id);
}

std::string app_action_form(std::string_view path,
                            std::string_view action,
                            std::string_view customer_id,
                            std::string_view app_id,
                            std::string_view extra = {}) {
    return std::format("<form method=\"post\" action=\"{}\">{}{}<button type=\"submit\">{}</button>"
                       "</form>",
                       path,
                       app_hidden(customer_id, app_id),
                       extra,
                       action);
}

std::string app_live_script(std::string_view customer_id,
                            std::string_view app_id,
                            const nlohmann::json& events) {
    std::string customer = nlohmann::json(std::string(customer_id)).dump();
    std::string app = nlohmann::json(std::string(app_id)).dump();
    std::uint64_t next_sequence = 1;
    for (const nlohmann::json& event : events) {
        if (event.contains("sequence") && event.at("sequence").is_number_unsigned()) {
            next_sequence = std::max(next_sequence, event.at("sequence").get<std::uint64_t>() + 1);
        }
    }
    return std::format(
        R"(<script id="app-live-events">(() => {{
    const customer = {};
    const app = {};
    const workspace = location.pathname.split('/')[2];
    const key = `lake-app-events:${{app}}`;
    let next = {};
    try {{
        next = Math.max(next, Number(localStorage.getItem(key)) || 1);
    }} catch {{
        // Storage may be disabled; the server-provided cursor still works.
    }}
    const log = document.querySelector('[data-app-events]');
    const phase = document.querySelector('[data-app-phase]');
    const fill = document.querySelector('.app-phase-fill');
    const show = event => {{
        if (log) {{
            const line = document.createElement('div');
            line.textContent = `${{event.sequence || ''}}  ${{event.type || 'event'}}  ${{event.message || JSON.stringify(event.status || {{}})}}`;
            log.append(line);
            log.scrollTop = log.scrollHeight;
        }}
        const status = event.status && event.status.phase;
        if (status && phase) phase.textContent = status;
        if (status && fill) fill.classList.toggle('ready', status === 'Running' || status === 'Ready');
    }};
    const follow = async () => {{
        while (document.body.isConnected) {{
            try {{
                const query = new URLSearchParams({{
                    customer_id: customer, workspace_id: workspace,
                    app_id: app, from_sequence: String(next), wait_ms: '25000'
                }});
                const response = await fetch('/v1/receive_app_events?' + query, {{credentials: 'same-origin'}});
                if (!response.ok) throw new Error('event request failed');
                const body = await response.json();
                const value = body.value || body.data || body;
                for (const event of value.events || []) show(event);
                next = value.next_sequence || next;
                try {{
                    localStorage.setItem(key, String(next));
                }} catch {{
                    // Keep following with the in-memory cursor.
                }}
            }} catch {{
                await new Promise(resolve => setTimeout(resolve, 2000));
            }}
        }}
    }};
    follow();
}})();</script>)",
        customer,
        app,
        next_sequence);
}

std::string build_live_script(std::string_view customer_id, const nlohmann::json& builds) {
    nlohmann::json ids = nlohmann::json::array();
    for (const nlohmann::json& build : builds) {
        const std::string id = build.value("id", build.value("build_id", std::string()));
        if (!id.empty()) {
            ids.push_back(id);
        }
    }
    return std::format(
        R"(<script id="build-live-events">(() => {{
    const customer = {};
    const builds = {};
    const workspace = location.pathname.split('/')[2];
    const follow = async id => {{
        const key = `lake-build-events:${{id}}`;
        let next = 1;
        try {{
            next = Number(localStorage.getItem(key)) || 1;
        }} catch {{
            // Storage may be disabled; start at the first retained event.
        }}
        const log = document.querySelector(`[data-build-events="${{CSS.escape(id)}}"]`);
        while (document.body.isConnected) {{
            try {{
                const query = new URLSearchParams({{
                    customer_id: customer, workspace_id: workspace,
                    build_id: id, from_sequence: String(next), wait_ms: '25000'
                }});
                const response = await fetch('/v1/receive_build_events?' + query, {{credentials: 'same-origin'}});
                if (!response.ok) throw new Error('event request failed');
                const body = await response.json();
                const value = body.value || body.data || body;
                for (const event of value.events || []) {{
                    if (log) {{
                        const line = document.createElement('div');
                        line.textContent = `${{event.sequence || ''}}  ${{event.message || event.type || JSON.stringify(event)}}`;
                        log.append(line);
                    }}
                }}
                next = value.next_sequence || next;
                try {{
                    localStorage.setItem(key, String(next));
                }} catch {{
                    // Keep following with the in-memory cursor.
                }}
            }} catch {{
                await new Promise(resolve => setTimeout(resolve, 2000));
            }}
        }}
    }};
    for (const id of builds) follow(id);
}})();</script>)",
        nlohmann::json(std::string(customer_id)).dump(),
        ids.dump());
}

std::string app_event_history(const AppPage& page) {
    std::string output = "<section class=\"panel\"><h2>Deploy History</h2>";
    bool found = false;
    output += "<table><thead><tr><th>Sequence</th><th>Action</th><th>Generation</th>"
              "<th>Actor</th><th>Target</th><th>Message</th><th></th></tr></thead><tbody>";
    const std::string app_id = json_text(page.app, "id");
    for (auto it = page.events.rbegin(); it != page.events.rend(); ++it) {
        const nlohmann::json& event = *it;
        const std::string type = json_text(event, "type");
        if (type != "deploy" && type != "rollback" && type != "create") {
            continue;
        }
        found = true;
        const nlohmann::json status = event.value("status", nlohmann::json::object());
        const nlohmann::json source = status.value("source", nlohmann::json::object());
        const std::string image = json_text(status, "image");
        const std::string target =
            image.empty() ? json_text(source, "repository") + " @ " + json_text(source, "revision")
                          : image;
        std::string rollback;
        if (!image.empty()) {
            rollback = app_action_form(
                "/ship/app/rollback", "Rollback", page.customer_id, app_id, hidden("image", image));
        } else if (!source.empty()) {
            rollback = app_action_form("/ship/app/rollback",
                                       "Rollback",
                                       page.customer_id,
                                       app_id,
                                       hidden("repository", json_text(source, "repository")) +
                                           hidden("revision", json_text(source, "revision")) +
                                           hidden("path", json_text(source, "path")) +
                                           hidden("entrypoint", json_text(source, "entrypoint")));
        }
        output += std::format("<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td><code>{}</code>"
                              "</td><td>{}</td><td>{}</td></tr>",
                              web_ui_escape(json_text(event, "sequence")),
                              web_ui_escape(type),
                              web_ui_escape(json_text(event, "generation")),
                              web_ui_escape(json_text(event, "principal")),
                              web_ui_escape(target),
                              web_ui_escape(json_text(event, "message")),
                              rollback);
    }
    output += "</tbody></table>";
    if (!found) {
        output += "<p class=\"muted\">No deployments recorded yet.</p>";
    }
    return output + "</section>";
}

} // namespace

std::string web_ui_apps(const AppsPage& page, WebUiViewer viewer) {
    std::string content =
        "<p class=\"eyebrow\">Ship</p><h1>Apps</h1><p class=\"sub\">Create, deploy and "
        "operate customer applications with live rollout status.</p>";
    content += app_customer_selector(page.customers, page.customer_id, "/ship/apps");
    if (!page.error.empty()) {
        content +=
            std::format("<p class=\"error\" role=\"alert\">{}</p>", web_ui_escape(page.error));
    }
    if (page.customer_id.empty()) {
        content += "<div class=\"panel empty\">You have no space to work in yet.</div>";
        return web_ui_shell("lake — apps", "Apps", content, viewer);
    }
    if (page.apps.empty()) {
        content += "<div class=\"panel empty\">No apps yet. Create one from a Lake repository or "
                   "a container image.</div>";
    } else {
        content += "<table><thead><tr><th>Name</th><th>URL</th><th>Status</th><th>Mode</th>"
                   "<th>Revision</th><th>Bindings</th></tr></thead><tbody>";
        for (const nlohmann::json& app : page.apps) {
            const nlohmann::json source = app.value("source", nlohmann::json::object());
            content += std::format(
                "<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td><code>{}</code></td>"
                "<td>{}</td></tr>",
                row_link("/ship/app?" + customer_query(page.customer_id) +
                             "&app_id=" + url_component(json_text(app, "id")),
                         json_text(app, "name")),
                app_url(app),
                app_phase_badge(json_text(app, "phase"), app.value("ready", false)),
                web_ui_escape(json_text(app, "mode")),
                web_ui_escape(app.value("mode", "") == "runtime" ? json_text(source, "revision")
                                                                 : json_text(app, "image")),
                web_ui_escape(app_bindings_summary(app)));
        }
        content += "</tbody></table>";
    }
    content += "<div class=\"grid\"><section class=\"panel\"><h2>Create From a Repository</h2>"
               "<form class=\"stack\" method=\"post\" action=\"/ship/apps/create\">" +
               hidden("customer_id", page.customer_id) +
               "<input type=\"hidden\" name=\"mode\" value=\"runtime\"><label>Name<input "
               "name=\"name\" required pattern=\"[a-z][a-z0-9-]{0,30}\"></label><label>Repository"
               "<select name=\"repository\" required>";
    for (const nlohmann::json& repository : page.repositories) {
        content += std::format("<option value=\"{}\">{}</option>",
                               web_ui_escape(json_text(repository, "id")),
                               web_ui_escape(json_text(repository, "name")));
    }
    content +=
        "</select></label><label>Revision<input name=\"revision\" value=\"main\" required>"
        "</label><label>Path<input name=\"path\" value=\".\"></label><label>Framework<input "
        "name=\"framework\" value=\"auto\" required></label><label>Port<input type=\"number\" "
        "name=\"port\" value=\"3000\" min=\"1\" max=\"65535\" required></label><button "
        "type=\"submit\">Create App</button></form></section><section class=\"panel\"><h2>Create "
        "From an Image</h2><form class=\"stack\" method=\"post\" action=\"/ship/apps/create\">" +
        hidden("customer_id", page.customer_id) +
        "<input type=\"hidden\" name=\"mode\" value=\"image\"><label>Name<input name=\"name\" "
        "required pattern=\"[a-z][a-z0-9-]{0,30}\"></label><label>Image<input name=\"image\" "
        "placeholder=\"registry.example/app:tag\" required></label><label>Framework<input "
        "name=\"framework\" value=\"container\" required></label><label>Port<input "
        "type=\"number\" name=\"port\" value=\"3000\" min=\"1\" max=\"65535\" required>"
        "</label><button type=\"submit\">Create App</button></form></section></div>";
    content +=
        "<section class=\"panel\"><h2>Build an App With the ApplicationAgent</h2><p class=\"sub\">"
        "Describe the app. The agent proposes files and app settings for you to confirm.</p><form "
        "class=\"stack\" method=\"post\" action=\"/ship/apps/agent\">";
    content += hidden("customer_id", page.customer_id);
    content += "<label>Description<textarea name=\"description\" placeholder=\"What should the app "
               "do?\"";
    if (!page.application_agent) {
        content += " disabled";
    }
    content += "></textarea></label><button type=\"submit\"";
    if (!page.application_agent) {
        content += " disabled";
    }
    content += ">Start ApplicationAgent</button></form>";
    if (!page.application_agent) {
        content += "<p class=\"notice status-warning\">ApplicationAgent is not registered on this "
                   "lake yet.</p>";
    }
    content += "</section>";
    if (!page.agent_task.empty()) {
        content += agent_events_link(
            page.customer_id, page.agent_task.value("task_id", std::string()));
        // A failed task carries "answer": null; its "message" is then the reply shown.
        nlohmann::json answer = page.agent_task.value("answer", nlohmann::json::object());
        if (!answer.is_object()) {
            answer = nlohmann::json::object();
        }
        nlohmann::json result = answer.value("result", nlohmann::json::object());
        if (!result.is_object()) {
            result = nlohmann::json::object();
        }
        std::string reply = json_text(answer, "reply");
        if (reply.empty()) {
            reply = json_text(page.agent_task, "message");
        }
        if (page.agent_working) {
            content +=
                "<section class=\"panel\" aria-live=\"polite\"><h2>ApplicationAgent Is "
                "Working</h2><p>It keeps working if you leave; this page checks again every 2 "
                "seconds.</p><p role=\"status\">" +
                web_ui_escape(reply) + "</p><meta http-equiv=\"refresh\" content=\"2\"></section>";
        } else if (result.value("kind", std::string()) == "proposal" &&
                   result.value("confirm", false)) {
            const nlohmann::json app = result.value("app", nlohmann::json::object());
            std::string files;
            for (const nlohmann::json& file :
                 result.value("files", nlohmann::json::array())) {
                files += std::format("<h3><code>{}</code></h3><pre><code>{}</code></pre>",
                                     web_ui_escape(json_text(file, "path")),
                                     web_ui_escape(json_text(file, "contents")));
            }
            content += std::format(
                "<section class=\"panel\" id=\"proposal\"><h2>Proposed App: {}</h2><div "
                "class=\"ask-reply\">{}</div><dl><dt>Mode</dt><dd>{}</dd><dt>Framework</dt>"
                "<dd>{}</dd><dt>Access</dt><dd>{}</dd><dt>Repository</dt><dd>{}</dd><dt>Path</dt>"
                "<dd>{}</dd></dl>{}<form class=\"stack\" method=\"post\" "
                "action=\"/ship/apps/agent/confirm\">{}{}{}<button class=\"button "
                "button-primary\" type=\"submit\">Confirm and deploy</button></form></section>",
                web_ui_escape(json_text(app, "name")),
                web_ui_markdown(reply),
                web_ui_escape(json_text(app, "mode")),
                web_ui_escape(json_text(app, "framework")),
                web_ui_escape(json_text(app, "auth")),
                web_ui_escape(app.value("repository", nlohmann::json::object())
                                  .value("id",
                                         app.value("repository", nlohmann::json::object())
                                             .value("name", std::string()))),
                web_ui_escape(json_text(app, "path")),
                files,
                hidden("customer_id", page.customer_id),
                hidden("task_id", page.agent_task.value("task_id", std::string())),
                hidden("proposal_sha256", page.proposal_sha256));
        } else {
            const nlohmann::json app = result.value("app", nlohmann::json::object());
            // The deployed App carries "url": null until its rollout reports one, so read
            // strings only; the App's own host is the fallback link.
            std::string url = json_text(app, "url");
            if (url.empty()) {
                const nlohmann::json rollout = app.value("rollout", nlohmann::json::object());
                url = rollout.is_object() ? json_text(rollout, "url") : std::string();
            }
            if (url.empty() && !json_text(app, "host").empty()) {
                url = "https://" + json_text(app, "host");
            }
            content += "<section class=\"panel\"><h2>ApplicationAgent Result</h2><div "
                       "class=\"ask-reply\">" +
                       web_ui_markdown(reply) + "</div>";
            if (!url.empty()) {
                content += std::format("<p><a href=\"{}\">Open the app</a></p>",
                                       web_ui_escape(url));
            }
            if (page.agent_reply) {
                content +=
                    "<h3>Reply</h3><form class=\"stack\" method=\"post\" "
                    "action=\"/ship/apps/agent\">" +
                    hidden("customer_id", page.customer_id) +
                    hidden("task_id", page.agent_task.value("task_id", std::string())) +
                    "<label>Reply<textarea name=\"reply\" rows=\"4\" required></textarea>"
                    "</label><button class=\"button button-primary\" type=\"submit\">Reply to "
                    "ApplicationAgent</button></form>";
            }
            content += "</section>";
        }
    }
    return web_ui_shell("lake — apps", "Apps", content, viewer);
}

std::string web_ui_app(const AppPage& page, WebUiViewer viewer) {
    const std::string id = json_text(page.app, "id");
    const std::string name = json_text(page.app, "name");
    const nlohmann::json source = page.app.value("source", nlohmann::json::object());
    std::string content = "<p class=\"eyebrow\">Ship · Apps</p><h1>" + web_ui_escape(name) +
                          "</h1><p class=\"sub\">" + app_url(page.app) + "</p>";
    if (!page.error.empty()) {
        content +=
            std::format("<p class=\"error\" role=\"alert\">{}</p>", web_ui_escape(page.error));
    }
    content += std::format(
        "<section class=\"panel\"><h2>Status</h2><div class=\"app-phase\"><span>Desired: "
        "{}</span>{}<span>Generation {} / observed {}</span></div><div class=\"app-phase-track\" "
        "aria-label=\"Rollout progress\"><span class=\"app-phase-fill{}\"></span></div><p>{}</p>"
        "</section>",
        web_ui_escape(json_text(page.app, "desired")),
        app_phase_badge(json_text(page.app, "phase"), page.app.value("ready", false)),
        web_ui_escape(json_text(page.app, "generation")),
        web_ui_escape(json_text(page.app, "observed_generation")),
        page.app.value("ready", false) ? " ready" : "",
        web_ui_escape(json_text(page.app, "message")));
    content += "<section class=\"panel\"><h2>Controls</h2><div class=\"app-controls\">";
    content += app_action_form("/ship/app/restart", "Restart", page.customer_id, id);
    content += app_action_form("/ship/app/stop", "Stop", page.customer_id, id);
    content += app_action_form("/ship/app/start", "Start", page.customer_id, id);
    content += "</div><form class=\"filters\" method=\"post\" action=\"/ship/app/scale\">" +
               app_hidden(page.customer_id, id) +
               "<label>Replicas<input type=\"number\" name=\"replicas\" min=\"1\" max=\"100\" "
               "value=\"" +
               web_ui_escape(json_text(page.app, "replicas")) +
               "\"></label><button type=\"submit\">Scale</button></form>";
    content += "<form class=\"stack\" method=\"post\" action=\"/ship/app/deploy\">" +
               app_hidden(page.customer_id, id);
    if (page.app.value("mode", "") == "runtime") {
        content += hidden("repository", json_text(source, "repository")) +
                   hidden("path", json_text(source, "path")) +
                   hidden("entrypoint", json_text(source, "entrypoint")) +
                   "<label>Deploy Revision<input name=\"revision\" value=\"" +
                   web_ui_escape(json_text(source, "revision")) + "\" required></label>";
    } else {
        content += "<label>Deploy Image<input name=\"image\" value=\"" +
                   web_ui_escape(json_text(page.app, "image")) + "\" required></label>";
    }
    content += "<button type=\"submit\">Deploy</button></form></section>";
    content += app_event_history(page);
    content += "<p>" + row_link("/train/stream?customer_id=" + url_component(page.customer_id) +
        "&zetta=workspace&topic=app-events." + url_component(id), "Browse app event stream") + "</p>";
    content += "<section class=\"panel\"><h2>Live Events</h2><div class=\"app-stream\" "
               "data-app-events role=\"log\">";
    for (const nlohmann::json& event : page.events) {
        content += std::format("<div>{}  {}  {}</div>",
                               web_ui_escape(json_text(event, "sequence")),
                               web_ui_escape(json_text(event, "type")),
                               web_ui_escape(json_text(event, "message")));
    }
    content += "</div></section>";
    content += "<section class=\"panel\"><h2>Logs</h2><pre class=\"app-stream\">";
    if (page.logs.contains("entries")) {
        for (const nlohmann::json& entry : page.logs.at("entries")) {
            content +=
                web_ui_escape(entry.is_string() ? entry.get<std::string>() : entry.dump()) + "\n";
        }
    } else {
        content += "Logs are not available on this lake yet.";
    }
    content += "</pre></section>";
    content += "<section class=\"panel\"><h2>Builds</h2>";
    if (!page.builds_available) {
        content += "<p class=\"muted\">Builds are not available on this lake yet.</p>";
        content += "<button type=\"button\" disabled>Build</button>";
    } else if (page.builds.empty()) {
        content += "<p class=\"muted\">No builds yet.</p>";
    } else {
        content += "<table><thead><tr><th>Build</th><th>Status</th><th>Revision</th><th></th></tr>"
                   "</thead><tbody>";
        for (const nlohmann::json& build : page.builds) {
            const std::string build_id = build.value("id", build.value("build_id", std::string()));
            std::string built_image = json_text(build, "image");
            if (built_image.empty()) {
                built_image = build.value("target", std::string());
            }
            std::string build_log;
            const nlohmann::json logs = build.value("logs", nlohmann::json::object());
            for (const nlohmann::json& entry : logs.value(
                     "lines",
                     logs.value("entries", logs.value("events", nlohmann::json::array())))) {
                build_log +=
                    web_ui_escape(entry.is_string() ? entry.get<std::string>() : entry.dump()) +
                    "\n";
            }
            std::string build_actions = app_action_form("/ship/app/build/cancel",
                                                        "Cancel Build",
                                                        page.customer_id,
                                                        id,
                                                        hidden("build_id", build_id));
            build_actions += row_link("/train/stream?customer_id=" + url_component(page.customer_id) +
                "&zetta=workspace&topic=build-events." + url_component(build_id), "Browse build stream");
            if (!built_image.empty()) {
                build_actions += app_action_form("/ship/app/deploy",
                                                 "Deploy Build",
                                                 page.customer_id,
                                                 id,
                                                 hidden("image", built_image));
            }
            content += std::format("<tr><td><code>{}</code></td><td>{}</td><td><code>{}</code></td>"
                                   "<td>{}<details><summary>Logs</summary><pre>{}</pre><div "
                                   "class=\"app-stream\" data-build-events=\"{}\"></div></details>"
                                   "</td></tr>",
                                   web_ui_escape(build_id),
                                   web_ui_escape(build.value(
                                       "phase", build.value("status", std::string()))),
                                   web_ui_escape(json_text(build, "revision")),
                                   build_actions,
                                   build_log,
                                   web_ui_escape(build_id));
        }
        content += "</tbody></table>";
    }
    if (page.builds_available) {
        content += app_action_form(
            "/ship/app/build", page.builds.empty() ? "Build" : "Rebuild", page.customer_id, id);
    }
    content += "</section>";
    content += "<section class=\"panel\"><h2>Environment</h2><table><thead><tr><th>Key</th>"
               "<th>Value</th><th></th></tr></thead><tbody>";
    for (const auto& [key, value] : page.app.value("env", nlohmann::json::object()).items()) {
        content += std::format(
            "<tr><td><code>{}</code></td><td>{}</td><td>{}</td></tr>",
            web_ui_escape(key),
            web_ui_escape(value.is_string() ? value.get<std::string>() : value.dump()),
            app_action_form(
                "/ship/app/env/unset", "Unset", page.customer_id, id, hidden("key", key)));
    }
    for (const auto& [key, value] :
         page.app.value("secret_env_refs", nlohmann::json::object()).items()) {
        content += std::format(
            "<tr><td><code>{}</code></td><td>••••••</td><td>{}</td></tr>",
            web_ui_escape(key),
            app_action_form(
                "/ship/app/env/unset", "Unset", page.customer_id, id, hidden("key", key)));
    }
    content +=
        "</tbody></table><form class=\"filters\" method=\"post\" action=\"/ship/app/env/set\">" +
        app_hidden(page.customer_id, id) +
        "<label>Key<input name=\"key\" required></label><label>Value<input name=\"value\">"
        "</label><label>Secret Reference<input name=\"secret_ref\"></label><button "
        "type=\"submit\">Set Environment</button></form></section>";
    const nlohmann::json bindings = page.app.value("bindings", nlohmann::json::object());
    const bool has_zetta = bindings.contains("zetta") && !bindings.at("zetta").is_null();
    content += "<section class=\"panel\"><h2>Bindings</h2><p>Workspace Zetta: <span class=\"badge" +
               std::string(has_zetta ? " status-success\">Attached" : "\">Detached") +
               "</span></p>" +
               app_action_form("/ship/app/zetta",
                               has_zetta ? "Detach Zetta" : "Attach Zetta",
                               page.customer_id,
                               id,
                               hidden("attached", has_zetta ? "0" : "1"));
    for (const nlohmann::json& service_id : bindings.value("services", nlohmann::json::array())) {
        if (!service_id.is_string()) {
            continue;
        }
        content += "<p><code>" + web_ui_escape(service_id.get<std::string>()) + "</code> " +
                   app_action_form("/ship/app/service/unbind",
                                   "Detach",
                                   page.customer_id,
                                   id,
                                   hidden("service_id", service_id.get<std::string>())) +
                   "</p>";
    }
    content += "<form class=\"filters\" method=\"post\" action=\"/ship/app/service/bind\">" +
               app_hidden(page.customer_id, id) + "<label>Service<select name=\"service_id\">";
    for (const nlohmann::json& service : page.services) {
        content += std::format("<option value=\"{}\">{} · {}</option>",
                               web_ui_escape(json_text(service, "id")),
                               web_ui_escape(json_text(service, "name")),
                               web_ui_escape(json_text(service, "kind")));
    }
    content += "</select></label><button type=\"submit\">Attach Service</button></form>"
               "<form class=\"filters\" method=\"post\" action=\"/ship/app/service/create\">" +
               app_hidden(page.customer_id, id) +
               "<label>Name<input name=\"name\" required></label><label>Kind<input name=\"kind\" "
               "required></label><label>Size<input name=\"size\"></label><button type=\"submit\">"
               "Create and Attach</button></form></section>";
    content += "<section class=\"panel\"><h2>Authentication</h2><form class=\"stack\" "
               "method=\"post\" action=\"/ship/app/auth\">" +
               app_hidden(page.customer_id, id) +
               "<label>Access<select name=\"auth\"><option value=\"lake\">Lake Sign-In</option>"
               "<option value=\"none\">Public</option></select></label><label><input "
               "type=\"checkbox\" name=\"confirm_public\" value=\"1\"> Confirm public access"
               "</label><button type=\"submit\">Change Authentication</button></form></section>";
    content += "<section class=\"panel danger\"><h2>Danger Zone</h2><form class=\"stack\" "
               "method=\"post\" action=\"/ship/app/delete\">" +
               app_hidden(page.customer_id, id) + "<label>Type <code>" + web_ui_escape(name) +
               "</code> to delete<input name=\"confirm\" required></label><button type=\"submit\">"
               "Delete App</button></form></section>";
    content += app_live_script(page.customer_id, id, page.events);
    if (page.builds_available) {
        content += build_live_script(page.customer_id, page.builds);
    }
    std::vector<WebUiCrumb> trail = default_trail(viewer, "lake — apps", "");
    trail.push_back({"Apps", "/ship/apps?" + customer_query(page.customer_id), {}});
    trail.push_back(web_ui_entity_crumb(name));
    return web_ui_shell("lake — " + name, trail, content, viewer);
}

std::string web_ui_deployments(const std::vector<DeploymentSummary>& environments,
                               std::string_view stale_since,
                               WebUiViewer viewer) {
    std::string content = "<p class=\"eyebrow\">Ship</p><h1>Deployments</h1><p class=\"sub\">"
                          "Every environment across the repositories you can open: what is "
                          "running, which change it runs, and where it is served.</p>";
    content += platform_stale_banner(stale_since);
    if (environments.empty()) {
        content += "<div class=\"panel empty\">Nothing is deployed yet. Deploy a repository with "
                   "<code>liquid deploy</code>, and its environments appear here.</div>";
    } else {
        content += "<table><thead><tr><th>Repository</th><th>Environment</th><th>Phase</th>"
                   "<th>Change</th><th>URL</th><th>Updated</th></tr></thead><tbody>";
        for (const DeploymentSummary& row : environments) {
            const std::string repository_id = url_component(row.repository.id);
            const std::string name = platform_text(row.environment, "name");
            const std::string url = platform_text(row.environment, "url");
            content += std::format(
                "<tr><td>{}</td><td>{}</td><td><span class=\"badge\">{}</span></td>"
                "<td><code>{}</code></td><td>{}</td><td>{}</td></tr>",
                row_link("/environments?repository_id=" + repository_id,
                         repository_name(row.repository)),
                row_link("/environment?repository_id=" + repository_id +
                             "&name=" + url_component(name),
                         name),
                web_ui_escape(platform_text(row.environment, "phase")),
                web_ui_escape(abbreviated_identifier(platform_text(row.environment, "change"))),
                url.empty() ? "<span class=\"muted\">—</span>" : platform_url(url),
                web_ui_escape(platform_text(row.environment, "updated_at")));
        }
        content += "</tbody></table>";
    }
    return web_ui_shell(
        "lake — deployments", std::format("{} environments", environments.size()), content, viewer);
}

std::string web_ui_environments(const control::Repository& repository,
                                const nlohmann::json& environments,
                                WebUiViewer viewer) {
    const std::string stale_since = platform_text(environments, "stale_since");
    const nlohmann::json items = environments.value("items", nlohmann::json::array());
    std::string content =
        platform_heading(repository, "platform", "Environments", "platform", viewer);
    content += platform_stale_banner(stale_since);
    content += "<p class=\"sub\">Deployments mirrored from the operator control plane.</p>";
    if (items.empty()) {
        content += "<div class=\"panel empty\">No environments have been deployed for this "
                   "repository.</div>";
    } else {
        content += "<table><thead><tr><th>Name</th><th>Phase</th><th>Change</th><th>Image</th>"
                   "<th>URL</th><th>Domains</th><th>Updated</th></tr></thead><tbody>";
        for (const nlohmann::json& item : items) {
            const std::string name = platform_text(item, "name");
            const std::string url = platform_text(item, "url");
            const nlohmann::json domains = item.value("domains", nlohmann::json::array());
            content += std::format(
                "<tr><td>{}</td><td><span class=\"badge\">{}</span></td><td><code>{}</code>"
                "</td><td><code>{}</code></td><td>{}</td><td>{}</td><td>{}</td></tr>",
                row_link("/environment?repository_id=" + url_component(repository.id) +
                             "&name=" + url_component(name),
                         name),
                web_ui_escape(platform_text(item, "phase")),
                web_ui_escape(abbreviated_identifier(platform_text(item, "change"))),
                web_ui_escape(short_image(platform_text(item, "image"))),
                url.empty() ? "<span class=\"muted\">—</span>" : platform_url(url),
                domains.size(),
                web_ui_escape(platform_text(item, "updated_at")));
        }
        content += "</tbody></table>";
    }
    return web_ui_shell(
        "lake — environments", std::format("{} environments", items.size()), content, viewer);
}

std::string web_ui_environment(const control::Repository& repository,
                               const nlohmann::json& environment,
                               const nlohmann::json& deployments,
                               WebUiViewer viewer) {
    const std::string name = platform_text(environment, "name");
    const std::string stale_since = platform_text(environment, "stale_since");
    const nlohmann::json deployment = environment.value("deployment", nlohmann::json::object());
    const nlohmann::json env = environment.value("env", nlohmann::json::array());
    const nlohmann::json domains = environment.value("domains", nlohmann::json::array());
    const nlohmann::json history = deployments.value("items", nlohmann::json::array());
    std::string content = platform_heading(repository, "environment", name, "platform", viewer);
    content += platform_stale_banner(stale_since);
    const std::string url = platform_text(deployment, "url");
    content += std::format(
        "<section class=\"panel\"><h2>Current Deployment</h2><dl><dt>Phase</dt><dd>{}</dd>"
        "<dt>Change</dt><dd><code>{}</code></dd><dt>Image</dt><dd><code>{}</code></dd>"
        "<dt>Namespace</dt><dd><code>{}</code></dd><dt>URL</dt><dd>{}</dd></dl><p>{}</p></section>",
        web_ui_escape(platform_text(deployment, "phase")),
        web_ui_escape(platform_text(deployment, "change")),
        web_ui_escape(platform_text(deployment, "image")),
        web_ui_escape(platform_text(deployment, "namespace")),
        url.empty() ? "<span class=\"muted\">Pending</span>" : platform_url(url),
        row_link("/logs?repository_id=" + url_component(repository.id) +
                     "&environment=" + url_component(name),
                 "View logs"));
    content += "<section class=\"panel\"><h2>Environment Variables</h2><table><thead><tr>"
               "<th>Key</th><th>Value</th><th>Updated</th><th></th></tr></thead><tbody>";
    for (const nlohmann::json& row : env) {
        content += std::format(
            "<tr><td><code>{}</code></td><td><code>{}</code></td><td>{}</td><td><form "
            "method=\"post\" action=\"/platform/env/unset\"><input type=\"hidden\" "
            "name=\"repository_id\" value=\"{}\"><input type=\"hidden\" name=\"environment\" "
            "value=\"{}\"><input type=\"hidden\" name=\"key\" value=\"{}\"><button "
            "type=\"submit\">Unset</button></form></td></tr>",
            web_ui_escape(platform_text(row, "key")),
            web_ui_escape(platform_text(row, "value")),
            web_ui_escape(platform_text(row, "updated_at")),
            web_ui_escape(repository.id),
            web_ui_escape(name),
            web_ui_escape(platform_text(row, "key")));
    }
    content +=
        "</tbody></table><form method=\"post\" action=\"/platform/env/set\"><input "
        "type=\"hidden\" name=\"repository_id\" value=\"" +
        web_ui_escape(repository.id) + "\"><input type=\"hidden\" name=\"environment\" value=\"" +
        web_ui_escape(name) +
        "\"><label>Key <input name=\"key\" required></label><label>Value <input "
        "name=\"value\" required></label><button type=\"submit\">Set variable</button></form>"
        "</section>";
    content += "<section class=\"panel\"><h2>Domains</h2><table><thead><tr><th>Host</th><th>TLS"
               "</th><th>Status</th><th>Message</th><th></th></tr></thead><tbody>";
    for (const nlohmann::json& row : domains) {
        content += std::format(
            "<tr><td>{}</td><td>{}</td><td><span class=\"badge\">{}</span></td><td>{}</td>"
            "<td><form method=\"post\" action=\"/platform/domain/delete\"><input type=\"hidden\" "
            "name=\"repository_id\" value=\"{}\"><input type=\"hidden\" name=\"environment\" "
            "value=\"{}\"><input type=\"hidden\" name=\"host\" value=\"{}\"><button "
            "type=\"submit\">Delete</button></form></td></tr>",
            web_ui_escape(platform_text(row, "host")),
            web_ui_escape(platform_text(row, "tls")),
            web_ui_escape(platform_text(row, "status")),
            web_ui_escape(platform_text(row, "message")),
            web_ui_escape(repository.id),
            web_ui_escape(name),
            web_ui_escape(platform_text(row, "host")));
    }
    content += "</tbody></table><form method=\"post\" action=\"/platform/domain/create\"><input "
               "type=\"hidden\" name=\"repository_id\" value=\"" +
               web_ui_escape(repository.id) +
               "\"><input type=\"hidden\" name=\"environment\" value=\"" + web_ui_escape(name) +
               "\"><label>Host <input name=\"host\" required></label><label>TLS <select "
               "name=\"tls\"><option value=\"acme\">ACME</option><option value=\"provided\">"
               "Provided</option></select></label><button type=\"submit\">Create domain</button>"
               "</form></section>";
    content +=
        "<section class=\"panel\"><h2>Deployment History</h2><table><thead><tr><th>Phase"
        "</th><th>Change</th><th>Image</th><th>Updated</th><th>Message</th></tr></thead><tbody>";
    for (const nlohmann::json& row : history) {
        content += std::format("<tr><td>{}</td><td><code>{}</code></td><td><code>{}</code></td>"
                               "<td>{}</td><td>{}</td></tr>",
                               web_ui_escape(platform_text(row, "phase")),
                               web_ui_escape(platform_text(row, "change")),
                               web_ui_escape(short_image(platform_text(row, "image"))),
                               web_ui_escape(platform_text(row, "updated_at")),
                               web_ui_escape(platform_text(row, "message")));
    }
    content += "</tbody></table></section>";
    return web_ui_shell("lake — " + name, name, content, viewer);
}

std::string web_ui_logs(const control::Repository& repository,
                        std::string_view environment,
                        const nlohmann::json& logs,
                        std::string_view stale_since,
                        WebUiViewer viewer) {
    const nlohmann::json items = logs.value("items", nlohmann::json::array());
    const std::string next_since = platform_text(logs, "next_since");
    std::string content =
        platform_heading(repository, "environment logs", environment, "platform", viewer);
    content += platform_stale_banner(stale_since);
    content += "<section class=\"panel\" id=\"platform-logs\" data-repository=\"" +
               web_ui_escape(repository.id) + "\" data-environment=\"" +
               web_ui_escape(environment) + "\" data-since=\"" + web_ui_escape(next_since) +
               "\"><h2>Logs</h2><pre aria-live=\"polite\"><code>";
    for (const nlohmann::json& item : items) {
        content += web_ui_escape(platform_text(item, "ts") + " " + platform_text(item, "pod") +
                                 "/" + platform_text(item, "container") + " " +
                                 platform_text(item, "line") + "\n");
    }
    content += "</code></pre><p><a href=\"/logs?repository_id=" + url_component(repository.id) +
               "&amp;environment=" + url_component(environment) +
               "&amp;since=" + url_component(next_since) + "\">Refresh</a></p></section>";
    content +=
        R"(<script>(()=>{const root=document.querySelector('#platform-logs');if(!root)return;const output=root.querySelector('code');let since=root.dataset.since;const poll=async()=>{try{const response=await fetch('/v1/environment_logs',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({repository_id:root.dataset.repository,environment:root.dataset.environment,since,limit:200,direction:'forward'})});const body=await response.json();if(!body.ok)return;for(const item of body.value.items){output.append(`${item.ts} ${item.pod}/${item.container} ${item.line}\n`);}since=body.value.next_since||since;}finally{setTimeout(poll,3000);}};setTimeout(poll,3000);})();</script>)";
    return web_ui_shell("lake — logs", std::string(environment) + " logs", content, viewer);
}

std::string web_ui_repository_tree(const control::Repository& repository,
                                   std::string_view ref,
                                   const std::vector<RepositoryTreeEntry>& entries,
                                   WebUiViewer viewer) {
    std::string content = path_breadcrumbs(repository, ref, "") +
                          std::format("<p class=\"eyebrow\">repository tree</p><h1>{}</h1>"
                                      "<p class=\"sub\">Browse <code>{}</code> by path and last "
                                      "change.</p>{}",
                                      web_ui_escape(repository_name(repository)),
                                      web_ui_escape(ref),
                                      forge_tabs(repository, "code", viewer));
    if (entries.empty()) {
        content += "<div class=\"panel empty\"><strong>This ref has no files.</strong><p>Push a "
                   "change to publish its first tree.</p></div>";
    } else {
        content += "<section class=\"panel tree-browser\"><div class=\"tree-browser-heading\">"
                   "<span>Path</span><span>Last change</span></div><ul class=\"tree-view\">";
        for (const RepositoryTreeEntry& entry : entries) {
            const std::string path = "/file?repository_id=" + repository.id +
                                     "&ref=" + url_component(ref) +
                                     "&path=" + url_component(entry.path);
            const std::string displayed =
                entry.kind == "blob" ? row_link(path, entry.path)
                                     : row_link("#" + url_component(entry.path), entry.path);
            content += std::format(
                "<li id=\"{}\"><div class=\"tree-entry-name\"><span class=\"tree-kind\" "
                "aria-hidden=\"true\">{}</span><div>{}<span class=\"tree-entry-meta\">{} · "
                "<code>{:o}</code>{}</span></div></div><div class=\"tree-last-change\">{}{}"
                "</div></li>",
                web_ui_escape(entry.path),
                entry.kind == "blob" ? "·" : "›",
                displayed,
                web_ui_escape(entry.kind),
                entry.mode,
                entry.kind == "blob" ? " · " + byte_count(entry.size) : "",
                entry.last_change.empty() ? "<span class=\"muted\">No recorded change</span>"
                                          : identifier_markup(entry.last_change),
                entry.last_message.empty()
                    ? ""
                    : "<span class=\"muted\">" + web_ui_escape(entry.last_message) + " · " +
                          web_ui_escape(entry.last_changed_at) + "</span>");
        }
        content += "</ul></section>";
    }
    return repository_shell("lake — tree", repository, "Code", content, viewer);
}

std::string web_ui_repository_file(const control::Repository& repository,
                                   std::string_view ref,
                                   const RepositoryFile& file,
                                   const std::vector<RepositoryBlameLine>& blame,
                                   WebUiViewer viewer) {
    const std::string query = "repository_id=" + url_component(repository.id) +
                              "&ref=" + url_component(ref) + "&path=" + url_component(file.path);
    std::string content =
        path_breadcrumbs(repository, ref, file.path) +
        std::format("<p class=\"eyebrow\">file</p><div class=\"file-heading\">"
                    "<div><h1>{}</h1><p class=\"sub\">{} at <code>{}</code></p>"
                    "</div><div class=\"actions\">{}{}</div></div>{}"
                    "<div class=\"file-meta\"><span>{}</span><span>{}</span>"
                    "<span>{}</span></div>{}",
                    web_ui_escape(file.path),
                    web_ui_escape(repository_name(repository)),
                    web_ui_escape(ref),
                    row_link("/file/raw?" + query, "Raw"),
                    row_link("/file/download?" + query, "Download"),
                    forge_tabs(repository, "code", viewer),
                    identifier_markup(file.blob),
                    byte_count(file.size),
                    syntax_language(file.path) ? file_language(file.path) : "Plain text",
                    source_view(file, blame));
    content +=
        R"(<script id="file-range-script">(()=>{const rows=[...document.querySelectorAll('[data-line]')];const select=()=>{rows.forEach(r=>r.classList.remove('selected'));const match=location.hash.match(/^#L(\d+)(?:-L(\d+))?$/);if(!match)return;const first=Number(match[1]);const last=Number(match[2]||match[1]);rows.forEach(r=>{const line=Number(r.dataset.line);if(line>=first&&line<=last)r.classList.add('selected');});};let anchor=null;document.addEventListener('click',event=>{const link=event.target.closest('[data-line-range]');if(!link)return;const line=Number(link.closest('[data-line]').dataset.line);if(event.shiftKey&&anchor!==null){event.preventDefault();const first=Math.min(anchor,line);const last=Math.max(anchor,line);location.hash=`L${first}-L${last}`;}else{anchor=line;}});addEventListener('hashchange',select);select();})();</script>)";
    return web_ui_shell("lake — file", file.path, content, viewer);
}

std::string web_ui_repository_file(const control::Repository& repository,
                                   const RepositoryFile& file,
                                   WebUiViewer viewer) {
    return web_ui_repository_file(repository, "main", file, {}, viewer);
}

std::string web_ui_repository_log(const control::Repository& repository,
                                  const std::vector<RepositoryChange>& changes,
                                  WebUiViewer viewer) {
    std::string content = std::format("<p class=\"eyebrow\">repository history</p><h1>Change "
                                      "Log</h1><p class=\"sub\">{}</p>",
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
    return repository_shell("lake — changes", repository, "Changes", content, viewer);
}

std::string web_ui_change(const control::Repository& repository,
                          const RepositoryChange& change,
                          const std::vector<RepositoryDiffEntry>& differences,
                          const ChangeReviewData& review,
                          WebUiViewer viewer) {
    std::string parents;
    for (const std::string& parent : change.parents) {
        if (!parents.empty()) {
            parents += ", ";
        }
        parents += parent;
    }
    const std::size_t newline = change.message.find('\n');
    std::string title = change.message.substr(0, newline);
    if (title.starts_with("# ")) {
        title.erase(0, 2);
    }
    const std::string body = newline == std::string::npos ? "" : change.message.substr(newline + 1);
    std::string content = std::format(
        "<p class=\"eyebrow\">change review</p><h1>{}</h1><p class=\"sub\">{}</p>{}"
        "<div class=\"change-toolbar\"><span>{}</span><button class=\"button secondary copy-id\" "
        "type=\"button\" data-copy=\"{}\">Copy identifier</button><div class=\"view-toggle\" "
        "role=\"group\" aria-label=\"Diff layout\"><button type=\"button\" data-view=\"unified\" "
        "aria-pressed=\"true\">Unified</button><button type=\"button\" data-view=\"split\" "
        "aria-pressed=\"false\">Side by side</button></div></div>"
        "<div class=\"panel\"><dl><dt>author</dt><dd>{}</dd><dt>created</dt>"
        "<dd>{}</dd><dt>parents</dt><dd>{}</dd></dl></div>",
        web_ui_escape(title.empty() ? "Untitled change" : title),
        web_ui_escape(repository_name(repository)),
        forge_tabs(repository, "changes", viewer),
        identifier_markup(change.id),
        web_ui_escape(change.id),
        identifier_markup(change.author),
        web_ui_escape(change.occurred_at),
        identifier_markup(parents));
    if (!body.empty()) {
        content +=
            std::format("<section class=\"panel change-message\"><h2>Description</h2>{}</section>",
                        web_ui_markdown(body));
    }
    content += "<section class=\"review-summary\"><h2>Review</h2>";
    if (review.reviews.empty()) {
        content += "<div class=\"panel empty\">No review has been requested.</div>";
    } else {
        for (const control::ForgeRecord& item : review.reviews) {
            content += std::format(
                "<div class=\"panel review-state\"><div><strong>{}</strong> "
                "{}</div><p class=\"sub\">Reviewers: {}</p></div>",
                web_ui_escape(item.value.value("state", "open")),
                badge_markup(item.value.value("state", "open")),
                identifier_list_markup(item.value.value("reviewers", std::vector<std::string>{})));
        }
    }
    content += "</section>";
    content += "<section><h2>Files Changed</h2>";
    std::size_t visible = 0;
    std::uint64_t total_added = 0;
    std::uint64_t total_removed = 0;
    std::string file_summary;
    for (const RepositoryDiffEntry& entry : differences) {
        if (hidden_reformatting_entry(entry)) {
            continue;
        }
        ++visible;
        std::uint64_t added = 0;
        std::uint64_t removed = 0;
        if (entry.line_hunks) {
            for (const RepositoryLineHunk& hunk : *entry.line_hunks) {
                added += hunk.added_lines.size();
                removed += hunk.removed_lines.size();
            }
        }
        total_added += added;
        total_removed += removed;
        file_summary += std::format(
            "<li><a href=\"#diff-{}\"><code>{}</code></a><span class=\"diff-stats\">"
            "<span class=\"additions\">+{}</span> <span class=\"deletions\">−{}</span></span></li>",
            web_ui_escape(markdown_slug(entry.path)),
            web_ui_escape(entry.path),
            added,
            removed);
    }
    if (visible != 0) {
        content += std::format("<div class=\"panel file-summary\"><strong>{} files</strong> "
                               "<span class=\"additions\">+{}</span> "
                               "<span class=\"deletions\">−{}</span><ul>{}</ul></div>",
                               visible,
                               total_added,
                               total_removed,
                               file_summary);
    }
    for (const RepositoryDiffEntry& entry : differences) {
        if (hidden_reformatting_entry(entry)) {
            continue;
        }
        std::string badges;
        for (const std::string& status : entry.changes) {
            badges += badge_markup(status);
        }
        content +=
            std::format("<section class=\"panel diff-entry\" id=\"diff-{}\"><h3><code>{}</code>"
                        "{}{}</h3><dl>"
                        "<dt>kind</dt><dd>{}</dd><dt>old path</dt><dd><code>{}</code></dd>"
                        "<dt>mode</dt><dd><code>{} → {}</code></dd><dt>blobs</dt>"
                        "<dd>{} → {}</dd></dl>",
                        web_ui_escape(markdown_slug(entry.path)),
                        web_ui_escape(entry.path),
                        badges,
                        "",
                        web_ui_escape(entry.kind),
                        web_ui_escape(entry.old_path.empty() ? "—" : entry.old_path),
                        mode_text(entry.old_mode),
                        mode_text(entry.new_mode),
                        identifier_markup(entry.old_blob.empty() ? "—" : entry.old_blob),
                        identifier_markup(entry.new_blob.empty() ? "—" : entry.new_blob));
        if (!entry.old_conflicts.empty() || !entry.new_conflicts.empty()) {
            content += std::format("<div class=\"notice\"><strong>Conflict values</strong><br>old: "
                                   "<code>{}</code><br>new: <code>{}</code></div>",
                                   web_ui_escape(joined(entry.old_conflicts)),
                                   web_ui_escape(joined(entry.new_conflicts)));
        }
        if (entry.line_hunks) {
            content += "<div class=\"diff-view unified-diff\" data-diff-view=\"unified\">";
            for (const RepositoryLineHunk& hunk : *entry.line_hunks) {
                content += std::format("<div class=\"diff-hunk\"><div class=\"hunk-heading\">"
                                       "@@ -{},{} +{},{} @@</div>",
                                       hunk.old_line,
                                       hunk.old_count,
                                       hunk.new_line,
                                       hunk.new_count);
                std::uint64_t old_line = hunk.old_line;
                for (const std::string& line : hunk.removed_lines) {
                    content += std::format("<div class=\"diff-line diff-remove\" id=\"{}-old-{}\">"
                                           "<a href=\"#{}-old-{}\">{}</a><code>-{}</code></div>",
                                           web_ui_escape(markdown_slug(entry.path)),
                                           old_line,
                                           web_ui_escape(markdown_slug(entry.path)),
                                           old_line,
                                           old_line,
                                           web_ui_escape(line));
                    ++old_line;
                }
                std::uint64_t new_line = hunk.new_line;
                for (const std::string& line : hunk.added_lines) {
                    content += std::format("<div class=\"diff-line diff-add\" id=\"{}-new-{}\">"
                                           "<a href=\"#{}-new-{}\">{}</a><code>+{}</code></div>",
                                           web_ui_escape(markdown_slug(entry.path)),
                                           new_line,
                                           web_ui_escape(markdown_slug(entry.path)),
                                           new_line,
                                           new_line,
                                           web_ui_escape(line));
                    ++new_line;
                }
                content += "</div>";
            }
            content += "</div><div class=\"diff-view split-diff\" data-diff-view=\"split\" hidden>";
            for (const RepositoryLineHunk& hunk : *entry.line_hunks) {
                const std::size_t rows =
                    std::max(hunk.removed_lines.size(), hunk.added_lines.size());
                content += std::format("<div class=\"hunk-heading\">@@ -{},{} +{},{} @@</div>",
                                       hunk.old_line,
                                       hunk.old_count,
                                       hunk.new_line,
                                       hunk.new_count);
                for (std::size_t index = 0; index < rows; ++index) {
                    const std::string old_text = index < hunk.removed_lines.size()
                                                     ? web_ui_escape(hunk.removed_lines[index])
                                                     : "";
                    const std::string new_text = index < hunk.added_lines.size()
                                                     ? web_ui_escape(hunk.added_lines[index])
                                                     : "";
                    content +=
                        std::format("<div class=\"split-row\"><code class=\"diff-remove\">{}</code>"
                                    "<code class=\"diff-add\">{}</code></div>",
                                    old_text,
                                    new_text);
                }
            }
            content += "</div>";
            for (const control::ForgeRecord& comment : review.comments) {
                const nlohmann::json anchor =
                    comment.value.value("anchor", nlohmann::json::object());
                if (anchor.value("path", "") != entry.path) {
                    continue;
                }
                const std::uint64_t line = anchor.value("line", std::uint64_t{0});
                content += std::format("<article class=\"review-comment\"><p class=\"sub\">{} · "
                                       "<a href=\"#{}-new-{}\">line {}</a></p>{}</article>",
                                       identifier_markup(comment.id),
                                       web_ui_escape(markdown_slug(entry.path)),
                                       line,
                                       line,
                                       web_ui_markdown(comment.value.value("body", "")));
            }
        }
        content += "</section>";
    }
    if (visible == 0) {
        content += "<div class=\"panel empty diff-entry\">No changes to display.</div>";
    }
    content += "</section><section><h2>Checks and Attestations</h2>";
    if (review.checks.items.empty()) {
        content += "<div class=\"panel empty\">No checks have reported for this change.</div>";
    } else {
        content += "<div class=\"stack\">";
        for (const RepositoryArtifact& check : review.checks.items) {
            std::vector<std::string> output_paths;
            output_paths.reserve(check.outputs.size());
            for (const RepositoryArtifactOutput& output : check.outputs) {
                output_paths.push_back(output.path);
            }
            std::vector<std::string> attestation_profiles;
            for (const RepositoryArtifactAttestation& attestation : check.attestations) {
                if (attestation.verified) {
                    attestation_profiles.push_back(attestation.profile);
                }
            }
            content += std::format(
                "<article class=\"panel check-row\"><strong>{}</strong>{}"
                "<span>{} attestations</span><span>outputs: <code>{}</code>"
                "</span><span>attested: <code>{}</code></span><span>{}</span>"
                "</article>",
                web_ui_escape(check.action),
                badge_markup(check.exit_code == 0 ? "passed" : "failed"),
                check.attestations.size(),
                web_ui_escape(output_paths.empty() ? "none" : joined(output_paths)),
                web_ui_escape(attestation_profiles.empty() ? "none" : joined(attestation_profiles)),
                identifier_markup(check.id));
        }
        content += "</div>";
    }
    content +=
        R"(</section><script id="change-review-script">(()=>{const views=[...document.querySelectorAll('[data-diff-view]')];document.addEventListener('click',async event=>{const copy=event.target.closest('[data-copy]');if(copy){await navigator.clipboard.writeText(copy.dataset.copy);copy.textContent='Copied';return;}const toggle=event.target.closest('[data-view]');if(!toggle)return;views.forEach(view=>{view.hidden=view.dataset.diffView!==toggle.dataset.view;});document.querySelectorAll('[data-view]').forEach(button=>{button.setAttribute('aria-pressed',String(button===toggle));});});})();</script>)";
    return repository_shell("lake — change", repository, "Change", content, viewer);
}

std::string web_ui_change(const control::Repository& repository,
                          const RepositoryChange& change,
                          const std::vector<RepositoryDiffEntry>& differences,
                          WebUiViewer viewer) {
    return web_ui_change(repository, change, differences, {}, viewer);
}

std::string web_ui_issues(const control::Repository& repository,
                          const std::vector<control::ForgeRecord>& issues,
                          std::string_view state,
                          std::string_view label,
                          std::string_view assignee,
                          WebUiViewer viewer) {
    std::string content = std::format(
        "<p class=\"eyebrow\">issue tracker</p><h1>Issues</h1><p class=\"sub\">{}</p>{}"
        "<form class=\"filters\" method=\"get\" action=\"/issues\">"
        "<input type=\"hidden\" name=\"repository_id\" value=\"{}\">"
        "<label>State<select name=\"state\"><option value=\"\"{}>All</option>"
        "<option value=\"open\"{}>Open</option><option value=\"closed\"{}>Closed</option>"
        "</select></label><label>Label<input name=\"label\" value=\"{}\"></label>"
        "<label>Assignee<input name=\"assignee\" value=\"{}\"></label>"
        "<button type=\"submit\">Filter</button></form>",
        web_ui_escape(repository_name(repository)),
        forge_tabs(repository, "issues", viewer),
        web_ui_escape(repository.id),
        state.empty() ? " selected" : "",
        state == "open" ? " selected" : "",
        state == "closed" ? " selected" : "",
        web_ui_escape(label),
        web_ui_escape(assignee));
    std::size_t visible = 0;
    content += "<div class=\"issue-list\">";
    for (const control::ForgeRecord& issue : issues) {
        const std::string issue_state = issue.value.value("state", "open");
        const std::vector<std::string> labels =
            issue.value.value("labels", std::vector<std::string>{});
        const std::vector<std::string> assignees =
            issue.value.value("assignees", std::vector<std::string>{});
        if ((!state.empty() && issue_state != state) ||
            (!label.empty() && !std::ranges::contains(labels, label)) ||
            (!assignee.empty() && !std::ranges::contains(assignees, assignee))) {
            continue;
        }
        ++visible;
        std::string label_markup;
        for (const std::string& item : labels) {
            label_markup += badge_markup(item);
        }
        content += std::format("<article class=\"panel issue-row\"><div><h2><a "
                               "href=\"/issue?repository_id={}&amp;id={}\">"
                               "{}</a></h2><p class=\"sub\">{} · opened {} · assignees "
                               "{}</p></div><div>{}{}</div></article>",
                               web_ui_escape(url_component(repository.id)),
                               web_ui_escape(url_component(issue.id)),
                               web_ui_escape(issue.value.value("title", "Untitled issue")),
                               identifier_markup(issue.id),
                               web_ui_escape(issue.created_at),
                               identifier_list_markup(assignees),
                               badge_markup(issue_state),
                               label_markup);
    }
    content += "</div>";
    if (visible == 0) {
        content += "<div class=\"panel empty\">No issues match these filters.</div>";
    }
    return repository_shell("lake — issues", repository, "Issues", content, viewer);
}

std::string web_ui_issue(const control::Repository& repository,
                         const control::ForgeRecord& issue,
                         const std::vector<control::ForgeRecord>& comments,
                         WebUiViewer viewer) {
    const std::vector<std::string> labels = issue.value.value("labels", std::vector<std::string>{});
    const std::vector<std::string> assignees =
        issue.value.value("assignees", std::vector<std::string>{});
    const std::vector<std::string> references =
        issue.value.value("references", std::vector<std::string>{});
    std::string labels_markup;
    for (const std::string& label : labels) {
        labels_markup += badge_markup(label);
    }
    std::string references_markup;
    for (const std::string& reference : references) {
        references_markup += std::format("<li>{}</li>", identifier_markup(reference));
    }
    if (references_markup.empty()) {
        references_markup = "<li class=\"muted\">No linked changes.</li>";
    }
    std::string content = std::format(
        "<p class=\"eyebrow\">issue</p><h1>{}</h1><p class=\"sub\">{} opened {} {}</p>{}"
        "<div class=\"issue-layout\"><div><article class=\"panel issue-body\">{}</article>"
        "<section class=\"discussion\"><h2>Discussion</h2>",
        web_ui_escape(issue.value.value("title", "Untitled issue")),
        identifier_markup(issue.id),
        web_ui_escape(issue.created_at),
        badge_markup(issue.value.value("state", "open")),
        forge_tabs(repository, "issues", viewer),
        web_ui_markdown(issue.value.value("body", "")));
    if (comments.empty()) {
        content += "<div class=\"panel empty\">No discussion yet.</div>";
    } else {
        for (const control::ForgeRecord& comment : comments) {
            content += std::format(
                "<article class=\"panel issue-comment\"><p class=\"sub\">{} · {}</p>{}</article>",
                identifier_markup(comment.id),
                web_ui_escape(comment.created_at),
                web_ui_markdown(comment.value.value("body", "")));
        }
    }
    content +=
        std::format("</section></div><aside class=\"panel issue-details\"><h2>Details</h2><dl>"
                    "<dt>labels</dt><dd>{}</dd><dt>assignees</dt><dd>{}</dd></dl>"
                    "<h3>Linked Changes</h3><ul>{}</ul></aside></div>",
                    labels_markup.empty() ? "<span class=\"muted\">None</span>" : labels_markup,
                    assignees.empty() ? "<span class=\"muted\">Unassigned</span>"
                                      : identifier_list_markup(assignees),
                    references_markup);
    return repository_shell("lake — issue", repository, "Issue", content, viewer);
}

std::string web_ui_releases(const control::Repository& repository,
                            const std::vector<control::ForgeRecord>& releases,
                            WebUiViewer viewer) {
    std::string content =
        std::format("<p class=\"eyebrow\">repository releases</p><h1>Releases</h1>"
                    "<p class=\"sub\">{}</p>{}",
                    web_ui_escape(repository_name(repository)),
                    forge_tabs(repository, "releases", viewer));
    if (releases.empty()) {
        content += "<div class=\"panel empty\">No releases have been published.</div>";
    }
    for (const control::ForgeRecord& release : releases) {
        content += std::format(
            "<article class=\"panel release-card\"><header><div><p class=\"eyebrow\">{}</p>"
            "<h2>{}</h2></div><span>{}</span></header><p class=\"sub\">Published {} at {}</p>{}",
            identifier_markup(release.id),
            web_ui_escape(release.value.value("tag", "Untitled release")),
            identifier_markup(release.value.value("change_id", "")),
            web_ui_escape(release.created_at),
            web_ui_escape(release.updated_at),
            web_ui_markdown(release.value.value("notes", "")));
        const nlohmann::json artifacts = release.value.value("artifacts", nlohmann::json::array());
        if (artifacts.empty()) {
            content += "<div class=\"empty compact-empty\">No release artifacts.</div>";
        } else {
            content += "<h3>Artifacts and Attestations</h3><ul class=\"release-artifacts\">";
            for (const nlohmann::json& artifact : artifacts) {
                const nlohmann::json attestations =
                    artifact.value("attestations", nlohmann::json::array());
                content += std::format("<li><span><strong>{}</strong> · {} outputs</span><span>{} "
                                       "· {} attestations</span></li>",
                                       web_ui_escape(artifact.value("action", "artifact")),
                                       artifact.value("outputs", 0),
                                       identifier_markup(artifact.value("id", "")),
                                       attestations.size());
            }
            content += "</ul>";
        }
        content += "</article>";
    }
    return repository_shell("lake — releases", repository, "Releases", content, viewer);
}

std::string web_ui_webhooks(const control::Repository& repository,
                            const std::vector<control::ForgeRecord>& webhooks,
                            const std::vector<control::ForgeRecord>& deliveries,
                            WebUiViewer viewer) {
    std::string content =
        std::format("<p class=\"eyebrow\">repository automation</p><h1>Webhooks</h1>"
                    "<p class=\"sub\">{} · signed delivery history</p>{}",
                    web_ui_escape(repository_name(repository)),
                    forge_tabs(repository, "webhooks", viewer));
    if (webhooks.empty()) {
        content += "<div class=\"panel empty\">No webhooks are configured.</div>";
    }
    for (const control::ForgeRecord& webhook : webhooks) {
        const std::vector<std::string> events =
            webhook.value.value("events", std::vector<std::string>{});
        content += std::format(
            "<section class=\"panel webhook-card\"><header><div><h2>{}</h2><p class=\"sub\">{}</p>"
            "</div>{}</header><p>Events: {}</p><h3>Deliveries</h3>",
            web_ui_escape(webhook.value.value("url", "")),
            web_ui_escape(webhook.created_at),
            identifier_markup(webhook.id),
            web_ui_escape(joined(events)));
        std::size_t count = 0;
        for (const control::ForgeRecord& delivery : deliveries) {
            if (delivery.value.value("webhook_id", "") != webhook.id) {
                continue;
            }
            ++count;
            const bool delivered = delivery.value.value("delivered", false);
            content += std::format(
                "<article class=\"delivery-row\"><div><strong>{}</strong>{}<p class=\"sub\">"
                "attempt {} · status {} · {}</p></div>{}",
                web_ui_escape(delivery.value.value("event", "event")),
                badge_markup(delivered ? "delivered" : "failed"),
                delivery.value.value("attempt", 0),
                delivery.value.value("status", 0),
                web_ui_escape(delivery.value.value("error", "no error")),
                identifier_markup(delivery.id));
            if (!delivered && viewer.signed_in) {
                content += std::format(
                    "<form method=\"post\" action=\"/webhooks/retry\"><input type=\"hidden\" "
                    "name=\"repository_id\" value=\"{}\"><input type=\"hidden\" "
                    "name=\"delivery_id\" "
                    "value=\"{}\"><button class=\"secondary\" "
                    "type=\"submit\">Retry</button></form>",
                    web_ui_escape(repository.id),
                    web_ui_escape(delivery.id));
            }
            content += "</article>";
        }
        if (count == 0) {
            content += "<div class=\"empty compact-empty\">No delivery attempts.</div>";
        }
        content += "</section>";
    }
    return repository_shell("lake — webhooks", repository, "Webhooks", content, viewer);
}

std::string web_ui_branch_protection(const control::Repository& repository,
                                     const std::vector<control::ForgeRecord>& rules,
                                     const std::vector<audit::Event>& events,
                                     WebUiViewer viewer) {
    std::string content =
        std::format("<p class=\"eyebrow\">repository policy</p><h1>Branch Protection</h1>"
                    "<p class=\"sub\">{}</p>{}<section><h2>Rules</h2>",
                    web_ui_escape(repository_name(repository)),
                    forge_tabs(repository, "protection", viewer));
    if (rules.empty()) {
        content += "<div class=\"panel empty\">No branch protection rules.</div>";
    }
    for (const control::ForgeRecord& rule : rules) {
        const std::vector<std::string> pushers =
            rule.value.value("allowed_pushers", std::vector<std::string>{});
        content += std::format(
            "<article class=\"panel protection-rule\"><header><h3><code>{}</code></h3>{}</header>"
            "<dl><dt>review</dt><dd>{}</dd><dt>verification</dt><dd>{}</dd>"
            "<dt>updates</dt><dd>{}</dd><dt>allowed pushers</dt><dd>{}</dd>"
            "<dt>policy object</dt><dd>{}</dd></dl></article>",
            web_ui_escape(rule.value.value("ref", "")),
            identifier_markup(rule.id),
            badge_markup(rule.value.value("required_review", false) ? "required" : "optional"),
            badge_markup(rule.value.value("required_verification", false) ? "required"
                                                                          : "optional"),
            badge_markup(rule.value.value("fast_forward_only", false) ? "fast-forward only"
                                                                      : "any"),
            identifier_list_markup(pushers),
            identifier_markup(rule.value.value("policy_object_id", "")));
    }
    content += "</section><section><h2>Refused Updates</h2>";
    std::size_t refused = 0;
    for (const audit::Event& event : events) {
        if (event.draft.kind != "forge.branch_protection.refused" ||
            event.draft.details.value("repository", "") != repository.id) {
            continue;
        }
        ++refused;
        content += std::format(
            "<article class=\"panel protection-event\"><header><strong>{}</strong>{}</header>"
            "<p>{}</p><p class=\"sub\">{} · {} · {}</p></article>",
            web_ui_escape(event.draft.details.value("reference", "reference")),
            badge_markup("refused"),
            web_ui_escape(event.draft.details.value("reason", "update refused")),
            identifier_markup(event.draft.details.value("caller", event.draft.principal)),
            web_ui_escape(event.draft.details.value("rule", "rule")),
            web_ui_escape(event.occurred_at));
    }
    if (refused == 0) {
        content += "<div class=\"panel empty\">No protected updates have been refused.</div>";
    }
    content += "</section>";
    return repository_shell("lake — branch protection", repository, "Protection", content, viewer);
}

namespace {

std::string conflict_kind_label(std::string_view kind) {
    if (kind == "text") {
        return "Text overlap";
    }
    if (kind == "structural") {
        return "Structural overlap";
    }
    if (kind == "crdt-suspicious") {
        return "Suspicious concurrent update";
    }
    if (kind == "suspicious-merge") {
        return "Suspicious merge proposal";
    }
    return "Conflict";
}

std::string conflict_region_markup(const RepositoryConflictRegion& region) {
    std::string output = "<code>" + web_ui_escape(region.path) + "</code>";
    if (region.bytes) {
        output += std::format(
            " · byte offset {} · length {}", region.bytes->offset, region.bytes->length);
    }
    if (region.nodes) {
        output += " · nodes " + identifier_markup(region.nodes->first) + " through " +
                  identifier_markup(region.nodes->last);
    }
    return output;
}

std::string conflict_reasons_markup(const RepositoryConflict& conflict) {
    if (conflict.reasons.empty()) {
        return {};
    }
    std::string output = "<h3>Why This Needs Attention</h3><ul>";
    for (const std::string& reason : conflict.reasons) {
        output += "<li>" + web_ui_escape(reason) + "</li>";
    }
    output += "</ul>";
    return output;
}

std::string conflict_resolution_markup(const control::Repository& repository,
                                       std::string_view ref,
                                       const RepositoryConflict& conflict,
                                       std::string_view action,
                                       bool signed_in) {
    if (!signed_in) {
        return "<p class=\"sub\">Sign in with write access to resolve this conflict.</p>";
    }
    std::string output =
        std::format("<h3>Choose the Value to Keep</h3><form class=\"stack\" method=\"post\" "
                    "action=\"{}\"><input type=\"hidden\" name=\"repository_id\" value=\"{}\">"
                    "<input type=\"hidden\" name=\"ref\" value=\"{}\"><input type=\"hidden\" "
                    "name=\"path\" value=\"{}\"><input type=\"hidden\" name=\"conflict_id\" "
                    "value=\"{}\"><div class=\"actions\"><button class=\"secondary\" "
                    "name=\"resolution\" value=\"left\" type=\"submit\">Keep left value</button>"
                    "<button class=\"secondary\" name=\"resolution\" value=\"right\" "
                    "type=\"submit\">Keep right value</button>",
                    web_ui_escape(action),
                    web_ui_escape(repository.id),
                    web_ui_escape(ref),
                    web_ui_escape(conflict.region.path),
                    web_ui_escape(conflict.id));
    if (conflict.proposal) {
        output += "<button class=\"primary\" name=\"resolution\" value=\"accept_proposal\" "
                  "type=\"submit\">Accept reviewed proposal</button>";
    }
    output += "</div><label>Prepared replacement text<textarea name=\"blob\" rows=\"6\"></textarea>"
              "</label><button class=\"secondary\" name=\"resolution\" value=\"blob\" "
              "type=\"submit\">Use prepared text</button></form>";
    return output;
}

std::string conflict_values_markup(const RepositoryConflict& conflict) {
    std::string output = "<dl><dt>Base value</dt><dd>" + identifier_markup(conflict.base) +
                         "</dd><dt>Left value</dt><dd>" + identifier_markup(conflict.left) +
                         "</dd><dt>Right value</dt><dd>" + identifier_markup(conflict.right) +
                         "</dd>";
    if (conflict.proposal) {
        output +=
            "<dt>Reviewed proposal</dt><dd>" + identifier_markup(*conflict.proposal) + "</dd>";
    }
    output += "</dl>";
    return output;
}

} // namespace

std::string web_ui_repository_conflicts(const control::Repository& repository,
                                        const std::vector<RepositoryConflict>& conflicts,
                                        WebUiViewer viewer) {
    std::string content =
        std::format("<p class=\"eyebrow\">repository conflicts</p><h1>Conflicts</h1>"
                    "<p class=\"sub\">{}</p>{}",
                    web_ui_escape(repository_name(repository)),
                    forge_tabs(repository, "conflicts", viewer));
    if (conflicts.empty()) {
        content += "<div class=\"panel empty\">No unresolved conflict values.</div>";
    } else {
        for (const RepositoryConflict& conflict : conflicts) {
            content += std::format(
                "<article class=\"panel "
                "conflict-value\"><header><h2><code>{}</code></h2>{}</header>"
                "<p><strong>{}</strong></p><p>{}</p><p class=\"sub\">Affected region: {}</p>",
                web_ui_escape(conflict.region.path),
                badge_markup(conflict.kind),
                web_ui_escape(conflict_kind_label(conflict.kind)),
                web_ui_escape(conflict.explanation),
                conflict_region_markup(conflict.region));
            content += conflict_reasons_markup(conflict);
            content += conflict_values_markup(conflict);
            content += row_link("/drive/conflict?repository_id=" + repository.id +
                                    "&ref=main&path=" + url_component(conflict.region.path),
                                "Compare preserved values");
            content += conflict_resolution_markup(
                repository, "main", conflict, "/conflicts/resolve", viewer.signed_in);
            content += "</article>";
        }
    }
    return repository_shell("lake — conflicts", repository, "Conflicts", content, viewer);
}

std::string web_ui_artifacts(const control::Repository& repository,
                             const RepositoryArtifactPage& page,
                             const RepositoryArtifactFilter& filter,
                             WebUiViewer viewer) {
    std::string content = std::format(
        "<p class=\"eyebrow\">repository artifacts</p><h1>Build Results</h1>"
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
            content += std::format(
                "<tr><td>{}</td><td><code>{}</code></td><td>{}</td><td>{}</td><td>{}</td></tr>",
                row_link("/artifact?repository_id=" + repository.id +
                             "&id=" + url_component(artifact.id),
                         artifact.id),
                web_ui_escape(artifact.action),
                web_ui_escape(joined(paths)),
                badge_markup(artifact.divergent ? "divergent" : artifact.cache_status),
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
    return repository_shell("lake — artifacts", repository, "Artifacts", content, viewer);
}

std::string web_ui_artifact(const control::Repository& repository,
                            const RepositoryArtifact& artifact,
                            WebUiViewer viewer) {
    std::string content =
        std::format("<p class=\"eyebrow\">artifact result</p><h1>{}</h1><p class=\"sub\">{}</p>"
                    "<div class=\"panel\"><dl><dt>action</dt><dd><code>{}</code></dd>"
                    "<dt>exit</dt><dd>{}</dd><dt>cache</dt><dd>{}</dd>"
                    "<dt>divergent</dt><dd>{}</dd><dt>worker</dt><dd>{}</dd>"
                    "<dt>observed</dt><dd>{}</dd><dt>started</dt><dd>{}</dd>"
                    "<dt>finished</dt><dd>{}</dd></dl></div>",
                    web_ui_escape(artifact.id),
                    web_ui_escape(repository_name(repository)),
                    web_ui_escape(artifact.action),
                    artifact.exit_code,
                    badge_markup(artifact.cache_status),
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
    content += "<section class=\"panel diff-entry\"><h2>Search Inside</h2>"
               "<p class=\"muted\">Search an output with "
               "<code>in:artifact:&lt;output&gt;@&lt;result&gt;</code>. "
               "Members nested more than one container deep are recorded but not indexed.</p>"
               "<form class=\"filters\" method=\"get\" action=\"/search\">"
               "<input type=\"hidden\" name=\"repository\" value=\"" +
               web_ui_escape(repository.id) +
               "\"><label>Symbol or text<input name=\"query\" "
               "placeholder=\"symbol in:artifact:output.deb@result\" required></label>"
               "<button type=\"submit\">Search</button></form></section>";
    content += "<section class=\"panel diff-entry\"><h2>Indexed Members</h2>";
    if (artifact.members.empty()) {
        content += "<p class=\"muted\">This artifact has not been indexed yet.</p>";
    } else {
        content += "<table><thead><tr><th>Artifact</th><th>Member</th><th>Digest</th>"
                   "<th>Indexed</th><th>Search</th></tr></thead><tbody>";
        for (const RepositoryArtifactMember& member : artifact.members) {
            const std::string query =
                member.path + " in:artifact:" + member.artifact + "@" + member.version;
            content += std::format("<tr><td><code>{}</code></td><td><code>{}</code></td>"
                                   "<td><code>{}</code></td><td>{}</td><td>{}</td></tr>",
                                   web_ui_escape(member.artifact),
                                   web_ui_escape(member.path),
                                   web_ui_escape(member.digest),
                                   web_ui_escape(member.indexed ? "yes" : member.reason),
                                   member.indexed ? row_link("/search?repository=" + repository.id +
                                                                 "&query=" + url_component(query),
                                                             "Search")
                                                  : "—");
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
            content += std::format(
                "<tr><td><span class=\"badge status-ai\">{}</span><br><code>{}</code></td>"
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
    return repository_shell("lake — artifact", repository, "Artifact", content, viewer);
}

std::string web_ui_organizations(const std::vector<control::Organization>& organizations,
                                 WebUiViewer viewer) {
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
                row_link("/organization?id=" + organization.id, "settings"),
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
                        viewer);
}

std::string web_ui_organization(const control::Organization& organization,
                                const OrganizationSettingsData& data,
                                std::string_view tab,
                                std::string_view error,
                                WebUiViewer viewer) {
    const std::string selected = tab.empty() ? "overview" : std::string(tab);
    const std::string base = "/organization?id=" + organization.id + "&tab=";
    std::string content =
        "<p class=\"eyebrow\">organization settings</p><h1>" +
        web_ui_escape(organization.display_name) +
        "</h1><p class=\"sub\">Organization administration, identity, access, and evidence.</p>" +
        "<nav class=\"tabs\" aria-label=\"Organization settings\">";
    for (const auto& [name, label] :
         std::vector<std::pair<std::string, std::string>>{{"overview", "Overview"},
                                                          {"teams", "Teams"},
                                                          {"members", "Members"},
                                                          {"invitations", "Invitations"},
                                                          {"access", "Access"},
                                                          {"sso", "SSO"},
                                                          {"applications", "Applications"},
                                                          {"metering", "Metering"},
                                                          {"audit", "Audit"}}) {
        content += "<a href=\"" + base + name + "\"" +
                   (selected == name ? " aria-current=\"page\"" : "") + ">" + label + "</a>";
    }
    content += "</nav>" + error_markup(error);
    if (selected == "overview") {
        content +=
            std::format("<section class=\"panel\"><dl><dt>Identity</dt><dd><code>{}</code></dd>"
                        "<dt>Teams</dt><dd>{}</dd><dt>Members</dt><dd>{}</dd>"
                        "<dt>Pending invitations</dt><dd>{}</dd></dl></section>",
                        web_ui_escape(organization.taap_id),
                        data.teams.size(),
                        data.memberships.size(),
                        data.invitations.size());
    } else if (selected == "teams") {
        content += "<section class=\"panel\"><h2>Teams</h2>";
        for (const auto& team : data.teams) {
            content +=
                "<p>" + row_link("/team?id=" + team.id, web_ui_escape(team.display_name)) + "</p>";
        }
        if (data.teams.empty()) {
            content += "<p class=\"empty\">No teams in this organization.</p>";
        }
        content += "</section>";
    } else if (selected == "members") {
        content += "<section class=\"panel\"><h2>Members</h2>";
        for (const auto& member : data.memberships) {
            content +=
                "<p>" +
                row_link("/member?id=" + member.id, web_ui_escape(member.principal_taap_id)) +
                " <span class=\"badge\">" + web_ui_escape(member.role) + "</span></p>";
        }
        if (data.memberships.empty()) {
            content += "<p class=\"empty\">No memberships in this organization.</p>";
        }
        content += "</section>";
    } else if (selected == "invitations") {
        content +=
            "<section class=\"panel\"><h2>Invite a Member</h2><form class=\"stack\" "
            "method=\"post\" action=\"/organization/invitations\">"
            "<input type=\"hidden\" name=\"organization_id\" value=\"" +
            web_ui_escape(organization.id) +
            "\"><label>Email or handle<input name=\"invitee\" required></label><label>Type<select "
            "name=\"invitee_type\"><option value=\"email\">Email</option><option "
            "value=\"handle\">Handle</option></select></label><label>Role<select "
            "name=\"role\"><option>member</option><option>admin</option><option>owner</option></"
            "select></label><label>Expiry<input name=\"expires_at\" type=\"datetime-local\" "
            "required></label><button class=\"primary\" type=\"submit\">Send "
            "invitation</button></form></section>";
        if (data.invitations.empty()) {
            content += "<div class=\"panel empty\">No pending invitations.</div>";
        }
    } else if (selected == "access") {
        content += "<section class=\"panel\"><h2>Effective Access</h2>";
        for (const auto& entry : data.access) {
            content += "<p><code>" + web_ui_escape(entry.principal_taap_id) + "</code> " +
                       "<span class=\"badge\">" + web_ui_escape(entry.role) + "</span></p>";
        }
        if (data.access.empty()) {
            content += "<p class=\"empty\">No effective access entries.</p>";
        }
        content += "</section>";
    } else if (selected == "sso") {
        content +=
            "<section class=\"panel\"><h2>Configure SSO Provider</h2><form class=\"stack\" "
            "method=\"post\" action=\"/organization/sso\">"
            "<input type=\"hidden\" name=\"organization_id\" value=\"" +
            web_ui_escape(organization.id) +
            "\"><label>Name<input name=\"name\" required></label><label>Protocol<select "
            "name=\"protocol\"><option value=\"oidc\">OpenID Connect</option><option "
            "value=\"saml\">SAML 2.0</option></select></label><label>Issuer<input name=\"issuer\" "
            "required></label><label>SCIM token<input name=\"scim_token\" required></label><button "
            "class=\"primary\" type=\"submit\">Save provider</button></form></section>";
        for (const auto& provider : data.providers) {
            content += "<p class=\"panel\"><code>" + web_ui_escape(provider.name) + "</code> " +
                       web_ui_escape(provider.protocol) + "</p>";
        }
    } else if (selected == "applications") {
        content += "<section class=\"panel\"><h2>Applications</h2>";
        for (const auto& application : data.applications) {
            content +=
                "<p>" +
                row_link("/application?id=" + application.id, web_ui_escape(application.name)) +
                "</p>";
        }
        if (data.applications.empty()) {
            content +=
                "<p class=\"empty\">No applications belong to this organization customer.</p>";
        }
        content += "</section>";
    } else if (selected == "metering") {
        const std::string customer_id = data.metering.value("customer_id", "");
        content += "<section class=\"panel\"><h2>Metering</h2>";
        if (customer_id.empty()) {
            content += "<p class=\"empty\">No customer is attached to this organization.</p>";
        } else {
            content += "<p>Storage quota: " +
                       std::to_string(data.metering.value("quota_bytes", std::uint64_t{0})) +
                       " bytes.</p><p>" +
                       row_link("/metering?customer_id=" + customer_id, "Open metering") + "</p>";
        }
        content += "</section>";
    } else if (selected == "audit") {
        content += "<section class=\"panel\"><h2>Recent Audit Outcomes</h2>";
        for (const auto& event : data.audit_events) {
            content += "<p><code>" + web_ui_escape(event.draft.action) + "</code> " +
                       web_ui_escape(event.draft.outcome) + "</p>";
        }
        if (data.audit_events.empty()) {
            content += "<p class=\"empty\">No matching audit events.</p>";
        }
        content += "</section>";
    }
    return web_ui_shell("lake — organization settings", organization.display_name, content, viewer);
}

std::string web_ui_team(const control::Team& team,
                        const std::vector<control::Membership>& memberships,
                        const std::vector<control::EffectiveAccessEntry>& access,
                        WebUiViewer viewer) {
    std::string content =
        "<p class=\"eyebrow\">organization settings</p><h1>" +
        web_ui_escape(team.display_name.empty() ? team.name : team.display_name) +
        "</h1><nav class=\"tabs\" aria-label=\"Team settings\"><a aria-current=\"page\" "
        "href=\"/team?id=" +
        web_ui_escape(team.id) +
        "\">Overview</a><a href=\"/members?organization_id=" + web_ui_escape(team.organization_id) +
        "&team_id=" + web_ui_escape(team.id) +
        "\">Members</a><a href=\"/team/access?id=" + web_ui_escape(team.id) +
        "\">Access</a></nav><section class=\"panel\"><h2>Members</h2>";
    for (const auto& member : memberships) {
        content += "<p>" +
                   row_link("/member?id=" + member.id, web_ui_escape(member.principal_taap_id)) +
                   "</p>";
    }
    if (memberships.empty()) {
        content += "<p class=\"empty\">No team members.</p>";
    }
    content += "</section><section class=\"panel\"><h2>Effective Access</h2><p>" +
               std::to_string(access.size()) + " access entries.</p></section>";
    return web_ui_shell("lake — team settings", team.name, content, viewer);
}

std::string web_ui_member(const control::Membership& membership,
                          const std::vector<control::EffectiveAccessEntry>& access,
                          std::string_view error,
                          bool confirm_removal,
                          WebUiViewer viewer) {
    std::string content =
        "<p class=\"eyebrow\">organization settings</p><h1>Member</h1>" + error_markup(error) +
        "<section class=\"panel\"><dl><dt>Principal</dt><dd><code>" +
        web_ui_escape(membership.principal_taap_id) + "</code></dd><dt>Role</dt><dd>" +
        web_ui_escape(membership.role) + "</dd><dt>Attestation evidence</dt><dd><code>" +
        web_ui_escape(membership.attestation_digest) + "</code></dd></dl>" +
        "<form class=\"inline\" method=\"post\" action=\"/member/role\"><input type=\"hidden\" "
        "name=\"id\" value=\"" +
        web_ui_escape(membership.id) +
        "\"><label>Role<select "
        "name=\"role\"><option>member</option><option>admin</option><option>owner</option></"
        "select></label><button type=\"submit\">Change role</button></form>";
    if (confirm_removal) {
        content += "<p class=\"notice\">Removing this member revokes their membership and derived "
                   "grants immediately.</p><form class=\"inline\" method=\"post\" "
                   "action=\"/member/remove\"><input type=\"hidden\" name=\"id\" value=\"" +
                   web_ui_escape(membership.id) +
                   "\"><input type=\"hidden\" name=\"confirmed\" value=\"yes\"><button "
                   "class=\"destructive\" type=\"submit\">Confirm removal</button></form>";
    } else {
        content += "<form class=\"inline\" method=\"post\" action=\"/member/remove\"><input "
                   "type=\"hidden\" name=\"id\" value=\"" +
                   web_ui_escape(membership.id) +
                   "\"><button class=\"destructive\" type=\"submit\">Remove member</button></form>";
    }
    content += "</section><section class=\"panel\"><h2>Effective Access</h2><p>" +
               std::to_string(access.size()) + " entries at this scope.</p></section>";
    return web_ui_shell("lake — member settings", membership.principal_taap_id, content, viewer);
}

std::string web_ui_settings(std::string_view identity,
                            const std::vector<control::Session>& sessions,
                            const std::vector<control::Device>& devices,
                            std::string_view tab,
                            std::string_view error,
                            WebUiViewer viewer) {
    const std::string selected = tab.empty() ? "profile" : std::string(tab);
    std::string content = "<p class=\"eyebrow\">personal settings</p><h1>Settings</h1><nav "
                          "class=\"tabs\" aria-label=\"Personal settings\">";
    for (const auto& [name, label] :
         std::vector<std::pair<std::string, std::string>>{{"profile", "Profile"},
                                                          {"sessions", "Sessions and devices"},
                                                          {"passkeys", "Passkeys"},
                                                          {"tokens", "Tokens"},
                                                          {"notifications", "Notifications"}}) {
        content += "<a href=\"/settings?tab=" + name + "\"" +
                   (selected == name ? " aria-current=\"page\"" : "") + ">" + label + "</a>";
    }
    content += "</nav>" + error_markup(error) + "<section class=\"panel\">";
    if (selected == "profile") {
        content += "<h2>Profile</h2><p><code>" + web_ui_escape(identity) + "</code></p>";
    } else if (selected == "sessions") {
        content += "<h2>Sessions and Devices</h2><p>" + std::to_string(sessions.size()) +
                   " sessions and " + std::to_string(devices.size()) + " devices.</p><p>" +
                   row_link("/sessions", "Manage sessions and devices") + "</p>";
    } else if (selected == "passkeys") {
        content += "<h2>Passkeys</h2><p>" + row_link("/passkeys", "Manage passkeys") + "</p>";
    } else if (selected == "tokens") {
        content += "<h2>Tokens</h2><p>" + row_link("/tokens", "Create or revoke tokens") + "</p>";
    } else {
        content += "<h2>Notifications</h2><p class=\"empty\">No notification delivery preferences "
                   "are configured.</p>";
    }
    content += "</section>";
    return web_ui_shell("lake — settings", selected, content, viewer);
}

std::string web_ui_sso(std::string_view organization_id,
                       const std::vector<control::SsoProvider>& providers,
                       const std::vector<control::FederationIssuer>& issuers,
                       const std::vector<control::ProvisioningLog>& logs,
                       WebUiViewer viewer) {
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
    content += "<h2>Allowed Issuer Claims</h2>";
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
    content += "<h2>Provisioning Log</h2>";
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
        "lake — enterprise SSO", "organization " + std::string(organization_id), content, viewer);
}

std::string web_ui_teams(std::string_view organization_id,
                         const std::vector<control::Team>& teams,
                         WebUiViewer viewer) {
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
    return web_ui_shell("lake — teams", web_ui_escape(organization_id), content, viewer);
}

std::string web_ui_memberships(std::string_view organization_id,
                               std::string_view team_id,
                               const std::vector<control::Membership>& memberships,
                               WebUiViewer viewer) {
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
    return web_ui_shell("lake — members", scope, content, viewer);
}

std::string web_ui_invitations(std::string_view organization_id,
                               std::string_view team_id,
                               const std::vector<control::Invitation>& invitations,
                               WebUiViewer viewer) {
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
                                   badge_markup(invitation.status),
                                   web_ui_escape(invitation.expires_at));
        }
        content += "</tbody></table>";
    }
    const std::string scope = team_id.empty() ? std::string(organization_id) : std::string(team_id);
    return web_ui_shell("lake — invitations", scope, content, viewer);
}

std::string web_ui_grants(const control::PolicyObject& object,
                          const std::vector<control::Grant>& grants,
                          WebUiViewer viewer) {
    std::string content = "<p class=\"eyebrow\">policy</p><h1>Direct Grants</h1>"
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
        "lake — grants", object.object_type + ":" + object.external_id, content, viewer);
}

std::string web_ui_effective_access(const control::PolicyObject& object,
                                    const std::vector<control::EffectiveAccessEntry>& entries,
                                    WebUiViewer viewer) {
    std::string content = "<p class=\"eyebrow\">sharing</p><h1>Effective Access</h1>"
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
        "lake — effective access", object.object_type + ":" + object.external_id, content, viewer);
}

std::string
web_ui_search(const search::Query& query, const search::QueryResult& result, WebUiViewer viewer) {
    std::string content =
        "<p class=\"eyebrow\">search</p><h1>Search</h1>"
        "<p class=\"sub\">Ranked text, quoted substring, or /regex/ search "
        "across repositories you may read. A tag uses its immutable snapshot; "
        "a change or complete history uses scan mode. Append "
        "<code>in:artifact:&lt;name&gt;@&lt;version&gt;</code> to search a published "
        "artifact.</p>"
        "<form class=\"filters\" method=\"get\" action=\"/search\">"
        "<label>Query<input name=\"query\" value=\"" +
        web_ui_escape(query.text) +
        "\" placeholder=\"Search repositories\" required></label>"
        "<label>Customer<input name=\"customer\" value=\"" +
        web_ui_escape(query.customer) +
        "\"></label><label>Repository<input name=\"repository\" value=\"" +
        web_ui_escape(query.repository) +
        "\"></label><label>Branch or tag<input name=\"ref\" value=\"" + web_ui_escape(query.ref) +
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
        content += "<table><thead><tr><th>Repository</th><th>Artifact</th><th>Path</th>"
                   "<th>Line</th><th>Change</th><th>Snippet</th></tr></thead><tbody>";
        for (const search::Hit& hit : result.items) {
            content += std::format(
                "<tr><td>{}/{}</td><td><code>{}</code></td><td><code>{}</code></td>"
                "<td>{}</td><td><code>{}</code></td><td>{}</td></tr>",
                web_ui_escape(hit.customer),
                web_ui_escape(hit.repository),
                web_ui_escape(hit.artifact.empty() ? "—"
                                                   : hit.artifact + "@" + hit.artifact_version),
                web_ui_escape(hit.path),
                hit.line == 0 ? "—" : std::to_string(hit.line),
                web_ui_escape(hit.change),
                web_ui_escape(hit.snippet));
        }
        content += "</tbody></table>";
    }
    return web_ui_shell(
        "lake — search", query.text.empty() ? "search" : query.text, content, viewer);
}

std::string web_ui_metering(const nlohmann::json& usage, WebUiViewer viewer) {
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
        return web_ui_shell("lake — metering", "metering", content, viewer);
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
    content += std::format(
        "<div class=\"panel\"><h2>Storage Quota</h2><p class=\"sub\">This value is used by "
        "storage enforcement.</p><form class=\"stack\" method=\"post\" action=\"/metering/quota\">"
        "<input type=\"hidden\" name=\"customer_id\" value=\"{}\"><label>Bytes<input "
        "type=\"number\" name=\"quota_bytes\" min=\"0\" value=\"{}\" required></label>"
        "<button class=\"primary\" type=\"submit\">Set quota</button></form></div>",
        web_ui_escape(customer_id),
        usage.value("quota_bytes", std::uint64_t{0}));
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
    return web_ui_shell("lake — metering", customer_id, content, viewer);
}

std::string web_ui_audit(const std::vector<audit::Event>& events,
                         const audit::Filter& filter,
                         const nlohmann::json& log_configuration,
                         WebUiViewer viewer) {
    // /activity is the same view limited to the signed-in person's own events (my_audit).
    const bool own = viewer.path == "/activity";
    std::string content =
        std::string(own ? "<p class=\"eyebrow\">Organization</p><h1>Your Activity</h1><p "
                          "class=\"sub\">Signed, hash-linked events for everything you did on "
                          "this lake.</p><form class=\"filters\" method=\"get\" "
                          "action=\"/activity\">"
                        : "<p class=\"eyebrow\">control plane</p><h1>Audit Log</h1><p "
                          "class=\"sub\">Signed, hash-linked events. Use the filters to narrow "
                          "the evidence view.</p><form class=\"filters\" method=\"get\" "
                          "action=\"/audit\">") +
        (own ? std::string()
             : "<label>Principal<input name=\"principal\" value=\"" +
                   web_ui_escape(filter.principal) + "\"></label>") +
        "<label>Object<input name=\"object\" value=\"" + web_ui_escape(filter.object) +
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
            "<div class=\"panel\"><h2>Transparency Log</h2><p class=\"sub\">Effective mode: "
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
                                   "<td>{}</td><td>{}</td>"
                                   "<td><code>{}</code></td></tr>",
                                   web_ui_escape(event.occurred_at),
                                   web_ui_escape(event.draft.principal),
                                   web_ui_escape(event.draft.object),
                                   web_ui_escape(event.draft.action),
                                   badge_markup(event.draft.outcome),
                                   web_ui_escape(event.digest));
        }
        content += "</tbody></table>";
    }
    content += "<p class=\"sub\">API: <code>GET /v1/audit</code>, <code>GET "
               "/v1/export_audit</code>, "
               "and <code>GET /v1/verify_audit</code>.</p>";
    return web_ui_shell(own ? "lake — activity" : "lake — audit",
                        std::format("{} events", events.size()),
                        content,
                        viewer);
}

std::string
web_ui_transparency(std::string_view identity, const nlohmann::json& scan, WebUiViewer viewer) {
    std::string content =
        "<p class=\"eyebrow\">public evidence</p><h1>Transparency Monitor</h1>"
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
    return web_ui_shell("lake — transparency", "public monitor", content, viewer);
}

std::string web_ui_secret_mounts(const std::vector<control::SecretMount>& mounts,
                                 WebUiViewer viewer) {
    std::string content =
        "<p class=\"eyebrow\">control plane</p><h1>Secret Mounts</h1>"
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
        "lake — secret mounts", std::format("{} mounts", mounts.size()), content, viewer);
}

std::string web_ui_secrets(const control::SecretMount& mount,
                           std::string_view name,
                           const control::SecretMetadata* metadata,
                           WebUiViewer viewer) {
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
        content += "<h2>Version History</h2><table><thead><tr><th>Version</th><th>Created</th>"
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
    return web_ui_shell("lake — secrets", mount.name, content, viewer);
}

std::string web_ui_encryption_keys(const control::SecretMount& mount,
                                   const std::vector<control::EncryptionKey>& keys,
                                   WebUiViewer viewer) {
    std::string content = "<p class=\"eyebrow\">encryption service</p><h1>" +
                          web_ui_escape(mount.name) +
                          " Keys</h1><p class=\"sub\">Named, versioned keys for encryption, "
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
        "lake — encryption keys", std::format("{} keys", keys.size()), content, viewer);
}

std::string web_ui_sign_up(std::string_view error, WebUiViewer viewer) {
    std::string content =
        "<p class=\"eyebrow\">identity</p><h1>Create an Account</h1>"
        "<p class=\"sub\">Get a personal customer you own, then create your first "
        "repository.</p>";
    content += error_markup(error);
    content += R"html(<section class="panel"><h2>Get Your Public Identity Document</h2>
<ol><li><a href="https://github.com/oramasearch/liquid/blob/main/docs/build/local.md">Install liquid</a> using the supported build instructions. You need access to the liquid repository.
<details><summary>Installation commands</summary><pre><code>git clone --recurse-submodules git@github.com:oramasearch/liquid.git
cd liquid
cmake -S . -B build -G Ninja -DLIQUID_BUILD_TESTS=OFF
cmake --build build -j 6
cmake --install build --prefix "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"</code></pre></details></li>
<li>Run <code>liquid identity show --document</code>. Liquid creates your identity on first use, or exports your existing identity's public document.
<a href="https://github.com/oramasearch/liquid/blob/main/docs/reference/cli/identity.md">Identity command reference</a>.</li>
<li>Copy the complete output into the identity field below, without quotes. It is one base64url string containing the public TAAP/1 document, not JSON.</li></ol>
<p>Your private signing key stays on your machine. Keep it safe; never paste it here.</p></section>)html";
    content +=
        R"html(<form class="stack" method="post" action="/identity/sign-up">
<label>Email<input type="email" name="email" autocomplete="email" required></label>
<label>Display name<input type="text" name="display_name" autocomplete="name"></label>
<label>Password<input type="password" name="password" autocomplete="new-password" minlength="12" required></label>
<label>Device name<input type="text" name="device_name" value="browser" required></label>
<label>TAAP/1 identity document<textarea name="identity_document" spellcheck="false" required></textarea></label>
<button class="primary" type="submit">Create account</button></form>
<p class="muted">Already registered? <a href="/sign-in">Sign in</a>.</p>)html";
    return web_ui_shell("lake — sign up", "identity", content, viewer);
}

std::string web_ui_sign_in(std::string_view error, WebUiViewer viewer) {
    std::string content = "<p class=\"eyebrow\">identity</p><h1>Sign In</h1>"
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
    return web_ui_shell("lake — sign in", "identity", content, viewer);
}

std::string web_ui_mfa(std::string_view error,
                       std::string_view otpauth_uri,
                       const std::vector<std::string>& recovery_codes,
                       WebUiViewer viewer) {
    std::string content = "<p class=\"eyebrow\">identity</p><h1>Multi-Factor Authentication</h1>";
    content += error_markup(error);
    if (viewer.signed_in) {
        content += "<p class=\"sub\">Enroll or replace a TOTP authenticator for password "
                   "sign-in. A new enrollment replaces existing recovery codes.</p>";
        if (!otpauth_uri.empty()) {
            content += std::format(
                "<p class=\"notice\">Save this authenticator URI now: <code>{}</code></p>",
                web_ui_escape(otpauth_uri));
            content += "<h2>Recovery Codes</h2><p>Save these now. Each code works once.</p><ul>";
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
    return web_ui_shell("lake — MFA", "identity", content, viewer);
}

std::string web_ui_passkeys(std::string_view error, WebUiViewer viewer) {
    std::string content = "<p class=\"eyebrow\">identity</p><h1>Passkeys</h1>"
                          "<p class=\"sub\">Register an attestation-less passkey "
                          "for this account.</p>";
    content += error_markup(error);
    content +=
        R"html(<div class="panel"><label>Passkey name <input id="passkey-name" type="text" value="passkey"></label>
<div class="actions"><button class="primary" id="register-passkey" type="button">Register passkey</button><span id="passkey-status" class="muted"></span></div></div>
<script>(()=>{const b=s=>Uint8Array.from(atob(s.replace(/-/g,'+').replace(/_/g,'/')+'==='.slice((s.length+3)%4)),c=>c.charCodeAt(0));const e=a=>btoa(String.fromCharCode(...new Uint8Array(a))).replace(/\+/g,'-').replace(/\//g,'_').replace(/=+$/,'');const q=(u,o)=>fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)}).then(r=>r.json());document.getElementById('register-passkey').onclick=async()=>{const s=document.getElementById('passkey-status');try{const a=await q('/v1/begin_passkey_registration',{});if(!a.ok)throw Error(a.error.message);const o=a.value.public_key,c=o.challenge;o.challenge=b(o.challenge);o.user.id=b(o.user.id);const k=await navigator.credentials.create({publicKey:o});const z=await q('/v1/register_passkey',{challenge:c,client_data_json:e(k.response.clientDataJSON),attestation_object:e(k.response.attestationObject),name:document.getElementById('passkey-name').value});if(!z.ok)throw Error(z.error.message);s.textContent='Passkey registered.'}catch(x){s.textContent=x.message}}})();</script>)html";
    return web_ui_shell("lake — passkeys", "identity", content, viewer);
}

std::string web_ui_passkey_sign_in(std::string_view error, WebUiViewer viewer) {
    std::string content = "<p class=\"eyebrow\">identity</p><h1>Sign In with a Passkey</h1>";
    content += error_markup(error);
    content +=
        R"html(<div class="panel"><label>Device name <input id="device-name" type="text" value="browser"></label>
<div class="actions"><button class="primary" id="use-passkey" type="button">Use passkey</button><span id="passkey-status" class="muted"></span></div></div>
<script>(()=>{const b=s=>Uint8Array.from(atob(s.replace(/-/g,'+').replace(/_/g,'/')+'==='.slice((s.length+3)%4)),c=>c.charCodeAt(0));const e=a=>btoa(String.fromCharCode(...new Uint8Array(a))).replace(/\+/g,'-').replace(/\//g,'_').replace(/=+$/,'');const q=(u,o)=>fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)}).then(r=>r.json());document.getElementById('use-passkey').onclick=async()=>{const s=document.getElementById('passkey-status');try{const a=await q('/v1/begin_passkey_sign_in',{});if(!a.ok)throw Error(a.error.message);const o=a.value.public_key,c=o.challenge;o.challenge=b(o.challenge);const k=await navigator.credentials.get({publicKey:o});const z=await q('/v1/sign_in_passkey',{challenge:c,credential_id:e(k.rawId),client_data_json:e(k.response.clientDataJSON),authenticator_data:e(k.response.authenticatorData),signature:e(k.response.signature),device_name:document.getElementById('device-name').value});if(!z.ok)throw Error(z.error.message);location='/sessions'}catch(x){s.textContent=x.message}}})();</script>)html";
    return web_ui_shell("lake — passkey sign in", "identity", content, viewer);
}

std::string web_ui_application_sign_up(const control::Application& application,
                                       std::string_view error) {
    const std::string base = "/apps/" + web_ui_escape(application.id);
    std::string content = "<p class=\"eyebrow\">secure sign-up</p><h1>Create Your Account</h1>";
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
    std::string content = "<p class=\"eyebrow\">secure sign-in</p><h1>Sign In</h1>";
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
    std::string content = "<p class=\"eyebrow\">secure sign-in</p><h1>Use a Passkey</h1>";
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
                                std::string_view error,
                                WebUiViewer viewer) {
    std::string content = "<p class=\"eyebrow\">auth as a service</p><h1>Applications</h1>";
    content += error_markup(error);
    content += "<div class=\"grid\"><div class=\"table-wrap\"><table><thead><tr><th>Name</th><th>"
               "Pool</th><th>Domain</th>"
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
    content += "</tbody></table></div><div class=\"panel\"><h2>Register Application</h2><form "
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
    return web_ui_shell("lake — applications",
                        std::format("{} applications", applications.size()),
                        content,
                        viewer);
}

std::string web_ui_application(const control::Application& application, WebUiViewer viewer) {
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
    return web_ui_shell(
        "lake — application: " + application.name, application.pool_id, content, viewer);
}

std::string web_ui_application_users(const control::Application& application,
                                     const std::vector<control::ApplicationUser>& users,
                                     WebUiViewer viewer) {
    std::string content = "<p class=\"eyebrow\">application users</p><h1>" +
                          web_ui_escape(application.name) +
                          " Users</h1><div class=\"table-wrap\"><table><thead><tr><th>Email</th>"
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
    content += "</tbody></table></div>";
    return web_ui_shell(
        "lake — application users", std::format("{} users", users.size()), content, viewer);
}

std::string
web_ui_application_organizations(const control::Application& application,
                                 const std::vector<control::ApplicationOrganization>& organizations,
                                 WebUiViewer viewer) {
    std::string content =
        "<p class=\"eyebrow\">application organizations</p><h1>" + web_ui_escape(application.name) +
        " Organizations</h1><div class=\"table-wrap\"><table><thead><tr><th>Name</th><th>TAAP "
        "identity</th><th>Updated</th><th></th></tr></thead><tbody>";
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
    content += "</tbody></table></div><div class=\"panel\"><h2>Create Organization</h2><form "
               "class=\"stack\" "
               "method=\"post\" action=\"/application/organizations\"><input type=\"hidden\" "
               "name=\"application_id\" value=\"" +
               web_ui_escape(application.id) +
               "\"><label>Name<input name=\"name\" required></label><label>TAAP identity<input "
               "name=\"taap_id\" required></label><button class=\"primary\" type=\"submit\">Create"
               "</button></form></div>";
    return web_ui_shell("lake — application organizations",
                        std::format("{} organizations", organizations.size()),
                        content,
                        viewer);
}

std::string web_ui_application_events(const control::Application& application,
                                      const std::vector<control::ApplicationWebhookEvent>& events,
                                      std::string_view webhook_secret,
                                      WebUiViewer viewer) {
    std::string content = "<p class=\"eyebrow\">application webhooks</p><h1>" +
                          web_ui_escape(application.name) + " Event Log</h1>";
    if (!webhook_secret.empty()) {
        content += "<p class=\"notice\">Save this webhook secret now: <code>" +
                   web_ui_escape(webhook_secret) + "</code></p>";
    }
    content += "<div class=\"table-wrap\"><table><thead><tr><th>Event</th><th>ID</th><th>Created"
               "</th></tr></thead><tbody>";
    for (const auto& event : events) {
        content += "<tr><td>" + web_ui_escape(event.event) + "</td><td><code>" +
                   web_ui_escape(event.id) + "</code></td><td>" + web_ui_escape(event.created_at) +
                   "</td></tr>";
    }
    content +=
        "</tbody></table></div><div class=\"panel\"><h2>Create Webhook</h2><form "
        "class=\"stack\" "
        "method=\"post\" action=\"/application/webhooks\"><input type=\"hidden\" "
        "name=\"application_id\" value=\"" +
        web_ui_escape(application.id) +
        "\"><label>HTTPS URL<input type=\"url\" name=\"url\" required></label><label>Events "
        "(comma-separated)<input name=\"events\" value=\"user.created,user.updated,user.deleted,"
        "organization.created,organization.updated,organization.deleted,session.started,mfa."
        "changed\" "
        "required></label><button class=\"primary\" type=\"submit\">Create</button></form></div>";
    return web_ui_shell(
        "lake — application events", std::format("{} events", events.size()), content, viewer);
}

std::string web_ui_sessions(const std::vector<control::Session>& sessions,
                            const std::vector<control::Device>& devices,
                            std::string_view error,
                            WebUiViewer viewer) {
    std::string content = "<p class=\"eyebrow\">identity</p><h1>Sessions and Devices</h1>"
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
    return web_ui_shell("lake — sessions", "identity", content, viewer);
}

std::string web_ui_tokens(const std::vector<control::Session>& sessions,
                          std::string_view issued_secret,
                          std::string_view error,
                          WebUiViewer viewer) {
    std::string content = "<p class=\"eyebrow\">identity</p><h1>API Tokens</h1>"
                          "<p class=\"sub\">Create named, scoped sessions and "
                          "revoke them here.</p>"
                          "<p>For Liquid push and pull, use these capability scopes: "
                          "<code>refs,find_missing,push,pull,upload_chunks,read_chunk</code>. "
                          "The token keeps your existing repository permissions.</p>";
    content += error_markup(error);
    if (!issued_secret.empty())
        content += std::format("<p class=\"notice\">Copy this token now: <code>{}</code></p>",
                               web_ui_escape(issued_secret));
    content +=
        R"html(<div class="panel"><form class="stack" method="post" action="/identity/token/create">
<label>Name<input type="text" name="name" required></label><label>Capability scopes, comma separated<input type="text" name="scopes" required></label>
<label>Expiry in seconds (leave empty for as long as this session allows, at most 30 days)<input type="number" name="expires_in_seconds" min="1"></label>
<label><span><input type="checkbox" name="static" value="true"> Static exception</span></label><label><span><input type="checkbox" name="never_expires" value="true"> Never expires (survives sign-out; needs your space's opt-in, a reason and named scopes)</span></label><label>Reason for a static or never-expiring token<input type="text" name="reason"></label>
<button class="primary" type="submit">Create token</button></form></div><h2>Existing Tokens</h2>
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
        content += std::format(
            "<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td><form "
            "class=\"inline\" method=\"post\" "
            "action=\"/identity/session/revoke\"><input type=\"hidden\" "
            "name=\"session_id\" value=\"{}\"><input type=\"hidden\" "
            "name=\"return_to\" value=\"/tokens\"><button "
            "type=\"submit\">Revoke</button></form></td></tr>",
            web_ui_escape(session.name),
            web_ui_escape(session.kind),
            web_ui_escape(scopes),
            session.expires_at.empty()
                ? "Never expires" + (session.reason.empty()
                                         ? std::string()
                                         : " <small>" + web_ui_escape(session.reason) + "</small>")
                : web_ui_escape(session.expires_at),
            state_badge(session.revoked_at.empty()),
            web_ui_escape(session.id));
    }
    if (count == 0)
        content += "<tr><td colspan=\"6\" class=\"empty\">No API tokens.</td></tr>";
    content += "</tbody></table>";
    return web_ui_shell("lake — tokens", "identity", content, viewer);
}

std::string web_ui_exports(const control::Repository& repository,
                           const std::vector<control::ForgeRecord>& exports,
                           WebUiViewer viewer) {
    std::string content = std::format(
        "<p class=\"eyebrow\">portability</p><h1>Repository Export</h1><p class=\"sub\">"
        "Download a signed archive for <code>{}</code>.</p><div class=\"panel\"><form "
        "class=\"stack\" method=\"get\" action=\"/exports/download\"><input type=\"hidden\" "
        "name=\"repository_id\" value=\"{}\"><label><span><input type=\"checkbox\" "
        "name=\"include_git\" value=\"true\"> Include Git projection</span></label><button "
        "class=\"primary\" type=\"submit\">Request and download export</button></form></div>",
        web_ui_escape(repository_name(repository)),
        web_ui_escape(repository.id));
    content += "<h2>Prior Exports</h2><table><thead><tr><th>Created</th><th>Git</th>"
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
    return repository_shell("lake — exports", repository, "Exports", content, viewer);
}

std::string web_ui_drive_folder(const control::Repository& repository,
                                std::string_view ref,
                                std::string_view path,
                                const std::vector<RepositoryTreeEntry>& entries,
                                const DriveFolderOptions& options,
                                WebUiViewer viewer) {
    const std::string query = drive_query(repository, ref, path);
    const std::string base = "/drive?repository_id=" + url_component(repository.id) +
                             "&ref=" + url_component(ref) + "&path=";
    std::string content = "<p class=\"eyebrow\">drive</p>";
    content += drive_breadcrumbs(repository, ref, path);
    content +=
        "<h1>" + web_ui_escape(path.empty() ? repository_name(repository) : path_name(path)) +
        "</h1><p class=\"sub\">Folder <code>" + web_ui_escape(path.empty() ? "/" : path) +
        "</code> at <code>" + web_ui_escape(ref) +
        "</code>. Moves and deletes create new history; prior versions remain available.</p>";
    content += "<div class=\"actions\">";
    content += row_link("/drive/history?" + query, "History");
    content += row_link("/drive/share?" + query, "Share");
    content += row_link("/conflicts?repository_id=" + url_component(repository.id), "Conflicts");
    content += "</div>";
    content +=
        "<form class=\"filters\" method=\"get\" action=\"/drive\" aria-label=\"Folder display\">";
    content += "<input type=\"hidden\" name=\"repository_id\" value=\"" +
               web_ui_escape(repository.id) + "\"><input type=\"hidden\" name=\"ref\" value=\"" +
               web_ui_escape(ref) + "\"><input type=\"hidden\" name=\"path\" value=\"" +
               web_ui_escape(path) + "\">";
    content += "<label>View<select name=\"view\"><option value=\"list\"";
    content += options.view == "list" ? " selected" : "";
    content += ">List</option><option value=\"grid\"";
    content += options.view == "grid" ? " selected" : "";
    content += ">Grid</option></select></label><label>Sort<select name=\"sort\">";
    for (const std::string_view value : {"name", "size", "modified"}) {
        content += "<option value=\"" + std::string(value) + "\"";
        content += options.sort == value ? " selected" : "";
        content += ">" + web_ui_escape(value) + "</option>";
    }
    content += "</select></label><label>Direction<select name=\"direction\">";
    content += options.descending ? "<option value=\"asc\">Ascending</option><option "
                                    "value=\"desc\" selected>Descending</option>"
                                  : "<option value=\"asc\" selected>Ascending</option><option "
                                    "value=\"desc\">Descending</option>";
    content += "</select></label><button type=\"submit\">Apply</button></form>";
    if (viewer.signed_in) {
        content += "<div class=\"drive-create grid\"><section class=\"panel\"><h2>New Folder</h2>"
                   "<form class=\"stack\" method=\"post\" action=\"/drive/folder\">";
        content += drive_hidden_context(repository, ref, path);
        content +=
            "<label>Name<input name=\"name\" required></label><button type=\"submit\">Create folder"
            "</button></form></section><section class=\"panel\"><h2>Upload</h2><form "
            "id=\"drive-upload\" class=\"stack\" method=\"post\" action=\"/drive/upload\" "
            "enctype=\"multipart/form-data\">";
        content += drive_hidden_context(repository, ref, path);
        content +=
            "<label id=\"drive-drop-zone\" class=\"drive-drop-zone\">Choose files or drop them here"
            "<input id=\"drive-files\" type=\"file\" name=\"files\" multiple required></label>"
            "<button type=\"submit\">Upload files</button></form></section></div>";
    }
    if (options.view == "grid") {
        content += "<div class=\"drive-grid\">";
    } else {
        content += "<div class=\"table-wrap\"><table><thead><tr><th>Name</th><th>Size</th>"
                   "<th>Modified</th><th>Actions</th></tr></thead><tbody>";
    }
    for (const RepositoryTreeEntry& entry : entries) {
        const std::string name = path_name(entry.path);
        const std::string href =
            entry.kind == "tree"
                ? base + url_component(entry.path)
                : "/drive/preview?repository_id=" + url_component(repository.id) +
                      "&ref=" + url_component(ref) + "&path=" + url_component(entry.path);
        const std::string size = entry.kind == "tree" ? "—" : display_size(entry.size);
        const std::string modified = entry.last_changed_at.empty() ? "—" : entry.last_changed_at;
        if (options.view == "grid") {
            content += "<article class=\"card drive-card\"><p>" + badge_markup(entry.kind) +
                       "</p><h2>" + row_link(href, name) + "</h2><p class=\"muted\">" +
                       web_ui_escape(size) + " · " + web_ui_escape(modified) + "</p>" +
                       drive_entry_actions(repository, ref, entry, viewer.signed_in) + "</article>";
        } else {
            content += "<tr><td>" + row_link(href, name) + " " + badge_markup(entry.kind) +
                       "</td><td>" + web_ui_escape(size) + "</td><td>" + web_ui_escape(modified) +
                       "</td><td>" + drive_entry_actions(repository, ref, entry, viewer.signed_in) +
                       "</td></tr>";
        }
    }
    if (entries.empty()) {
        content += options.view == "grid"
                       ? "<div class=\"panel empty\">This folder is empty.</div>"
                       : "<tr><td colspan=\"4\" class=\"empty\">This folder is empty.</td></tr>";
    }
    content += options.view == "grid" ? "</div>" : "</tbody></table></div>";
    if (viewer.signed_in) {
        content += R"html(<script id="drive-drop-island">
(() => {
  const zone = document.getElementById('drive-drop-zone');
  const input = document.getElementById('drive-files');
  const form = document.getElementById('drive-upload');
  if (!zone || !input || !form) {
    return;
  }
  for (const name of ['dragenter', 'dragover']) {
    zone.addEventListener(name, event => {
      event.preventDefault();
      zone.dataset.active = 'true';
    });
  }
  for (const name of ['dragleave', 'drop']) {
    zone.addEventListener(name, event => {
      event.preventDefault();
      delete zone.dataset.active;
    });
  }
  zone.addEventListener('drop', event => {
    if (!event.dataTransfer.files.length) {
      return;
    }
    input.files = event.dataTransfer.files;
    form.requestSubmit();
  });
})();
</script>)html";
    }
    return repository_shell("lake — drive", repository, "Files", content, viewer);
}

std::string web_ui_drive_preview(const control::Repository& repository,
                                 std::string_view ref,
                                 const RepositoryFile& file,
                                 const DrivePreview& preview,
                                 WebUiViewer viewer) {
    const std::string parent = parent_path(file.path);
    const std::string download = "/drive/download?" + drive_query(repository, ref, file.path);
    const std::string content =
        drive_breadcrumbs(repository, ref, file.path) +
        std::format(
            "<p class=\"eyebrow\">drive preview</p><h1>{}</h1><p class=\"sub\"><span "
            "class=\"badge\">{}</span> {} · digest <code>{}</code></p><div class=\"actions\"><a "
            "href=\"/drive?repository_id={}&ref={}&path={}\">Back to folder</a><a href=\"{}\" "
            "download>Download</a><a href=\"/drive/history?{}\">History</a></div><div "
            "class=\"panel\">"
            "{}</div>",
            web_ui_escape(file.path),
            web_ui_escape(preview.format),
            web_ui_escape(display_size(file.size)),
            web_ui_escape(file.blob),
            url_component(repository.id),
            url_component(ref),
            url_component(parent),
            web_ui_escape(download),
            web_ui_escape(drive_query(repository, ref, file.path)),
            preview.html);
    return repository_shell("lake — preview", repository, "Files", content, viewer);
}

std::string web_ui_drive_history(const control::Repository& repository,
                                 std::string_view ref,
                                 std::string_view path,
                                 const std::vector<nlohmann::json>& changes,
                                 WebUiViewer viewer) {
    std::string content = drive_breadcrumbs(repository, ref, parent_path(path));
    content += std::format(
        "<p class=\"eyebrow\">drive history</p><h1>{}</h1><p class=\"sub\">Every version is "
        "a history query. Restoring writes a new change and does not rewrite prior versions.</p>",
        web_ui_escape(path));
    content += "<div class=\"actions\">";
    content +=
        row_link("/drive?" + drive_query(repository, ref, parent_path(path)), "Back to folder");
    content += "</div><form method=\"get\" action=\"/drive/history/diff\">";
    content += drive_hidden_context(repository, ref, path);
    content += "<div class=\"table-wrap\"><table><thead><tr><th>When</th><th>Change</th>"
               "<th>What changed</th><th>Compare from</th><th>Compare to</th><th>Restore</th>"
               "</tr></thead><tbody>";
    std::size_t version = 0;
    for (const nlohmann::json& change : changes) {
        std::vector<std::string> kinds = change.value("changes", std::vector<std::string>{});
        const std::vector<std::string> conflicts =
            change.value("new_conflicts", std::vector<std::string>{});
        const std::string change_id = change.value("id", "");
        const bool has_file = !change.value("new_blob", "").empty();
        content += "<tr><td>" + web_ui_escape(change.value("occurred_at", "")) + "</td><td><code>" +
                   web_ui_escape(change_id) + "</code><br>" +
                   web_ui_escape(change.value("message", "")) + "</td><td>" +
                   web_ui_escape(joined(kinds));
        if (!conflicts.empty()) {
            content += "<br><span class=\"muted\">Conflict records:</span><ul>";
            for (const std::string& conflict : conflicts) {
                content += "<li><code>" + web_ui_escape(conflict) + "</code></li>";
            }
            content += "</ul>";
        }
        content += "</td>";
        if (has_file) {
            content += "<td><input aria-label=\"Compare from " + web_ui_escape(change_id) +
                       "\" type=\"radio\" name=\"left\" value=\"" + web_ui_escape(change_id) + "\"";
            content += version == 1 ? " checked" : "";
            content += "></td><td><input aria-label=\"Compare to " + web_ui_escape(change_id) +
                       "\" type=\"radio\" name=\"right\" value=\"" + web_ui_escape(change_id) +
                       "\"";
            content += version == 0 ? " checked" : "";
            content += "></td><td>";
            if (viewer.signed_in) {
                content += "<button type=\"submit\" form=\"restore-" + std::to_string(version) +
                           "\">Restore</button>";
            } else {
                content += "—";
            }
            content += "</td>";
            ++version;
        } else {
            content += "<td>—</td><td>—</td><td>Deleted</td>";
        }
        content += "</tr>";
    }
    if (changes.empty()) {
        content += "<tr><td colspan=\"6\" class=\"empty\">No changes touch this path.</td></tr>";
    }
    content += "</tbody></table></div>";
    if (version >= 2) {
        content += "<p><button type=\"submit\">Compare selected text versions</button></p>";
    }
    content += "</form>";
    if (viewer.signed_in) {
        version = 0;
        for (const nlohmann::json& change : changes) {
            if (change.value("new_blob", "").empty()) {
                continue;
            }
            content += "<form hidden id=\"restore-" + std::to_string(version) +
                       "\" method=\"post\" action=\"/drive/history/restore\">";
            content += drive_hidden_context(repository, ref, path);
            content += "<input type=\"hidden\" name=\"change_id\" value=\"" +
                       web_ui_escape(change.value("id", "")) + "\"></form>";
            ++version;
        }
    }
    return repository_shell("lake — drive history", repository, "File History", content, viewer);
}

std::string web_ui_drive_version_diff(const control::Repository& repository,
                                      std::string_view ref,
                                      std::string_view path,
                                      std::string_view left_revision,
                                      std::string_view right_revision,
                                      const DriveVersionDiff& difference,
                                      WebUiViewer viewer) {
    std::string content = "<p class=\"eyebrow\">drive version comparison</p><h1>" +
                          web_ui_escape(path) +
                          "</h1><p class=\"sub\">Two preserved text versions. "
                          "Neither version is changed by this comparison.</p>";
    content += "<div class=\"actions\">" +
               row_link("/drive/history?repository_id=" + url_component(repository.id) +
                            "&ref=" + url_component(ref) + "&path=" + url_component(path),
                        "Back to history") +
               "</div><div class=\"grid\"><section class=\"panel\"><h2>From</h2><code>" +
               web_ui_escape(left_revision) + "</code><p>" + display_size(difference.left.size) +
               " · <code>" + web_ui_escape(difference.left.blob) +
               "</code></p></section><section class=\"panel\"><h2>To</h2><code>" +
               web_ui_escape(right_revision) + "</code><p>" + display_size(difference.right.size) +
               " · <code>" + web_ui_escape(difference.right.blob) + "</code></p></section></div>";
    content += drive_diff_markup(difference);
    return repository_shell(
        "lake — drive version diff", repository, "Version Comparison", content, viewer);
}

std::string web_ui_drive_conflict(const control::Repository& repository,
                                  std::string_view ref,
                                  const RepositoryConflict& conflict,
                                  const std::vector<RepositoryFile>& versions,
                                  const std::optional<DriveVersionDiff>& difference,
                                  WebUiViewer viewer) {
    std::string content =
        "<p class=\"eyebrow\">drive conflict</p><h1>Two versions of " +
        web_ui_escape(conflict.region.path) + "</h1><p><strong>" +
        web_ui_escape(conflict_kind_label(conflict.kind)) + "</strong></p><p>" +
        web_ui_escape(conflict.explanation) +
        "</p><p class=\"sub\">Affected region: " + conflict_region_markup(conflict.region) +
        "</p>" + conflict_reasons_markup(conflict) +
        "<p class=\"sub\">Both versions are preserved. Compare them before "
        "choosing how to reconcile the file.</p><div class=\"grid\">";
    for (std::size_t index = 0; index < versions.size(); ++index) {
        content += "<section class=\"panel\"><h2>Version " + std::to_string(index + 1) +
                   "</h2><dl><dt>Digest</dt><dd><code>" + web_ui_escape(versions[index].blob) +
                   "</code></dd><dt>Size</dt><dd>" + display_size(versions[index].size) +
                   "</dd></dl></section>";
    }
    content += "</div><h2>What Differs</h2>";
    content += difference
                   ? drive_diff_markup(*difference)
                   : "<div class=\"panel\">A structural preview is not registered for this "
                     "format. The digests and sizes above identify both preserved values.</div>";
    content += conflict_values_markup(conflict);
    content += conflict_resolution_markup(
        repository, ref, conflict, "/drive/conflict/resolve", viewer.signed_in);
    return repository_shell("lake — drive conflict", repository, "Conflict", content, viewer);
}

std::string web_ui_drive_share(const control::Repository& repository,
                               std::string_view ref,
                               std::string_view path,
                               const std::vector<control::EffectiveAccessEntry>& access,
                               const std::vector<nlohmann::json>& links,
                               std::string_view created_link,
                               std::string_view error,
                               WebUiViewer viewer) {
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
    content += "<h2>Who Can Access This Folder Now</h2><p class=\"sub\">This is the live policy "
               "result. It names people and organization identities, their actual actions, and "
               "whether each access path is inherited.</p><table><thead><tr><th>Who</th><th>What "
               "they can do</th><th>How access arrives</th></tr></thead><tbody>";
    for (const control::EffectiveAccessEntry& entry : access) {
        content += std::format("<tr><td><code>{}</code></td><td>{}: {}</td><td>{}</td></tr>",
                               web_ui_escape(entry.principal_taap_id),
                               web_ui_escape(entry.role),
                               web_ui_escape(joined(entry.actions)),
                               entry.inherited ? "inherited" : "direct");
    }
    if (access.empty()) {
        content += "<tr><td colspan=\"3\" class=\"empty\">No grants.</td></tr>";
    }
    content += "</tbody></table><h2>Choose a Delivery</h2><div class=\"grid\"><section "
               "class=\"panel\"><h3>People</h3><p>A named person receives a live folder. "
               "Updates and moves continue to apply.</p><form class=\"stack\" method=\"post\" "
               "action=\"/drive/share/direct\">";
    const std::string hidden = drive_hidden_context(repository, ref, path);
    content += hidden;
    content +=
        "<label>Person's TAAP identity<input name=\"principal_taap_id\" required></label>"
        "<label>What they can do<select name=\"role\"><option value=\"read\">View — "
        "browse and download</option><option value=\"write\">Edit — upload, rename, move, "
        "and delete</option><option value=\"share\">Share — browse and create shares</option>"
        "</select></label><label>Attestation digest<input name=\"attestation_digest\" "
        "required></label><button class=\"primary\" type=\"submit\">Share with person"
        "</button></form></section><section class=\"panel\"><h3>Organization</h3><p>A named "
        "organization identity receives the same live folder. People appear only when their "
        "identity has effective access above.</p><form class=\"stack\" method=\"post\" "
        "action=\"/drive/share/direct\">" +
        hidden +
        "<label>Organization TAAP identity<input name=\"principal_taap_id\" required></label>"
        "<label>What it can do<select name=\"role\"><option value=\"read\">View — browse "
        "and download</option><option value=\"write\">Edit — upload, rename, move, and "
        "delete</option><option value=\"share\">Share — browse and create shares</option>"
        "</select></label><label>Attestation digest<input name=\"attestation_digest\" "
        "required></label><button class=\"primary\" type=\"submit\">Share with organization"
        "</button></form></section><section class=\"panel\"><h3>Link</h3><p>Anyone holding "
        "the link can open this live folder until its expiry or revocation.</p><form "
        "class=\"stack\" "
        "method=\"post\" action=\"/drive/share/link\">" +
        hidden +
        "<label>What link holders can do<select name=\"role\"><option value=\"read\">View — "
        "browse and download</option><option value=\"write\">Write scope — capability-aware "
        "clients only</option></select></label><label>Until when (UTC)<input name=\"expires_at\" "
        "placeholder=\"2030-01-01T00:00:00Z\" required></label><label>Passcode<input disabled "
        "placeholder=\"Not available for drive links\"></label><p class=\"muted\">Passcodes are "
        "not supported for drive-share links. No passcode is sent or stored.</p>"
        "<button class=\"primary\" type=\"submit\">Create link</button></form></section>"
        "<section class=\"panel\"><h3>Independent Copy</h3><p>Downloaded bytes stop being a "
        "live object. They cannot be updated or recalled by changing access "
        "here.</p></section></div>";
    content +=
        "<h2>Folder Share Management</h2><p class=\"sub\">A link is a continuing capability, "
        "not an attachment. Revoking it prevents its next open; copies already downloaded remain "
        "independent.</p><table><thead><tr><th>Who can access</th><th>What they can "
        "do</th><th>Until "
        "when</th><th>State</th><th></th></tr></thead><tbody>";
    for (const nlohmann::json& link : links) {
        const std::string link_id = link.value("id", "");
        content += std::format(
            "<tr><td>Anyone with this link</td><td>{}</td><td>{}</td><td>{}</td><td><a "
            "href=\"{}\">Details"
            "</a> <form class=\"inline\" method=\"post\" "
            "action=\"/drive/share/revoke\"><input type=\"hidden\" name=\"link_id\" "
            "value=\"{}\"><input type=\"hidden\" name=\"repository_id\" value=\"{}\"><input "
            "type=\"hidden\" name=\"ref\" value=\"{}\"><input type=\"hidden\" name=\"path\" "
            "value=\"{}\"><button type=\"submit\">Revoke link</button></form></td></tr>",
            web_ui_escape(share_link_actions(link.value("role", ""))),
            web_ui_escape(link.value("expires_at", "")),
            web_ui_escape(share_link_state(link)),
            web_ui_escape(drive_share_link_url(repository, ref, path, link_id)),
            web_ui_escape(link_id),
            web_ui_escape(repository.id),
            web_ui_escape(ref),
            web_ui_escape(path));
    }
    if (links.empty()) {
        content += "<tr><td colspan=\"4\" class=\"empty\">No links.</td></tr>";
    }
    content += "</tbody></table>";
    return repository_shell("lake — drive sharing", repository, "Sharing", content, viewer);
}

std::string web_ui_drive_share_link(const control::Repository& repository,
                                    std::string_view ref,
                                    std::string_view path,
                                    const nlohmann::json& link,
                                    std::string_view error,
                                    WebUiViewer viewer) {
    const std::string state = share_link_state(link);
    std::string content =
        "<p class=\"eyebrow\">drive share</p><h1>Link for " + web_ui_escape(path) +
        "</h1><p class=\"sub\">This page answers who can use "
        "the link, what it allows, when it stops, and what revocation changes.</p>" +
        error_markup(error);
    content +=
        "<dl class=\"details\"><dt>Who can access</dt><dd>Anyone holding this bearer link</dd>"
        "<dt>What they can do</dt><dd>" +
        web_ui_escape(share_link_actions(link.value("role", ""))) + "</dd><dt>Until when</dt><dd>" +
        web_ui_escape(link.value("expires_at", "")) + "</dd><dt>State</dt><dd>" +
        web_ui_escape(state) + "</dd></dl>";
    if (state == "active") {
        content +=
            "<section class=\"panel\"><h2>Revoke This Link</h2><p>Revoking prevents this link "
            "from opening the live folder on its next use. It does not recall files someone "
            "already "
            "downloaded.</p><form method=\"post\" action=\"/drive/share/revoke\">" +
            std::format("<input type=\"hidden\" name=\"link_id\" value=\"{}\">",
                        web_ui_escape(link.value("id", ""))) +
            drive_hidden_context(repository, ref, path) +
            "<input type=\"hidden\" name=\"return_to\" value=\"detail\"><button class=\"danger\" "
            "type=\"submit\">Revoke link</button></form></section>";
    } else {
        content += "<p class=\"notice\">This link no longer opens the folder.</p>";
    }
    content +=
        row_link("/drive/share?" + drive_query(repository, ref, path), "Back to folder shares");
    return repository_shell("lake — drive share link", repository, "Share Link", content, viewer);
}

std::string web_ui_shared_with_me(const std::vector<nlohmann::json>& folders,
                                  std::string_view error,
                                  WebUiViewer viewer) {
    std::string content =
        "<p class=\"eyebrow\">drive</p><h1>Shared with Me</h1><p class=\"sub\">Live "
        "folders shared directly with your identity. A move keeps the same folder identity; "
        "a downloaded copy remains independent.</p>" +
        error_markup(error);
    content +=
        "<table><thead><tr><th>Folder</th><th>Shared by</th><th>What you can do</th><th>State</th>"
        "</tr></thead><tbody>";
    for (const nlohmann::json& folder : folders) {
        const nlohmann::json& repository = folder.at("repository");
        const std::string repository_id = repository.value("id", "");
        const std::string ref = folder.value("ref", "main");
        const std::string path = folder.value("path", "");
        const std::string href = "/drive?repository_id=" + url_component(repository_id) +
                                 "&ref=" + url_component(ref) + "&path=" + url_component(path);
        const bool available = folder.value("available", false);
        const std::string label = repository.value("name", repository_id) + "/" + path;
        const std::string location = available ? row_link(href, label) : web_ui_escape(label);
        content +=
            std::format("<tr><td>{}</td><td><code>{}</code></td><td>{}</td><td>{}</td></tr>",
                        location,
                        web_ui_escape(folder.value("shared_by", "")),
                        web_ui_escape(joined(folder.value("actions", std::vector<std::string>{}))),
                        available ? "available" : "removed");
    }
    if (folders.empty()) {
        content +=
            "<tr><td colspan=\"4\" class=\"empty\">No folders are shared directly with you.</td>"
            "</tr>";
    }
    content += "</tbody></table>";
    return web_ui_shell("lake — shared with me", "drive", content, viewer);
}

namespace {

std::string shared_drive_url(std::string_view token,
                             std::string_view ref,
                             std::string_view path,
                             std::string_view action = {}) {
    std::string url = "/share/" + url_component(token);
    if (!action.empty()) {
        url += "/" + std::string(action);
    }
    return url + "?ref=" + url_component(ref) + "&path=" + url_component(path);
}

} // namespace

std::string web_ui_drive_link(std::string_view path,
                              const std::vector<RepositoryTreeEntry>& entries,
                              std::string_view error,
                              std::string_view token,
                              std::string_view ref,
                              std::string_view root_path,
                              WebUiViewer viewer) {
    std::string content = "<p class=\"eyebrow\">shared drive folder</p><h1>" + web_ui_escape(path) +
                          "</h1>" + error_markup(error);
    if (!error.empty()) {
        return web_ui_shell("lake — shared drive folder", "share link", content, viewer);
    }
    content += "<p class=\"sub\">View – browse and download. Access ends when this link expires "
               "or is revoked. Downloaded copies are independent.</p>";
    if (path != root_path && !root_path.empty()) {
        content += "<div class=\"actions\">" +
                   row_link(shared_drive_url(token, ref, root_path), "Shared folder") +
                   row_link(shared_drive_url(token, ref, parent_path(path)), "Up one folder") +
                   "</div>";
    }
    content += "<div class=\"table-wrap\"><table><thead><tr><th>Name</th><th>Kind</th>"
               "<th>Actions</th></tr></thead><tbody>";
    for (const RepositoryTreeEntry& entry : entries) {
        const bool folder = entry.kind == "tree";
        const bool file = entry.kind == "blob";
        const std::string name = path_name(entry.path);
        const std::string open_url =
            shared_drive_url(token, ref, entry.path, folder ? "" : "preview");
        content += "<tr><td>" + (folder || file ? row_link(open_url, name) : web_ui_escape(name)) +
                   "</td><td>" + web_ui_escape(entry.kind) + "</td><td>";
        if (file) {
            content += "<a href=\"" +
                       web_ui_escape(shared_drive_url(token, ref, entry.path, "download")) +
                       "\" download>Download</a>";
        } else if (folder) {
            content += row_link(open_url, "Open folder");
        }
        content += "</td></tr>";
    }
    if (entries.empty()) {
        content += "<tr><td colspan=\"3\" class=\"empty\">This folder is empty.</td></tr>";
    }
    content += "</tbody></table></div>";
    return web_ui_shell("lake — shared drive folder", "share link", content, viewer);
}

std::string web_ui_drive_link_preview(std::string_view token,
                                      std::string_view ref,
                                      const RepositoryFile& file,
                                      const DrivePreview& preview,
                                      WebUiViewer viewer) {
    const std::string content =
        "<p class=\"eyebrow\">shared file preview</p><h1>" + web_ui_escape(path_name(file.path)) +
        "</h1><p class=\"sub\">" + web_ui_escape(preview.format) + " · " +
        web_ui_escape(display_size(file.size)) + "</p><div class=\"actions\">" +
        row_link(shared_drive_url(token, ref, parent_path(file.path)), "Back to folder") +
        "<a href=\"" + web_ui_escape(shared_drive_url(token, ref, file.path, "download")) +
        "\" download>Download</a></div><div class=\"panel\">" + preview.html + "</div>";
    return web_ui_shell("lake — shared file preview", "share link", content, viewer);
}

std::string web_ui_model(const nlohmann::json& model, WebUiViewer viewer) {
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
    content += "</dl></section><section class=\"panel\"><h2>Training Provenance</h2>";
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
    return web_ui_shell("lake — model", model.value("modelId", ""), content, viewer);
}

namespace {

// The MCP page's copy buttons (docs/api/mcp.md): the one script this page adds. Each button copies
// the text of the element its data-copy names.
constexpr std::string_view copy_script =
    R"(<script id="copy-buttons">(()=>{for(const b of document.querySelectorAll('[data-copy]')){b.addEventListener('click',async()=>{const e=document.getElementById(b.dataset.copy);if(!e)return;try{await navigator.clipboard.writeText(e.textContent);b.textContent='Copied';setTimeout(()=>{b.textContent='Copy';},2000);}catch{b.textContent='Select and copy';}});}})();</script>)";

std::string copy_block(std::string_view id, std::string_view heading, std::string_view text) {
    return std::format("<div class=\"copy-block\"><div class=\"copy-head\"><h3>{}</h3>"
                       "<button class=\"button\" type=\"button\" data-copy=\"{}\">Copy</button>"
                       "</div><pre><code id=\"{}\">{}</code></pre></div>",
                       web_ui_escape(heading),
                       id,
                       id,
                       web_ui_escape(text));
}

} // namespace

std::string web_ui_mcp(const McpPage& page, WebUiViewer viewer) {
    const std::string endpoint = page.origin + "/mcp";
    const std::string key = page.key.empty() ? "$LAKE_KEY" : page.key;
    std::string content =
        "<p class=\"eyebrow\">Developer</p><h1>MCP</h1><p class=\"sub\">Connect an agent to lake. "
        "Every capability on this site is an MCP tool at <code>" +
        web_ui_escape(endpoint) +
        "</code>, the same registry REST and OpenAPI serve, checked and audited as your own "
        "calls.</p>";
    if (!page.error.empty()) {
        content += "<div class=\"panel notice status-danger\" role=\"alert\"><p>" +
                   web_ui_escape(page.error) + "</p></div>";
    }
    if (!page.key.empty()) {
        content += "<section class=\"panel notice\" role=\"status\"><h2>Your MCP Key</h2><p>Shown "
                   "once. It is in every block below; copy what you need now. Revoke it on "
                   "<a href=\"/tokens\">Tokens</a>.</p>" +
                   copy_block("mcp-key", "Key", page.key) + "</section>";
    } else {
        content +=
            "<section class=\"panel\"><h2>Create an MCP Key</h2><p>A lake API token for your "
            "agent or script. It acts as you. By default it lasts as long as your sign-in, at most "
            "30 days; choose <em>Never</em> for a standing key, if your space allows keys that never "
            "expire. Revoke any key on <a href=\"/tokens\">Tokens</a>.</p>"
            "<form class=\"stack\" method=\"post\" action=\"/developer/mcp/key\">"
            "<fieldset><legend>Access</legend>"
            "<label><input type=\"radio\" name=\"access\" value=\"all\" checked> Every tool</label>"
            "<label><input type=\"radio\" name=\"access\" value=\"read\"> Read-only tools</label>"
            "</fieldset><label>Lifetime<select name=\"expires_in_seconds\">"
            "<option value=\"\">As long as my sign-in allows (at most 30 days)</option>"
            "<option value=\"86400\">1 day</option><option value=\"604800\">7 days</option>"
            "<option value=\"never\">Never (a standing key)</option>"
            "</select></label><label>Why this key exists<small>Needed for a key that never expires."
            "</small><input name=\"reason\" placeholder=\"the nightly sync from our data "
            "warehouse\"></label><button class=\"button button-primary\" type=\"submit\">Create an "
            "MCP key</button></form></section>";
    }
    const std::string claude_code =
        std::format("claude mcp add --transport http lake {} --header \"Authorization: Bearer {}\"",
                    endpoint,
                    key);
    const std::string cursor = std::format(
        "{{\n  \"mcpServers\": {{\n    \"lake\": {{\n      \"url\": \"{}\",\n      \"headers\": "
        "{{ \"Authorization\": \"Bearer {}\" }}\n    }}\n  }}\n}}",
        endpoint,
        key);
    const std::string desktop = std::format(
        "{{\n  \"mcpServers\": {{\n    \"lake\": {{\n      \"command\": \"npx\",\n      \"args\": "
        "[\"-y\", \"mcp-remote\", \"{}\", \"--header\", \"Authorization: Bearer {}\"]\n    }}\n  "
        "}}\n}}",
        endpoint,
        key);
    const std::string curl = std::format(
        "curl -s {0} -H \"Authorization: Bearer {1}\" -H 'Content-Type: application/json' \\\n"
        "  -d '{{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
        "{{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{{}},\"clientInfo\":"
        "{{\"name\":\"curl\",\"version\":\"1\"}}}}}}'\n"
        "curl -s {0} -H \"Authorization: Bearer {1}\" -H 'Content-Type: application/json' \\\n"
        "  -d '{{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}}'",
        endpoint,
        key);
    content +=
        "<section class=\"panel\"><h2>Set Up</h2>" +
        std::string(page.key.empty() ? "<p class=\"muted\">Create a key above, or put yours in "
                                       "<code>$LAKE_KEY</code>.</p>"
                                     : "") +
        copy_block("mcp-claude-code", "Claude Code", claude_code) +
        copy_block("mcp-cursor", "Cursor (mcp.json)", cursor) +
        copy_block("mcp-desktop", "Claude Desktop (claude_desktop_config.json)", desktop) +
        copy_block("mcp-curl", "Any client: JSON-RPC over HTTP", curl) + "</section>";
    content += "<section class=\"panel\"><h2>Teach Your Agent</h2><ul>"
               "<li><a href=\"/skills/lake/SKILL.md\" download=\"SKILL.md\">Download the lake "
               "skill</a>: a Claude skill (<code>SKILL.md</code>) with the setup, which tools to "
               "use for what, the flows below and the rules. Put it in "
               "<code>~/.claude/skills/lake/</code>.</li>"
               "<li><a href=\"/llms.txt\">llms.txt</a>: lake in one page for any agent.</li>"
               "<li><a href=\"/llms-full.txt\">llms-full.txt</a>: every tool with its input "
               "schema.</li>"
               "<li><a href=\"/v1/openapi.json\">OpenAPI</a>: the same tools as REST, "
               "<code>GET /v1/&lt;tool&gt;</code> for a read and <code>POST /v1/&lt;tool&gt;"
               "</code> for an action.</li></ul></section>";
    content += "<section class=\"panel\"><h2>Worked Flows</h2>";
    for (const agent_docs::Flow& flow : agent_docs::flows()) {
        content += "<h3>" + web_ui_escape(flow.title) + "</h3><p>" + web_ui_escape(flow.summary) +
                   "</p><ol>";
        for (const agent_docs::FlowStep& step : flow.steps) {
            content += "<li><code>" + web_ui_escape(step.tool) +
                       "</code>: " + web_ui_escape(step.note) + "</li>";
        }
        content += "</ol>";
    }
    content += "</section><section class=\"panel\"><h2>What the Tools Do</h2>";
    for (const agent_docs::Group& group : page.groups) {
        content += std::format("<details><summary>{} <span class=\"muted\">{} tools</span>"
                               "</summary><p>{}</p><div class=\"table-wrap\"><table><thead><tr>"
                               "<th>Tool</th><th>What it does</th><th>Access</th></tr></thead>"
                               "<tbody>",
                               web_ui_escape(group.title),
                               group.tools.size(),
                               web_ui_escape(group.summary));
        for (const agent_docs::Tool& tool : group.tools) {
            content += std::format("<tr><td><code>{}</code></td><td>{}</td><td>{}</td></tr>",
                                   web_ui_escape(tool.name),
                                   web_ui_escape(tool.title),
                                   tool.writes ? "write" : "read");
        }
        content += "</tbody></table></div></details>";
    }
    content += "</section>" + std::string(copy_script);
    return web_ui_shell("lake — MCP", "", content, viewer);
}

std::string web_ui_ask(const AskPage& page, WebUiViewer viewer) {
    std::string content =
        page.embed ? std::string("<p class=\"sub\">Ask about anything in your lake. Changes are "
                                 "proposed for you to confirm.</p>")
                   : std::string("<p class=\"eyebrow\">Ask Orama</p><h1>Ask Orama</h1><p "
                                 "class=\"sub\">Ask about anything in your lake. Ask Orama reads "
                                 "what you may read, proposes every change for you to confirm, and "
                                 "brings in the right agent when another does it better.</p>");
    const std::string stay = page.embed ? hidden("embed", "1") : std::string();
    if (!page.error.empty()) {
        content +=
            std::format("<p class=\"error\" role=\"alert\">{}</p>", web_ui_escape(page.error));
    }
    if (!page.from.empty()) {
        content +=
            std::format("<p class=\"sub\">About the page <a href=\"{}\" target=\"_top\">{}</a></p>",
                        web_ui_escape(page.from),
                        web_ui_escape(page.from.substr(0, page.from.find('?'))));
    }
    content += "<section class=\"panel ask-conversation\" aria-live=\"polite\">";
    if (page.messages.empty()) {
        content += "<p>Ask anything about your lake: your repositories, projects, data, runs, "
                   "models, services or agents.</p>";
    }
    // Which proposals have already been confirmed, so a confirmed one is never offered again.
    std::set<std::string> confirmed;
    for (const nlohmann::json& message : page.messages) {
        const nlohmann::json payload = message.value("payload", nlohmann::json::object());
        if (payload.is_object() && payload.contains("confirmed")) {
            confirmed.insert(json_text(payload["confirmed"], "message_id") + "/" +
                             json_text(payload["confirmed"], "index"));
        }
    }
    for (const nlohmann::json& message : page.messages) {
        const bool person = json_text(message, "role") == "user";
        const nlohmann::json payload = message.value("payload", nlohmann::json::object());
        const std::string agent = payload.is_object() ? json_text(payload, "agent") : "";
        // An agent answers in markdown (tables, lists, code); the renderer escapes every
        // character before adding markup, so a reply cannot inject HTML.
        content += std::format(
            "<article class=\"ask-message {}\"><p class=\"eyebrow\">{}</p>{}",
            person ? "ask-person" : "ask-agent",
            person ? std::string("You")
                   : (agent.empty() || agent == "agent-ask-orama" ? std::string("Ask Orama")
                                                                  : web_ui_escape(agent)),
            person ? "<p>" + web_ui_escape(json_text(message, "content")) + "</p>"
                   : "<div class=\"ask-reply\">" +
                         web_ui_markdown(json_text(message, "content")) + "</div>");
        if (payload.is_object()) {
            for (const nlohmann::json& delegation :
                 payload.value("delegations", nlohmann::json::array())) {
                content += std::format("<p class=\"sub\">Asked {}: {}</p>",
                                       web_ui_escape(json_text(delegation, "agent")),
                                       web_ui_escape(json_text(delegation, "request")));
                // A Project Agent proposal waits on its pipeline page (lake#249).
                const nlohmann::json answered =
                    delegation.is_object() ? delegation.value("answer", nlohmann::json()) : nullptr;
                const std::string review =
                    answered.is_object() ? json_text(answered, "review_url") : std::string();
                if (review.starts_with("/design/pipeline?")) {
                    content += std::format("<p><a class=\"button button-primary\" href=\"{}"
                                           "#proposal\">Review the proposed change</a></p>",
                                           web_ui_escape(review));
                }
            }
            const nlohmann::json proposals = payload.value("proposals", nlohmann::json::array());
            for (std::size_t index = 0; index < proposals.size(); ++index) {
                const nlohmann::json& proposal = proposals[index];
                const bool done =
                    confirmed.contains(json_text(message, "id") + "/" + std::to_string(index));
                content += std::format(
                    "<div class=\"panel ask-proposal\"><p><strong>Proposed:</strong> "
                    "<code>{}</code></p>"
                    "<p>{}</p>{}</div>",
                    web_ui_escape(json_text(proposal, "capability")),
                    web_ui_escape(json_text(proposal, "impact")),
                    done ? std::string("<p><span class=\"badge\">Confirmed</span></p>")
                         : std::format(
                               "<form class=\"inline\" method=\"post\" action=\"/ask/confirm\">"
                               "{}{}{}{}{}{}<button class=\"button button-primary\" "
                               "type=\"submit\">Confirm</button></form>",
                               hidden("customer_id", page.customer_id),
                               hidden("conversation", page.conversation_id),
                               hidden("message_id", json_text(message, "id")),
                               hidden("index", std::to_string(index)),
                               hidden("from", page.from),
                               stay));
            }
        }
        content += "</article>";
    }
    if (page.working) {
        content += "<article class=\"ask-message ask-agent\" role=\"status\"><p class=\"eyebrow\">"
                   "Ask Orama</p><p><em>Thinking…</em> It keeps working if you leave; this page "
                   "checks again every 2 seconds.</p><meta http-equiv=\"refresh\" "
                   "content=\"2\"></article>";
    }
    content += "</section>";
    content += agent_events_link(page.customer_id, page.task_id);
    std::string options;
    for (const nlohmann::json& agent : page.agents) {
        options += std::format("<option value=\"{}\"{}>{}</option>",
                               web_ui_escape(json_text(agent, "id")),
                               json_text(agent, "id") == page.agent ? " selected" : "",
                               web_ui_escape(json_text(agent, "name")));
    }
    content += std::format(
        "<section class=\"panel\"><form class=\"stack\" method=\"post\" action=\"/ask/send\">{}{}{}{}"
        "<label>Talk to<select name=\"agent\">{}</select></label><label>Message<textarea "
        "name=\"message\" rows=\"3\" required placeholder=\"What should I look at first?\">"
        "</textarea></label><button class=\"button button-primary\" type=\"submit\">Send</button>"
        "</form><p class=\"sub\"><a href=\"/ask?{}&from={}{}\">Start a new "
        "conversation</a></p></section>",
        hidden("customer_id", page.customer_id),
        hidden("conversation", page.conversation_id),
        hidden("from", page.from),
        stay,
        options,
        customer_query(page.customer_id),
        url_component(page.from),
        page.embed ? "&embed=1" : "");
    if (page.embed) {
        // The panel beside a page: the conversation alone, in lake's styles.
        return "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta "
               "name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Ask "
               "Orama</title>" +
               stylesheet_link() + "</head><body class=\"ask-embedded\"><main>" + content +
               "</main></body></html>";
    }
    return web_ui_shell(
        "lake — Ask Orama", std::vector<WebUiCrumb>{{"Ask Orama", ""}}, content, viewer);
}

std::string web_ui_factory(const FactoryPage& page, WebUiViewer viewer) {
    const std::string query = customer_query(page.customer_id);
    std::string content =
        "<p class=\"eyebrow\">Train</p><h1>Agent Factory</h1><p class=\"sub\">Tell AgentCreator "
        "what you want an agent for. It asks what it must, then proposes the agent. Nothing is "
        "created until you approve the proposal.</p>";
    if (!page.error.empty()) {
        content +=
            std::format("<p class=\"error\" role=\"alert\">{}</p>", web_ui_escape(page.error));
    }
    content += "<section class=\"panel ask-conversation\" aria-live=\"polite\">";
    for (const nlohmann::json& message : page.messages) {
        const bool person = json_text(message, "role") == "user";
        content +=
            std::format("<article class=\"ask-message {}\"><p class=\"eyebrow\">{}</p>{}"
                        "</article>",
                        person ? "ask-person" : "ask-agent",
                        person ? "You" : "AgentCreator",
                        person ? "<p>" + web_ui_escape(json_text(message, "content")) + "</p>"
                               : "<div class=\"ask-reply\">" +
                                     web_ui_markdown(json_text(message, "content")) + "</div>");
    }
    if (page.messages.empty()) {
        content += "<p>Describe the agent: what it is for, what it may touch, and what done looks "
                   "like.</p>";
    }
    if (page.working) {
        content += "<article class=\"ask-message ask-agent\" role=\"status\"><p class=\"eyebrow\">"
                   "AgentCreator</p><p><em>Working on it…</em> It keeps working if you leave; this "
                   "page checks again every 2 seconds.</p><meta http-equiv=\"refresh\" "
                   "content=\"2\"></article>";
    }
    content += "</section>";
    content += agent_events_link(page.customer_id, page.task_id);
    const nlohmann::json& latest = page.latest;
    if (json_text(latest, "kind") == "proposal") {
        const nlohmann::json proposal = latest.value("proposal", nlohmann::json::object());
        const nlohmann::json agent = proposal.value("agent", nlohmann::json::object());
        const nlohmann::json defects = latest.value("defects", nlohmann::json::array());
        std::string files;
        for (const nlohmann::json& file : proposal.value("files", nlohmann::json::array())) {
            files += std::format("<h4><code>{}</code></h4><pre><code>{}</code></pre>",
                                 web_ui_escape(json_text(file, "path")),
                                 web_ui_escape(json_text(file, "contents")));
        }
        std::string pipelines;
        for (const nlohmann::json& grant : proposal.value("pipelines", nlohmann::json::array())) {
            pipelines += (pipelines.empty() ? "" : ", ") + json_text(grant, "graph") + " in " +
                         json_text(grant, "project");
        }
        const nlohmann::json area = agent.value("work_area", nlohmann::json::object());
        content += std::format(
            "<section class=\"panel\" id=\"proposal\"><h2>Proposed Agent: {}</h2><p>{}</p><dl>"
            "<dt>Type</dt><dd>{} — {}</dd><dt>Stored in</dt><dd>{}</dd><dt>Work area</dt><dd>{} "
            "{}</dd><dt>Instructions</dt><dd>{}</dd><dt>What success looks like</dt><dd>{}</dd><dt>What it "
            "may do</dt><dd>{}</dd>"
            "<dt>Trigger</dt><dd>{}</dd><dt>Pipelines</dt><dd>{}</dd></dl>{}",
            web_ui_escape(json_text(agent, "name")),
            web_ui_escape(json_text(agent, "purpose")),
            web_ui_escape(json_text(proposal, "type")),
            web_ui_escape(json_text(proposal, "why_type")),
            web_ui_escape(json_text(proposal, "type") == "python"
                              ? json_text(proposal, "storage") + " — " +
                                    json_text(proposal, "why_storage")
                              : std::string("the workspace (native)")),
            web_ui_escape(json_text(area, "kind")),
            web_ui_escape(json_text(area, "detail")),
            web_ui_escape(json_text(agent, "manifesto")),
            web_ui_escape(json_text(agent, "goal")),
            web_ui_escape(agent_skill_labels(agent.value("skills", nlohmann::json::array()))),
            web_ui_escape(json_text(agent, "trigger").empty() ? std::string("Conversation only")
                                                              : json_text(agent, "trigger")),
            web_ui_escape(pipelines.empty() ? std::string("—") : pipelines),
            files);
        if (!defects.empty()) {
            content += "<div class=\"error\" role=\"alert\"><p>This proposal has defects and "
                       "cannot be created yet:</p><ul>";
            for (const nlohmann::json& defect : defects) {
                content +=
                    "<li>" +
                    web_ui_escape(defect.is_string() ? defect.get<std::string>() : defect.dump()) +
                    "</li>";
            }
            content += "</ul><p>Tell AgentCreator what to change.</p></div>";
        } else {
            content += std::format("<form class=\"stack\" method=\"post\" "
                                   "action=\"/train/agents/new/approve\">{}{}{}{}"
                                   "<button class=\"button button-primary\" "
                                   "type=\"submit\">Approve and create</button>"
                                   "</form>",
                                   hidden("customer_id", page.customer_id),
                                   hidden("conversation", page.conversation_id),
                                   hidden("message_id", page.message_id),
                                   hidden("proposal_sha256", page.proposal_sha256));
        }
        content += "</section>";
    }
    content += std::format(
        "<section class=\"panel\"><form class=\"stack\" method=\"post\" "
        "action=\"/train/agents/new/send\">{}{}<label>{}<textarea name=\"message\" rows=\"3\" "
        "required placeholder=\"An agent that flags renewals at risk every Monday\"></textarea>"
        "</label><button class=\"button button-primary\" type=\"submit\">Send</button></form><p "
        "class=\"sub\"><a href=\"/train/agents?{}\">Back to agents</a></p></section>",
        hidden("customer_id", page.customer_id),
        hidden("conversation", page.conversation_id),
        json_text(latest, "kind") == "proposal" ? "Change something" : "Message",
        query);
    return web_ui_shell("lake — Agent Factory",
                        std::vector<WebUiCrumb>{{"Train", ""},
                                                {"Agents", "/train/agents?" + query},
                                                {"Agent Factory", ""}},
                        content,
                        viewer);
}

} // namespace lake::api

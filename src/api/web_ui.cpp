#include "lake/api/web_ui.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <optional>
#include <set>
#include <sstream>

#include <liquid/hash.hpp>

#include "modules/frontends/common/syntax.hpp"

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

std::string markdown_inline(std::string_view text) {
    std::string output;
    std::size_t offset = 0;
    while (offset < text.size()) {
        if (text[offset] == '`') {
            const std::size_t end = text.find('`', offset + 1);
            if (end == std::string_view::npos) {
                output += "&#96;";
                ++offset;
                continue;
            }
            output += "<code>";
            output += web_ui_escape(text.substr(offset + 1, end - offset - 1));
            output += "</code>";
            offset = end + 1;
            continue;
        }
        if (text.substr(offset).starts_with("**")) {
            const std::size_t end = text.find("**", offset + 2);
            if (end != std::string_view::npos) {
                output += "<strong>";
                output += web_ui_escape(text.substr(offset + 2, end - offset - 2));
                output += "</strong>";
                offset = end + 2;
                continue;
            }
        }
        output += web_ui_escape(text.substr(offset, 1));
        ++offset;
    }
    return output;
}

} // namespace

std::string web_ui_markdown(std::string_view markdown) {
    std::istringstream input{std::string(markdown)};
    std::string line;
    std::string output = "<article class=\"markdown-body\">";
    std::map<std::string, std::size_t> heading_counts;
    bool code_open = false;
    bool list_open = false;
    bool paragraph_open = false;
    auto close_paragraph = [&]() {
        if (paragraph_open) {
            output += "</p>";
            paragraph_open = false;
        }
    };
    auto close_list = [&]() {
        if (list_open) {
            output += "</ul>";
            list_open = false;
        }
    };
    while (std::getline(input, line)) {
        if (line.starts_with("```")) {
            close_paragraph();
            close_list();
            if (code_open) {
                output += "</code></pre>";
            } else {
                output += "<pre><code>";
            }
            code_open = !code_open;
            continue;
        }
        if (code_open) {
            output += web_ui_escape(line);
            output += '\n';
            continue;
        }
        std::size_t level = 0;
        while (level < line.size() && level < 6 && line[level] == '#') {
            ++level;
        }
        if (level > 0 && level < line.size() && line[level] == ' ') {
            close_paragraph();
            close_list();
            const std::string_view heading{line.data() + level + 1, line.size() - level - 1};
            std::string slug = markdown_slug(heading);
            const std::size_t duplicate = heading_counts[slug]++;
            if (duplicate > 0) {
                slug += '-' + std::to_string(duplicate + 1);
            }
            output += std::format("<h{} id=\"{}\">{}<a class=\"heading-anchor\" href=\"#{}\" "
                                  "aria-label=\"Link to this heading\">#</a></h{}>",
                                  level,
                                  slug,
                                  markdown_inline(heading),
                                  slug,
                                  level);
            continue;
        }
        if (line.starts_with("- ") || line.starts_with("* ")) {
            close_paragraph();
            if (!list_open) {
                output += "<ul>";
                list_open = true;
            }
            output += "<li>" + markdown_inline(std::string_view(line).substr(2)) + "</li>";
            continue;
        }
        if (line.empty()) {
            close_paragraph();
            close_list();
            continue;
        }
        close_list();
        if (!paragraph_open) {
            output += "<p>";
            paragraph_open = true;
        } else {
            output += ' ';
        }
        output += markdown_inline(line);
    }
    close_paragraph();
    close_list();
    if (code_open) {
        output += "</code></pre>";
    }
    output += "</article>";
    return output;
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
    static constexpr std::string_view projects[] = {"/design"};
    static constexpr std::string_view data[] = {"/train/data"};
    static constexpr std::string_view runs[] = {"/train/runs", "/train/run"};
    static constexpr std::string_view models[] = {"/ship/models"};
    static constexpr std::string_view indexes[] = {"/ship/indexes"};
    static constexpr std::string_view packages[] = {"/ship/packages"};
    static constexpr std::string_view services[] = {"/services"};
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
    if (key == "data")
        return data;
    if (key == "runs")
        return runs;
    if (key == "models")
        return models;
    if (key == "indexes")
        return indexes;
    if (key == "services")
        return services;
    if (key == "packages")
        return packages;
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
    return {};
}

bool route_owns(std::string_view path, std::string_view route) {
    if (route == "/") {
        return path == "/";
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
        if (route_owns(viewer.path, route)) {
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

// The sidebar follows docs/design/cloud.md: the viewer's repositories first, then the sections
// that act on them. A section appears only when it has a page the viewer can open; Design and
// Train join when their pages exist. Account pages live in the top-bar account menu.
std::string nav(const WebUiViewer& viewer, std::string_view title) {
    std::string out = "<nav class=\"primary-nav\" aria-label=\"Primary\">";
    out += nav_link("/", "Home", "home", "home", viewer, title);
    out += nav_link("/repositories",
                    viewer.signed_in ? "Repositories" : "Public repositories",
                    "repository",
                    "repositories",
                    viewer,
                    title);
    if (viewer.signed_in) {
        if (!viewer.administrator) {
            out += nav_link(
                "/shared-with-me", "Shared with me", "repository", "shared with me", viewer, title);
        }
        out += nav_link("/search", "Search", "search", "search", viewer, title);
        out += "<p class=\"nav-label\">Design</p>";
        out += nav_link("/design/projects", "Projects", "audit", "projects", viewer, title);
        out += "<p class=\"nav-label\">Train</p>";
        out += nav_link("/train/data", "Data", "metering", "data", viewer, title);
        out += nav_link("/train/runs", "Runs", "metering", "runs", viewer, title);
        out += "<p class=\"nav-label\">Ship</p>";
        out += nav_link("/ship/models", "Models", "api", "models", viewer, title);
        out += nav_link("/ship/indexes", "Search indexes", "search", "indexes", viewer, title);
        out += nav_link("/ship/packages", "Packages", "repository", "packages", viewer, title);
        out += nav_link("/deployments", "Deployments", "api", "deployments", viewer, title);
        out += nav_link("/services", "Services", "api", "services", viewer, title);
    }
    // A signed-in account's own organizations, secrets, usage and activity. Operators reach the
    // instance-wide versions under Administration instead.
    if (viewer.signed_in && !viewer.administrator) {
        out += "<p class=\"nav-label\">Organization</p>";
        out += nav_link(
            "/organizations", "Organizations", "organization", "organizations", viewer, title);
        out += nav_link("/secret-mounts", "Secrets", "secrets", "secret", viewer, title);
        out += nav_link("/metering", "Usage", "metering", "metering", viewer, title);
        out += nav_link("/activity", "Activity", "audit", "activity", viewer, title);
    }
    if (viewer.administrator) {
        out += "<p class=\"nav-label\">Administration</p>";
        out += nav_link("/customers", "Customers", "customers", "customers", viewer, title);
        out += nav_link(
            "/organizations", "Organizations", "organization", "organizations", viewer, title);
        out += nav_link("/applications", "Applications", "api", "applications", viewer, title);
        out += nav_link("/metering", "Metering", "metering", "metering", viewer, title);
        out += nav_link("/secret-mounts", "Secrets", "secrets", "secret", viewer, title);
        out += nav_link("/audit", "Audit", "audit", "audit", viewer, title);
    }
    if (viewer.signed_in) {
        out += "<p class=\"nav-label\">Developer</p>";
        out += nav_link("/v1/openapi.json", "API", "api", "api", viewer, title);
    }
    out += "</nav>";
    return out;
}

// Signed out: sign in and create account. Signed in: an account menu that needs no script
// (<details>), holding the person's own pages and sign out. Operators have no personal pages.
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
    return std::format("<details class=\"account-menu\"><summary>{}<span>Account</span></summary>"
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

std::string web_ui_shell(std::string_view title,
                         std::string_view meta,
                         std::string_view content,
                         WebUiViewer viewer) {
    return std::format(
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
        web_ui_escape(meta),
        web_ui_icon("theme"),
        session_control(viewer),
        content,
        theme_toggle_script);
}

std::string web_ui_error(std::string_view code, std::string_view message, WebUiViewer viewer) {
    const std::string content = std::format(
        "<div class=\"state-page error-state\" role=\"alert\"><p class=\"eyebrow\">{}</p>"
        "<h1>We could not load this page</h1><p class=\"sub\">{}</p>"
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
        "<p class=\"sub\">Keep your files, datasets and models in shared repositories, train "
        "models on them, and put them into production, in Orama's cloud or your own.</p>";
    if (viewer.signed_in) {
        content += "<p class=\"actions\"><a class=\"button button-primary\" "
                   "href=\"/repositories\">Open your repositories</a> <a class=\"button\" "
                   "href=\"/deployments\">See your deployments</a></p>";
    } else {
        content += "<p class=\"actions\"><a class=\"button button-primary\" "
                   "href=\"/sign-up\">Create account</a> <a class=\"button\" "
                   "href=\"/sign-in\">Sign in</a></p>";
    }
    content += "<div class=\"grid\">"
               "<section class=\"card\"><h3>Design</h3>"
               "<p>Start from the business question and the decisions it drives. Keep the plan, "
               "the data and the work in one repository your team edits together.</p></section>"
               "<section class=\"card\"><h3>Train</h3>"
               "<p>Put datasets and models in repositories. Each revision is stored once, and "
               "every model records the data and code it was trained from.</p></section>"
               "<section class=\"card\"><h3>Ship</h3>"
               "<p>Deploy a repository to an environment, serve it on your domain, and read its "
               "logs. Push your work with <code>liquid push</code> and a token from your "
               "account. Installing Liquid currently requires access to its source "
               "repository.</p>";
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
        sign_up ? "Create your account" : "Sign in",
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
        "<p class=\"eyebrow\">Operator access</p><h1>Operator sign in</h1>"
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
            : "<p class=\"eyebrow\">Explore</p><h1>Public repositories</h1>"
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
            "<section class=\"panel\"><h2>Create a repository</h2>"
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
               "<h2>Recent changes</h2>" +
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
               "details\"><section class=\"panel\"><h2>Clone and push</h2>";
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
    content += "</section><section class=\"panel\"><h2>Effective access</h2>";
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

struct WorkspaceSection {
    std::string_view key, verb, title, path, sub;
};

// The Design, Train and Ship pages, in navigation order. `path` is the list page a detail page
// belongs to.
constexpr std::array<WorkspaceSection, 8> workspace_sections{{
    {"projects",
     "Design",
     "Projects",
     "/design/projects",
     "A project starts from the business question: the decision it should improve and how you "
     "will measure it. Its pipelines are built from that intent."},
    {"project", "Design", "Project", "/design/projects", ""},
    {"data",
     "Train",
     "Data",
     "/train/data",
     "The connections and datasets your workspace trains on."},
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
     "Search indexes",
     "/ship/indexes",
     "Search indexes over your data, and whether each is published for queries."},
    {"services",
     "Ship",
     "Services",
     "/services",
     "The Zetta and Postgres clusters this space runs on lake, each its own, with its data kept "
     "apart from every other customer's. Your workspace is the one Design, Train and Ship use."},
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

// Deleting needs the box ticked; its value is the workspace id, which delete_workspace checks.
std::string workspace_delete_form(const WorkspacePage& page,
                                  std::string_view heading,
                                  std::string_view explanation,
                                  std::string_view button) {
    const std::string slug = page.state.value("workspace", std::string());
    if (slug.empty()) {
        return {};
    }
    return std::format(
        "<section class=\"panel\"><h2>{}</h2><p>{}</p><form class=\"stack\" method=\"post\" "
        "action=\"/workspace/delete\">{}<label><input type=\"checkbox\" name=\"confirm\" "
        "value=\"{}\" required> I understand that workspace <code>{}</code> and everything in it "
        "is deleted and cannot be recovered.</label><button class=\"button button-danger\" "
        "type=\"submit\">{}</button></form></section>",
        web_ui_escape(heading),
        web_ui_escape(explanation),
        hidden("customer_id", page.customer_id),
        web_ui_escape(slug),
        web_ui_escape(slug),
        web_ui_escape(button));
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
                       "<div class=\"panel notice status-danger\"><h2>Your workspace could not "
                       "be prepared</h2><p>{}</p></div>",
                       quota ? "This lake already runs as many workspaces as it allows (" +
                                   web_ui_escape(message) +
                                   "). An operator can raise the limit; yours starts as soon as "
                                   "there is room, or cancel the request below."
                             : web_ui_escape(message)) +
                   workspace_delete_form(page,
                                         "Cancel this workspace",
                                         "Withdraw the request so you can start again later.",
                                         "Cancel workspace");
        }
        return std::format(
            "<div class=\"panel\" role=\"status\"><h2>Preparing your workspace</h2><p>lake asked "
            "for a Zetta cluster of your own, where your projects, data and models live apart "
            "from every other customer's. {}</p><p class=\"muted\">This page checks again every "
            "15 seconds.</p><meta http-equiv=\"refresh\" content=\"15\"></div>",
            phase.empty() ? "It has not been picked up yet."
                          : "Status: " + web_ui_escape(phase) + ".");
    }
    return std::format(
        "<div class=\"panel\"><h2>Start your workspace</h2><p>Design, Train and Ship run on a "
        "Zetta cluster of your own, so your projects, data and models are never shared with "
        "another customer. It takes a minute or two to prepare.</p>"
        "<form method=\"post\" action=\"/workspace/start\">{}{}"
        "<button class=\"button button-primary\" type=\"submit\">Start your workspace</button>"
        "</form></div>",
        hidden("customer_id", page.customer_id),
        hidden("return", section.path));
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
    const nlohmann::json services = page.data.value("list_services", nlohmann::json::object())
                                        .value("services", nlohmann::json::array());
    std::string out;
    bool settling = false;
    if (const auto connection = page.data.find("service_connection");
        connection != page.data.end()) {
        out += std::format(
            "<section class=\"panel notice\" role=\"status\"><h2>Connection to {}</h2><p>Reachable "
            "inside the lake cluster, from your other services. Copy it now; it is not shown "
            "again unless you ask.</p><p><code>{}</code></p></section>",
            web_ui_escape(json_text(*connection, "name")),
            web_ui_escape(json_text(*connection, "uri")));
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
            if (!deleting && kind == "postgres" && phase == "Ready") {
                actions += std::format("<form method=\"post\" action=\"/services/connection\">{}{}"
                                       "<button type=\"submit\">Show connection</button></form>",
                                       hidden("customer_id", page.customer_id),
                                       hidden("service_id", id));
            }
            if (!deleting) {
                actions += std::format(
                    "<details><summary>Delete</summary><form class=\"stack\" method=\"post\" "
                    "action=\"/services/delete\">{}{}<label>Type <code>{}</code> to delete it "
                    "and all of its data, which cannot be recovered<input name=\"confirm\" "
                    "required autocomplete=\"off\"></label><button class=\"button button-danger\" "
                    "type=\"submit\">Delete {}</button></form></details>",
                    hidden("customer_id", page.customer_id),
                    hidden("service_id", id),
                    web_ui_escape(name),
                    web_ui_escape(name));
            }
            out += std::format(
                "<tr><td>{}{}</td><td>{}</td><td>{} × {} GiB</td><td>{}{}</td><td><code>{}</code>"
                "</td><td>{}</td></tr>",
                web_ui_escape(name),
                service.value("default", false) ? " <span class=\"muted\">(workspace)</span>" : "",
                kind == "postgres" ? "Postgres" : "Zetta",
                web_ui_escape(json_text(service, "instances")),
                web_ui_escape(json_text(service, "storage_gi")),
                web_ui_escape(phase),
                json_text(service, "message").empty()
                    ? std::string()
                    : "<br><span class=\"muted\">" + web_ui_escape(json_text(service, "message")) +
                          "</span>",
                web_ui_escape(json_text(service, "endpoint")),
                actions);
        }
        out += "</tbody></table>";
    }
    if (settling) {
        out += "<p class=\"muted\">This page checks again every 15 seconds while a service is "
               "changing.</p><meta http-equiv=\"refresh\" content=\"15\">";
    }
    out += std::format(
        "<section class=\"panel\"><h2>New service</h2><form class=\"stack\" method=\"post\" "
        "action=\"/services/create\">{}<label>Kind<select name=\"kind\"><option "
        "value=\"postgres\">Postgres</option><option value=\"zetta\">Zetta</option></select>"
        "</label><label>Name<input name=\"name\" required pattern=\"[a-z][a-z0-9\\-]{{0,30}}\" "
        "placeholder=\"crm\"></label><label>Instances<select name=\"instances\"><option>1"
        "</option><option>2</option><option>3</option></select></label><label>Storage (GiB)"
        "<input name=\"storage_gi\" type=\"number\" min=\"1\" max=\"100\" value=\"10\">"
        "</label><p class=\"muted\">This space may run {} services at once.</p><button "
        "class=\"button button-primary\" type=\"submit\">Create service</button></form>"
        "</section>",
        hidden("customer_id", page.customer_id),
        web_ui_escape(
            json_text(page.data.value("list_services", nlohmann::json::object()), "limit")));
    return out;
}

std::string workspace_content(const WorkspacePage& page) {
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
        out += std::format(
            "<section class=\"panel\"><h2>New project</h2><form class=\"stack\" method=\"post\" "
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
        out += std::format("<p class=\"sub\">{}</p>",
                           web_ui_escape(json_text(project, "description").empty()
                                             ? "No description."
                                             : json_text(project, "description")));
        const nlohmann::json graphs = project.value("graphs", nlohmann::json::array());
        out += "<section class=\"panel\"><h2>Pipelines</h2>";
        if (graphs.empty()) {
            out += "<p>No pipelines yet. Describe what the project should predict or answer, "
                   "and Zetta builds one.</p>";
        } else {
            out += "<table><thead><tr><th>Pipeline</th><th>Approval</th><th></th></tr></thead>"
                   "<tbody>";
            for (const nlohmann::json& graph : graphs) {
                const std::string id = json_text(graph, "id");
                // Zetta verifies and runs only an approved plan; a person approves it here first.
                const bool approved = json_text(graph, "approval") == "approved";
                out += std::format(
                    "<tr><td>{}</td><td>{}</td><td><form method=\"post\" action=\"{}\">{}{}{}"
                    "<button type=\"submit\">{}</button></form></td></tr>",
                    web_ui_escape(json_text(graph, "name").empty() ? id : json_text(graph, "name")),
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
            "</section><section class=\"panel\"><h2>Build a pipeline</h2><form class=\"stack\" "
            "method=\"post\" action=\"/design/project/compile\">{}{}<label>What should it predict "
            "or answer?<textarea name=\"intent\" rows=\"3\" required placeholder=\"Predict which "
            "accounts will churn in the next 90 days, with the reasons\"></textarea></label>"
            "<button class=\"button button-primary\" type=\"submit\">Build pipeline</button>"
            "</form></section>",
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
        out += "<section class=\"panel\"><h2>Connections</h2>";
        if (connections.empty()) {
            out += "<p>No connections yet. Add the database your data lives in below.</p>";
        } else {
            out += "<table><thead><tr><th>Name</th><th>Type</th><th>Where</th><th>Saved</th>"
                   "<th></th></tr></thead><tbody>";
            for (const nlohmann::json& connection : connections) {
                const nlohmann::json connector =
                    connection.value("connector", nlohmann::json::object());
                const std::string where =
                    json_text(connector, "type") == "postgres"
                        ? json_text(connector, "host") + "/" + json_text(connector, "database")
                        : json_text(connector, "account");
                out += std::format(
                    "<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td><form method=\"post\" "
                    "action=\"/train/data/disconnect\">{}{}<button type=\"submit\">Remove"
                    "</button></form></td></tr>",
                    web_ui_escape(json_text(connection, "name")),
                    web_ui_escape(json_text(connector, "type")),
                    web_ui_escape(where),
                    web_ui_escape(json_text(connection, "created")),
                    hidden("customer_id", page.customer_id),
                    hidden("name", json_text(connection, "name")));
            }
            out += "</tbody></table>";
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
        out += datasets.empty() ? "<p>No datasets yet.</p>"
                                : workspace_table(datasets,
                                                  {{"name", "Name"},
                                                   {"rows", "Rows"},
                                                   {"size_bytes", "Bytes"},
                                                   {"format", "Format"}});
        out += std::format(
            "<details><summary>Upload a file</summary><form class=\"stack\" method=\"post\" "
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
            out += std::format(
                "<details><summary>Pull a dataset</summary><form class=\"stack\" "
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
        out += "</section>";
    } else if (page.section == "runs") {
        const nlohmann::json runs = data.value("list_runs", nlohmann::json::object())
                                        .value("runs", nlohmann::json::array());
        if (runs.empty()) {
            out += "<div class=\"panel empty\">No runs yet. Run a pipeline from a project.</div>";
        } else {
            out += "<table><thead><tr><th>Run</th><th>Project</th><th>Pipeline</th><th>Status</th>"
                   "<th>Updated</th></tr></thead><tbody>";
            for (const nlohmann::json& run : runs) {
                const std::string id = json_text(run, "id");
                out += std::format(
                    "<tr><td>{}</td><td>{}</td><td>{}</td><td><span class=\"badge\">{}</span></td>"
                    "<td>{}</td></tr>",
                    row_link("/train/run?" + query + "&id=" + url_component(id), id),
                    web_ui_escape(json_text(run, "project")),
                    web_ui_escape(json_text(run, "graph")),
                    web_ui_escape(json_text(run, "status")),
                    web_ui_escape(json_text(run, "updated")));
            }
            out += "</tbody></table>";
        }
    } else if (page.section == "run") {
        const nlohmann::json run = data.value("get_run", nlohmann::json::object());
        const std::string status = json_text(run, "status");
        out += std::format("<p>Status: <span class=\"badge\">{}</span></p>",
                           web_ui_escape(status.empty() ? "unknown" : status));
        if (data.contains("get_run_report")) {
            out += std::format("<section class=\"panel\"><h2>Report</h2><pre>{}</pre></section>",
                               web_ui_escape(data.at("get_run_report").dump(2)));
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
            out += "<table><thead><tr><th>Model</th><th>Kind</th><th>Project</th><th>Run</th>"
                   "<th>Endpoint</th></tr></thead><tbody>";
            for (const nlohmann::json& model : models) {
                const std::string name = json_text(model, "name");
                std::string forms;
                for (const auto& [expose, label] : std::array<std::pair<bool, std::string_view>, 2>{
                         {{true, "Publish"}, {false, "Unpublish"}}}) {
                    forms += std::format("<form class=\"inline\" method=\"post\" "
                                         "action=\"/ship/models/publish\">{}{}{}<button "
                                         "type=\"submit\">{}</button></form> ",
                                         hidden("customer_id", page.customer_id),
                                         hidden("model", name),
                                         hidden("publish", expose ? "1" : "0"),
                                         label);
                }
                out += std::format("<tr><td>{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td>"
                                   "</tr>",
                                   web_ui_escape(name),
                                   web_ui_escape(json_text(model, "kind")),
                                   web_ui_escape(json_text(model, "project")),
                                   web_ui_escape(json_text(model, "run")),
                                   forms);
            }
            out += "</tbody></table><p class=\"muted\">A published model answers "
                   "<code>POST /v1/models/&lt;name&gt;/predict</code> inside your workspace.</p>";
        }
    } else if (page.section == "indexes") {
        const nlohmann::json indexes = data.value("list_indexes", nlohmann::json::object())
                                           .value("indexes", nlohmann::json::array());
        out += indexes.empty() ? "<div class=\"panel empty\">No search indexes yet.</div>"
                               : workspace_table(indexes,
                                                 {{"id", "Index"},
                                                  {"table", "Table"},
                                                  {"kind", "Kind"},
                                                  {"backend", "Backend"},
                                                  {"created", "Created"}});
    }
    return out;
}

} // namespace

std::string web_ui_workspace(const WorkspacePage& page, WebUiViewer viewer) {
    const WorkspaceSection& section = workspace_section(page.section);
    const std::string title = page.subject.empty()
                                  ? std::string(section.title)
                                  : std::string(section.title) + ": " + page.subject;
    std::string content = std::format("<p class=\"eyebrow\">{}</p><h1>{}</h1>",
                                      web_ui_escape(section.verb),
                                      web_ui_escape(title));
    if (!section.sub.empty()) {
        content += std::format("<p class=\"sub\">{}</p>", web_ui_escape(section.sub));
    }
    if (page.customers.size() > 1) {
        content += std::format("<form class=\"inline\" method=\"get\" action=\"{}\"><label>Space "
                               "<select name=\"customer_id\">",
                               section.path);
        for (const WorkspaceCustomer& customer : page.customers) {
            content += std::format("<option value=\"{}\"{}>{}{}</option>",
                                   web_ui_escape(customer.id),
                                   customer.id == page.customer_id ? " selected" : "",
                                   web_ui_escape(customer.handle),
                                   customer.yours ? " (your personal space)" : "");
        }
        content += "</select></label> <button type=\"submit\">Show</button></form>";
    }
    if (!page.error.empty()) {
        content +=
            std::format("<p class=\"error\" role=\"alert\">{}</p>", web_ui_escape(page.error));
    }
    if (!page.unavailable.empty()) {
        content +=
            std::format("<div class=\"panel empty\">{}</div>", web_ui_escape(page.unavailable));
    } else if (page.section == "services") {
        content += services_content(page);
    } else if (!page.state.value("ready", false)) {
        content += workspace_gate(page, section);
    } else {
        content += workspace_content(page);
        if (page.section == "projects") {
            content += workspace_delete_form(
                page,
                "Delete workspace",
                "Removes your Zetta cluster with every project, dataset, run and model in it. "
                "Nothing else in lake is touched, and you can start a new, empty workspace "
                "afterwards.",
                "Delete workspace");
        }
    }
    return web_ui_shell("lake — " + title, std::string(section.verb), content, viewer);
}

std::string web_ui_packages(const nlohmann::json& catalog,
                            bool available,
                            std::string_view origin,
                            WebUiViewer viewer) {
    std::string content =
        "<p class=\"eyebrow\">Ship</p><h1>Packages</h1><p class=\"sub\">Container images, npm "
        "packages and Python distributions published to this lake. Anyone can install them; "
        "publishing needs the operator credential for now.</p>";
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
            const std::string name = item.value("name", std::string());
            std::string versions;
            std::string latest;
            for (const nlohmann::json& version : item.value("versions", nlohmann::json::array())) {
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
    return web_ui_shell("lake — packages", "Packages", content, viewer);
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
        "<section class=\"panel\"><h2>Current deployment</h2><dl><dt>Phase</dt><dd>{}</dd>"
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
    content += "<section class=\"panel\"><h2>Environment variables</h2><table><thead><tr>"
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
        "<section class=\"panel\"><h2>Deployment history</h2><table><thead><tr><th>Phase"
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
    return web_ui_shell("lake — tree", repository_name(repository), content, viewer);
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
    return web_ui_shell("lake — changes", repository_name(repository), content, viewer);
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
    content += "<section><h2>Files changed</h2>";
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
    content += "</section><section><h2>Checks and attestations</h2>";
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
    return web_ui_shell("lake — change", repository_name(repository), content, viewer);
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
    return web_ui_shell("lake — issues", repository_name(repository), content, viewer);
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
                    "<h3>Linked changes</h3><ul>{}</ul></aside></div>",
                    labels_markup.empty() ? "<span class=\"muted\">None</span>" : labels_markup,
                    assignees.empty() ? "<span class=\"muted\">Unassigned</span>"
                                      : identifier_list_markup(assignees),
                    references_markup);
    return web_ui_shell("lake — issue", repository_name(repository), content, viewer);
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
            content += "<h3>Artifacts and attestations</h3><ul class=\"release-artifacts\">";
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
    return web_ui_shell("lake — releases", repository_name(repository), content, viewer);
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
    return web_ui_shell("lake — webhooks", repository_name(repository), content, viewer);
}

std::string web_ui_branch_protection(const control::Repository& repository,
                                     const std::vector<control::ForgeRecord>& rules,
                                     const std::vector<audit::Event>& events,
                                     WebUiViewer viewer) {
    std::string content =
        std::format("<p class=\"eyebrow\">repository policy</p><h1>Branch protection</h1>"
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
    content += "</section><section><h2>Refused updates</h2>";
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
    return web_ui_shell("lake — branch protection", repository_name(repository), content, viewer);
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
    std::string output = "<h3>Why this needs attention</h3><ul>";
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
        std::format("<h3>Choose the value to keep</h3><form class=\"stack\" method=\"post\" "
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
    return web_ui_shell("lake — conflicts", repository_name(repository), content, viewer);
}

std::string web_ui_artifacts(const control::Repository& repository,
                             const RepositoryArtifactPage& page,
                             const RepositoryArtifactFilter& filter,
                             WebUiViewer viewer) {
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
    return web_ui_shell("lake — artifacts", repository_name(repository), content, viewer);
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
    content += "<section class=\"panel diff-entry\"><h2>Search inside</h2>"
               "<p class=\"muted\">Search an output with "
               "<code>in:artifact:&lt;output&gt;@&lt;result&gt;</code>. "
               "Members nested more than one container deep are recorded but not indexed.</p>"
               "<form class=\"filters\" method=\"get\" action=\"/search\">"
               "<input type=\"hidden\" name=\"repository\" value=\"" +
               web_ui_escape(repository.id) +
               "\"><label>Symbol or text<input name=\"query\" "
               "placeholder=\"symbol in:artifact:output.deb@result\" required></label>"
               "<button type=\"submit\">Search</button></form></section>";
    content += "<section class=\"panel diff-entry\"><h2>Indexed members</h2>";
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
    return web_ui_shell("lake — artifact", repository_name(repository), content, viewer);
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
            "<section class=\"panel\"><h2>Invite a member</h2><form class=\"stack\" "
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
        content += "<section class=\"panel\"><h2>Effective access</h2>";
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
            "<section class=\"panel\"><h2>Configure SSO provider</h2><form class=\"stack\" "
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
        content += "<section class=\"panel\"><h2>Recent audit outcomes</h2>";
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
    content += "</section><section class=\"panel\"><h2>Effective access</h2><p>" +
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
    content += "</section><section class=\"panel\"><h2>Effective access</h2><p>" +
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
        content += "<h2>Sessions and devices</h2><p>" + std::to_string(sessions.size()) +
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
        "lake — grants", object.object_type + ":" + object.external_id, content, viewer);
}

std::string web_ui_effective_access(const control::PolicyObject& object,
                                    const std::vector<control::EffectiveAccessEntry>& entries,
                                    WebUiViewer viewer) {
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
        "<div class=\"panel\"><h2>Storage quota</h2><p class=\"sub\">This value is used by "
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
        std::string(own ? "<p class=\"eyebrow\">Organization</p><h1>Your activity</h1><p "
                          "class=\"sub\">Signed, hash-linked events for everything you did on "
                          "this lake.</p><form class=\"filters\" method=\"get\" "
                          "action=\"/activity\">"
                        : "<p class=\"eyebrow\">control plane</p><h1>Audit log</h1><p "
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
    return web_ui_shell("lake — transparency", "public monitor", content, viewer);
}

std::string web_ui_secret_mounts(const std::vector<control::SecretMount>& mounts,
                                 WebUiViewer viewer) {
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
    return web_ui_shell("lake — secrets", mount.name, content, viewer);
}

std::string web_ui_encryption_keys(const control::SecretMount& mount,
                                   const std::vector<control::EncryptionKey>& keys,
                                   WebUiViewer viewer) {
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
        "lake — encryption keys", std::format("{} keys", keys.size()), content, viewer);
}

std::string web_ui_sign_up(std::string_view error, WebUiViewer viewer) {
    std::string content =
        "<p class=\"eyebrow\">identity</p><h1>Create an account</h1>"
        "<p class=\"sub\">Get a personal customer you own, then create your first "
        "repository.</p>";
    content += error_markup(error);
    content += R"html(<section class="panel"><h2>Get your public identity document</h2>
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
    return web_ui_shell("lake — sign in", "identity", content, viewer);
}

std::string web_ui_mfa(std::string_view error,
                       std::string_view otpauth_uri,
                       const std::vector<std::string>& recovery_codes,
                       WebUiViewer viewer) {
    std::string content = "<p class=\"eyebrow\">identity</p><h1>Multi-factor authentication</h1>";
    content += error_markup(error);
    if (viewer.signed_in) {
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
    std::string content = "<p class=\"eyebrow\">identity</p><h1>Sign in with a passkey</h1>";
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
                          " users</h1><div class=\"table-wrap\"><table><thead><tr><th>Email</th>"
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
        " organizations</h1><div class=\"table-wrap\"><table><thead><tr><th>Name</th><th>TAAP "
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
    content += "</tbody></table></div><div class=\"panel\"><h2>Create organization</h2><form "
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
                          web_ui_escape(application.name) + " event log</h1>";
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
        "</tbody></table></div><div class=\"panel\"><h2>Create webhook</h2><form "
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
    return web_ui_shell("lake — sessions", "identity", content, viewer);
}

std::string web_ui_tokens(const std::vector<control::Session>& sessions,
                          std::string_view issued_secret,
                          std::string_view error,
                          WebUiViewer viewer) {
    std::string content = "<p class=\"eyebrow\">identity</p><h1>API tokens</h1>"
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
    return web_ui_shell("lake — tokens", "identity", content, viewer);
}

std::string web_ui_exports(const control::Repository& repository,
                           const std::vector<control::ForgeRecord>& exports,
                           WebUiViewer viewer) {
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
    return web_ui_shell("lake — exports", repository_name(repository), content, viewer);
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
        content += "<div class=\"drive-create grid\"><section class=\"panel\"><h2>New folder</h2>"
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
    return web_ui_shell("lake — drive", repository_name(repository), content, viewer);
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
    return web_ui_shell("lake — preview", repository_name(repository), content, viewer);
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
    return web_ui_shell("lake — drive history", repository_name(repository), content, viewer);
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
    return web_ui_shell("lake — drive version diff", repository_name(repository), content, viewer);
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
    content += "</div><h2>What differs</h2>";
    content += difference
                   ? drive_diff_markup(*difference)
                   : "<div class=\"panel\">A structural preview is not registered for this "
                     "format. The digests and sizes above identify both preserved values.</div>";
    content += conflict_values_markup(conflict);
    content += conflict_resolution_markup(
        repository, ref, conflict, "/drive/conflict/resolve", viewer.signed_in);
    return web_ui_shell("lake — drive conflict", repository_name(repository), content, viewer);
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
    content += "<h2>Who can access this folder now</h2><p class=\"sub\">This is the live policy "
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
    content += "</tbody></table><h2>Choose a delivery</h2><div class=\"grid\"><section "
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
        "<section class=\"panel\"><h3>Independent copy</h3><p>Downloaded bytes stop being a "
        "live object. They cannot be updated or recalled by changing access "
        "here.</p></section></div>";
    content +=
        "<h2>Folder share management</h2><p class=\"sub\">A link is a continuing capability, "
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
    return web_ui_shell("lake — drive sharing", repository_name(repository), content, viewer);
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
            "<section class=\"panel\"><h2>Revoke this link</h2><p>Revoking prevents this link "
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
    return web_ui_shell("lake — drive share link", repository_name(repository), content, viewer);
}

std::string web_ui_shared_with_me(const std::vector<nlohmann::json>& folders,
                                  std::string_view error,
                                  WebUiViewer viewer) {
    std::string content =
        "<p class=\"eyebrow\">drive</p><h1>Shared with me</h1><p class=\"sub\">Live "
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
    return web_ui_shell("lake — model", model.value("modelId", ""), content, viewer);
}

} // namespace lake::api

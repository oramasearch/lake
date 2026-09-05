#include "lake/api/web_ui.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <optional>
#include <set>
#include <sstream>

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

std::string web_ui_stylesheet() {
    static constexpr std::string_view tokens =
#include "web_ui_tokens.inc"
        ;
    static constexpr std::string_view styles =
#include "web_ui_styles.inc"
        ;
    return std::string(tokens) + std::string(styles);
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

bool navigation_is_current(std::string_view title, std::string_view key) {
    if (key == "home") {
        return title == "lake — hello";
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

std::string nav_link(std::string_view href,
                     std::string_view label,
                     std::string_view icon,
                     std::string_view key,
                     std::string_view title) {
    return std::format("<a class=\"nav-link\" href=\"{}\"{}>{}<span>{}</span></a>",
                       href,
                       navigation_is_current(title, key) ? " aria-current=\"page\"" : "",
                       web_ui_icon(icon),
                       label);
}

std::string nav(bool signed_in, std::string_view title) {
    std::string out = "<nav class=\"primary-nav\" aria-label=\"Primary\">"
                      "<p class=\"nav-label\">Workspace</p>";
    out += nav_link("/", "Home", "home", "home", title);
    out += nav_link("/repositories", "Repositories", "repository", "repositories", title);
    out += nav_link("/shared-with-me", "Shared with me", "repository", "shared", title);
    out += nav_link("/search", "Search", "search", "search", title);
    out += "<p class=\"nav-label\">Administration</p>";
    out += nav_link("/customers", "Customers", "customers", "customers", title);
    out += nav_link("/organizations", "Organizations", "organization", "organizations", title);
    out += nav_link("/applications", "Applications", "api", "applications", title);
    out += nav_link("/metering", "Metering", "metering", "metering", title);
    out += nav_link("/secret-mounts", "Secrets", "secrets", "secret", title);
    out += nav_link("/audit", "Audit", "audit", "audit", title);
    if (signed_in) {
        out += "<p class=\"nav-label\">Identity</p>";
        out += nav_link("/sessions", "Sessions", "identity", "session", title);
        out += nav_link("/passkeys", "Passkeys", "identity", "passkey", title);
        out += nav_link("/tokens", "Tokens", "identity", "token", title);
    }
    out += "<p class=\"nav-label\">Developer</p>";
    out += nav_link("/v1/openapi.json", "API", "api", "api", title);
    out += "</nav>";
    return out;
}

std::string session_control(bool signed_in) {
    if (signed_in) {
        return "<span class=\"session\"><form class=\"inline\" method=\"post\" "
               "action=\"/logout\"><button type=\"submit\">Sign out</button></form></span>";
    }
    return "<span class=\"session\"><a href=\"/sign-in\">Sign in</a><a "
           "href=\"/sign-up\">Sign up</a></span>";
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

std::string forge_tabs(const control::Repository& repository, std::string_view current) {
    const std::string id = url_component(repository.id);
    const std::vector<std::tuple<std::string, std::string, std::string>> tabs{
        {"Code", "/repository?id=" + id, "code"},
        {"Changes", "/changes?repository_id=" + id, "changes"},
        {"Issues", "/issues?repository_id=" + id, "issues"},
        {"Releases", "/releases?repository_id=" + id, "releases"},
        {"Artifacts", "/artifacts?repository_id=" + id, "artifacts"},
        {"Webhooks", "/webhooks?repository_id=" + id, "webhooks"},
        {"Protection", "/protection?repository_id=" + id, "protection"},
    };
    std::string output = "<nav class=\"tabs forge-tabs\" aria-label=\"Repository\">";
    for (const auto& [label, href, key] : tabs) {
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
    std::string html =
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta name=\"color-scheme\" content=\"light dark\">"
        "<title>" +
        web_ui_escape(title) + "</title>" + std::string(theme_preference_script) +
        "<link rel=\"stylesheet\" href=\"/style.css\"><style>:root{--accent:" + accent +
        ";--paper:" + background + ";--app-accent:" + accent + ";--app-background:" + background +
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
                         bool signed_in) {
    return std::format(
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta name=\"color-scheme\" content=\"light dark\"><title>{}</title>{}"
        "<link rel=\"stylesheet\" href=\"/style.css\"></head><body>"
        "<a class=\"skip-link\" href=\"#main-content\">Skip to content</a>"
        "<div class=\"app-shell\"><aside class=\"sidebar\"><a class=\"brand\" href=\"/\">"
        "<span class=\"brand-mark\">{}</span><span>lake</span></a>{}"
        "<div class=\"sidebar-footer\">Orama data plane</div></aside>"
        "<div class=\"app-frame\"><header class=\"topbar\"><div class=\"topbar-context\">{}</div>"
        "<div class=\"topbar-actions\"><button class=\"theme-toggle\" type=\"button\" "
        "data-theme-toggle aria-label=\"Toggle theme\">{}<span class=\"sr-only\">Toggle "
        "theme</span>"
        "</button>{}</div></header><main id=\"main-content\" tabindex=\"-1\">"
        "<div class=\"page-container\">{}</div></main></div></div>{}</body></html>",
        web_ui_escape(title),
        theme_preference_script,
        web_ui_icon("lake"),
        nav(signed_in, title),
        web_ui_escape(meta),
        web_ui_icon("theme"),
        session_control(signed_in),
        content,
        theme_toggle_script);
}

std::string web_ui_error(std::string_view code, std::string_view message, bool signed_in) {
    const std::string content = std::format(
        "<div class=\"state-page error-state\" role=\"alert\"><p class=\"eyebrow\">{}</p>"
        "<h1>We could not load this page</h1><p class=\"sub\">{}</p>"
        "<p><a class=\"button primary\" href=\"/repositories\">Back to repositories</a></p></div>",
        web_ui_escape(code),
        web_ui_escape(message));
    return web_ui_shell("lake — error", code, content, signed_in);
}

std::string web_ui_loading(std::string_view subject, bool signed_in) {
    const std::string content = std::format(
        "<div class=\"state-page loading-state\" role=\"status\" aria-live=\"polite\" "
        "aria-busy=\"true\"><p class=\"eyebrow\">loading</p><h1>{}</h1>"
        "<p class=\"sub\">Loading.</p><div class=\"loading-bars\" "
        "aria-hidden=\"true\"><span></span><span></span><span></span></div></div>",
        web_ui_escape(subject));
    return web_ui_shell("lake — loading", subject, content, signed_in);
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
    content += "<form class=\"login\" method=\"post\" action=\"/session\"><label>Admin token<input "
               "type=\"password\" "
               "name=\"token\" "
               "placeholder=\"admin token\" autocomplete=\"current-password\" "
               "required></label><button "
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
                              const RepositoryHomeData& data,
                              bool signed_in) {
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
        forge_tabs(repository, "code"));
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
    content +=
        "</section></div><aside class=\"repository-sidebar\" aria-label=\"Repository "
        "details\"><section class=\"panel\"><h2>Clone and push</h2><p class=\"muted\">"
        "Use the repository URL with liquid.</p><pre><code>LIQUID_TOKEN=&lt;token&gt; liquid "
        "pull https://lake.live/v1/repositories/" +
        web_ui_escape(repository.id) +
        "</code></pre><pre><code>LIQUID_TOKEN=&lt;token&gt; liquid push "
        "https://lake.live/v1/repositories/" +
        web_ui_escape(repository.id) + "</code></pre></section>";
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
        row_link("/repository/access?id=" + repository.id, "View access details"),
        identifier_markup(repository.id),
        row_link("/repositories?customer_id=" + repository.customer_id, owner_name),
        web_ui_escape(repository.created_at),
        web_ui_escape(repository.updated_at));
    return web_ui_shell("lake — " + owner_name + "/" + repository.name,
                        owner_name + "/" + repository.name,
                        content,
                        signed_in);
}

std::string web_ui_repository(const control::Repository& repository,
                              const control::Customer* owner,
                              const std::vector<RepositoryContentRef>& refs,
                              bool signed_in) {
    RepositoryHomeData data;
    data.refs = refs;
    return web_ui_repository(repository, owner, data, signed_in);
}

std::string web_ui_repository_tree(const control::Repository& repository,
                                   std::string_view ref,
                                   const std::vector<RepositoryTreeEntry>& entries,
                                   bool signed_in) {
    std::string content = path_breadcrumbs(repository, ref, "") +
                          std::format("<p class=\"eyebrow\">repository tree</p><h1>{}</h1>"
                                      "<p class=\"sub\">Browse <code>{}</code> by path and last "
                                      "change.</p>{}",
                                      web_ui_escape(repository_name(repository)),
                                      web_ui_escape(ref),
                                      forge_tabs(repository, "code"));
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
    return web_ui_shell("lake — tree", repository_name(repository), content, signed_in);
}

std::string web_ui_repository_file(const control::Repository& repository,
                                   std::string_view ref,
                                   const RepositoryFile& file,
                                   const std::vector<RepositoryBlameLine>& blame,
                                   bool signed_in) {
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
                    forge_tabs(repository, "code"),
                    identifier_markup(file.blob),
                    byte_count(file.size),
                    syntax_language(file.path) ? file_language(file.path) : "Plain text",
                    source_view(file, blame));
    content +=
        R"(<script id="file-range-script">(()=>{const rows=[...document.querySelectorAll('[data-line]')];const select=()=>{rows.forEach(r=>r.classList.remove('selected'));const match=location.hash.match(/^#L(\d+)(?:-L(\d+))?$/);if(!match)return;const first=Number(match[1]);const last=Number(match[2]||match[1]);rows.forEach(r=>{const line=Number(r.dataset.line);if(line>=first&&line<=last)r.classList.add('selected');});};let anchor=null;document.addEventListener('click',event=>{const link=event.target.closest('[data-line-range]');if(!link)return;const line=Number(link.closest('[data-line]').dataset.line);if(event.shiftKey&&anchor!==null){event.preventDefault();const first=Math.min(anchor,line);const last=Math.max(anchor,line);location.hash=`L${first}-L${last}`;}else{anchor=line;}});addEventListener('hashchange',select);select();})();</script>)";
    return web_ui_shell("lake — file", file.path, content, signed_in);
}

std::string web_ui_repository_file(const control::Repository& repository,
                                   const RepositoryFile& file,
                                   bool signed_in) {
    return web_ui_repository_file(repository, "main", file, {}, signed_in);
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
                          const ChangeReviewData& review,
                          bool signed_in) {
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
        forge_tabs(repository, "changes"),
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
            content += std::format("<article class=\"panel check-row\"><strong>{}</strong>{}"
                                   "<span>{} attestations</span><span>{}</span></article>",
                                   web_ui_escape(check.action),
                                   badge_markup(check.exit_code == 0 ? "passed" : "failed"),
                                   check.attestations.size(),
                                   identifier_markup(check.id));
        }
        content += "</div>";
    }
    content +=
        R"(</section><script id="change-review-script">(()=>{const views=[...document.querySelectorAll('[data-diff-view]')];document.addEventListener('click',async event=>{const copy=event.target.closest('[data-copy]');if(copy){await navigator.clipboard.writeText(copy.dataset.copy);copy.textContent='Copied';return;}const toggle=event.target.closest('[data-view]');if(!toggle)return;views.forEach(view=>{view.hidden=view.dataset.diffView!==toggle.dataset.view;});document.querySelectorAll('[data-view]').forEach(button=>{button.setAttribute('aria-pressed',String(button===toggle));});});})();</script>)";
    return web_ui_shell("lake — change", repository_name(repository), content, signed_in);
}

std::string web_ui_change(const control::Repository& repository,
                          const RepositoryChange& change,
                          const std::vector<RepositoryDiffEntry>& differences,
                          bool signed_in) {
    return web_ui_change(repository, change, differences, {}, signed_in);
}

std::string web_ui_issues(const control::Repository& repository,
                          const std::vector<control::ForgeRecord>& issues,
                          std::string_view state,
                          std::string_view label,
                          std::string_view assignee,
                          bool signed_in) {
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
        forge_tabs(repository, "issues"),
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
    return web_ui_shell("lake — issues", repository_name(repository), content, signed_in);
}

std::string web_ui_issue(const control::Repository& repository,
                         const control::ForgeRecord& issue,
                         const std::vector<control::ForgeRecord>& comments,
                         bool signed_in) {
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
        forge_tabs(repository, "issues"),
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
    return web_ui_shell("lake — issue", repository_name(repository), content, signed_in);
}

std::string web_ui_releases(const control::Repository& repository,
                            const std::vector<control::ForgeRecord>& releases,
                            bool signed_in) {
    std::string content =
        std::format("<p class=\"eyebrow\">repository releases</p><h1>Releases</h1>"
                    "<p class=\"sub\">{}</p>{}",
                    web_ui_escape(repository_name(repository)),
                    forge_tabs(repository, "releases"));
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
    return web_ui_shell("lake — releases", repository_name(repository), content, signed_in);
}

std::string web_ui_webhooks(const control::Repository& repository,
                            const std::vector<control::ForgeRecord>& webhooks,
                            const std::vector<control::ForgeRecord>& deliveries,
                            bool signed_in) {
    std::string content =
        std::format("<p class=\"eyebrow\">repository automation</p><h1>Webhooks</h1>"
                    "<p class=\"sub\">{} · signed delivery history</p>{}",
                    web_ui_escape(repository_name(repository)),
                    forge_tabs(repository, "webhooks"));
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
            if (!delivered && signed_in) {
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
    return web_ui_shell("lake — webhooks", repository_name(repository), content, signed_in);
}

std::string web_ui_branch_protection(const control::Repository& repository,
                                     const std::vector<control::ForgeRecord>& rules,
                                     const std::vector<audit::Event>& events,
                                     bool signed_in) {
    std::string content =
        std::format("<p class=\"eyebrow\">repository policy</p><h1>Branch protection</h1>"
                    "<p class=\"sub\">{}</p>{}<section><h2>Rules</h2>",
                    web_ui_escape(repository_name(repository)),
                    forge_tabs(repository, "protection"));
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
    return web_ui_shell(
        "lake — branch protection", repository_name(repository), content, signed_in);
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
        content += "<table><thead><tr><th>Path</th><th>Conflicting blobs</th><th>Comparison</th>"
                   "</tr></thead><tbody>";
        for (const RepositoryConflict& conflict : conflicts) {
            std::string blobs;
            for (const std::string& blob : conflict.blobs) {
                if (!blobs.empty()) {
                    blobs += ", ";
                }
                blobs += blob;
            }
            content += std::format("<tr><td><code>{}</code></td><td><code>{}</code></td><td>{}</td>"
                                   "</tr>",
                                   web_ui_escape(conflict.path),
                                   web_ui_escape(blobs),
                                   row_link("/drive/conflict?repository_id=" + repository.id +
                                                "&ref=main&path=" + url_component(conflict.path),
                                            "Compare versions"));
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
    return web_ui_shell("lake — artifacts", repository_name(repository), content, signed_in);
}

std::string web_ui_artifact(const control::Repository& repository,
                            const RepositoryArtifact& artifact,
                            bool signed_in) {
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
                                   badge_markup(invitation.status),
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
    return web_ui_shell(
        "lake — application: " + application.name, application.pool_id, content, true);
}

std::string web_ui_application_users(const control::Application& application,
                                     const std::vector<control::ApplicationUser>& users) {
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
        "lake — application users", std::format("{} users", users.size()), content, true);
}

std::string web_ui_application_organizations(
    const control::Application& application,
    const std::vector<control::ApplicationOrganization>& organizations) {
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
                                const DriveFolderOptions& options,
                                bool signed_in) {
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
    if (signed_in) {
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
                       drive_entry_actions(repository, ref, entry, signed_in) + "</article>";
        } else {
            content += "<tr><td>" + row_link(href, name) + " " + badge_markup(entry.kind) +
                       "</td><td>" + web_ui_escape(size) + "</td><td>" + web_ui_escape(modified) +
                       "</td><td>" + drive_entry_actions(repository, ref, entry, signed_in) +
                       "</td></tr>";
        }
    }
    if (entries.empty()) {
        content += options.view == "grid"
                       ? "<div class=\"panel empty\">This folder is empty.</div>"
                       : "<tr><td colspan=\"4\" class=\"empty\">This folder is empty.</td></tr>";
    }
    content += options.view == "grid" ? "</div>" : "</tbody></table></div>";
    if (signed_in) {
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
    return web_ui_shell("lake — drive", repository_name(repository), content, signed_in);
}

std::string web_ui_drive_preview(const control::Repository& repository,
                                 std::string_view ref,
                                 const RepositoryFile& file,
                                 const DrivePreview& preview,
                                 bool signed_in) {
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
    return web_ui_shell("lake — preview", repository_name(repository), content, signed_in);
}

std::string web_ui_drive_history(const control::Repository& repository,
                                 std::string_view ref,
                                 std::string_view path,
                                 const std::vector<nlohmann::json>& changes,
                                 bool signed_in) {
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
        const std::string change_id = change.value("id", "");
        const bool has_file = !change.value("new_blob", "").empty();
        content += "<tr><td>" + web_ui_escape(change.value("occurred_at", "")) + "</td><td><code>" +
                   web_ui_escape(change_id) + "</code><br>" +
                   web_ui_escape(change.value("message", "")) + "</td><td>" +
                   web_ui_escape(joined(kinds)) + "</td>";
        if (has_file) {
            content += "<td><input aria-label=\"Compare from " + web_ui_escape(change_id) +
                       "\" type=\"radio\" name=\"left\" value=\"" + web_ui_escape(change_id) + "\"";
            content += version == 1 ? " checked" : "";
            content += "></td><td><input aria-label=\"Compare to " + web_ui_escape(change_id) +
                       "\" type=\"radio\" name=\"right\" value=\"" + web_ui_escape(change_id) +
                       "\"";
            content += version == 0 ? " checked" : "";
            content += "></td><td>";
            if (signed_in) {
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
    if (signed_in) {
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
    return web_ui_shell("lake — drive history", repository_name(repository), content, signed_in);
}

std::string web_ui_drive_version_diff(const control::Repository& repository,
                                      std::string_view ref,
                                      std::string_view path,
                                      std::string_view left_revision,
                                      std::string_view right_revision,
                                      const DriveVersionDiff& difference,
                                      bool signed_in) {
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
    return web_ui_shell(
        "lake — drive version diff", repository_name(repository), content, signed_in);
}

std::string web_ui_drive_conflict(const control::Repository& repository,
                                  std::string_view path,
                                  const std::vector<RepositoryFile>& versions,
                                  const std::optional<DriveVersionDiff>& difference,
                                  bool signed_in) {
    std::string content = "<p class=\"eyebrow\">drive conflict</p><h1>Two versions of " +
                          web_ui_escape(path) +
                          "</h1><p class=\"sub\">Both versions are preserved. Compare them before "
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
    return web_ui_shell("lake — drive conflict", repository_name(repository), content, signed_in);
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
    return web_ui_shell("lake — drive sharing", repository_name(repository), content, true);
}

std::string web_ui_drive_share_link(const control::Repository& repository,
                                    std::string_view ref,
                                    std::string_view path,
                                    const nlohmann::json& link,
                                    std::string_view error) {
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
    return web_ui_shell("lake — drive share link", repository_name(repository), content, true);
}

std::string web_ui_shared_with_me(const std::vector<nlohmann::json>& folders,
                                  std::string_view error) {
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
    return web_ui_shell("lake — shared with me", "drive", content, true);
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

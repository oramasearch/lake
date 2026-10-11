#include "lake/api/web_ui.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <map>

#include <liquid/hash.hpp>

#include "modules/frontends/common/syntax.hpp"

// What lake still renders on the server (docs/ui/web-app.md): the hosted sign-in pages an
// identity application shows its own users, the stylesheet and fonts they use, and a few pure
// helpers the capabilities share. The console is the web app in frontend/.
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

namespace {

constexpr std::string_view theme_preference_script =
    R"(<script id="theme-preference">(()=>{try{const t=localStorage.getItem('lake-theme');if(t==='light'||t==='dark')document.documentElement.dataset.theme=t;}catch{}})();</script>)";
constexpr std::string_view theme_toggle_script =
    R"(<script id="theme-toggle-script">(()=>{const b=document.querySelector('[data-theme-toggle]');if(!b)return;const current=()=>document.documentElement.dataset.theme||(matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light');const label=()=>{const t=current();b.setAttribute('aria-label',`Use ${t==='dark'?'light':'dark'} theme`);b.setAttribute('aria-pressed',String(t==='dark'));};b.addEventListener('click',()=>{const t=current()==='dark'?'light':'dark';document.documentElement.dataset.theme=t;try{localStorage.setItem('lake-theme',t);}catch{}label();});label();})();</script>)";

constexpr std::string_view theme_icon =
    R"(<svg class="icon" viewBox="0 0 16 16" aria-hidden="true" focusable="false">)"
    R"(<path d="M8 1a7 7 0 1 0 7 7 7 7 0 0 0-7-7Zm0 12V3a5 5 0 0 1 0 10Z"/></svg>)";

std::string stylesheet_link() {
    return std::format("<link rel=\"stylesheet\" href=\"{}\">", web_ui_stylesheet_url());
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

std::string error_markup(std::string_view error) {
    return error.empty() ? std::string()
                         : std::format("<p class=\"error\">{}</p>", web_ui_escape(error));
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
                       std::string(theme_icon) +
                       "<span class=\"sr-only\">Toggle theme</span></button></header>"
                       "<main class=\"hosted-main\" id=\"main-content\" tabindex=\"-1\"><div "
                       "class=\"hosted-card\">" +
                       std::string(content) + "</div></main>" + std::string(theme_toggle_script) +
                       "</body></html>";
    return html;
}

} // namespace

std::string web_ui_space_label(const control::Customer& customer, std::string_view user_id) {
    if (customer.customer_type == "individual" && !user_id.empty() && customer.user_id == user_id) {
        return "Your workspaces";
    }
    return customer.name;
}

std::optional<nlohmann::json> web_ui_source_units(std::string_view path,
                                                  std::string_view contents) {
    const auto language = syntax_language(path);
    if (!language) {
        return std::nullopt;
    }
    const auto* first = reinterpret_cast<const std::uint8_t*>(contents.data());
    nlohmann::json units = nlohmann::json::array();
    for (const liquid_frontend::syntax_unit& unit : liquid_frontend::parse_units(
             *language, std::span<const std::uint8_t>{first, contents.size()})) {
        units.push_back(
            {{"start_line", unit.start.line}, {"end_line", unit.end.line}, {"kind", unit.kind}});
    }
    return units;
}

// A handler's unexpected failure, to a browser (route_guard.hpp): a standalone page, since the
// console's own frame is the web app.
std::string web_ui_error(std::string_view code, std::string_view message) {
    return std::format(
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta name=\"color-scheme\" content=\"light dark\"><title>lake — error</title>{}{}"
        "</head><body class=\"hosted-shell\"><main class=\"hosted-main\" id=\"main-content\">"
        "<div class=\"hosted-card\" role=\"alert\"><p class=\"eyebrow\">{}</p>"
        "<h1>We Could Not Load This Page</h1><p class=\"sub\">{}</p>"
        "<p><a class=\"button primary\" href=\"/\">Back to lake</a></p></div></main></body></html>",
        theme_preference_script,
        stylesheet_link(),
        web_ui_escape(code),
        web_ui_escape(message));
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

} // namespace lake::api

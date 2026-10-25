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

// A coupon code an ad's link carries (`?code=`, lake#1156): the picker posts back to the same
// query and continues to it on the chosen region's host, so the code goes along; the page says so.
// Only letters, digits and hyphens are shown; anything else shows nothing.
std::string coupon_note(std::string_view target) {
    const auto query = target.find('?');
    if (query == std::string_view::npos) {
        return {};
    }
    std::string_view rest = target.substr(query + 1);
    while (!rest.empty()) {
        const auto amp = rest.find('&');
        const std::string_view pair = rest.substr(0, amp);
        rest = amp == std::string_view::npos ? std::string_view() : rest.substr(amp + 1);
        if (!pair.starts_with("code=")) {
            continue;
        }
        const std::string_view code = pair.substr(5);
        if (code.empty() || code.size() > 64 || !std::ranges::all_of(code, [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) || c == '-';
            })) {
            return {};
        }
        return std::format("<p class=\"notice\" data-coupon>Your code <strong>{}</strong> goes "
                           "with you: it is applied when you sign up in the region you "
                           "choose.</p>",
                           web_ui_escape(code));
    }
    return {};
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
        "<meta name=\"color-scheme\" content=\"light dark\"><title>Lake — error</title>{}{}"
        "</head><body class=\"hosted-shell\"><main class=\"hosted-main\" id=\"main-content\">"
        "<div class=\"hosted-card\" role=\"alert\"><p class=\"eyebrow\">{}</p>"
        "<h1>We Could Not Load This Page</h1><p class=\"sub\">{}</p>"
        "<p><a class=\"button primary\" href=\"/\">Back to Lake</a></p></div></main></body></html>",
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

std::string web_ui_application_identity_proof(const control::Application& application,
                                              const WebUiIdentityProof& proof,
                                              std::string_view error) {
    const std::string base = "/apps/" + web_ui_escape(application.id);
    const std::string command = "liquid sign artifact " + proof.challenge;
    std::string content = "<p class=\"eyebrow\">identity proof</p><h1>Prove You Hold Your Key</h1>";
    content += error_markup(error);
    content += "<p>Your identity document is public, so sign this challenge with your liquid "
               "identity within five minutes and paste everything the command prints.</p><pre>" +
               web_ui_escape(command) + "</pre><form class=\"stack\" method=\"post\" action=\"" +
               base + web_ui_escape(proof.action) + "\">";
    for (const auto& [name, value] : proof.fields) {
        content += "<input type=\"hidden\" name=\"" + web_ui_escape(name) + "\" value=\"" +
                   web_ui_escape(value) + "\">";
    }
    content += "<input type=\"hidden\" name=\"identity_nonce\" value=\"" +
               web_ui_escape(proof.nonce) +
               "\"><input type=\"hidden\" name=\"identity_expires_at\" value=\"" +
               std::to_string(proof.expires_at) + "\">";
    if (proof.password) {
        content += "<label>Password<input type=\"password\" name=\"password\" "
                   "autocomplete=\"new-password\" minlength=\"12\" required></label>";
    }
    content += "<label>What liquid sign printed<textarea name=\"identity_signature\" "
               "spellcheck=\"false\" required></textarea></label><button class=\"primary\" "
               "type=\"submit\">Continue</button></form>";
    return application_shell(application, application.name + " — identity proof", content);
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

std::string web_ui_region_picker(const Regions& regions,
                                 const Region& selected,
                                 std::string_view target,
                                 std::string_view error) {
    std::string choices;
    for (const Region& region : regions.list) {
        const bool chosen = region.name == selected.name;
        choices += std::format(
            "<label class=\"region-choice\"><input type=\"radio\" name=\"region\" value=\"{}\"{}>"
            "<span><strong>{}</strong> <span class=\"muted\">{}</span></span></label>",
            web_ui_escape(region.name),
            chosen ? " checked" : "",
            web_ui_escape(region.label),
            web_ui_escape(region.host));
    }
    const std::string action = web_ui_escape(target.empty() ? std::string_view("/") : target);
    return std::format(
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta name=\"color-scheme\" content=\"light dark\"><title>Lake — Choose a Region</title>"
        "{}{}<style>:root{{--app-accent:var(--lake-action);--app-background:var(--lake-canvas)}}"
        "</style></head><body class=\"hosted-shell\">"
        "<a class=\"skip-link\" href=\"#main-content\">Skip to content</a>"
        "<header class=\"hosted-header\"><span class=\"hosted-brand\"><span "
        "class=\"hosted-brand-mark\" aria-hidden=\"true\"></span>Lake</span></header>"
        "<main class=\"hosted-main\" id=\"main-content\" tabindex=\"-1\"><div "
        "class=\"hosted-card\">"
        "<p class=\"eyebrow\">Lake by Orama</p><h1>Choose a Region</h1>"
        "<p class=\"sub\">Each region is a separate Lake with its own accounts. Your workspaces "
        "stay "
        "in the region you create them in.</p>{}"
        "<form class=\"stack\" method=\"post\" action=\"{}\"><fieldset class=\"region-choices\">"
        "<legend>Region</legend>{}</fieldset>"
        "<button class=\"primary\" type=\"submit\">Continue</button></form></div></main>"
        "</body></html>",
        theme_preference_script,
        stylesheet_link(),
        error_markup(error) + coupon_note(target),
        action,
        choices);
}

namespace {

// The apex pages' frame (lake#831): the region picker's header and colours, `wide` for the order
// sheet's cards.
std::string apex_page(std::string_view title, std::string_view content, bool wide = false) {
    return std::format(
        "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta name=\"color-scheme\" content=\"light dark\"><title>{}</title>"
        "{}{}<style>:root{{--app-accent:var(--lake-action);--app-background:var(--lake-canvas)}}"
        "</style></head><body class=\"hosted-shell\">"
        "<a class=\"skip-link\" href=\"#main-content\">Skip to content</a>"
        "<header class=\"hosted-header\"><a class=\"hosted-brand\" href=\"/\"><span "
        "class=\"hosted-brand-mark\" aria-hidden=\"true\"></span>Lake</a></header>"
        "<main class=\"hosted-main{}\" id=\"main-content\" tabindex=\"-1\">{}</main>"
        "</body></html>",
        web_ui_escape(title),
        theme_preference_script,
        stylesheet_link(),
        wide ? " order-sheet-main" : "",
        content);
}

std::string region_choices(const Regions& regions, const Region& selected) {
    std::string choices;
    for (const Region& region : regions.list) {
        const bool chosen = region.name == selected.name;
        choices += std::format(
            "<label class=\"region-choice\"><input type=\"radio\" name=\"region\" value=\"{}\"{}>"
            "<span><strong>{}</strong> <span class=\"muted\">{}</span></span></label>",
            web_ui_escape(region.name),
            chosen ? " checked" : "",
            web_ui_escape(region.label),
            web_ui_escape(region.host));
    }
    return choices;
}

std::string bullet_list(const std::vector<std::string>& items) {
    if (items.empty()) {
        return {};
    }
    std::string out = "<ul class=\"order-features\">";
    for (const std::string& item : items) {
        out += "<li>" + web_ui_escape(item) + "</li>";
    }
    return out + "</ul>";
}

std::string offer_price(const OrderOffer& offer) {
    return std::format("<p class=\"order-price\">{}{} <small>USD / month</small></p>",
                       offer.starting_from ? "<small>Starting from</small> " : "",
                       web_ui_escape(format_usd(offer.monthly_cents)));
}

std::string offer_credit(const OrderOffer& offer) {
    if (offer.credit_percent == 0) {
        return "<p class=\"order-credit\">Usage is billed at the published price list.</p>";
    }
    return std::format(
        "<p class=\"order-credit\">Includes {}{} USD of usage credit each month ({}% of the "
        "fee). Usage beyond it is billed at the published price list.</p>",
        offer.starting_from ? "at least " : "",
        web_ui_escape(format_usd(included_credit_cents(offer))),
        offer.credit_percent);
}

// The regions beyond the shared ones a dedicated cluster can run in, by what a person reads
// before the parenthesis ("Canada", "Asia"), in order.
std::string region_areas(const OrderSheet& sheet) {
    std::vector<std::string> areas;
    for (const OrderProvider& provider : sheet.providers) {
        for (const OrderRegion& region : provider.regions) {
            std::string area = region.label.substr(0, region.label.find(" ("));
            if (std::ranges::find(areas, area) == areas.end()) {
                areas.push_back(area);
            }
        }
    }
    std::string out;
    for (std::size_t index = 0; index < areas.size(); ++index) {
        out += (index == 0 ? "" : index + 1 == areas.size() ? " and " : ", ") + areas[index];
    }
    return out;
}

std::string field_error(const OrderFormErrors& errors, std::string_view field) {
    const auto found = errors.find(std::string(field));
    return found == errors.end() ? std::string()
                                 : std::format("<span class=\"error\" id=\"error-{}\">{}</span>",
                                               web_ui_escape(field),
                                               web_ui_escape(found->second));
}

std::string described_by(const OrderFormErrors& errors, std::string_view field) {
    return errors.contains(std::string(field))
               ? std::format(" aria-invalid=\"true\" aria-describedby=\"error-{}\"",
                             web_ui_escape(field))
               : std::string();
}

std::string selected_if(bool chosen) {
    return chosen ? " selected" : "";
}

std::string
order_summary(const OrderSheet& sheet, const Regions& regions, const control::CloudOrder& order) {
    const OrderOffer* offer = sheet.find_offer(order.offer);
    std::string rows;
    const auto row = [&rows](std::string_view name, std::string_view value) {
        rows += std::format("<dt>{}</dt><dd>{}</dd>", web_ui_escape(name), web_ui_escape(value));
    };
    row("Order", order.id);
    row("Offer", offer != nullptr ? offer->title : order.offer);
    row("Where", order_location_label(sheet, regions, order));
    row("Quantity", std::to_string(order.quantity));
    row("Price",
        std::format("{}{} USD / month",
                    offer != nullptr && offer->starting_from ? "From " : "",
                    format_usd(order.amount_cents)));
    row("Included usage credit", format_usd(order.included_credit_cents) + " USD / month");
    row("Contact", order.contact_name + " <" + order.contact_email + ">");
    if (!order.company.empty()) {
        row("Company", order.company);
    }
    if (!order.details.empty()) {
        row("Details", order.details);
    }
    return "<dl class=\"order-summary\">" + rows + "</dl>";
}

} // namespace

std::string web_ui_order_sheet(const OrderSheet& sheet,
                               const Regions& regions,
                               const Region& selected,
                               std::string_view target,
                               std::string_view error) {
    std::string trust;
    for (const OrderTrustClaim& claim : sheet.trust) {
        trust += std::format(
            "<li><a href=\"{}\">{}</a></li>", web_ui_escape(claim.url), web_ui_escape(claim.text));
    }
    if (!trust.empty()) {
        trust = "<ul class=\"order-trust\" aria-label=\"Compliance and source\">" + trust + "</ul>";
    }
    const std::string price_list =
        sheet.price_list_url.empty() ? std::string("/order/price-list") : sheet.price_list_url;
    const std::string action = web_ui_escape(target.empty() ? std::string_view("/") : target);
    std::string cards = std::format(
        "<section class=\"order-offer\" id=\"shared\" aria-labelledby=\"offer-shared\">"
        "<h2 id=\"offer-shared\">{}</h2>{}"
        "<p class=\"order-price\">Usage-based <small>USD, by the minute</small></p>"
        "<p class=\"order-credit\"><a href=\"{}\">The published price list</a></p>"
        "<p class=\"muted\">{}</p>{}"
        "<form class=\"stack\" method=\"post\" action=\"{}\"><fieldset class=\"region-choices\">"
        "<legend>Region</legend>{}</fieldset>"
        "<button class=\"primary\" type=\"submit\">Continue</button></form></section>",
        web_ui_escape(sheet.shared_title),
        sheet.show_promise ? "<p class=\"order-promise\">" + web_ui_escape(sheet.promise) + "</p>"
                           : std::string(),
        web_ui_escape(price_list),
        web_ui_escape(sheet.shared_summary),
        bullet_list(sheet.shared_features) + coupon_note(target),
        action,
        region_choices(regions, selected));
    for (const OrderOffer& offer : sheet.offers) {
        const std::string slug = offer_slug(offer.id);
        std::string where;
        if (offer.id == "dedicated_cluster") {
            std::string providers;
            for (const OrderProvider& provider : sheet.providers) {
                providers += (providers.empty() ? "" : ", ") + provider.label;
            }
            where = "<p class=\"muted\">" +
                    web_ui_escape(providers + ", in " + region_areas(sheet) + ".") + "</p>";
        }
        cards += std::format(
            "<section class=\"order-offer\" id=\"{0}\" aria-labelledby=\"offer-{0}\">"
            "<h2 id=\"offer-{0}\">{1}</h2>{2}{3}<p class=\"muted\">{4}</p>{5}{6}"
            "<p class=\"order-actions\"><a class=\"button button-primary\" href=\"/order/{0}\">{7}"
            "</a></p></section>",
            web_ui_escape(slug),
            web_ui_escape(offer.title),
            offer_price(offer),
            offer_credit(offer),
            web_ui_escape(offer.summary),
            where,
            bullet_list(offer.features),
            offer.checkout ? "Order" : "Send an order form");
    }
    std::string content = std::format(
        "<p class=\"eyebrow\">Lake by Orama</p><h1>Get Lake</h1>"
        "<p class=\"sub\">Every way to run Lake, with its price. Prices are in US dollars (USD). "
        "Each region is a separate Lake with its own accounts; your workspaces stay in the region "
        "you create them in.</p>{}{}<div class=\"order-sheet\">{}</div>"
        "<p class=\"order-contact\">Need a cloud provider, hardware or region that is not listed, "
        "or a larger order? <a href=\"/order/dedicated-cluster?location=other\">Contact us</a> "
        "with the order form.</p>",
        trust,
        error_markup(error),
        cards);
    return apex_page("Lake — Get Lake", content, true);
}

std::string web_ui_order_form(const OrderSheet& sheet,
                              const Regions& regions,
                              const OrderOffer& offer,
                              const OrderFormInput& values,
                              const OrderFormErrors& errors,
                              bool checkout_available) {
    std::string location;
    if (offer.id == "private_warehouse") {
        std::string options;
        for (const Region& region : regions.list) {
            options += std::format("<option value=\"{}\"{}>{}</option>",
                                   web_ui_escape(region.name),
                                   selected_if(values.location == region.name),
                                   web_ui_escape(region.label));
        }
        location = std::format(
            "<label>Region<select name=\"location\"{}>{}<option value=\"other\"{}>Another region "
            "(tell us below)</option></select>{}</label>",
            described_by(errors, "location"),
            options,
            selected_if(values.location == "other"),
            field_error(errors, "location"));
    } else if (offer.id == "dedicated_cluster") {
        std::string groups;
        for (const OrderProvider& provider : sheet.providers) {
            std::string options;
            for (const OrderRegion& region : provider.regions) {
                const std::string value = provider.id + ":" + region.id;
                options += std::format(
                    "<option value=\"{}\"{}>{}{}</option>",
                    web_ui_escape(value),
                    selected_if(values.location == value),
                    web_ui_escape(region.label),
                    provider.checkout && !region.checkout
                        ? " (order form: we'll confirm availability)"
                        : "");
            }
            groups += std::format("<optgroup label=\"{}{}\">{}</optgroup>",
                                  web_ui_escape(provider.label),
                                  provider.checkout ? "" : " (order form)",
                                  options);
        }
        location = std::format(
            "<label>Cloud provider and region<select name=\"location\"{}>{}<optgroup "
            "label=\"Not listed\"><option value=\"other\"{}>Another provider, hardware or region "
            "(tell us below)</option></optgroup></select>{}</label>",
            described_by(errors, "location"),
            groups,
            selected_if(values.location == "other"),
            field_error(errors, "location"));
    }
    const bool pay = offer.checkout && checkout_available;
    std::string actions;
    if (pay) {
        actions += std::format(
            "<button class=\"primary\" type=\"submit\" name=\"path\" value=\"checkout\">Pay {} "
            "USD / month with card</button>",
            web_ui_escape(format_usd(offer.monthly_cents)));
    }
    actions +=
        std::format("<button{} type=\"submit\" name=\"path\" value=\"order_form\">Send as an order "
                    "form</button>",
                    pay ? "" : " class=\"primary\"");
    std::string note;
    if (offer.checkout) {
        note = pay ? "Paying starts a monthly subscription through Stripe; we set it up once the "
                     "payment is in. Sending the order form skips payment: we get in touch, and "
                     "nothing is set up until we have spoken."
                   : "Card payment is not available right now. Send the order form and we will "
                     "be in touch; nothing is set up or charged until we have spoken.";
        note += " Providers and regions marked \"order form\" and anything not listed are ordered "
                "with the order form.";
    } else {
        note = "We get in touch to plan it with you. Nothing is charged by sending this.";
    }
    const std::string quantity = values.quantity.empty() ? "1" : values.quantity;
    std::string content = std::format(
        "<p class=\"eyebrow\"><a href=\"/#{}\">Get Lake</a></p><h1>{}</h1>{}{}"
        "<p class=\"muted\">{}</p>{}"
        "<form class=\"stack order-form\" method=\"post\" action=\"/order\" novalidate>"
        "<input type=\"hidden\" name=\"offer\" value=\"{}\">{}"
        "<label>Your name<input name=\"contact_name\" autocomplete=\"name\" maxlength=\"200\" "
        "required value=\"{}\"{}>{}</label>"
        "<label>Email<input type=\"email\" name=\"contact_email\" autocomplete=\"email\" "
        "maxlength=\"254\" required value=\"{}\"{}>{}</label>"
        "<label>Company<input name=\"company\" autocomplete=\"organization\" maxlength=\"200\" "
        "value=\"{}\"{}>{}</label>"
        "<label>Quantity<input type=\"number\" name=\"quantity\" min=\"1\" max=\"1000\" "
        "inputmode=\"numeric\" value=\"{}\"{}>{}</label>"
        "<label>{}<textarea name=\"details\" maxlength=\"4000\"{}>{}</textarea>{}</label>"
        "<div class=\"order-honeypot\" aria-hidden=\"true\"><label>Leave this empty"
        "<input name=\"website\" tabindex=\"-1\" autocomplete=\"off\"></label></div>"
        "{}<p class=\"order-actions\">{}</p><p class=\"muted\">{}</p></form>",
        web_ui_escape(offer_slug(offer.id)),
        web_ui_escape(offer.title),
        offer_price(offer),
        offer_credit(offer),
        web_ui_escape(offer.summary),
        bullet_list(offer.features),
        web_ui_escape(offer.id),
        location,
        web_ui_escape(values.contact_name),
        described_by(errors, "contact_name"),
        field_error(errors, "contact_name"),
        web_ui_escape(values.contact_email),
        described_by(errors, "contact_email"),
        field_error(errors, "contact_email"),
        web_ui_escape(values.company),
        described_by(errors, "company"),
        field_error(errors, "company"),
        web_ui_escape(quantity),
        described_by(errors, "quantity"),
        field_error(errors, "quantity"),
        offer.id == "on_prem"
            ? "Where it runs: cloud providers or bare metal, accelerators, regions, multi-region"
            : "Anything else: another provider, hardware, regions, invoicing or procurement",
        described_by(errors, "details"),
        web_ui_escape(values.details),
        field_error(errors, "details"),
        field_error(errors, "path").empty()
            ? std::string()
            : "<p class=\"error\">" + web_ui_escape(errors.at("path")) + "</p>",
        actions,
        web_ui_escape(note));
    return apex_page("Lake — Order " + offer.title, content);
}

std::string web_ui_order_received(const OrderSheet& sheet,
                                  const Regions& regions,
                                  const control::CloudOrder& order,
                                  std::string_view status_url) {
    std::string content = std::format(
        "<div class=\"hosted-card\"><p class=\"eyebrow\">Order {}</p><h1>Order Received</h1>"
        "<p class=\"sub\">Thank you. Your order form has reached our support team, and a copy is "
        "on its way to {}. We will be in touch; nothing is charged or set up until we have "
        "spoken.</p>{}<p><a href=\"{}\">Your order's status</a></p></div>",
        web_ui_escape(order.id),
        web_ui_escape(order.contact_email),
        order_summary(sheet, regions, order),
        web_ui_escape(status_url));
    return apex_page("Lake — Order Received", content);
}

std::string web_ui_order_status(const OrderSheet& sheet,
                                const Regions& regions,
                                const control::CloudOrder& order,
                                std::string_view checkout) {
    std::string title;
    std::string message;
    const std::string& state = order.state;
    if (state == "submitted") {
        title = "Order Received";
        message = "Your order form reached our support team. We will be in touch; nothing is "
                  "charged or set up until we have spoken.";
    } else if (state == "pending_payment") {
        title = "Waiting for Payment";
        message = checkout == "cancelled"
                      ? "The payment was not completed, so nothing is charged and nothing will be "
                        "set up. You can order again from the order sheet, or send it as an order "
                        "form."
                      : "We are waiting for Stripe to confirm the payment. This page shows the "
                        "order as paid once it does.";
    } else if (state == "paid") {
        title = "Paid";
        message = "Your payment is in. We are about to set it up, and will email you when it is "
                  "ready.";
    } else if (state == "provisioning") {
        title = "Setting Up";
        message = "We are setting it up now, and will email you when it is ready.";
    } else if (state == "ready") {
        title = "Ready";
        message = "It is ready.";
    } else if (state == "failed") {
        title = "Something Went Wrong";
        message = "We could not finish setting it up. Our support team has been told and will be "
                  "in touch.";
    } else {
        title = "Cancelled";
        message = "This order was cancelled. Nothing is charged and nothing will be set up.";
    }
    std::string extra;
    if (state == "ready" && !order.host.empty()) {
        extra = std::format("<p><a class=\"button button-primary\" href=\"https://{0}/\">Open "
                            "{0}</a></p>",
                            web_ui_escape(order.host));
    }
    std::string content = std::format(
        "<div class=\"hosted-card\"><p class=\"eyebrow\">Order {}</p><h1>{}</h1>"
        "<p class=\"sub\" role=\"status\">{}</p>{}{}<p class=\"muted\">Questions: reply to "
        "the email we sent you, quoting your order id.</p></div>",
        web_ui_escape(order.id),
        web_ui_escape(title),
        web_ui_escape(message),
        extra,
        order_summary(sheet, regions, order));
    return apex_page("Lake — Order " + title, content);
}

std::string web_ui_order_message(std::string_view title, std::string_view message) {
    return apex_page(std::string("Lake — ") + std::string(title),
                     std::format("<div class=\"hosted-card\"><h1>{}</h1><p class=\"sub\">{}</p>"
                                 "<p><a href=\"/\">Back to the order sheet</a></p></div>",
                                 web_ui_escape(title),
                                 web_ui_escape(message)));
}

std::string web_ui_price_list(const std::vector<PriceListRow>& rows,
                              std::string_view effective_from) {
    std::string body;
    if (rows.empty()) {
        body = "<p class=\"sub\">The price list is not published yet. Send us an order form from "
               "the order sheet for prices.</p>";
    } else {
        std::string table;
        for (const PriceListRow& row : rows) {
            table += std::format("<tr><th scope=\"row\">{}</th><td>{}</td></tr>",
                                 web_ui_escape(row.title),
                                 web_ui_escape(row.rate));
        }
        body = std::format(
            "<p class=\"sub\">Shared regions are billed by usage only, at these rates, in US "
            "dollars (USD), in force from {}. Paid offers include usage credit; usage beyond it "
            "is billed at these rates.</p><table class=\"order-prices\"><thead><tr><th "
            "scope=\"col\">Usage</th><th scope=\"col\">Price (USD)</th></tr></thead><tbody>{}"
            "</tbody></table>",
            web_ui_escape(effective_from),
            table);
    }
    return apex_page("Lake — Price List",
                     "<div class=\"hosted-card\"><p class=\"eyebrow\"><a href=\"/\">Get "
                     "Lake</a></p><h1>Price List</h1>" +
                         body + "</div>");
}

} // namespace lake::api

// The instance brand (lake#1135, lake#1137, docs/serving/brand.md): read and validated once at
// start, then read by every surface that names the product.

#include "lake/api/brand.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "lake/api/resend.hpp"
#include "lake/api/web_ui.hpp"

namespace lake::api {
namespace {

// The largest image a brand may carry: each is inlined into every page that shows it.
constexpr std::size_t max_image_bytes = 256 * 1024;

bool word_character(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// Every occurrence of `from` in `text` as a whole word replaced by `to`. `from` is plain text.
std::string replace_words(std::string_view text, std::string_view from, std::string_view to) {
    std::string out;
    out.reserve(text.size());
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t found = text.find(from, at);
        if (found == std::string_view::npos) {
            break;
        }
        const bool starts = found == 0 || !word_character(text[found - 1]);
        const std::size_t end = found + from.size();
        const bool ends = end == text.size() || !word_character(text[end]);
        out.append(text.substr(at, found - at));
        if (starts && ends) {
            out.append(to);
        } else {
            out.append(from);
        }
        at = end;
    }
    out.append(text.substr(std::min(at, text.size())));
    return out;
}

std::string base64(std::string_view bytes) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const auto n = (static_cast<unsigned char>(bytes[i]) << 16) |
                       (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                       static_cast<unsigned char>(bytes[i + 2]);
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += alphabet[(n >> 6) & 63];
        out += alphabet[n & 63];
    }
    if (i + 1 == bytes.size()) {
        const auto n = static_cast<unsigned char>(bytes[i]) << 16;
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const auto n = (static_cast<unsigned char>(bytes[i]) << 16) |
                       (static_cast<unsigned char>(bytes[i + 1]) << 8);
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += alphabet[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

// A name shown as text: 1 to `limit` bytes of printable text, without the characters that would
// need escaping in HTML, JSON or Markdown (so no surface has to trust its escaping alone).
std::optional<std::string> name_problem(std::string_view value, std::size_t limit) {
    if (value.empty() || value.size() > limit) {
        return std::format("must be 1 to {} characters", limit);
    }
    if (value.front() == ' ' || value.back() == ' ') {
        return "must not start or end with a space";
    }
    for (const char c : value) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f || c == '<' || c == '>' || c == '&' || c == '"' || c == '\\' ||
            c == '`' || c == '*' || c == '[' || c == ']') {
            return "must not contain control characters or any of < > & \" \\ ` * [ ]";
        }
    }
    return std::nullopt;
}

bool short_name_ok(std::string_view value) {
    return !value.empty() && value.size() <= 32 && value.front() >= 'a' && value.front() <= 'z' &&
           value.back() != '-' && std::ranges::all_of(value, [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
           });
}

// "Acme Data" -> "acme-data": the short name a brand gets when it names none.
std::string short_name_of(std::string_view name) {
    std::string out;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) != 0 && u < 0x80) {
            out += static_cast<char>(std::tolower(u));
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
    }
    while (!out.empty() && (out.back() == '-')) {
        out.pop_back();
    }
    while (!out.empty() && !(out.front() >= 'a' && out.front() <= 'z')) {
        out.erase(out.begin());
    }
    if (out.size() > 32) {
        out.resize(32);
        while (!out.empty() && out.back() == '-') {
            out.pop_back();
        }
    }
    return out;
}

bool colour_ok(std::string_view value) {
    return (value.size() == 7 || value.size() == 4) && value.front() == '#' &&
           std::ranges::all_of(value.substr(1),
                               [](unsigned char c) { return std::isxdigit(c) != 0; });
}

bool https_url_ok(std::string_view value) {
    if (!value.starts_with("https://") || value.size() <= 8 || value.size() > 2048) {
        return false;
    }
    return std::ranges::none_of(value, [](unsigned char c) {
        return c <= ' ' || c == 0x7f || c == '"' || c == '\'' || c == '<' || c == '>' ||
               c == '\\' || c == '`' || c == '(' || c == ')';
    });
}

constexpr std::array<std::pair<std::string_view, std::string_view>, 7> image_types{{
    {".png", "image/png"},
    {".svg", "image/svg+xml"},
    {".ico", "image/x-icon"},
    {".webp", "image/webp"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
}};

// An image setting as a data: URI: one given as a base64 data:image/... URI as it is, or a file
// (relative paths are read against the brand file's directory) read and inlined.
std::expected<std::string, std::string>
image(std::string_view setting, std::string_view value, const std::filesystem::path& base) {
    if (value.empty()) {
        return std::string();
    }
    if (value.starts_with("data:")) {
        const std::size_t comma = value.find(',');
        const std::string_view header =
            comma == std::string_view::npos ? value : value.substr(0, comma);
        const bool typed = std::ranges::any_of(image_types, [&](const auto& type) {
            return header == std::string("data:") + std::string(type.second) + ";base64";
        });
        const std::string_view data =
            comma == std::string_view::npos ? std::string_view{} : value.substr(comma + 1);
        if (!typed || data.empty() || data.size() > max_image_bytes * 4 / 3 + 4 ||
            !std::ranges::all_of(data, [](unsigned char c) {
                return std::isalnum(c) != 0 || c == '+' || c == '/' || c == '=';
            })) {
            return std::unexpected(std::format(
                "{} must be a base64 data:image/(png|svg+xml|x-icon|webp|jpeg|gif) URI of at most "
                "{} bytes, or a path to such an image file",
                setting,
                max_image_bytes));
        }
        return std::string(value);
    }
    std::filesystem::path path{std::string(value)};
    if (path.is_relative()) {
        path = base / path;
    }
    std::string extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const auto type = std::ranges::find(image_types,
                                        std::string_view(extension),
                                        &std::pair<std::string_view, std::string_view>::first);
    if (type == image_types.end()) {
        return std::unexpected(std::format(
            "{} must name a .png, .svg, .ico, .webp, .jpg or .gif file, or be a data:image URI",
            setting));
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(std::format("{}: cannot read {}", setting, path.string()));
    }
    std::ostringstream bytes;
    bytes << in.rdbuf();
    const std::string contents = std::move(bytes).str();
    if (contents.empty() || contents.size() > max_image_bytes) {
        return std::unexpected(
            std::format("{}: {} must be 1 to {} bytes", setting, path.string(), max_image_bytes));
    }
    return std::format("data:{};base64,{}", type->second, base64(contents));
}

// The address of a From value: "Name <address>" or a bare address.
std::string_view address_of(std::string_view from) {
    const std::size_t open = from.rfind('<');
    if (open != std::string_view::npos && from.ends_with('>')) {
        return from.substr(open + 1, from.size() - open - 2);
    }
    return from;
}

std::string lowercase(std::string_view text) {
    std::string out(text);
    std::ranges::transform(
        out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// An address's domain, lower case ("" for none).
std::string domain_of(std::string_view address) {
    const std::string lower = lowercase(address_of(address));
    const std::size_t at = lower.rfind('@');
    return at == std::string::npos ? std::string() : lower.substr(at + 1);
}

// Whether an address is on the domain of this Lake's own sender or support address
// (LAKE_RESEND_FROM, LAKE_SUPPORT_EMAIL), or beneath it: a custom brand must use its own.
bool lake_address(std::string_view address,
                  std::string_view resend_from,
                  std::string_view support_email) {
    const std::string domain = domain_of(address);
    if (domain.empty()) {
        return false;
    }
    for (const std::string_view own_address : {resend_from, support_email}) {
        const std::string own = domain_of(own_address);
        if (!own.empty() && (domain == own || domain.ends_with("." + own))) {
            return true;
        }
    }
    return false;
}

// The action-colour tokens of the design system (tools/ui/build-tokens.mjs), set from one colour:
// hover and pressed are darker in light and lighter in dark, soft is the colour faint.
std::string action_tokens(std::string_view colour, bool dark) {
    const std::string_view toward = dark ? "white" : "black";
    return std::format(
        "--orama-action-default:{0};--orama-action-filled:{0};--orama-action-focus:{0};"
        "--orama-action-hover:color-mix(in srgb,{0} 82%,{1});"
        "--orama-action-pressed:color-mix(in srgb,{0} 68%,{1});"
        "--orama-action-filled-hover:color-mix(in srgb,{0} 82%,black);"
        "--orama-action-filled-pressed:color-mix(in srgb,{0} 68%,black);"
        "--orama-action-soft:color-mix(in srgb,{0} {2}%,transparent)",
        colour,
        toward,
        dark ? 28 : 12);
}

struct Installed {
    std::mutex mutex;
    std::vector<std::unique_ptr<const Brand>> kept; // every brand ever installed, never freed
    std::atomic<const Brand*> current{nullptr};
};

Installed& installed() {
    static Installed state;
    return state;
}

const Brand& lake_brand() {
    static const Brand brand;
    return brand;
}

} // namespace

std::string Brand::eyebrow() const {
    return by_orama ? name + " by Orama" : name;
}

std::string Brand::apply(std::string_view text) const {
    if (!custom) {
        return std::string(text);
    }
    std::string out(text);
    // Longest first, so a phrase is rewritten before the words in it.
    out = replace_words(out, "lake.live is prepaid", name + " is prepaid when billing is on");
    out = replace_words(out, "Lake by Orama", eyebrow());
    out = replace_words(out, "Lake Console", console_title);
    out = replace_words(out, "Ask Orama", assistant_name);
    out = replace_words(out, "The Orama operator", "The operator");
    out = replace_words(out, "the Orama operator", "the operator");
    out = replace_words(out, "Orama operator", "operator");
    out = replace_words(out, "The orama-operator", "The operator");
    out = replace_words(out, "the orama-operator", "the operator");
    out = replace_words(out, "Orama", company);
    out = replace_words(out, "Lake", name);
    return out;
}

void Brand::apply_to_documentation(nlohmann::json& document) const {
    if (!custom) {
        return;
    }
    if (document.is_array()) {
        for (nlohmann::json& item : document) {
            apply_to_documentation(item);
        }
        return;
    }
    if (!document.is_object()) {
        return;
    }
    for (auto& [key, value] : document.items()) {
        if (value.is_string() && (key == "title" || key == "summary" || key == "description")) {
            value = apply(value.get_ref<const std::string&>());
        } else if (value.is_structured()) {
            apply_to_documentation(value);
        }
    }
}

std::string Brand::head_markup() const {
    std::string out;
    if (!favicon.empty()) {
        out += "<link rel=\"icon\" href=\"" + web_ui_escape(favicon) + "\">";
    }
    if (!accent.empty()) {
        const std::string dark = accent_dark.empty() ? accent : accent_dark;
        // The same selectors as the generated token layer, unlayered and after it, so these win
        // in the web app (whose tokens are in a cascade layer) and on lake's own pages alike.
        out += std::format("<style id=\"lake-brand\">:root,[data-theme=\"light\"]{{{}}}"
                           "@media (prefers-color-scheme: dark){{:root:not([data-theme=\"light\"])"
                           "{{{}}}}}:root[data-theme=\"dark\"]{{{}}}</style>",
                           action_tokens(accent, false),
                           action_tokens(dark, true),
                           action_tokens(dark, true));
    }
    return out;
}

std::string Brand::footer_markup() const {
    if (legal_name.empty() && terms_url.empty() && privacy_url.empty()) {
        return {};
    }
    std::string out = "<footer class=\"hosted-footer\">";
    if (!legal_name.empty()) {
        out += "<span>© " + web_ui_escape(legal_name) + "</span>";
    }
    if (!terms_url.empty()) {
        out += " <a href=\"" + web_ui_escape(terms_url) + "\">Terms</a>";
    }
    if (!privacy_url.empty()) {
        out += " <a href=\"" + web_ui_escape(privacy_url) + "\">Privacy</a>";
    }
    out += "</footer>";
    return out;
}

nlohmann::json Brand::to_json() const {
    const auto or_null = [](const std::string& value) {
        return value.empty() ? nlohmann::json(nullptr) : nlohmann::json(value);
    };
    return {{"custom", custom},
            {"name", name},
            {"short_name", short_name},
            {"company", company},
            {"assistant_name", assistant_name},
            {"console_title", console_title},
            {"eyebrow", eyebrow()},
            {"by_orama", by_orama},
            {"logo", or_null(logo)},
            {"logo_dark", or_null(logo_dark)},
            {"favicon", or_null(favicon)},
            {"accent", or_null(accent)},
            {"accent_dark", or_null(accent_dark)},
            {"support_email", or_null(support_email)},
            {"legal_name", or_null(legal_name)},
            {"terms_url", or_null(terms_url)},
            {"privacy_url", or_null(privacy_url)}};
}

std::expected<Brand, std::string> brand_from_sources(const BrandSources& given,
                                                     std::string_view resend_from,
                                                     std::string_view support_email) {
    BrandSources sources = given;
    std::filesystem::path base = std::filesystem::current_path();
    if (!given.file.empty()) {
        const std::filesystem::path file{given.file};
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            return std::unexpected("LAKE_BRAND_FILE: cannot read " + given.file);
        }
        std::ostringstream text;
        text << in.rdbuf();
        const nlohmann::json document =
            nlohmann::json::parse(std::move(text).str(), nullptr, false);
        if (!document.is_object()) {
            return std::unexpected("LAKE_BRAND_FILE: " + given.file + " is not a JSON object");
        }
        base = file.has_parent_path() ? file.parent_path() : base;
        const std::array<std::pair<std::string_view, std::string*>, 16> fields{{
            {"name", &sources.name},
            {"short_name", &sources.short_name},
            {"company", &sources.company},
            {"assistant_name", &sources.assistant_name},
            {"console_title", &sources.console_title},
            {"by_orama", &sources.by_orama},
            {"logo", &sources.logo},
            {"logo_dark", &sources.logo_dark},
            {"favicon", &sources.favicon},
            {"accent", &sources.accent},
            {"accent_dark", &sources.accent_dark},
            {"support_email", &sources.support_email},
            {"email_from", &sources.email_from},
            {"legal_name", &sources.legal_name},
            {"terms_url", &sources.terms_url},
            {"privacy_url", &sources.privacy_url},
        }};
        for (const auto& [key, value] : document.items()) {
            const auto field = std::ranges::find(
                fields, std::string_view(key), &std::pair<std::string_view, std::string*>::first);
            if (field == fields.end()) {
                return std::unexpected("LAKE_BRAND_FILE: unknown setting \"" + key + "\"");
            }
            std::string text_value;
            if (value.is_string()) {
                text_value = value.get<std::string>();
            } else if (value.is_boolean() && key == "by_orama") {
                text_value = value.get<bool>() ? "true" : "false";
            } else {
                return std::unexpected("LAKE_BRAND_FILE: \"" + key + "\" must be a string" +
                                       (key == "by_orama" ? " or a boolean" : ""));
            }
            // The environment's value wins over the file's.
            if (field->second->empty()) {
                *field->second = std::move(text_value);
            }
        }
    }

    Brand brand;
    const std::array settings{&sources.name,
                              &sources.short_name,
                              &sources.company,
                              &sources.assistant_name,
                              &sources.console_title,
                              &sources.by_orama,
                              &sources.logo,
                              &sources.logo_dark,
                              &sources.favicon,
                              &sources.accent,
                              &sources.accent_dark,
                              &sources.support_email,
                              &sources.email_from,
                              &sources.legal_name,
                              &sources.terms_url,
                              &sources.privacy_url};
    brand.custom = std::ranges::any_of(settings, [](const std::string* s) { return !s->empty(); });
    brand.support_email = std::string(support_email);
    brand.email_from = std::string(resend_from);
    if (!brand.custom) {
        return brand;
    }

    if (sources.name.empty()) {
        return std::unexpected("LAKE_BRAND_NAME must be set when any LAKE_BRAND_* setting is");
    }
    if (const auto problem = name_problem(sources.name, 64)) {
        return std::unexpected("LAKE_BRAND_NAME " + *problem);
    }
    brand.name = sources.name;
    brand.short_name = sources.short_name.empty() ? short_name_of(brand.name) : sources.short_name;
    if (!short_name_ok(brand.short_name)) {
        return std::unexpected(
            sources.short_name.empty()
                ? "LAKE_BRAND_SHORT_NAME must be set: LAKE_BRAND_NAME gives no usable short name"
                : "LAKE_BRAND_SHORT_NAME must be 1 to 32 lowercase letters, digits and hyphens, "
                  "starting with a letter and not ending with a hyphen");
    }
    brand.company = sources.company.empty() ? brand.name : sources.company;
    brand.assistant_name =
        sources.assistant_name.empty() ? "Ask " + brand.name : sources.assistant_name;
    brand.console_title =
        sources.console_title.empty() ? brand.name + " Console" : sources.console_title;
    for (const auto& [setting, value] :
         {std::pair<std::string_view, const std::string*>{"LAKE_BRAND_COMPANY", &brand.company},
          {"LAKE_BRAND_ASSISTANT_NAME", &brand.assistant_name},
          {"LAKE_BRAND_CONSOLE_TITLE", &brand.console_title}}) {
        if (const auto problem = name_problem(*value, 80)) {
            return std::unexpected(std::string(setting) + " " + *problem);
        }
    }
    if (sources.by_orama.empty() || sources.by_orama == "false") {
        brand.by_orama = false;
    } else if (sources.by_orama == "true") {
        brand.by_orama = true;
    } else {
        return std::unexpected("LAKE_BRAND_BY_ORAMA must be true or false");
    }

    for (const auto& [setting, value, target] :
         {std::tuple<std::string_view, const std::string*, std::string*>{
              "LAKE_BRAND_LOGO", &sources.logo, &brand.logo},
          {"LAKE_BRAND_LOGO_DARK", &sources.logo_dark, &brand.logo_dark},
          {"LAKE_BRAND_FAVICON", &sources.favicon, &brand.favicon}}) {
        auto read = image(setting, *value, base);
        if (!read) {
            return std::unexpected(read.error());
        }
        *target = std::move(*read);
    }

    for (const auto& [setting, value, target] :
         {std::tuple<std::string_view, const std::string*, std::string*>{
              "LAKE_BRAND_ACCENT", &sources.accent, &brand.accent},
          {"LAKE_BRAND_ACCENT_DARK", &sources.accent_dark, &brand.accent_dark}}) {
        if (!value->empty() && !colour_ok(*value)) {
            return std::unexpected(std::string(setting) + " must be a colour such as #0b6e99");
        }
        *target = lowercase(*value);
    }
    if (!brand.accent_dark.empty() && brand.accent.empty()) {
        return std::unexpected("LAKE_BRAND_ACCENT must be set with LAKE_BRAND_ACCENT_DARK");
    }

    if (sources.support_email.empty()) {
        return std::unexpected("LAKE_BRAND_SUPPORT_EMAIL must be set for a custom brand: people "
                               "would be told to write to Orama");
    }
    if (!plausible_email_address(sources.support_email) ||
        lake_address(sources.support_email, resend_from, support_email)) {
        return std::unexpected(
            "LAKE_BRAND_SUPPORT_EMAIL must be one email address of the brand's own domain");
    }
    brand.support_email = sources.support_email;
    if (!sources.email_from.empty()) {
        if (const auto problem = name_problem(sources.email_from, 254);
            problem && sources.email_from.find_first_of("&\"\\`*[]") != std::string::npos) {
            return std::unexpected("LAKE_BRAND_EMAIL_FROM " + *problem);
        }
        if (!plausible_email_address(address_of(sources.email_from))) {
            return std::unexpected(
                "LAKE_BRAND_EMAIL_FROM must be an address or \"Name <address>\"");
        }
        brand.email_from = sources.email_from;
    }
    if (brand.email_from.empty() || lake_address(brand.email_from, resend_from, support_email) ||
        brand.email_from == default_resend_from) {
        return std::unexpected("LAKE_BRAND_EMAIL_FROM must be set for a custom brand, on a domain "
                               "verified in Resend: email would still be sent as " +
                               brand.email_from);
    }

    if (const auto problem =
            sources.legal_name.empty() ? std::nullopt : name_problem(sources.legal_name, 120)) {
        return std::unexpected("LAKE_BRAND_LEGAL_NAME " + *problem);
    }
    brand.legal_name = sources.legal_name;
    for (const auto& [setting, value, target] :
         {std::tuple<std::string_view, const std::string*, std::string*>{
              "LAKE_BRAND_TERMS_URL", &sources.terms_url, &brand.terms_url},
          {"LAKE_BRAND_PRIVACY_URL", &sources.privacy_url, &brand.privacy_url}}) {
        if (!value->empty() && !https_url_ok(*value)) {
            return std::unexpected(std::string(setting) + " must be an https URL");
        }
        *target = *value;
    }
    return brand;
}

const Brand& instance_brand() {
    const Brand* current = installed().current.load(std::memory_order_acquire);
    return current != nullptr ? *current : lake_brand();
}

void set_instance_brand(const Brand& brand) {
    Installed& state = installed();
    std::lock_guard lock(state.mutex);
    state.kept.push_back(std::make_unique<const Brand>(brand));
    state.current.store(state.kept.back().get(), std::memory_order_release);
}

} // namespace lake::api

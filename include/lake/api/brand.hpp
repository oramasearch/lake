#pragma once

// The instance brand (lake#1135, lake#1137, docs/serving/brand.md): the name, logo, colours and
// addresses a Lake shows its people in the console, on the pages lake renders itself, in every
// email it sends and on its agent surfaces (llms.txt, llms-full.txt, the skill, MCP and OpenAPI).
// A white-labelled Lake (Private Warehouse and above) shows its customer's brand instead of Lake
// and Orama. The default is Lake's own brand, and with it every surface is byte-for-byte what it
// was before the brand existed.
//
// The brand is instance configuration the operator renders (LAKE_BRAND_FILE, a mounted JSON
// document, and LAKE_BRAND_* environment variables over it), read and validated once at start:
// an invalid brand stops lake. It is never workspace data and never stored.

#include <expected>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace lake::api {

struct Brand {
    // The product's name in prose, titles, headings and email ("Lake").
    std::string name = "Lake";
    // The name in identifiers: the skill's path and name (/skills/<short_name>/SKILL.md), the MCP
    // server's serverInfo.name and the `claude mcp add` name. Lowercase letters, digits and
    // hyphens, starting with a letter.
    std::string short_name = "lake";
    // Who ships the catalogues and library agents ("Orama's AI solutions").
    std::string company = "Orama";
    // The in-console assistant ("Ask Orama").
    std::string assistant_name = "Ask Orama";
    // The console's document title and label ("Lake Console").
    std::string console_title = "Lake Console";
    // Whether the eyebrow reads "<name> by Orama" (true for Lake's own brand).
    bool by_orama = true;
    // Images as data: URIs (a file named in the configuration is read into one at start); empty
    // keeps Lake's built-in wordmark and the browser's default icon.
    std::string logo;
    std::string logo_dark;
    std::string favicon;
    // The action colour, #rrggbb, light and dark; empty keeps the design tokens' colour.
    std::string accent;
    std::string accent_dark;
    // The address people are told to write to, and the From of every email. Empty takes
    // LAKE_SUPPORT_EMAIL's and LAKE_RESEND_FROM's.
    std::string support_email;
    std::string email_from;
    // The legal footer of the pages and the sign-in screens: the legal name, and https links to
    // the terms and the privacy notice. All empty: no footer.
    std::string legal_name;
    std::string terms_url;
    std::string privacy_url;
    // Whether any LAKE_BRAND_* setting was given: false is Lake's own brand.
    bool custom = false;

    // "Lake by Orama", or the name alone when by_orama is off.
    std::string eyebrow() const;
    // `text` written for Lake with this brand's names: "Lake" (a whole word), "Orama", "Ask
    // Orama", "Lake Console", "Lake by Orama" and "lake.live is prepaid". The text unchanged for
    // Lake's own brand. Only lake's own copy goes through it, never a person's content.
    std::string apply(std::string_view text) const;
    // apply() over every "title", "summary" and "description" string of a JSON document (an
    // OpenAPI document, an MCP tool list), never over a key or an enum value.
    void apply_to_documentation(nlohmann::json& document) const;
    // The `<link rel="icon">` and the colour overrides of the design tokens for a page's <head>;
    // empty for Lake's own brand.
    std::string head_markup() const;
    // The legal footer (legal name, terms, privacy) as HTML; empty when none is configured.
    std::string footer_markup() const;
    // What the console reads (viewer's `brand`).
    nlohmann::json to_json() const;
};

// The values the brand is read from: LAKE_BRAND_FILE's contents and directory, and each
// LAKE_BRAND_* variable (empty = unset). Environment values win over the file's.
struct BrandSources {
    std::string file; // LAKE_BRAND_FILE, a path; empty: none
    std::string name; // LAKE_BRAND_NAME
    std::string short_name;
    std::string company;
    std::string assistant_name;
    std::string console_title;
    std::string by_orama; // "true" or "false"
    std::string logo;     // a data:image/... URI or a file path
    std::string logo_dark;
    std::string favicon;
    std::string accent;
    std::string accent_dark;
    std::string support_email;
    std::string email_from;
    std::string legal_name;
    std::string terms_url;
    std::string privacy_url;
};

// Reads and validates the brand. Every error names the setting at fault, never a file's contents.
// A brand with any setting is a custom brand: it must name itself (LAKE_BRAND_NAME) and give its
// own support address and email sender, neither on lake.live, orama.com or oramadev.net.
// `resend_from` and `support_email` (LAKE_RESEND_FROM's and LAKE_SUPPORT_EMAIL's values) are what
// Lake's own brand sends from and points people to.
std::expected<Brand, std::string> brand_from_sources(const BrandSources& sources,
                                                     std::string_view resend_from,
                                                     std::string_view support_email);

// The brand this process serves, set once at start (ApiServer::start installs
// ServerOptions::brand). Lake's own brand until then.
const Brand& instance_brand();
void set_instance_brand(const Brand& brand);

} // namespace lake::api

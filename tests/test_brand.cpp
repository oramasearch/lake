// The instance brand (lake#1135, lake#1137, docs/serving/brand.md): Lake's own brand changes
// nothing, a custom brand reaches every page lake renders, every email and every agent surface
// with no Lake, Orama or lake.live left, and an invalid brand is refused.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/api/agent_docs.hpp"
#include "lake/api/balance.hpp"
#include "lake/api/billing.hpp"
#include "lake/api/brand.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/mcp.hpp"
#include "lake/api/orders.hpp"
#include "lake/api/regions.hpp"
#include "lake/api/resend.hpp"
#include "lake/api/rest.hpp"
#include "lake/api/web_ui.hpp"
#include "lake/control/store.hpp"
#include "temporary_directory.hpp"

namespace {

using nlohmann::json;
using namespace lake::api;

// A white-labelled customer's brand, every setting given.
BrandSources acme_sources() {
    BrandSources sources;
    sources.name = "Acme Data";
    sources.short_name = "acme-data";
    sources.company = "Acme";
    sources.assistant_name = "Ask Acme";
    sources.logo = "data:image/png;base64,iVBORw0KGgo=";
    sources.favicon = "data:image/x-icon;base64,AAABAA==";
    sources.accent = "#AA3300";
    sources.support_email = "help@acme.example";
    sources.email_from = "Acme Data <hello@acme.example>";
    sources.legal_name = "Acme Data Inc.";
    sources.terms_url = "https://acme.example/terms";
    sources.privacy_url = "https://acme.example/privacy";
    return sources;
}

Brand acme() {
    auto brand = brand_from_sources(acme_sources(), default_resend_from, default_support_email);
    EXPECT_TRUE(brand) << brand.error();
    return brand ? *brand : Brand{};
}

// Installs a brand for one test and puts Lake's back after it.
class ScopedBrand {
public:
    explicit ScopedBrand(const Brand& brand) {
        set_instance_brand(brand);
    }
    ~ScopedBrand() {
        set_instance_brand(Brand{});
    }
    ScopedBrand(const ScopedBrand&) = delete;
    ScopedBrand& operator=(const ScopedBrand&) = delete;
};

// Every place `text` still names Lake (a whole word), Orama or lake.live, with what is around it,
// so a failure says where. A person's text (pages, email) may not say orama in any case; an agent
// document may keep the API's lowercase identifiers (`source: orama`, `agent-ask-orama`,
// `oramacore_fulltext`), which are wire values, not names.
std::vector<std::string> leftovers(std::string_view text, bool identifiers = false) {
    static const std::regex prose(R"(\bLake\b|[Oo][Rr][Aa][Mm][Aa]|lake\.live|lake by)",
                                  std::regex::ECMAScript);
    static const std::regex documents(R"(\bLake\b|Orama|ORAMA|lake\.live|lake by)",
                                      std::regex::ECMAScript);
    const std::regex& brand = identifiers ? documents : prose;
    std::vector<std::string> found;
    // The design tokens' names (--orama-action-default, ...) are not text anyone reads.
    std::string copy(text);
    for (std::size_t at = copy.find("--orama-"); at != std::string::npos;
         at = copy.find("--orama-", at)) {
        copy.replace(at, 8, "--token-");
    }
    for (auto it = std::sregex_iterator(copy.begin(), copy.end(), brand);
         it != std::sregex_iterator();
         ++it) {
        const std::size_t at = static_cast<std::size_t>(it->position());
        const std::size_t from = at < 50 ? 0 : at - 50;
        found.push_back(copy.substr(from, 100));
    }
    return found;
}

#define EXPECT_NO_BRAND_LEFT(where, text)                                                          \
    for (const std::string& left : leftovers(text)) {                                              \
        ADD_FAILURE() << (where) << " still names Lake or Orama: ..." << left << "...";            \
    }
#define EXPECT_NO_BRAND_NAME_LEFT(where, text)                                                     \
    for (const std::string& left : leftovers(text, true)) {                                        \
        ADD_FAILURE() << (where) << " still names Lake or Orama: ..." << left << "...";            \
    }

Registry every_capability() {
    Registry registry;
    register_core_capabilities(registry, "test");
    register_billing_capabilities(registry);
    return registry;
}

Regions customer_regions() {
    auto regions = regions_from_environment(
        "westeurope",
        "",
        R"([{"name":"westeurope","label":"Europe","host":"data.acme.example","latitude":"52.37","longitude":"4.89"}])",
        "acme.example");
    EXPECT_TRUE(regions) << regions.error();
    return regions ? *regions : Regions{};
}

lake::control::CloudOrder sample_order() {
    lake::control::CloudOrder order;
    order.id = "0b6f2c1e-1111-4222-8333-444455556666";
    order.offer = "dedicated_cluster";
    order.path = "order_form";
    order.cloud_provider = "azure";
    order.region = "westeurope";
    order.quantity = 1;
    order.amount_cents = 500'000;
    order.included_credit_cents = 200'000;
    order.contact_name = "Ada";
    order.contact_email = "ada@example.com";
    order.state = "submitted";
    order.created_at = "2026-10-24T12:00:00Z";
    return order;
}

// Every title, summary and description string of a JSON document.
void collect_documentation(const json& document, std::vector<std::string>& out) {
    if (document.is_array()) {
        for (const json& item : document) {
            collect_documentation(item, out);
        }
    } else if (document.is_object()) {
        for (const auto& [key, value] : document.items()) {
            if (value.is_string() && (key == "title" || key == "summary" || key == "description")) {
                out.push_back(value.get<std::string>());
            } else {
                collect_documentation(value, out);
            }
        }
    }
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Configuration

TEST(BrandConfig, NoSettingIsLakesOwnBrandAndChangesNothing) {
    const auto brand = brand_from_sources({}, default_resend_from, default_support_email);
    ASSERT_TRUE(brand) << brand.error();
    EXPECT_FALSE(brand->custom);
    EXPECT_EQ(brand->name, "Lake");
    EXPECT_EQ(brand->short_name, "lake");
    EXPECT_EQ(brand->company, "Orama");
    EXPECT_EQ(brand->assistant_name, "Ask Orama");
    EXPECT_EQ(brand->console_title, "Lake Console");
    EXPECT_EQ(brand->eyebrow(), "Lake by Orama");
    EXPECT_EQ(brand->email_from, default_resend_from);
    EXPECT_EQ(brand->support_email, default_support_email);
    EXPECT_TRUE(brand->head_markup().empty());
    EXPECT_TRUE(brand->footer_markup().empty());
    const std::string copy = "Ask Orama in the Lake Console; Lake by Orama; lake.live is prepaid.";
    EXPECT_EQ(brand->apply(copy), copy);
    json document = {{"title", "Lake"}, {"description", "Orama's"}};
    const json before = document;
    brand->apply_to_documentation(document);
    EXPECT_EQ(document, before);
}

TEST(BrandConfig, ACustomBrandRewritesLakesWordsOnly) {
    const Brand brand = acme();
    EXPECT_TRUE(brand.custom);
    EXPECT_EQ(brand.console_title, "Acme Data Console");
    EXPECT_EQ(brand.eyebrow(), "Acme Data");
    EXPECT_EQ(brand.accent, "#aa3300");
    EXPECT_EQ(brand.apply("Lake by Orama"), "Acme Data");
    EXPECT_EQ(brand.apply("the Lake Console"), "the Acme Data Console");
    EXPECT_EQ(brand.apply("Ask Orama proposes"), "Ask Acme proposes");
    EXPECT_EQ(brand.apply("The Orama operator runs it"), "The operator runs it");
    EXPECT_EQ(brand.apply("Orama's AI solutions"), "Acme's AI solutions");
    EXPECT_EQ(brand.apply("lake.live is prepaid"), "Acme Data is prepaid when billing is on");
    EXPECT_EQ(brand.apply("this Lake's region"), "this Acme Data's region");
    // Identifiers and longer words are not the name.
    EXPECT_EQ(brand.apply("LakeSet lake_session Lakes LAKE_KEY"),
              "LakeSet lake_session Lakes LAKE_KEY");
    // Schemas keep their keys and enum values.
    json schema = {{"description", "a Lake id"},
                   {"enum", json::array({"Lake"})},
                   {"properties", {{"title", {{"type", "string"}, {"title", "Lake"}}}}}};
    brand.apply_to_documentation(schema);
    EXPECT_EQ(schema["description"], "a Acme Data id");
    EXPECT_EQ(schema["enum"][0], "Lake");
    EXPECT_EQ(schema["properties"]["title"]["title"], "Acme Data");
    // A name alone derives the rest.
    BrandSources minimal;
    minimal.name = "Northwind Analytics";
    minimal.support_email = "support@northwind.example";
    minimal.email_from = "support@northwind.example";
    const auto derived = brand_from_sources(minimal, default_resend_from, default_support_email);
    ASSERT_TRUE(derived) << derived.error();
    EXPECT_EQ(derived->short_name, "northwind-analytics");
    EXPECT_EQ(derived->company, "Northwind Analytics");
    EXPECT_EQ(derived->assistant_name, "Ask Northwind Analytics");
    EXPECT_FALSE(derived->by_orama);
}

TEST(BrandConfig, InvalidSettingsAreRefusedNamingTheSetting) {
    struct Case {
        std::string why;
        void (*change)(BrandSources&);
        std::string setting;
    };
    const std::vector<Case> cases{
        {"no name", [](BrandSources& s) { s.name.clear(); }, "LAKE_BRAND_NAME"},
        {"markup in the name", [](BrandSources& s) { s.name = "Acme <b>"; }, "LAKE_BRAND_NAME"},
        {"a long name", [](BrandSources& s) { s.name = std::string(65, 'a'); }, "LAKE_BRAND_NAME"},
        {"a short name with capitals",
         [](BrandSources& s) { s.short_name = "Acme"; },
         "LAKE_BRAND_SHORT_NAME"},
        {"a short name with a slash",
         [](BrandSources& s) { s.short_name = "acme/x"; },
         "LAKE_BRAND_SHORT_NAME"},
        {"a name with no usable short name",
         [](BrandSources& s) {
             s.name = "日本";
             s.short_name.clear();
         },
         "LAKE_BRAND_SHORT_NAME"},
        {"a colour word", [](BrandSources& s) { s.accent = "red"; }, "LAKE_BRAND_ACCENT"},
        {"a dark accent alone",
         [](BrandSources& s) {
             s.accent.clear();
             s.accent_dark = "#000000";
         },
         "LAKE_BRAND_ACCENT"},
        {"a logo URL",
         [](BrandSources& s) { s.logo = "https://cdn.example/x.png"; },
         "LAKE_BRAND_LOGO"},
        {"a logo data URI of text",
         [](BrandSources& s) { s.logo = "data:text/html;base64,PGI+"; },
         "LAKE_BRAND_LOGO"},
        {"a missing favicon file",
         [](BrandSources& s) { s.favicon = "/nonexistent/favicon.ico"; },
         "LAKE_BRAND_FAVICON"},
        {"no support address",
         [](BrandSources& s) { s.support_email.clear(); },
         "LAKE_BRAND_SUPPORT_EMAIL"},
        {"this Lake's support address",
         [](BrandSources& s) { s.support_email = "help@lake.example.com"; },
         "LAKE_BRAND_SUPPORT_EMAIL"},
        {"no sender", [](BrandSources& s) { s.email_from.clear(); }, "LAKE_BRAND_EMAIL_FROM"},
        {"Lake's sender",
         [](BrandSources& s) { s.email_from = "Acme <orders@mail.lake.example.com>"; },
         "LAKE_BRAND_EMAIL_FROM"},
        {"a sender that is no address",
         [](BrandSources& s) { s.email_from = "Acme Data"; },
         "LAKE_BRAND_EMAIL_FROM"},
        {"an http terms link",
         [](BrandSources& s) { s.terms_url = "http://acme.example/terms"; },
         "LAKE_BRAND_TERMS_URL"},
        {"a script privacy link",
         [](BrandSources& s) { s.privacy_url = "javascript:alert(1)"; },
         "LAKE_BRAND_PRIVACY_URL"},
        {"by_orama not a boolean",
         [](BrandSources& s) { s.by_orama = "yes"; },
         "LAKE_BRAND_BY_ORAMA"},
    };
    for (const Case& c : cases) {
        BrandSources sources = acme_sources();
        c.change(sources);
        const auto brand = brand_from_sources(
            sources, "Lake <orders@lake.example.com>", "support@lake.example.com");
        ASSERT_FALSE(brand) << c.why;
        EXPECT_NE(brand.error().find(c.setting), std::string::npos)
            << c.why << ": " << brand.error();
    }
    // Any brand setting makes a custom brand, which must name itself.
    BrandSources colour_only;
    colour_only.accent = "#112233";
    const auto refused =
        brand_from_sources(colour_only, default_resend_from, default_support_email);
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.error().find("LAKE_BRAND_NAME"), std::string::npos);
}

TEST(BrandConfig, TheFileGivesTheBrandAndTheEnvironmentWins) {
    const auto folder = lake::test::fixture_path("brand");
    std::filesystem::create_directories(folder);
    {
        std::ofstream logo(folder / "logo.svg", std::ios::binary);
        logo << "<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
        std::ofstream file(folder / "brand.json");
        file << json{{"name", "File Name"},
                     {"short_name", "filed"},
                     {"logo", "logo.svg"},
                     {"by_orama", false},
                     {"support_email", "help@acme.example"},
                     {"email_from", "hello@acme.example"}}
                    .dump();
    }
    BrandSources sources;
    sources.file = (folder / "brand.json").string();
    sources.name = "Environment Name";
    const auto brand = brand_from_sources(sources, default_resend_from, default_support_email);
    ASSERT_TRUE(brand) << brand.error();
    EXPECT_EQ(brand->name, "Environment Name");
    EXPECT_EQ(brand->short_name, "filed");
    EXPECT_TRUE(brand->logo.starts_with("data:image/svg+xml;base64,PHN2Zy")) << brand->logo;
    EXPECT_EQ(brand->email_from, "hello@acme.example");

    {
        std::ofstream file(folder / "unknown.json");
        file << R"({"name":"X","colour":"#000000"})";
    }
    sources = {};
    sources.file = (folder / "unknown.json").string();
    auto refused = brand_from_sources(sources, default_resend_from, default_support_email);
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.error().find("unknown setting \"colour\""), std::string::npos)
        << refused.error();
    {
        std::ofstream file(folder / "broken.json");
        file << "{not json";
    }
    sources.file = (folder / "broken.json").string();
    refused = brand_from_sources(sources, default_resend_from, default_support_email);
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.error().find("LAKE_BRAND_FILE"), std::string::npos);
    sources.file = (folder / "missing.json").string();
    EXPECT_FALSE(brand_from_sources(sources, default_resend_from, default_support_email));
}

// ------------------------------------------------------------------------------------------------
// Email

// Lake's own brand sends exactly the email it sent before the brand existed.
TEST(BrandEmail, LakesOwnBrandSendsTheSameEmail) {
    set_instance_brand(Brand{});
    auto settings = resend_settings_from_environment(
        "re_lake1135fakekey0001", "Lake <orders@example.com>", "support@example.com");
    ASSERT_TRUE(settings) << settings.error();
    const auto sheet = order_sheet_from_environment("");
    ASSERT_TRUE(sheet) << sheet.error();
    const Email customer = order_email(
        *sheet, customer_regions(), *settings, sample_order(), "customer", "https://x/orders/1");
    EXPECT_EQ(customer.subject,
              "We received your Lake order (0b6f2c1e-1111-4222-8333-444455556666)");
    EXPECT_EQ(customer.reply_to, "support@example.com");
    EXPECT_TRUE(customer.text.starts_with(
        "Thank you. We received your order form and will be in touch shortly. Nothing has been "
        "charged and nothing is set up until we have spoken.\n\nYour order: https://x/orders/1\n\n"
        "Order: 0b6f2c1e-1111-4222-8333-444455556666\n"))
        << customer.text;
    EXPECT_TRUE(customer.text.ends_with(
        "\nQuestions: reply to this email or write to support@example.com.\n"))
        << customer.text;
    EXPECT_EQ(settings->from, "Lake <orders@example.com>");

    BalanceView view;
    view.balance_micros = -2'500'000;
    view.overdraft_micros = 10'000'000;
    view.add_micros = 20'000'000;
    const Email balance =
        balance_email(BalanceCrossing::negative, view, "https://x/billing", {"a@example.com"});
    EXPECT_EQ(balance.subject, "Your Lake balance is below $0");
    EXPECT_EQ(balance.text,
              "Your Lake account's balance is -$2.50: your usage has passed what you added. Lake "
              "keeps working until the balance reaches -$10.00, then stops starting anything that "
              "costs money.\n\nAdd $20.00 or more here:\nhttps://x/billing\n\nThis email is sent "
              "once each time your balance crosses this point, and not again until you add "
              "funds.\n\nLake\n");
}

TEST(BrandEmail, ACustomBrandsEmailNamesNoLakeOrOrama) {
    const Brand brand = acme();
    ScopedBrand scoped(brand);
    auto settings = resend_settings_from_environment("re_lake1135fakekey0001", "", "");
    ASSERT_TRUE(settings) << settings.error();
    settings->from = brand.email_from;
    const auto sheet = order_sheet_from_environment("");
    ASSERT_TRUE(sheet) << sheet.error();
    lake::control::CloudOrder order = sample_order();
    for (const std::string state : {"submitted", "paid", "ready", "failed"}) {
        order.state = state;
        order.host = state == "ready" ? "data.acme.example" : "";
        const Email customer = order_email(*sheet,
                                           customer_regions(),
                                           *settings,
                                           order,
                                           "customer",
                                           "https://data.acme.example/o");
        EXPECT_EQ(customer.reply_to, "help@acme.example") << state;
        EXPECT_NO_BRAND_LEFT("order email " + state + " subject", customer.subject);
        EXPECT_NO_BRAND_LEFT("order email " + state + " text", customer.text);
        EXPECT_NE(customer.subject.find("Acme Data"), std::string::npos) << state;
        EXPECT_NE(customer.text.find("write to help@acme.example."), std::string::npos) << state;
        if (state == "paid") {
            const Email support =
                order_email(*sheet, customer_regions(), *settings, order, "support", "");
            EXPECT_NE(support.text.find("The operator provisions it"), std::string::npos)
                << support.text;
        }
    }
    order.state = "submitted";
    EXPECT_EQ(order_email(*sheet, customer_regions(), *settings, order, "customer", "").subject,
              "We received your Acme Data order (0b6f2c1e-1111-4222-8333-444455556666)");

    BalanceView view;
    view.balance_micros = -2'500'000;
    view.overdraft_micros = 10'000'000;
    view.add_micros = 20'000'000;
    for (const BalanceCrossing crossing : {BalanceCrossing::limit_warning,
                                           BalanceCrossing::negative,
                                           BalanceCrossing::blocked,
                                           BalanceCrossing::auto_top_up_failed}) {
        const Email email =
            balance_email(crossing, view, "https://data.acme.example/billing", {"a@example.com"});
        EXPECT_NO_BRAND_LEFT(std::string("balance email ") + std::string(to_string(crossing)),
                             email.subject + "\n" + email.text);
        EXPECT_TRUE(email.text.ends_with("\n\nAcme Data\n")) << email.text;
    }
}

// ------------------------------------------------------------------------------------------------
// Pages lake renders itself

TEST(BrandPages, ACustomBrandsPagesNameNoLakeOrOrama) {
    const Brand brand = acme();
    ScopedBrand scoped(brand);
    const Regions regions = customer_regions();
    const auto sheet = order_sheet_from_environment("");
    ASSERT_TRUE(sheet) << sheet.error();
    lake::control::CloudOrder order = sample_order();
    std::vector<std::pair<std::string, std::string>> pages{
        {"error", web_ui_error("store_unavailable", "try again")},
        {"region picker", web_ui_region_picker(regions, regions.list.front(), "/", "")},
        {"order message", web_ui_order_message("Not Found", "No such order.")},
        {"price list", web_ui_price_list({}, "2026-10-01")},
        {"order received", web_ui_order_received(*sheet, regions, order, "/o")},
        {"order status", web_ui_order_status(*sheet, regions, order, "")},
    };
    for (const auto& [name, html] : pages) {
        // The stylesheet's link and the design tokens' names are not text a person reads.
        EXPECT_NO_BRAND_LEFT(name, html);
        EXPECT_NE(html.find("Acme Data"), std::string::npos) << name;
        EXPECT_NE(html.find("<style id=\"lake-brand\">"), std::string::npos) << name;
        EXPECT_NE(html.find("--orama-action-default:#aa3300"), std::string::npos) << name;
        EXPECT_NE(html.find("<link rel=\"icon\" href=\"data:image/x-icon;base64,AAABAA==\">"),
                  std::string::npos)
            << name;
        EXPECT_NE(html.find("© Acme Data Inc."), std::string::npos) << name;
        EXPECT_NE(html.find("href=\"https://acme.example/terms\""), std::string::npos) << name;
    }
    const std::string picker = pages[1].second;
    EXPECT_NE(picker.find("<title>Acme Data — Choose a Region</title>"), std::string::npos);
    EXPECT_NE(picker.find("<img class=\"hosted-brand-logo\" src=\"data:image/png;base64,"),
              std::string::npos);
    EXPECT_NE(picker.find("<p class=\"eyebrow\">Acme Data</p>"), std::string::npos);
    // The order sheet's own frame and words; its offers are the operator's LAKE_ORDER_SHEET.
    const std::string offers = web_ui_order_sheet(*sheet, regions, regions.list.front(), "/", "");
    EXPECT_NE(offers.find("<h1>Get Acme Data</h1>"), std::string::npos);
    EXPECT_EQ(offers.find("Lake by Orama"), std::string::npos);
    EXPECT_EQ(offers.find("Every way to run Lake"), std::string::npos);
}

TEST(BrandPages, LakesOwnBrandAddsNothingToThePages) {
    set_instance_brand(Brand{});
    const Regions regions = customer_regions();
    const std::string picker = web_ui_region_picker(regions, regions.list.front(), "/", "");
    EXPECT_NE(picker.find("<title>Lake — Choose a Region</title>"), std::string::npos);
    EXPECT_NE(picker.find("<span class=\"hosted-brand\"><span class=\"hosted-brand-mark\" "
                          "aria-hidden=\"true\"></span>Lake</span>"),
              std::string::npos)
        << picker;
    EXPECT_NE(picker.find("<p class=\"eyebrow\">Lake by Orama</p>"), std::string::npos);
    EXPECT_NE(picker.find("<p class=\"sub\">Each region is a separate Lake with its own accounts. "
                          "Your workspaces stay in the region you create them in.</p>"),
              std::string::npos);
    for (const std::string& html : {picker, web_ui_error("x", "y")}) {
        EXPECT_EQ(html.find("lake-brand"), std::string::npos);
        EXPECT_EQ(html.find("rel=\"icon\""), std::string::npos);
        EXPECT_EQ(html.find("<footer"), std::string::npos);
    }
    EXPECT_NE(web_ui_error("x", "y").find("<title>Lake — error</title>"), std::string::npos);
    EXPECT_NE(web_ui_error("x", "y").find(">Back to Lake</a>"), std::string::npos);
}

// ------------------------------------------------------------------------------------------------
// Agent surfaces (lake#1137)

TEST(BrandAgentDocs, ACustomBrandsAgentSurfacesNameNoLakeOrOrama) {
    const Brand brand = acme();
    ScopedBrand scoped(brand);
    const Registry registry = every_capability();
    const std::string origin = "https://data.acme.example";
    const std::string llms = agent_docs::llms_txt(registry, origin);
    const std::string full = agent_docs::llms_full_txt(registry, origin);
    const std::string skill = agent_docs::skill_md(registry, origin);
    for (const auto& [name, text] : std::vector<std::pair<std::string, std::string>>{
             {"llms.txt", llms}, {"llms-full.txt", full}, {"SKILL.md", skill}}) {
        EXPECT_NO_BRAND_NAME_LEFT(name, text);
        EXPECT_NE(text.find(origin + "/skills/acme-data/SKILL.md"), std::string::npos) << name;
        EXPECT_EQ(text.find("/skills/lake/"), std::string::npos) << name;
        EXPECT_NE(text.find("claude mcp add --transport http acme-data " + origin + "/mcp"),
                  std::string::npos)
            << name;
        EXPECT_NE(text.find("$ACME_DATA_KEY"), std::string::npos) << name;
    }
    EXPECT_TRUE(llms.starts_with("# Acme Data\n\n> Acme Data is Acme's platform"))
        << llms.substr(0, 200);
    EXPECT_TRUE(skill.starts_with("---\nname: acme-data\n")) << skill.substr(0, 100);
    EXPECT_TRUE(full.starts_with("# Acme Data — every tool\n"));

    // MCP: initialize and tools/list.
    ApiContext context;
    const auto initialized = mcp_dispatch(
        registry, json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}}, context, "test");
    ASSERT_TRUE(initialized);
    const json answer = json::parse(*initialized);
    EXPECT_EQ(answer["result"]["serverInfo"]["name"], "acme-data");
    EXPECT_EQ(answer["result"]["serverInfo"]["title"], "Acme Data");
    EXPECT_NO_BRAND_NAME_LEFT("MCP initialize", answer.dump());
    std::vector<std::string> texts;
    for (const auto audience : {SchemaAudience::Caller, SchemaAudience::AgentTask}) {
        collect_documentation(mcp_tools(registry, audience), texts);
        collect_documentation(openapi_document(registry, "test", audience), texts);
    }
    ASSERT_GT(texts.size(), registry.list().size());
    for (const std::string& text : texts) {
        EXPECT_NO_BRAND_NAME_LEFT("tools/list or OpenAPI", text);
    }
    EXPECT_EQ(openapi_document(registry, "test")["info"]["title"], "Acme Data control surface");
}

// Lake's own brand serves the agent surfaces exactly as before: the skill named lake at
// /skills/lake/, `claude mcp add ... lake`, $LAKE_KEY, and MCP's server named lake.
TEST(BrandAgentDocs, LakesOwnBrandServesTheSameAgentSurfaces) {
    set_instance_brand(Brand{});
    const Registry registry = every_capability();
    const std::string origin = "https://lake.example";
    const std::string llms = agent_docs::llms_txt(registry, origin);
    EXPECT_TRUE(llms.starts_with("# Lake\n\n> Lake is Orama's platform for designing, training and "
                                 "shipping AI on your own data."));
    EXPECT_NE(llms.find("## The Lake skill for Claude\n\nDownload [SKILL.md](https://lake.example/"
                        "skills/lake/SKILL.md) (public, no key needed) and install it:\n\n```sh\n"
                        "mkdir -p ~/.claude/skills/lake && curl -fsSL https://lake.example/skills/"
                        "lake/SKILL.md -o ~/.claude/skills/lake/SKILL.md\n```"),
              std::string::npos);
    EXPECT_NE(llms.find("- Claude Code: `claude mcp add --transport http lake https://lake.example/"
                        "mcp --header \"Authorization: Bearer $LAKE_KEY\"`"),
              std::string::npos);
    EXPECT_NE(llms.find("lake.live is prepaid"), std::string::npos);
    EXPECT_TRUE(agent_docs::llms_full_txt(registry, origin).starts_with("# Lake — every tool\n\n"));
    EXPECT_NE(agent_docs::llms_full_txt(registry, origin)
                  .find("\nThe Lake skill for Claude: https://lake.example/skills/lake/SKILL.md "
                        "(public).\n"),
              std::string::npos);
    EXPECT_TRUE(agent_docs::skill_md(registry, origin).starts_with("---\nname: lake\n"));
    ApiContext context;
    const auto initialized = mcp_dispatch(
        registry, json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}}, context, "test");
    ASSERT_TRUE(initialized);
    EXPECT_EQ(json::parse(*initialized)["result"]["serverInfo"]["name"], "lake");
    EXPECT_EQ(openapi_document(registry, "test")["info"]["title"], "Lake control surface");
}

#include "lake/api/capabilities.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "lake/api/agent_docs.hpp"
#include "lake/api/billing.hpp"
#include "lake/api/clerk.hpp"
#include "lake/api/markdown.hpp"
#include "lake/api/web_ui.hpp"
#include "lake/control/policy.hpp"
#include "lake/control/store.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "lake/version.hpp"

// The reads lake's web app needs that the server-rendered pages computed for themselves (lake#561,
// docs/api/web-app.md): who is signed in and how this lake signs people in, the Docs guides,
// Developer → MCP, the Packages catalog, and which buttons a person may use.
namespace lake::api {
namespace {

using nlohmann::json;

json object_schema(json properties, json required = json::array()) {
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

json string_schema() {
    return {{"type", "string"}};
}

json string_array_schema() {
    return {{"type", "array"}, {"items", string_schema()}};
}

std::expected<std::string, ApiError> no_object(const json&, ApiContext&) {
    return std::string{};
}

std::expected<std::string, ApiError> self_object(const json&, ApiContext& context) {
    return "principal/" + context.acting_identity;
}

// lake's browser-facing origin: the configured external origin, else the public host, else the
// origin this request came to (the same order as the server-rendered pages).
std::string public_origin(const ApiContext& context) {
    if (!context.external_origin.empty()) {
        return context.external_origin;
    }
    if (!context.public_host.empty()) {
        return "https://" + context.public_host;
    }
    return context.request_origin;
}

// Another capability, called as the same person with its own authorization and audit, on a copy
// of the context so this call's own audit record is left as it is.
liquid::runtime::task<ApiResult>
call_as_caller_async(ApiContext& context, std::string name, json input) {
    if (context.registry == nullptr) {
        co_return ApiResult::failure("store_unavailable", "no registry to call " + name);
    }
    ApiContext inner = context;
    auto done = co_await context.registry->call_async(name, input, inner);
    co_return done ? std::move(*done) : ApiResult::failure("store_error", done.error().message);
}

// A string field, or empty when it is absent or null (a customer's user_id is null for an
// organization's space).
std::string text_of(const json& value, const char* key) {
    const auto found = value.find(key);
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::string();
}

std::string lower(std::string text) {
    std::ranges::transform(
        text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// ---- viewer ----

json workspace_item_schema() {
    return object_schema({{"id", string_schema()},
                          {"name", string_schema()},
                          {"customer_id", string_schema()},
                          {"status", string_schema()}},
                         json::array({"id", "name", "customer_id", "status"}));
}

json space_schema() {
    return object_schema({{"id", string_schema()},
                          {"name", string_schema()},
                          {"label", string_schema()},
                          {"personal", {{"type", "boolean"}}},
                          {"kind", {{"type", "string"}, {"enum", {"individual", "organization"}}}},
                          {"workspaces", {{"type", "array"}, {"items", workspace_item_schema()}}}},
                         json::array({"id", "name", "label", "personal", "kind", "workspaces"}));
}

json viewer_schema() {
    return object_schema(
        {{"signed_in", {{"type", "boolean"}}},
         {"administrator", {{"type", "boolean"}}},
         {"user_id", string_schema()},
         {"taap_id", string_schema()},
         {"session_id", string_schema()},
         {"credential_kind", string_schema()},
         {"display_name", string_schema()},
         {"email", string_schema()},
         {"mfa_enrolled", {{"type", "boolean"}}},
         {"customer_id", string_schema()},
         {"workspace_id", string_schema()},
         {"spaces", {{"type", "array"}, {"items", space_schema()}}},
         {"clerk",
          object_schema({{"enabled", {{"type", "boolean"}}},
                         {"publishable_key", string_schema()},
                         {"frontend_api", string_schema()}},
                        json::array({"enabled", "publishable_key", "frontend_api"}))},
         {"password_sign_up", {{"type", "boolean"}}},
         {"external_origin", string_schema()},
         {"public_host", string_schema()},
         {"config",
          object_schema({{"frontier_model", string_schema()}}, json::array({"frontier_model"}))},
         {"billing", {{"type", "boolean"}}},
         {"version", string_schema()},
         {"revision", string_schema()}},
        json::array({"signed_in",
                     "administrator",
                     "mfa_enrolled",
                     "spaces",
                     "clerk",
                     "password_sign_up",
                     "external_origin",
                     "public_host",
                     "config",
                     "billing",
                     "version",
                     "revision"}));
}

// A space's live workspaces as the selector lists them (desired != "deleted").
json live_workspaces(control::Store& store, const std::string& customer_id) {
    json out = json::array();
    const auto rows = store.list_workspaces(customer_id);
    if (!rows) {
        return out;
    }
    for (const control::Workspace& row : *rows) {
        if (row.desired == "deleted") {
            continue;
        }
        out.push_back({{"id", row.id},
                       {"name", row.name},
                       {"customer_id", row.customer_id},
                       {"status", row.phase.empty() ? std::string("pending") : lower(row.phase)}});
    }
    return out;
}

ApiResult viewer(const json& input, ApiContext& context, const std::string& version) {
    const auto clerk = clerk_config(context.clerk_publishable_key);
    json out{
        {"signed_in", context.authorized},
        {"administrator", context.authorized && context.operator_credential},
        {"mfa_enrolled", false},
        {"spaces", json::array()},
        {"clerk",
         {{"enabled", clerk.has_value()},
          {"publishable_key", clerk ? clerk->publishable_key : std::string()},
          {"frontend_api", clerk ? clerk->frontend_api : std::string()}}},
        // Under Clerk, sign-up is Clerk's and restricted to invitations (docs/api/clerk.md).
        {"password_sign_up", !clerk.has_value()},
        {"external_origin", public_origin(context)},
        {"public_host", context.public_host},
        {"config", {{"frontier_model", ""}}},
        // Whether this lake bills (LAKE_BILLING, lake#564): the web app shows Billing only then.
        {"billing", billing_registered(context.registry)},
        {"version", version},
        {"revision", std::string(build_revision_string())}};
    if (!context.authorized || context.store == nullptr) {
        return ApiResult::success(std::move(out));
    }
    const control::WorkspaceConfig defaults;
    const control::WorkspaceConfig& config =
        context.workspaces != nullptr ? context.workspaces->config() : defaults;
    // The base model's path on the workspace's volume is lake's own (lake#594); create_agent
    // fills it in for a native agent.
    out["config"] = {{"frontier_model", config.frontier_model}};
    if (!context.operator_credential && !context.acting_user_id.empty()) {
        out["user_id"] = context.acting_user_id;
        out["taap_id"] = context.acting_identity;
        out["session_id"] = context.credential_session_id;
        out["credential_kind"] = context.credential_kind;
        if (const auto user = context.store->get_user(context.acting_user_id)) {
            out["display_name"] = user->display_name;
            out["email"] = user->email;
        }
        const auto enrolled = context.store->mfa_enrolled(context.acting_user_id, context.pool_id);
        out["mfa_enrolled"] = enrolled && *enrolled;
    }
    // The spaces are exactly what list_customers gives this caller.
    const ApiResult customers = readable_customers(context, 200);
    if (!customers.ok) {
        return customers;
    }
    json spaces = json::array();
    for (const json& row : customers.value.value("items", json::array())) {
        control::Customer customer;
        customer.id = text_of(row, "id");
        customer.customer_type = text_of(row, "customer_type");
        customer.user_id = text_of(row, "user_id");
        customer.name = text_of(row, "name");
        const std::string& id = customer.id;
        const std::string& kind = customer.customer_type;
        const bool personal = kind == "individual" && !context.acting_user_id.empty() &&
                              customer.user_id == context.acting_user_id;
        spaces.push_back({{"id", id},
                          {"name", customer.name},
                          {"label", web_ui_space_label(customer, context.acting_user_id)},
                          {"personal", personal},
                          {"kind", kind == "organization" ? "organization" : "individual"},
                          {"workspaces", live_workspaces(*context.store, id)}});
    }
    const auto in_spaces = [&](const std::string& customer_id) {
        return std::ranges::any_of(spaces, [&](const json& space) {
            return space.value("id", std::string()) == customer_id;
        });
    };
    // The space: the one asked for when the viewer can see it, else the asked workspace's space,
    // else the first. The workspace: the one asked for when it is live in one of those spaces,
    // else the space's only workspace, else none.
    std::string customer_id = input.value("customer_id", std::string());
    if (!in_spaces(customer_id)) {
        customer_id.clear();
    }
    std::string workspace_id = input.value("workspace_id", std::string());
    if (!workspace_id.empty()) {
        const auto workspace = context.store->get_workspace(workspace_id);
        if (workspace && workspace->desired != "deleted" && in_spaces(workspace->customer_id) &&
            (customer_id.empty() || customer_id == workspace->customer_id)) {
            customer_id = workspace->customer_id;
        } else {
            workspace_id.clear();
        }
    }
    if (customer_id.empty() && !spaces.empty()) {
        customer_id = spaces.front().value("id", std::string());
    }
    if (workspace_id.empty() && !customer_id.empty()) {
        for (const json& space : spaces) {
            const json& workspaces = space.at("workspaces");
            if (space.value("id", std::string()) == customer_id && workspaces.size() == 1) {
                workspace_id = workspaces.front().value("id", std::string());
            }
        }
    }
    if (!customer_id.empty()) {
        out["customer_id"] = customer_id;
    }
    if (!workspace_id.empty()) {
        out["workspace_id"] = workspace_id;
    }
    out["spaces"] = std::move(spaces);
    return ApiResult::success(std::move(out));
}

// ---- my_permissions ----

constexpr std::string_view permission_actions[] = {
    "read", "write", "admin", "deploy", "review", "share", "promote"};

// The policy object a my_permissions call is about: a drive folder is the nearest folder (or the
// repository) the Drive capabilities authorize, every other type the object of that type and id.
// The object types my_permissions takes (lake#594): the policy objects (db/schema.sql
// objects_type_check), a workspace (its space's permissions) and a drive folder.
const std::vector<std::string> permission_object_types = {"customer",
                                                          "workspace",
                                                          "organization",
                                                          "team",
                                                          "member",
                                                          "repository",
                                                          "path",
                                                          "environment",
                                                          "secret_mount",
                                                          "secret",
                                                          "application",
                                                          "drive folder"};

std::expected<std::string, ApiError> permission_object(const json& input, ApiContext& context) {
    const std::string type = input.at("object_type");
    const std::string id = input.at("object_id");
    if (type == "drive folder") {
        const auto drive =
            context.registry ? context.registry->find("drive_effective_access") : nullptr;
        if (!drive || !drive->resolve_object) {
            return std::unexpected(ApiError{"store_unavailable", "Drive is not available", {}});
        }
        return drive->resolve_object({{"repository_id", id},
                                      {"ref", input.value("ref", std::string("main"))},
                                      {"path", input.value("path", std::string())}},
                                     context);
    }
    // A workspace and its drives are authorized on the space that owns them (lake#594).
    if (type == "workspace") {
        const auto workspace = context.store->get_workspace(id);
        if (!workspace || workspace->desired == "deleted") {
            return std::unexpected(ApiError{"not_found", "no workspace " + id, "/object_id"});
        }
        const auto space = context.store->find_policy_object("customer", workspace->customer_id);
        if (!space) {
            return std::unexpected(
                ApiError{space.error().code, space.error().message, "/object_id"});
        }
        return space->id;
    }
    if (type == "drive") {
        return std::unexpected(ApiError{
            "invalid_input",
            "a drive is authorized on its workspace: send object_type workspace and the drive's "
            "workspace_id as object_id",
            "/object_type"});
    }
    if (std::ranges::find(permission_object_types, type) == permission_object_types.end()) {
        return std::unexpected(ApiError{"invalid_input",
                                        "expected one of " + json(permission_object_types).dump(),
                                        "/object_type"});
    }
    const auto object = context.store->find_policy_object(type, id);
    if (!object) {
        return std::unexpected(ApiError{object.error().code, object.error().message, "/object_id"});
    }
    return object->id;
}

liquid::runtime::task<ApiResult> my_permissions(const json& input, ApiContext& context) {
    if (context.store == nullptr) {
        co_return ApiResult::failure("store_unavailable", "no control-plane store is configured");
    }
    // A drive folder is found in the repository's tree: a repository store read, off the server
    // thread (lake#380).
    // Not std::expected: a liquid task already wraps its value in one (see ZettaAnswer).
    const auto resolve = [&input, &context] {
        const auto found = permission_object(input, context);
        return found ? ApiResult::success(*found)
                     : ApiResult::failure(
                           found.error().code, found.error().message, found.error().path);
    };
    ApiResult resolved;
    if (input.at("object_type") == "drive folder") {
        auto completed = co_await run_blocking(resolve);
        resolved = completed ? std::move(*completed)
                             : ApiResult::failure("busy", completed.error().message);
    } else {
        resolved = resolve();
    }
    if (!resolved.ok) {
        co_return resolved;
    }
    const std::string object = resolved.value.get<std::string>();
    json allowed = json::object();
    for (const json& requested : input.at("actions")) {
        const std::string action = requested.get<std::string>();
        // Promoting a trial is an administrator's decision on the space (docs/api/experiments.md).
        const std::string checked = action == "promote" ? std::string("admin") : action;
        if (context.acting_identity == "admin") {
            allowed[action] = true;
            continue;
        }
        const auto decision = control::authorize(
            *context.store, context.acting_identity, object, checked, context.authorization_cache);
        if (!decision) {
            co_return ApiResult::failure(decision.error().code, decision.error().message);
        }
        allowed[action] = decision->allowed;
    }
    co_return ApiResult::success({{"allowed", std::move(allowed)}});
}

// ---- list_packages ----

json package_schema() {
    return object_schema({{"name", string_schema()}, {"versions", string_array_schema()}},
                         json::array({"name", "versions"}));
}

liquid::runtime::task<ApiResult> list_packages(const json& input, ApiContext& context) {
    json catalog = context.artifact_catalog ? context.artifact_catalog() : json::object();
    std::string origin = context.external_origin;
    if (origin.empty() && !context.public_host.empty()) {
        origin = "https://" + context.public_host;
    }
    json out{{"available", catalog.value("available", false)},
             {"origin", origin},
             {"npm", catalog.value("npm", json::array())},
             {"python", catalog.value("python", json::array())},
             {"oci", catalog.value("oci", json::array())}};
    // The space's own images, from their routes and the tags in each routed workspace (lake#383),
    // for a viewer who may read the space.
    if (const std::string customer_id = input.value("customer_id", std::string());
        !customer_id.empty()) {
        auto listed = co_await call_as_caller_async(
            context, "list_image_repositories", {{"customer_id", customer_id}});
        const ApiResult images =
            listed ? std::move(*listed) : ApiResult::failure("store_error", listed.error().message);
        if (!images.ok) {
            co_return images;
        }
        for (const json& image : images.value.value("items", json::array())) {
            if (image.value("name", std::string()).starts_with("cache/")) {
                continue;
            }
            json versions = json::array();
            for (const json& tag : image.value("tags", json::array())) {
                versions.push_back(tag.value("tag", std::string()));
            }
            out["oci"].push_back(
                {{"name", image.value("repository", std::string())}, {"versions", versions}});
        }
    }
    co_return ApiResult::success(std::move(out));
}

// ---- Docs ----

json doc_link_schema() {
    return object_schema(
        {{"slug", string_schema()}, {"title", string_schema()}, {"section", string_schema()}},
        json::array({"slug", "title"}));
}

json doc_heading_schema() {
    return object_schema({{"level", {{"type", "integer"}}},
                          {"text", string_schema()},
                          {"anchor", string_schema()}},
                         json::array({"level", "text", "anchor"}));
}

json doc_page_schema() {
    return object_schema({{"slug", string_schema()},
                          {"title", string_schema()},
                          {"section", string_schema()},
                          {"headings", {{"type", "array"}, {"items", doc_heading_schema()}}},
                          {"match", {{"type", "string"}, {"enum", {"title", "heading", "text"}}}},
                          {"anchor", string_schema()},
                          {"excerpt", string_schema()}},
                         json::array({"slug", "title", "section", "headings"}));
}

std::string ascii_lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// One heading of a guide as the renderers anchor it: every #–#### heading outside a code block is
// counted, so a repeated heading's anchor is numbered -2, -3, … exactly as markdown_to_html and the
// web app's Markdown number it. `at` is the heading line's offset in the Markdown.
struct DocHeading {
    int level = 0;
    std::string text, anchor;
    std::size_t at = 0;
};

std::vector<DocHeading> doc_headings(std::string_view markdown) {
    std::vector<DocHeading> out;
    std::map<std::string, int> seen;
    bool code = false;
    std::size_t start = 0;
    while (start <= markdown.size()) {
        const std::size_t end = std::min(markdown.find('\n', start), markdown.size());
        const std::string_view line = markdown.substr(start, end - start);
        if (line.starts_with("```")) {
            code = !code;
        } else if (!code) {
            int level = 0;
            while (level < static_cast<int>(line.size()) && line[level] == '#') {
                ++level;
            }
            if (level >= 1 && level <= 4 && line.size() > static_cast<std::size_t>(level) &&
                line[level] == ' ') {
                const std::string_view raw = line.substr(level + 1);
                std::string anchor = markdown_anchor(raw);
                if (const int count = ++seen[anchor]; count > 1) {
                    anchor += "-" + std::to_string(count);
                }
                // The heading as read: inline code and emphasis markers are not part of its text.
                std::string text;
                for (const char c : raw) {
                    if (c != '`' && c != '*') {
                        text += c;
                    }
                }
                out.push_back({level, std::move(text), std::move(anchor), start});
            }
        }
        if (end == markdown.size()) {
            break;
        }
        start = end + 1;
    }
    return out;
}

// A short excerpt of `text` around byte `at`, whitespace collapsed, never splitting a UTF-8
// character.
std::string doc_excerpt(std::string_view text, std::size_t at, std::size_t length) {
    std::size_t from = at > 60 ? at - 60 : 0;
    std::size_t to = std::min(text.size(), from + length);
    const auto continuation = [&](std::size_t i) {
        return i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80;
    };
    while (from > 0 && continuation(from)) {
        --from;
    }
    while (to < text.size() && continuation(to)) {
        ++to;
    }
    std::string out = from > 0 ? "…" : "";
    bool space = false;
    for (const char c : text.substr(from, to - from)) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = true;
            continue;
        }
        if (space && !out.empty() && out != "…") {
            out += ' ';
        }
        space = false;
        out += c;
    }
    if (to < text.size()) {
        out += "…";
    }
    return out;
}

json doc_link(const WebUiGuidePage& page) {
    return {{"slug", page.slug}, {"title", page.title}, {"section", page.section}};
}

// The guides in reading order with their sections and headings; with `q`, only the guides whose
// title, headings or text contain it (ASCII case ignored), best match first: a title match, then a
// heading match (its anchor), then the text (the heading above the first match, and an excerpt).
// A guide this lake does not show: Billing where billing is off (an enterprise install, lake#564).
bool hidden_guide(const WebUiGuidePage& page, const ApiContext& context) {
    return page.slug == "billing" && !billing_registered(context.registry);
}

ApiResult list_docs(const json& input, const ApiContext& context) {
    const std::string q = ascii_lower(input.value("q", std::string()));
    if (q.size() > 200) {
        return ApiResult::failure("invalid_input", "q is at most 200 characters", "/q");
    }
    std::vector<std::string> words;
    for (std::size_t at = 0; at < q.size();) {
        const std::size_t end = std::min(q.find(' ', at), q.size());
        if (end > at) {
            words.push_back(q.substr(at, end - at));
        }
        at = end + 1;
    }
    json sections = json::array();
    std::vector<std::pair<int, json>> pages;
    for (const WebUiGuidePage& page : web_ui_guide()) {
        if (hidden_guide(page, context)) {
            continue;
        }
        if (sections.empty() || sections.back() != page.section) {
            sections.push_back(page.section);
        }
        const std::vector<DocHeading> headings = doc_headings(page.markdown);
        json item = doc_link(page);
        item["headings"] = json::array();
        for (const DocHeading& heading : headings) {
            if (heading.level == 2 || heading.level == 3) {
                item["headings"].push_back(
                    {{"level", heading.level}, {"text", heading.text}, {"anchor", heading.anchor}});
            }
        }
        int rank = 0;
        if (!words.empty()) {
            const std::string body = ascii_lower(page.markdown);
            // Every word, in any order (lake#594): "upload a csv" finds the page that says
            // "Upload … a CSV file".
            const auto has_all = [&](const std::string& text) {
                return std::ranges::all_of(words, [&](const std::string& word) {
                    return text.find(word) != std::string::npos;
                });
            };
            const auto in_heading = std::ranges::find_if(headings, [&](const DocHeading& heading) {
                return heading.level > 1 && has_all(ascii_lower(heading.text));
            });
            // The body after the title line, so a title match is not also the first text match.
            const std::size_t after_title = std::min(body.find('\n'), body.size());
            // The text match: the whole query where it appears as written, else the first word.
            std::size_t hit = body.find(q, after_title);
            if (hit == std::string::npos && has_all(body)) {
                hit = body.find(words.front(), after_title);
            }
            if (has_all(ascii_lower(page.title))) {
                item["match"] = "title";
            } else if (in_heading != headings.end()) {
                item["match"] = "heading";
                item["anchor"] = in_heading->anchor;
                rank = 1;
            } else if (hit != std::string::npos && has_all(body)) {
                item["match"] = "text";
                rank = 2;
                for (const DocHeading& heading : headings) {
                    if (heading.level > 1 && heading.at < hit) {
                        item["anchor"] = heading.anchor;
                    }
                }
            } else {
                continue;
            }
            if (hit != std::string::npos) {
                item["excerpt"] = doc_excerpt(page.markdown, hit, 180);
            }
        }
        pages.emplace_back(rank, std::move(item));
    }
    std::ranges::stable_sort(pages, {}, &std::pair<int, json>::first);
    json out = json::array();
    for (auto& [rank, item] : pages) {
        out.push_back(std::move(item));
    }
    return ApiResult::success({{"pages", std::move(out)}, {"sections", std::move(sections)}});
}

ApiResult get_doc(const json& input, const ApiContext& context) {
    std::vector<WebUiGuidePage> pages;
    for (const WebUiGuidePage& page : web_ui_guide()) {
        if (!hidden_guide(page, context)) {
            pages.push_back(page);
        }
    }
    const std::string slug = input.value("slug", std::string());
    const auto found =
        slug.empty() ? pages.begin() : std::ranges::find(pages, slug, &WebUiGuidePage::slug);
    if (found == pages.end()) {
        return ApiResult::failure("not_found", "there is no Docs page " + slug, "/slug");
    }
    json out{{"slug", found->slug},
             {"title", found->title},
             {"section", found->section},
             {"markdown", found->markdown}};
    if (found != pages.begin()) {
        out["previous"] = doc_link(*std::prev(found));
    }
    if (const auto after = std::next(found); after != pages.end()) {
        out["next"] = doc_link(*after);
    }
    return ApiResult::success(std::move(out));
}

// ---- mcp_setup ----

json mcp_setup(ApiContext& context) {
    const std::string origin = public_origin(context);
    json groups = json::array();
    json all = json::array();
    if (context.registry != nullptr) {
        for (const agent_docs::Group& group : agent_docs::tool_groups(*context.registry)) {
            json tools = json::array();
            for (const agent_docs::Tool& tool : group.tools) {
                tools.push_back(
                    {{"name", tool.name}, {"title", tool.title}, {"writes", tool.writes}});
            }
            groups.push_back(
                {{"title", group.title}, {"summary", group.summary}, {"tools", std::move(tools)}});
        }
        for (const auto& capability : context.registry->list()) {
            all.push_back(capability->name);
        }
    }
    json flows = json::array();
    for (const agent_docs::Flow& flow : agent_docs::flows()) {
        json steps = json::array();
        for (const agent_docs::FlowStep& step : flow.steps) {
            steps.push_back({{"tool", step.tool}, {"note", step.note}});
        }
        flows.push_back(
            {{"title", flow.title}, {"summary", flow.summary}, {"steps", std::move(steps)}});
    }
    json read_only = json::array();
    if (context.registry != nullptr) {
        for (const std::string& name : agent_docs::read_tool_names(*context.registry)) {
            read_only.push_back(name);
        }
    }
    return {{"origin", origin},
            {"mcp_url", origin + "/mcp"},
            {"tool_groups", std::move(groups)},
            {"flows", std::move(flows)},
            {"scopes", {{"read_only", std::move(read_only)}, {"all", std::move(all)}}}};
}

json mcp_setup_schema() {
    const json tool = object_schema(
        {{"name", string_schema()}, {"title", string_schema()}, {"writes", {{"type", "boolean"}}}},
        json::array({"name", "title", "writes"}));
    const json group = object_schema({{"title", string_schema()},
                                      {"summary", string_schema()},
                                      {"tools", {{"type", "array"}, {"items", tool}}}},
                                     json::array({"title", "summary", "tools"}));
    const json step = object_schema({{"tool", string_schema()}, {"note", string_schema()}},
                                    json::array({"tool", "note"}));
    const json flow = object_schema({{"title", string_schema()},
                                     {"summary", string_schema()},
                                     {"steps", {{"type", "array"}, {"items", step}}}},
                                    json::array({"title", "summary", "steps"}));
    return object_schema(
        {{"origin", string_schema()},
         {"mcp_url", string_schema()},
         {"tool_groups", {{"type", "array"}, {"items", group}}},
         {"flows", {{"type", "array"}, {"items", flow}}},
         {"scopes",
          object_schema({{"read_only", string_array_schema()}, {"all", string_array_schema()}},
                        json::array({"read_only", "all"}))}},
        json::array({"origin", "mcp_url", "tool_groups", "flows", "scopes"}));
}

} // namespace

void register_console_capabilities(Registry& registry, std::string_view version) {
    const std::string version_string(version);
    registry.add(Capability{
        .name = "viewer",
        .title = "Who is signed in",
        .description =
            "Who is calling and how this lake signs people in, for the web app: signed in or "
            "not, operator or person, the person's identity, session and MFA state, the spaces "
            "they can see with each space's live workspaces, the current space and workspace "
            "(`workspace_id` when it is live in one of those spaces, else the space's only "
            "workspace), Clerk's publishable key and Frontend API host when Clerk signs people "
            "in, the browser origin, the agent model settings, and this lake's version. Needs no "
            "credential: a visitor gets `signed_in: false` and no spaces.",
        .input_schema =
            object_schema({{"workspace_id", string_schema()}, {"customer_id", string_schema()}}),
        .output_schema = viewer_schema(),
        .kind = CapabilityKind::Read,
        .requires_auth = false,
        .object = "server",
        .action = "read",
        .resolve_object = no_object,
        .handler = [version_string](const json& input, ApiContext& context) {
            return viewer(input, context, version_string);
        }});
    registry.add(Capability{
        .name = "my_permissions",
        .title = "What I may do",
        .description =
            "Whether you may take each of `actions` on one object, so a page shows only the "
            "buttons you can use. `object_type` and `object_id` name a policy object (`customer` "
            "and a space id, `workspace` and a workspace id (its space's permissions, which "
            "also govern its drives), `repository` and a repository id, ...); `drive folder` "
            "names a repository by `object_id` and a folder by `path` at `ref` (default "
            "`main`), as the Drive capabilities authorize it. `promote` is `admin` on the object. "
            "It never "
            "replaces the check each capability makes itself.",
        .input_schema =
            object_schema({{"object_type",
                            {{"type", "string"},
                             {"description",
                              "customer, workspace, organization, team, member, repository, "
                              "path, environment, secret_mount, secret, application or drive "
                              "folder"}}},
                           {"object_id", string_schema()},
                           {"actions",
                            {{"type", "array"},
                             {"items",
                              {{"type", "string"},
                               {"enum",
                                json(std::vector<std::string>(std::begin(permission_actions),
                                                              std::end(permission_actions)))}}}}},
                           {"ref", string_schema()},
                           {"path", string_schema()}},
                          json::array({"object_type", "object_id", "actions"})),
        .output_schema = object_schema(
            {{"allowed", {{"type", "object"}, {"additionalProperties", {{"type", "boolean"}}}}}},
            json::array({"allowed"})),
        .kind = CapabilityKind::Read,
        .post_alias = true,
        .object = "principal",
        .action = "read",
        .resolve_object = self_object,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto done = co_await my_permissions(input, context);
            co_return done ? std::move(*done)
                           : ApiResult::failure("store_error", done.error().message);
        }});
    registry.add(Capability{
        .name = "list_packages",
        .title = "List packages",
        .description =
            "What this lake's registries hold, for Ship → Packages: npm packages, Python "
            "projects and one-component OCI images with their versions (public reads), plus the "
            "tenant images of `customer_id` when given, and the origin install commands use. "
            "`available` is false when this lake has no registry storage.",
        .input_schema = object_schema({{"customer_id", string_schema()}}),
        .output_schema =
            object_schema({{"available", {{"type", "boolean"}}},
                           {"origin", string_schema()},
                           {"npm", {{"type", "array"}, {"items", package_schema()}}},
                           {"python", {{"type", "array"}, {"items", package_schema()}}},
                           {"oci", {{"type", "array"}, {"items", package_schema()}}}},
                          json::array({"available", "origin", "npm", "python", "oci"})),
        .kind = CapabilityKind::Read,
        .object = "principal",
        .action = "read",
        .resolve_object = self_object,
        // The instance packages are public reads; a space's images are authorized by
        // list_image_repositories, called as the caller.
        .authorizes_candidates = true,
        .async_handler = [](const json& input,
                            ApiContext& context) -> liquid::runtime::task<ApiResult> {
            auto done = co_await list_packages(input, context);
            co_return done ? std::move(*done)
                           : ApiResult::failure("store_error", done.error().message);
        }});
    registry.add(Capability{
        .name = "list_docs",
        .title = "List Docs pages",
        .description =
            "The Docs guides in reading order, each by its slug, title, section (the group the "
            "Docs navigation shows it under) and its headings (level 2 and 3, each with the "
            "anchor that links to it), plus the sections in order. With `q`, only the guides "
            "whose title, headings or text contain every word of it, in any order (case "
            "ignored), best match first, each saying what matched, the anchor of the heading to "
            "open and an excerpt. Read one with get_doc by its `slug`. Public.",
        .input_schema = object_schema({{"q", string_schema()}}),
        .output_schema =
            object_schema({{"pages", {{"type", "array"}, {"items", doc_page_schema()}}},
                           {"sections", string_array_schema()}},
                          json::array({"pages", "sections"})),
        .kind = CapabilityKind::Read,
        .requires_auth = false,
        .object = "server",
        .action = "read",
        .resolve_object = no_object,
        // Every guide is public.
        .authorizes_candidates = true,
        .handler = [](const json& input, ApiContext& context) {
            return list_docs(input, context);
        }});
    registry.add(Capability{
        .name = "get_doc",
        .title = "Read a Docs page",
        .description = "One Docs guide as Markdown, by the `slug` list_docs answers (absent is "
                       "the first, Getting Started), with its section and the previous and next "
                       "guides in reading order. `page` is the deprecated name of `slug`. Public.",
        .input_schema = object_schema({{"slug", string_schema()}}),
        .output_schema = object_schema({{"slug", string_schema()},
                                        {"title", string_schema()},
                                        {"section", string_schema()},
                                        {"markdown", string_schema()},
                                        {"previous", doc_link_schema()},
                                        {"next", doc_link_schema()}},
                                       json::array({"slug", "title", "section", "markdown"})),
        .kind = CapabilityKind::Read,
        .requires_auth = false,
        .object = "server",
        .action = "read",
        .resolve_object = no_object,
        .handler = [](const json& input, ApiContext& context) { return get_doc(input, context); },
        // list_docs answers `slug`, so get_doc takes it (lake#591).
        .input_aliases = {{"page", "slug"}}});
    registry.add(Capability{
        .name = "mcp_setup",
        .title = "MCP setup",
        .description =
            "What Developer → MCP shows: lake's origin and MCP endpoint, every tool by product "
            "group (name, title, whether it writes), the worked flows, and the scopes a "
            "read-only key and an every-tool key carry. Create the key itself with create_token.",
        .input_schema = object_schema(json::object()),
        .output_schema = mcp_setup_schema(),
        .kind = CapabilityKind::Read,
        .object = "principal",
        .action = "read",
        .resolve_object = self_object,
        .handler = [](const json&, ApiContext& context) {
            return ApiResult::success(mcp_setup(context));
        }});
}

} // namespace lake::api

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/control/store.hpp"

// What lake still renders on the server, as pure functions (data in, HTML out; no HTTP type, no
// I/O). The console is the web app in frontend/ (docs/ui/web-app.md); the one server-rendered
// surface left is the hosted sign-in pages an identity application shows its own users, with the
// stylesheet and fonts they link.
namespace lake::api {

std::string web_ui_escape(std::string_view text);
// A space's label for people: the viewer's own individual space reads "Your workspaces", never its
// generated handle (lake#452) or "personal space", which the product does not say (Steve,
// 2026-10-10); every other space keeps its name. The `viewer` capability labels spaces this way
// (lake#561).
std::string web_ui_space_label(const control::Customer& customer, std::string_view user_id);
// The syntax units of a source file, as the File page shades them (lake#561): an array of
// {start_line, end_line, kind} from liquid's parser for the formats it parses (C and C++ by
// extension), or nothing when the format has none. repository_file returns it with `units: true`.
std::optional<nlohmann::json> web_ui_source_units(std::string_view path, std::string_view contents);
// The page a browser sees when a handler fails unexpectedly (route_guard.hpp).
std::string web_ui_error(std::string_view code, std::string_view message);

// The token-driven stylesheet and the self-hosted IBM Plex faces it names, each also served at a
// content-hashed URL that is immutable for a year; the plain URLs are `no-cache`.
std::string web_ui_hashed_asset_url(std::string_view path, std::string_view contents);
std::string_view web_ui_stylesheet();
std::string_view web_ui_stylesheet_url();
std::span<const std::string_view> web_ui_font_names();
std::string_view web_ui_font_url(std::string_view name);
std::string_view web_ui_asset_cache_control(std::string_view path);
// A self-hosted IBM Plex WOFF2 face embedded in the binary. Unknown names return an empty view.
std::string_view web_ui_font(std::string_view name);

// The Docs guides (lake#252): docs/guide/*.md embedded at build time, in reading order, read by
// the list_docs and get_doc capabilities.
struct WebUiGuidePage {
    std::string_view slug, title, markdown;
};
const std::vector<WebUiGuidePage>& web_ui_guide();

// The hosted pages of an identity application (docs/api/apps.md), in its name and colours, at
// /apps/<id>/... and on its custom sign-in domain. They work without JavaScript, except the
// passkey page, which needs navigator.credentials.
std::string web_ui_application_sign_up(const control::Application& application,
                                       std::string_view error);
std::string web_ui_application_sign_in(const control::Application& application,
                                       std::string_view error);
std::string web_ui_application_mfa(const control::Application& application, std::string_view error);
std::string web_ui_application_passkey_sign_in(const control::Application& application,
                                               std::string_view error);

} // namespace lake::api

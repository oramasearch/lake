#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "lake/api/orders.hpp"
#include "lake/api/regions.hpp"
#include "lake/control/store.hpp"

// What lake still renders on the server, as pure functions (data in, HTML out; no HTTP type, no
// I/O). The console is the web app in frontend/ (docs/ui/web-app.md); the server-rendered
// surfaces left are the hosted sign-in pages an identity application shows its own users and the
// apex host's region picker (docs/api/regions.md), with the stylesheet and fonts they link.
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
// the list_docs and get_doc capabilities. `section` is the group the Docs navigation shows the page
// under (cmake/embed_guide.cmake's GUIDE_SECTIONS, lake#561); pages of one section are adjacent.
struct WebUiGuidePage {
    std::string_view slug, title, markdown, section;
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

// The apex host's region picker (lake#757, docs/ui/regions.md): every region of the deployment as a
// choice, `selected` preselected, posting back to `target` (the request's own path and query),
// which remembers the choice and continues to that path on the chosen region's host. Works without
// JavaScript. `error` is shown above the choices when not empty.
std::string web_ui_region_picker(const Regions& regions,
                                 const Region& selected,
                                 std::string_view target,
                                 std::string_view error);

// The apex order sheet (lake#831, docs/ui/regions.md#the-order-sheet), server-rendered and working
// without JavaScript. The sheet: the trust line, then the shared option holding the region picker
// above unchanged (`selected` preselected, posting back to `target`), and one card per paid offer
// with its price in USD, its included usage credit and an Order link to its form.
std::string web_ui_order_sheet(const OrderSheet& sheet,
                               const Regions& regions,
                               const Region& selected,
                               std::string_view target,
                               std::string_view error);
// One offer's order form, posting to /order: where it runs, the contact details, the quantity and
// free text, a hidden field people never fill (a bot does), and the two ways to send it: pay with
// card (when `checkout_available` and the offer is paid through Checkout) or send it as an order
// form. `values` refill the form; `errors` are shown beside their fields.
std::string web_ui_order_form(const OrderSheet& sheet,
                              const Regions& regions,
                              const OrderOffer& offer,
                              const OrderFormInput& values,
                              const OrderFormErrors& errors,
                              bool checkout_available);
// After an order form is sent: its id and the status link.
std::string web_ui_order_received(const OrderSheet& sheet,
                                  const Regions& regions,
                                  const control::CloudOrder& order,
                                  std::string_view status_url);
// An order's status page: its state in words, the host once ready, what was ordered.
// `checkout` is the `checkout` query value Stripe returns with (done or cancelled).
std::string web_ui_order_status(const OrderSheet& sheet,
                                const Regions& regions,
                                const control::CloudOrder& order,
                                std::string_view checkout);
// A short page on the apex that says one thing (an order not found, too many orders).
std::string web_ui_order_message(std::string_view title, std::string_view message);
// The published price list the shared option links to: the list in force, or a sentence when none
// is published yet.
std::string web_ui_price_list(const std::vector<PriceListRow>& rows,
                              std::string_view effective_from);

} // namespace lake::api

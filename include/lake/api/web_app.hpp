// SPDX-License-Identifier: MIT

#pragma once

// The web app: the single-page application built from frontend/ (React, Vite) and embedded into
// the binary by tools/ui/embed-frontend.mjs. The server answers every page route with
// /index.html and serves the content-hashed files under /ui/ immutable. See docs/ui/README.md.

#include <span>
#include <string>
#include <string_view>

namespace lake::api {

struct WebAppAsset {
    std::string_view path;         // served path, e.g. "/index.html", "/ui/index-3f9a1c2e.js"
    std::string_view content_type; // media type sent with it
    std::string_view body;
};

// Every embedded file of the built app.
std::span<const WebAppAsset> web_app_assets();

// The embedded file served at `path`, or nullptr.
const WebAppAsset* web_app_asset(std::string_view path);

// The app's entry document (/index.html).
std::string_view web_app_index();

// Whether a request is a browser opening a page of the app, answered with index.html: a GET or
// HEAD that accepts text/html, for a path no server route owns (the API, downloads, protocol
// routes, the hosted application pages). `custom_domain` is true when the request's host is not
// lake's own host; there an identity application's hosted sign-in pages own /sign-in, /sign-up,
// /mfa and /passkey-sign-in.
bool web_app_serves(std::string_view method,
                    std::string_view path,
                    std::string_view accept,
                    bool custom_domain);

// The Content-Security-Policy of a page of the web app (lake#991,
// docs/serving/http.md#the-web-apps-script-policy): scripts only from lake itself and, by SHA-256,
// the inline scripts `html` carries (index.html's theme script; a `type="application/json"` data
// block runs nothing and is not listed); requests, fonts, media and workers only to lake (`blob:`
// for a recording's preview); images from lake and `data:`; styles from lake and inline (the
// brand's colours and Clerk's components); frames of an https page (an App's preview); no plugins,
// no <base>, forms only to lake, framed only by lake. `clerk_frontend_api` (the Clerk Frontend API
// host, empty without Clerk) adds that host for scripts and requests, Clerk's image host and
// Cloudflare's challenge script, which Clerk's bot protection loads.
std::string web_app_content_security_policy(std::string_view html,
                                            std::string_view clerk_frontend_api);

// The SHA-256 source expressions ('sha256-<base64>') of every inline script in `html` that a
// browser runs: a <script> with no src whose type is absent, empty, a JavaScript type or `module`.
std::string inline_script_hashes(std::string_view html);

// The Cache-Control for an embedded file: hashed /ui/ files are immutable, index.html is not.
std::string_view web_app_cache_control(std::string_view path);

} // namespace lake::api

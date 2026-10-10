#pragma once

// The web app: the single-page application built from frontend/ (React, Vite) and embedded into
// the binary by tools/ui/embed-frontend.mjs. The server answers every page route with
// /index.html and serves the content-hashed files under /ui/ immutable. See docs/ui/README.md.

#include <span>
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

// The Cache-Control for an embedded file: hashed /ui/ files are immutable, index.html is not.
std::string_view web_app_cache_control(std::string_view path);

} // namespace lake::api

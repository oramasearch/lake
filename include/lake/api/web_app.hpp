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

} // namespace lake::api

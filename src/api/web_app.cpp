#include "lake/api/web_app.hpp"

namespace lake::api {

const WebAppAsset* web_app_asset(std::string_view path) {
    for (const WebAppAsset& asset : web_app_assets()) {
        if (asset.path == path) {
            return &asset;
        }
    }
    return nullptr;
}

std::string_view web_app_index() {
    const WebAppAsset* index = web_app_asset("/index.html");
    return index ? index->body : std::string_view{};
}

} // namespace lake::api

#include "lake/control/store_contents.hpp"

#include "lake/control/drives.hpp"

namespace lake::control {

std::string store_kind_refusal(std::string_view type) {
    if (type == "volume") {
        return "a volume is shown, not browsed: its contents belong to the service that mounts it";
    }
    if (type != "object" && type != "filesystem") {
        return "only a bucket or a filesystem opens as a drive";
    }
    return {};
}

std::string store_browse_refusal(std::string_view type, std::string_view provider) {
    if (std::string kind = store_kind_refusal(type); !kind.empty()) {
        return kind;
    }
    const StoreProvider* known = find_store_provider(type, provider);
    const std::string label = known != nullptr ? std::string(known->label)
                                               : std::string(type) + " " + std::string(provider);
    return "Lake cannot browse this store (" + label + ") yet: " + std::string(store_browse_issue);
}

bool valid_store_path(std::string_view path) {
    if (path.empty()) {
        return true;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string_view part = path.substr(
            start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (!valid_drive_name(part)) {
            return false;
        }
        if (slash == std::string_view::npos) {
            return true;
        }
        start = slash + 1;
    }
    return false;
}

std::string join_store_path(std::string_view folder, std::string_view name) {
    return folder.empty() ? std::string(name) : std::string(folder) + "/" + std::string(name);
}

std::string store_parent(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
}

std::string store_name(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

} // namespace lake::control

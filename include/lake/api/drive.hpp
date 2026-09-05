#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lake/api/sync_store.hpp"

namespace lake::api {

struct DrivePreview {
    std::string format;
    std::string media_type;
    std::string html;
};

class DrivePreviewResult {
public:
    DrivePreviewResult(DrivePreview preview);

    static DrivePreviewResult failure(std::string message);

    [[nodiscard]] explicit operator bool() const;
    [[nodiscard]] const DrivePreview& operator*() const;
    [[nodiscard]] const DrivePreview* operator->() const;
    [[nodiscard]] const std::string& error() const;

private:
    DrivePreviewResult() = default;

    std::optional<DrivePreview> preview_;
    std::string error_;
};

using DrivePreviewRenderer = std::function<DrivePreviewResult(const RepositoryFile&)>;

class DriveFormatRegistry {
public:
    bool add(std::string extension, DrivePreviewRenderer renderer);
    [[nodiscard]] DrivePreviewResult preview(const RepositoryFile& file) const;
    [[nodiscard]] std::vector<std::string> extensions() const;

    static DriveFormatRegistry defaults();

private:
    std::map<std::string, DrivePreviewRenderer> renderers_;
    std::vector<std::string> extensions_;
};

} // namespace lake::api

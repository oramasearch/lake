#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lake/api/sync_store.hpp"

namespace lake::control {
struct Repository;
}

namespace lake::api {

struct ApiContext;

// Capture destinations for authorization under SyncStore's mutation lock. The context must
// outlive the synchronous commit call; no repository reads are made by the callback.
[[nodiscard]] RepositoryWriteAuthorizer
repository_write_authorizer(const control::Repository& repository,
                            const std::vector<RepositoryWriteOperation>& operations,
                            ApiContext& context);

struct DrivePreview {
    std::string format;
    std::string media_type;
    std::string html;
};

struct DriveFolderOptions {
    std::string view = "list";
    std::string sort = "name";
    bool descending = false;
};

struct DriveDiffLine {
    std::string kind;
    std::string text;
};

struct DriveVersionDiff {
    RepositoryFile left;
    RepositoryFile right;
    std::vector<DriveDiffLine> lines;
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

[[nodiscard]] bool drive_preview_is_text(const DrivePreview& preview);
[[nodiscard]] DriveVersionDiff drive_text_diff(RepositoryFile left, RepositoryFile right);

} // namespace lake::api

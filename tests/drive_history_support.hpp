#pragma once

// One Drive history exercise (lake#123) shared by the memory, Postgres and workspace-store
// suites: an ordinary overwrite through `upload_drive_files` keeps the previous version as a
// normal prior version that can be compared and restored, a restore is a stable-node modification,
// and a real deletion stays distinct. Every step goes through lake's HTTP routes.

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace lake::test {

// Sends one request to the running lake and answers the HTTP status and the raw body.
using DriveRequest = std::function<std::pair<unsigned, std::string>(
    std::string_view method, const std::string& path, const std::string& body)>;

inline std::string drive_base64(std::string_view text) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    std::size_t index = 0;
    for (; index + 2 < text.size(); index += 3) {
        const unsigned value = (static_cast<unsigned char>(text[index]) << 16U) |
                               (static_cast<unsigned char>(text[index + 1]) << 8U) |
                               static_cast<unsigned char>(text[index + 2]);
        encoded += alphabet[(value >> 18U) & 63U];
        encoded += alphabet[(value >> 12U) & 63U];
        encoded += alphabet[(value >> 6U) & 63U];
        encoded += alphabet[value & 63U];
    }
    if (index < text.size()) {
        unsigned value = static_cast<unsigned char>(text[index]) << 16U;
        if (index + 1 < text.size()) {
            value |= static_cast<unsigned char>(text[index + 1]) << 8U;
        }
        encoded += alphabet[(value >> 18U) & 63U];
        encoded += alphabet[(value >> 12U) & 63U];
        encoded += index + 1 < text.size() ? alphabet[(value >> 6U) & 63U] : '=';
        encoded += '=';
    }
    return encoded;
}

inline std::size_t drive_count(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string_view::npos;
         at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

// Uploads `shared/hello.txt` twice into `repository_id`, whose main branch already exists, then
// checks history, comparison, restore and deletion. `pages` also checks the rendered
// `/drive/history` page.
inline void expect_replaced_drive_file_keeps_its_history(const DriveRequest& request,
                                                         const std::string& repository_id,
                                                         bool pages = true) {
    using nlohmann::json;
    const std::string path = "shared/hello.txt";
    const std::string context = "repository_id=" + repository_id + "&ref=main&path=" + path;
    const auto api = [&](std::string_view method, const std::string& capability,
                         const json& input) -> json {
        const std::string suffix =
            method == "GET" ? "/v1/" + capability + "?" + input.get<std::string>()
                            : "/v1/" + capability;
        const auto [status, body] =
            request(method, suffix, method == "GET" ? std::string() : input.dump());
        const json parsed = json::parse(body, nullptr, false);
        EXPECT_EQ(status, 200U) << capability << ": " << body;
        EXPECT_TRUE(parsed.is_object() && parsed.value("ok", false)) << capability << ": " << body;
        return parsed.is_object() ? parsed.value("value", json::object()) : json::object();
    };
    const auto upload = [&](std::string_view text) {
        return api("POST",
                   "upload_drive_files",
                   {{"repository_id", repository_id},
                    {"ref", "main"},
                    {"path", "shared"},
                    {"files", json::array({{{"name", "hello.txt"},
                                            {"content", drive_base64(text)}}})}});
    };
    const auto history = [&]() { return api("GET", "drive_history", json(context))["items"]; };

    const json first = upload("Version 1\n");
    const json second = upload("Version 2\n");
    ASSERT_FALSE(first.value("change", std::string()).empty()) << first.dump();
    ASSERT_FALSE(second.value("change", std::string()).empty()) << second.dump();
    ASSERT_NE(first["change"], second["change"]);

    // Two available versions: the replacement is current, the first upload a normal prior one.
    const json replaced = history();
    ASSERT_EQ(replaced.size(), 2U) << replaced.dump();
    const json& current = replaced[0];
    const json& previous = replaced[1];
    EXPECT_EQ(current["id"], second["change"]) << replaced.dump();
    EXPECT_EQ(previous["id"], first["change"]) << replaced.dump();
    EXPECT_EQ(current["path"], path);
    EXPECT_EQ(current["changes"], json::array({"modified"})) << replaced.dump();
    EXPECT_EQ(previous["changes"], json::array({"added"})) << replaced.dump();
    ASSERT_FALSE(current.value("new_blob", std::string()).empty()) << replaced.dump();
    ASSERT_FALSE(previous.value("new_blob", std::string()).empty()) << replaced.dump();
    EXPECT_NE(current["new_blob"], previous["new_blob"]);
    EXPECT_EQ(current["old_blob"], previous["new_blob"])
        << "the replacement names the version it replaced";
    const json preview = api("GET", "drive_preview", json(context));
    EXPECT_NE(preview.dump().find("Version 2"), std::string::npos) << preview.dump();

    // The two versions compare.
    const json compared = api("GET",
                              "diff_drive_versions",
                              json(context + "&left=" + previous["id"].get<std::string>() +
                                   "&right=" + current["id"].get<std::string>()));
    EXPECT_EQ(compared["lines"],
              json::array({{{"kind", "removed"}, {"text", "Version 1"}},
                           {{"kind", "added"}, {"text", "Version 2"}},
                           {{"kind", "context"}, {"text", ""}}}))
        << compared.dump();

    if (pages) {
        const auto [status, page] = request("GET", "/drive/history?" + context, {});
        EXPECT_EQ(status, 200U) << page;
        EXPECT_EQ(page.find(">Deleted<"), std::string::npos) << "a replacement is not a deletion";
        for (const json* version : {&current, &previous}) {
            const std::string id = (*version)["id"];
            EXPECT_NE(page.find("aria-label=\"Compare from " + id + "\""), std::string::npos)
                << page;
            EXPECT_NE(page.find("aria-label=\"Compare to " + id + "\""), std::string::npos) << page;
            EXPECT_NE(page.find("name=\"change_id\" value=\"" + id + "\""), std::string::npos)
                << "every available version has its restore form";
        }
        EXPECT_NE(page.find("Compare selected text versions"), std::string::npos);
    }

    // Restoring the previous version writes a new change: a modification of the same node.
    const json restored = api("POST",
                              "restore_drive_version",
                              {{"repository_id", repository_id},
                               {"ref", "main"},
                               {"path", path},
                               {"change_id", previous["id"]}});
    ASSERT_FALSE(restored.value("id", std::string()).empty()) << restored.dump();
    const json after_restore = history();
    ASSERT_EQ(after_restore.size(), 3U) << after_restore.dump();
    EXPECT_EQ(after_restore[0]["id"], restored["id"]);
    EXPECT_EQ(after_restore[0]["changes"], json::array({"modified"})) << after_restore.dump();
    EXPECT_EQ(after_restore[0]["new_blob"], previous["new_blob"]);
    EXPECT_EQ(after_restore[0]["old_blob"], current["new_blob"]);
    const json read_back = api("GET", "drive_preview", json(context));
    EXPECT_NE(read_back.dump().find("Version 1"), std::string::npos) << read_back.dump();

    // A real deletion stays distinct and unavailable at its revision.
    api("POST",
        "delete_drive_path",
        {{"repository_id", repository_id}, {"ref", "main"}, {"path", path}});
    const json deleted = history();
    ASSERT_EQ(deleted.size(), 4U) << deleted.dump();
    EXPECT_EQ(deleted[0]["changes"], json::array({"removed"})) << deleted.dump();
    EXPECT_EQ(deleted[0]["new_blob"], "");
    EXPECT_EQ(deleted[0]["old_blob"], previous["new_blob"]);
    if (pages) {
        const auto [status, page] = request("GET", "/drive/history?" + context, {});
        EXPECT_EQ(status, 200U) << page;
        EXPECT_EQ(drive_count(page, ">Deleted<"), 1U) << page;
        EXPECT_EQ(page.find("name=\"change_id\" value=\"" + deleted[0]["id"].get<std::string>() +
                            "\""),
                  std::string::npos)
            << "a deletion has no restore form of its own";
    }
}

} // namespace lake::test

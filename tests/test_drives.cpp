// Drives (docs/api/drives.md, lake#561): drive records, workspace files sealed in the workspace
// Zetta's `lake_files` bucket, virtual drives of shortcuts, and a repository drive that works from
// the moment it is created. The in-process cases run the capabilities over a workspace service, a
// fake record store (StreamCaller) and a fake object store (FakeZettaObjects); the HTTP cases run
// a whole lake with a real repository store and stream a download.

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "lake/api/audit_routing.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/server.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/drive_files.hpp"
#include "lake/control/drives.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "one_thread_io.hpp"
#include "stream_fixture.hpp"
#include "temporary_directory.hpp"
#include "zetta_object_fixture.hpp"

namespace {

using json = nlohmann::json;
using lake::api::ApiResult;
using lake::control::DriveEntry;
using lake::control::DriveTree;

constexpr std::string_view kek = "6c616b652d776f726b73706163652d7265636f7264732d746573742d6b656b31";
constexpr std::uint64_t MiB = 1024 * 1024;

template <class Task> auto run(Task task) {
    return lake::control::settled(liquid::runtime::block_on(std::move(task)));
}

std::string base64(std::string_view bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t index = 0;
    for (; index + 2 < bytes.size(); index += 3) {
        const auto a = static_cast<unsigned char>(bytes[index]);
        const auto b = static_cast<unsigned char>(bytes[index + 1]);
        const auto c = static_cast<unsigned char>(bytes[index + 2]);
        out += alphabet[a >> 2];
        out += alphabet[((a & 3) << 4) | (b >> 4)];
        out += alphabet[((b & 15) << 2) | (c >> 6)];
        out += alphabet[c & 63];
    }
    if (index < bytes.size()) {
        const auto a = static_cast<unsigned char>(bytes[index]);
        const bool two = index + 1 < bytes.size();
        const auto b = two ? static_cast<unsigned char>(bytes[index + 1]) : 0;
        out += alphabet[a >> 2];
        out += alphabet[((a & 3) << 4) | (b >> 4)];
        out += two ? alphabet[(b & 15) << 2] : '=';
        out += '=';
    }
    return out;
}

// Bytes no block repeats: a counter written through the file.
std::string pattern(std::uint64_t size, char seed = 'a') {
    std::string out;
    out.reserve(size);
    std::uint64_t state = static_cast<unsigned char>(seed);
    while (out.size() < size) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        out.push_back(static_cast<char>(state >> 56));
    }
    return out;
}

DriveEntry entry(std::string id, std::string parent, std::string kind, std::string name) {
    DriveEntry out;
    out.id = std::move(id);
    out.customer_id = "c";
    out.drive_id = "d";
    out.parent_id = std::move(parent);
    out.kind = std::move(kind);
    out.name = std::move(name);
    if (out.kind == "file") {
        out.object_key = "f/" + out.id;
    }
    return out;
}

lake::control::ClerkIdentity person(std::string subject) {
    const auto identity = lake::audit::mint_custodial_identity(std::string(kek));
    return {subject,
            subject + "@example.test",
            "",
            "browser",
            identity->taap_id,
            identity->public_key,
            identity->wrapped_seed};
}

// Two spaces with one Ready workspace each (and a second one in the first space), every workspace
// Zetta at its own endpoint with its own records and its own object buckets.
struct Spaces {
    std::shared_ptr<lake::control::InMemoryStore> store =
        std::make_shared<lake::control::InMemoryStore>();
    std::filesystem::path dir = lake::test::fixture_path("drives-" + lake::control::new_uuid_v4());
    std::shared_ptr<lake::test::StreamCaller> zetta = std::make_shared<lake::test::StreamCaller>();
    std::shared_ptr<lake::test::FakeZettaObjects> objects =
        std::make_shared<lake::test::FakeZettaObjects>();
    std::shared_ptr<lake::control::WorkspaceService> service;
    lake::api::Registry registry;
    struct Space {
        lake::control::Principal principal;
        std::string customer_id;
        std::vector<std::string> workspaces;
    };
    std::vector<Space> spaces;

    Spaces() {
        std::filesystem::create_directories(dir);
        service = std::make_shared<lake::control::WorkspaceService>(
            lake::control::WorkspaceConfig{dir.string(), 0}, store, zetta);
        lake::api::register_core_capabilities(registry, "test");
        for (const std::string name : {"first", "second"}) {
            const auto session =
                store->sign_in_clerk(person(name + "-" + lake::control::new_uuid_v4()));
            EXPECT_TRUE(session);
            Space space;
            space.principal = *store->context_for(session->secret);
            space.customer_id = store->list_customers(10, space.principal.user_id)->front().id;
            for (const std::string workspace : {"home", "lab"}) {
                const auto created =
                    store->create_workspace({space.customer_id, workspace, "dev", ""});
                EXPECT_TRUE(created) << created.error().message;
                ready(created->id);
                space.workspaces.push_back(created->id);
            }
            spaces.push_back(std::move(space));
        }
    }

    static std::string url(const std::string& workspace_id) {
        return "http://" + workspace_id + ".test";
    }

    void ready(const std::string& workspace_id) {
        const auto workspace = store->get_workspace(workspace_id);
        const auto zetta_service = store->get_service(workspace->zetta_service_id);
        EXPECT_TRUE(store->set_service_status(zetta_service->id, "Ready", "", url(workspace_id)));
        std::ofstream(dir / (zetta_service->slug + ".json"))
            << json{{"kind", "zetta"}, {"token", "token-" + workspace_id}}.dump();
        objects->add_node(url(workspace_id), "token-" + workspace_id);
    }

    lake::api::ApiContext context(const Space& space) {
        lake::api::ApiContext context;
        context.store = store.get();
        context.workspaces = service.get();
        context.instance_kek = std::string(kek);
        context.object_transport = objects;
        context.authorized = true;
        context.session_credential = true;
        context.acting_user_id = space.principal.user_id;
        context.acting_identity = space.principal.taap_id;
        context.credential_session_id = space.principal.session_id;
        context.scopes = space.principal.scopes;
        context.request_id = lake::control::new_uuid_v4();
        return context;
    }

    ApiResult call(const Space& space, const std::string& capability, json input) {
        auto context = this->context(space);
        const auto completed =
            liquid::runtime::block_on(registry.call_async(capability, input, context));
        EXPECT_TRUE(completed);
        return *completed;
    }

    // `call` in the space's first workspace (or `workspace`).
    ApiResult
    in(const Space& space, const std::string& capability, json input, std::size_t workspace = 0) {
        input["customer_id"] = space.customer_id;
        input["workspace_id"] = space.workspaces[workspace];
        return call(space, capability, std::move(input));
    }

    std::map<std::string, std::string> files(const std::string& workspace_id) {
        return objects->objects(url(workspace_id), "lake_files");
    }
};

lake::control::DriveFileStore
file_store(lake::test::FakeZettaObjects& fake,
           const std::shared_ptr<lake::test::FakeZettaObjects>& transport,
           const std::string& workspace) {
    fake.add_node(Spaces::url(workspace), "token");
    auto cipher = lake::control::DriveFileCipher::open(kek, workspace);
    EXPECT_TRUE(cipher);
    return lake::control::DriveFileStore(
        std::make_shared<lake::control::ZettaObjectClient>(
            transport,
            lake::control::ObjectEndpoint{Spaces::url(workspace), "token"},
            std::string(lake::control::drive_files_bucket)),
        std::move(*cipher));
}

} // namespace

TEST(Drives, NamesShortcutsAndTheFolderTree) {
    EXPECT_TRUE(lake::control::valid_drive_name("Q3 report (final).pdf"));
    EXPECT_TRUE(lake::control::valid_drive_name("été"));
    for (const std::string& bad : std::vector<std::string>{
             "", ".", "..", ".lake-folder", "a/b", "a\\b", "tab\there", std::string(256, 'x')}) {
        EXPECT_FALSE(lake::control::valid_drive_name(bad)) << bad;
    }
    EXPECT_FALSE(lake::control::drive_shortcut_problem(
        {{"kind", "repository_path"}, {"repository_id", "r"}, {"path", "a/b.txt"}}));
    EXPECT_FALSE(
        lake::control::drive_shortcut_problem({{"kind", "url"}, {"url", "https://x.test/a"}}));
    EXPECT_TRUE(
        lake::control::drive_shortcut_problem({{"kind", "url"}, {"url", "javascript:alert(1)"}}));
    EXPECT_TRUE(lake::control::drive_shortcut_problem({{"kind", "url"}, {"url", "https://a b"}}));
    EXPECT_TRUE(lake::control::drive_shortcut_problem(
        {{"kind", "repository_path"}, {"repository_id", "r"}}));
    EXPECT_TRUE(lake::control::drive_shortcut_problem({{"kind", "printer"}}));
    EXPECT_TRUE(lake::control::drive_shortcut_problem("repository"));

    DriveTree tree({entry("a", "", "folder", "Trip"),
                    entry("b", "a", "folder", "Day 1"),
                    entry("c", "b", "file", "photo.jpg"),
                    entry("d", "", "file", "notes.txt"),
                    entry("e", "a", "shortcut", "Link")});
    EXPECT_EQ(tree.path("c"), "Trip/Day 1/photo.jpg");
    EXPECT_EQ(tree.resolve("", "Trip/Day 1")->id, "b");
    EXPECT_EQ(tree.resolve("a", "Day 1/photo.jpg")->id, "c");
    EXPECT_EQ(tree.resolve("", "Trip/Nope"), nullptr);
    EXPECT_TRUE(tree.within("c", "a"));
    EXPECT_FALSE(tree.within("d", "a"));
    std::vector<std::string> order;
    for (const DriveEntry* item : tree.subtree("a")) {
        order.push_back(item->id);
    }
    ASSERT_EQ(order.size(), 4U);
    EXPECT_EQ(order.back(), "a") << "the folder goes last";
    EXPECT_LT(std::ranges::find(order, "c") - order.begin(),
              std::ranges::find(order, "b") - order.begin())
        << "a child goes before its parent";
    EXPECT_EQ(lake::control::drive_free_name(tree, "", "notes.txt"), "notes (2).txt");
    EXPECT_EQ(lake::control::drive_free_name(tree, "", "Trip"), "Trip (2)");
    EXPECT_EQ(lake::control::drive_free_name(tree, "", "fresh.txt"), "fresh.txt");
    DriveEntry uploading = entry("f", "", "file", "half.bin");
    uploading.state = "uploading";
    tree.add(uploading);
    EXPECT_EQ(tree.children("").size(), 2U) << "an unfinished upload is never listed";
    EXPECT_EQ(lake::control::drive_free_name(tree, "", "half.bin"), "half (2).bin")
        << "but its name is taken";
}

TEST(Drives, BlocksSealToTheSameBytesAndOpenOnlyWhereTheyWereWritten) {
    const std::string workspace = lake::control::new_uuid_v4();
    auto cipher = lake::control::DriveFileCipher::open(kek, workspace);
    ASSERT_TRUE(cipher) << cipher.error().message;
    const std::string block = pattern(MiB);
    const std::string sealed = cipher->seal("f/one", 3, block);
    ASSERT_EQ(sealed.size(), MiB + lake::control::drive_file_block_overhead);
    EXPECT_EQ(cipher->seal("f/one", 3, block), sealed) << "a resent chunk is the same bytes";
    EXPECT_NE(cipher->seal("f/one", 3, pattern(MiB, 'b')).substr(0, 12), sealed.substr(0, 12))
        << "another block gets another nonce";
    EXPECT_EQ(sealed.find(block.substr(100, 64)), std::string::npos) << "no plaintext is stored";
    const auto opened = cipher->unseal("f/one", 3, sealed);
    ASSERT_TRUE(opened);
    EXPECT_EQ(*opened, block);
    EXPECT_EQ(cipher->unseal("f/one", 4, sealed).error().code, "corrupt") << "another place";
    EXPECT_EQ(cipher->unseal("f/two", 3, sealed).error().code, "corrupt") << "another file";
    auto other = lake::control::DriveFileCipher::open(kek, lake::control::new_uuid_v4());
    EXPECT_EQ(other->unseal("f/one", 3, sealed).error().code, "corrupt") << "another workspace";
    std::string flipped = sealed;
    flipped[100] = static_cast<char>(flipped[100] ^ 1);
    EXPECT_EQ(cipher->unseal("f/one", 3, flipped).error().code, "corrupt");
    EXPECT_FALSE(lake::control::DriveFileCipher::open("", workspace));
}

TEST(Drives, AFileIsWrittenInChunksAndReadInAnyRange) {
    auto fake = std::make_shared<lake::test::FakeZettaObjects>();
    const std::string workspace = lake::control::new_uuid_v4();
    auto store = file_store(*fake, fake, workspace);
    const std::string bytes = pattern(2 * MiB + MiB / 2);
    const std::string key = lake::control::new_drive_file_key();
    ASSERT_TRUE(run(store.write(key, 0, bytes.substr(0, 2 * MiB), false)));
    // A chunk sent again (its answer lost) is accepted unchanged.
    ASSERT_TRUE(run(store.write(key, MiB, bytes.substr(MiB, MiB), false)));
    // Only whole blocks before the last, and only at a block boundary.
    EXPECT_EQ(run(store.write(key, 2 * MiB, "short", false)).error().code, "invalid_input");
    EXPECT_EQ(run(store.write(key, 7, "x", true)).error().code, "invalid_input");
    // A chunk past the end of what was sent is refused: the upload goes in order.
    EXPECT_EQ(run(store.write(key, 3 * MiB, "late", true)).error().code, "conflict");
    EXPECT_EQ(run(store.digest(key, bytes.size())).error().code, "conflict")
        << "an incomplete upload has no digest";
    fake->lose_next_answers(1);
    ASSERT_TRUE(run(store.write(key, 2 * MiB, bytes.substr(2 * MiB), true)))
        << "a lost answer is retried and the repeat accepted";
    const auto object = fake->objects(Spaces::url(workspace), "lake_files").at(key);
    EXPECT_EQ(object.size(), lake::control::drive_file_sealed_size(bytes.size()));
    EXPECT_EQ(object.find(bytes.substr(5000, 64)), std::string::npos);

    const auto digest = run(store.digest(key, bytes.size()));
    ASSERT_TRUE(digest) << digest.error().message;
    EXPECT_EQ(*digest, lake::control::drive_sha256_hex(bytes));
    EXPECT_EQ(run(store.digest(key, bytes.size() - 1)).error().code, "conflict")
        << "an object longer or shorter than the size declared is not the file";
    for (const auto& [offset, length] :
         std::vector<std::pair<std::uint64_t, std::uint64_t>>{{0, 10},
                                                              {MiB - 5, 10},
                                                              {MiB, MiB},
                                                              {2 * MiB + 3, 1000},
                                                              {0, bytes.size()},
                                                              {bytes.size() - 1, 100}}) {
        const auto read = run(store.read(key, bytes.size(), offset, length));
        ASSERT_TRUE(read) << offset << ": " << read.error().message;
        EXPECT_EQ(*read, bytes.substr(offset, length)) << offset;
    }
    EXPECT_TRUE(run(store.read(key, bytes.size(), bytes.size(), 10))->empty());
    ASSERT_TRUE(run(store.remove(key)));
    EXPECT_TRUE(run(store.remove(key))) << "removing a removed object is done";
    EXPECT_FALSE(fake->objects(Spaces::url(workspace), "lake_files").contains(key));
}

TEST(Drives, AWorkspaceFilesDriveKeepsFoldersFilesAndTheirBytes) {
    Spaces s;
    const auto& first = s.spaces[0];
    const std::string home = first.workspaces[0];
    const ApiResult drive = s.in(first, "create_drive", {{"name", "Team"}, {"storage", "files"}});
    ASSERT_TRUE(drive.ok) << drive.error.message;
    EXPECT_EQ(drive.value["storage"], "files");
    EXPECT_TRUE(drive.value["repository_id"].is_null());
    const std::string id = drive.value["id"];
    EXPECT_EQ(s.in(first, "create_drive", {{"name", "Team"}, {"storage", "virtual"}}).error.code,
              "conflict");
    EXPECT_EQ(s.in(first, "create_drive", {{"name", "a/b"}, {"storage", "files"}}).error.path,
              "/name");
    EXPECT_EQ(
        s.in(first, "create_drive", {{"name", "Odd"}, {"storage", "files"}, {"repository_id", "x"}})
            .error.code,
        "invalid_input");
    EXPECT_EQ(s.in(first, "list_drives", json::object()).value["drives"].size(), 1U);
    EXPECT_TRUE(s.in(first, "list_drives", json::object(), 1).value["drives"].empty())
        << "another workspace of the space has its own drives";
    EXPECT_EQ(s.in(first, "get_drive", {{"id", id}}, 1).error.code, "not_found");
    const auto rows = s.zetta->records[Spaces::url(home)].rows("drives");
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows.begin()->second.dump().find("Team"), std::string::npos)
        << "the record is sealed";

    // A file in a folder that does not exist yet: the folders are made, the file is listed only
    // once its upload finishes.
    const std::string bytes = pattern(2 * MiB + 1234);
    const ApiResult begun =
        s.in(first,
             "begin_drive_upload",
             {{"drive_id", id}, {"path", "Trip/Day 1/notes.txt"}, {"size", bytes.size()}});
    ASSERT_TRUE(begun.ok) << begun.error.message;
    EXPECT_EQ(begun.value["block_size"], MiB);
    EXPECT_EQ(begun.value["entry"]["path"], "Trip/Day 1/notes.txt");
    EXPECT_EQ(begun.value["entry"]["content_type"], "text/plain");
    EXPECT_EQ(begun.value["entry"]["state"], "uploading");
    const std::string file = begun.value["entry"]["id"];
    const auto top = s.in(first, "list_drive_entries", {{"drive_id", id}});
    ASSERT_TRUE(top.ok) << top.error.message;
    ASSERT_EQ(top.value["items"].size(), 1U);
    EXPECT_EQ(top.value["items"][0]["name"], "Trip");
    const auto day = s.in(first, "list_drive_entries", {{"drive_id", id}, {"path", "Trip/Day 1"}});
    ASSERT_TRUE(day.ok) << day.error.message;
    EXPECT_TRUE(day.value["items"].empty()) << "the uploading file is not listed";
    EXPECT_EQ(day.value["ancestors"].size(), 2U);
    EXPECT_EQ(day.value["path"], "Trip/Day 1");
    const std::string day_id = day.value["folder_id"];

    const auto append = [&](std::uint64_t offset, std::uint64_t length) {
        return s.in(first,
                    "append_drive_upload",
                    {{"drive_id", id},
                     {"id", file},
                     {"offset", offset},
                     {"content", base64(bytes.substr(offset, length))}});
    };
    EXPECT_EQ(s.in(first, "finish_drive_upload", {{"drive_id", id}, {"id", file}}).error.code,
              "conflict")
        << "nothing was sent yet";
    ASSERT_TRUE(append(0, MiB).ok);
    ASSERT_TRUE(append(0, MiB).ok) << "a chunk sent twice changes nothing";
    EXPECT_EQ(append(MiB, 10).error.path, "/content") << "not a whole block before the last";
    EXPECT_EQ(s.in(first,
                   "append_drive_upload",
                   {{"drive_id", id},
                    {"id", file},
                    {"offset", MiB},
                    {"content", base64(bytes.substr(MiB) + "more")}})
                  .error.path,
              "/offset")
        << "past the declared size";
    ASSERT_TRUE(append(MiB, MiB).ok);
    const ApiResult last = append(2 * MiB, 1234);
    ASSERT_TRUE(last.ok) << last.error.message;
    EXPECT_EQ(last.value["complete"], true);
    EXPECT_EQ(s.in(first,
                   "finish_drive_upload",
                   {{"drive_id", id}, {"id", file}, {"sha256", std::string(64, '0')}})
                  .error.code,
              "invalid_digest");
    const ApiResult finished = s.in(first, "finish_drive_upload", {{"drive_id", id}, {"id", file}});
    ASSERT_TRUE(finished.ok) << finished.error.message;
    EXPECT_EQ(finished.value["sha256"], lake::control::drive_sha256_hex(bytes));
    EXPECT_EQ(finished.value["state"], "ready");
    EXPECT_EQ(append(0, MiB).error.code, "conflict") << "a finished upload takes no more bytes";
    EXPECT_EQ(s.in(first, "list_drive_entries", {{"drive_id", id}, {"folder_id", day_id}})
                  .value["items"][0]["id"],
              file);
    const auto stored = s.files(home);
    ASSERT_EQ(stored.size(), 1U);
    const std::string key = stored.begin()->first;
    EXPECT_TRUE(key.starts_with("f/")) << "an opaque key: " << key;
    EXPECT_EQ(key.find("notes"), std::string::npos);

    const ApiResult range =
        s.in(first,
             "read_drive_file",
             {{"drive_id", id}, {"id", file}, {"offset", MiB - 2}, {"length", 4}});
    ASSERT_TRUE(range.ok) << range.error.message;
    EXPECT_EQ(range.value["content_base64"], base64(bytes.substr(MiB - 2, 4)));

    // Moving and renaming rewrite the record only: the same object, the same bytes.
    const ApiResult moved =
        s.in(first,
             "move_drive_entry",
             {{"drive_id", id}, {"id", file}, {"parent_id", ""}, {"name", "Notes.txt"}});
    ASSERT_TRUE(moved.ok) << moved.error.message;
    EXPECT_EQ(moved.value["path"], "Notes.txt");
    EXPECT_TRUE(moved.value["parent_id"].is_null());
    EXPECT_EQ(s.files(home), stored) << "no byte was copied";
    EXPECT_EQ(s.in(first, "read_drive_file", {{"drive_id", id}, {"id", file}, {"length", 8}})
                  .value["content_base64"],
              base64(bytes.substr(0, 8)));
    const std::string trip = top.value["items"][0]["id"];
    EXPECT_EQ(
        s.in(first, "move_drive_entry", {{"drive_id", id}, {"id", trip}, {"parent_id", day_id}})
            .error.code,
        "invalid_input")
        << "a folder never moves into itself";

    // Small files in one call; a name taken gets the next free one, or replaces it when asked.
    const ApiResult small =
        s.in(first,
             "upload_drive_entries",
             {{"drive_id", id},
              {"files",
               json::array({{{"path", "Notes.txt"}, {"content", base64("one")}},
                            {{"path", "Trip/b.csv"}, {"content", base64("a,b\n")}}})}});
    ASSERT_TRUE(small.ok) << small.error.message;
    EXPECT_EQ(small.value["items"][0]["name"], "Notes (2).txt");
    EXPECT_EQ(small.value["items"][1]["path"], "Trip/b.csv");
    EXPECT_EQ(small.value["items"][1]["content_type"], "text/csv");
    const ApiResult replaced =
        s.in(first,
             "upload_drive_entries",
             {{"drive_id", id},
              {"on_conflict", "replace"},
              {"files", json::array({{{"path", "Notes (2).txt"}, {"content", base64("two")}}})}});
    ASSERT_TRUE(replaced.ok) << replaced.error.message;
    EXPECT_EQ(replaced.value["items"][0]["name"], "Notes (2).txt");
    EXPECT_EQ(s.in(first,
                   "upload_drive_entries",
                   {{"drive_id", id},
                    {"on_conflict", "refuse"},
                    {"files", json::array({{{"path", "Notes.txt"}, {"content", ""}}})}})
                  .error.code,
              "conflict");
    const auto names = [&](const json& listing) {
        std::set<std::string> out;
        for (const json& item : listing["items"]) {
            out.insert(item["name"]);
        }
        return out;
    };
    EXPECT_EQ(names(s.in(first, "list_drive_entries", {{"drive_id", id}}).value),
              (std::set<std::string>{"Notes.txt", "Notes (2).txt", "Trip"}));
    EXPECT_EQ(s.files(home).size(), 3U) << "the replaced file's object is gone";
    EXPECT_EQ(
        s.in(first, "read_drive_file", {{"drive_id", id}, {"id", replaced.value["items"][0]["id"]}})
            .value["content_base64"],
        base64("two"));

    const ApiResult recent = s.in(first, "recent_drive_entries", {{"limit", 2}});
    ASSERT_TRUE(recent.ok) << recent.error.message;
    ASSERT_EQ(recent.value["items"].size(), 2U);
    EXPECT_EQ(recent.value["items"][0]["entry"]["name"], "Notes (2).txt");
    EXPECT_EQ(recent.value["items"][0]["drive"]["name"], "Team");

    // Deleting a folder deletes what is in it, objects included.
    const ApiResult gone = s.in(first, "delete_drive_entry", {{"drive_id", id}, {"id", trip}});
    ASSERT_TRUE(gone.ok) << gone.error.message;
    EXPECT_EQ(gone.value["entries_removed"], 3U) << "Trip, Day 1 and b.csv";
    EXPECT_EQ(s.files(home).size(), 2U);

    // Renaming the drive, then deleting it with its name typed.
    ASSERT_TRUE(s.in(first, "rename_drive", {{"id", id}, {"name", "Team files"}}).ok);
    EXPECT_EQ(s.in(first, "delete_drive", {{"id", id}, {"confirm_name", "Team"}}).error.path,
              "/confirm_name");
    const ApiResult deleted =
        s.in(first, "delete_drive", {{"id", id}, {"confirm_name", "Team files"}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(deleted.value["entries_removed"], 2U);
    EXPECT_TRUE(s.files(home).empty());
    EXPECT_TRUE(s.zetta->records[Spaces::url(home)].rows("drive_entries").empty());
    EXPECT_TRUE(s.in(first, "list_drives", json::object()).value["drives"].empty());
}

TEST(Drives, AnotherSpaceNeitherReadsNorWritesADrive) {
    Spaces s;
    const auto& first = s.spaces[0];
    const auto& second = s.spaces[1];
    const ApiResult drive =
        s.in(first, "create_drive", {{"name", "Private"}, {"storage", "files"}});
    ASSERT_TRUE(drive.ok) << drive.error.message;
    const json foreign{{"customer_id", first.customer_id},
                       {"workspace_id", first.workspaces[0]},
                       {"drive_id", drive.value["id"]}};
    EXPECT_EQ(s.call(second, "list_drive_entries", foreign).error.code, "unauthorized");
    EXPECT_EQ(s.call(second,
                     "list_drives",
                     {{"customer_id", first.customer_id}, {"workspace_id", first.workspaces[0]}})
                  .error.code,
              "unauthorized");
    auto write = foreign;
    write["path"] = "x.txt";
    write["size"] = 1;
    EXPECT_EQ(s.call(second, "begin_drive_upload", write).error.code, "unauthorized");
    // The second space's own workspace does not hold the first space's drive.
    EXPECT_EQ(s.in(second, "list_drive_entries", {{"drive_id", drive.value["id"]}}).error.code,
              "not_found");
}

TEST(Drives, AVirtualDriveHoldsShortcutsThatSayWhenTheyBreak) {
    Spaces s;
    const auto& first = s.spaces[0];
    const ApiResult files = s.in(first, "create_drive", {{"name", "Files"}, {"storage", "files"}});
    const ApiResult links =
        s.in(first, "create_drive", {{"name", "Links"}, {"storage", "virtual"}});
    ASSERT_TRUE(files.ok && links.ok);
    const std::string virtual_id = links.value["id"];
    EXPECT_EQ(s.in(first,
                   "begin_drive_upload",
                   {{"drive_id", virtual_id}, {"path", "a.txt"}, {"size", 1}})
                  .error.path,
              "/drive_id")
        << "a virtual drive holds no files";
    const ApiResult folder = s.in(
        first, "create_drive_folder_entry", {{"drive_id", virtual_id}, {"path", "Q3/Sources"}});
    ASSERT_TRUE(folder.ok) << folder.error.message;
    EXPECT_EQ(folder.value["path"], "Q3/Sources");
    EXPECT_EQ(s.in(first, "create_drive_folder_entry", {{"drive_id", virtual_id}, {"path", "Q3"}})
                  .value["kind"],
              "folder")
        << "a folder already there is answered as it is";
    const std::string sources = folder.value["id"];

    const ApiResult uploaded =
        s.in(first,
             "upload_drive_entries",
             {{"drive_id", files.value["id"]},
              {"files", json::array({{{"path", "data.csv"}, {"content", base64("x")}}})}});
    ASSERT_TRUE(uploaded.ok) << uploaded.error.message;
    const std::string data_id = uploaded.value["items"][0]["id"];
    const ApiResult transcript =
        s.in(first,
             "create_transcript",
             {{"title", "Kickoff"}, {"source", "text"}, {"text", "We start."}});
    ASSERT_TRUE(transcript.ok) << transcript.error.message;

    const auto shortcut = [&](std::string name, json target) {
        return s.in(first,
                    "create_drive_shortcut",
                    {{"drive_id", virtual_id},
                     {"parent_id", sources},
                     {"name", std::move(name)},
                     {"target", std::move(target)}});
    };
    const ApiResult to_file = shortcut(
        "Data", {{"kind", "drive_entry"}, {"drive_id", files.value["id"]}, {"entry_id", data_id}});
    ASSERT_TRUE(to_file.ok) << to_file.error.message;
    EXPECT_EQ(to_file.value["status"], "ok");
    ASSERT_TRUE(shortcut("Call", {{"kind", "transcript"}, {"id", transcript.value["id"]}}).ok);
    ASSERT_TRUE(shortcut("Site", {{"kind", "url"}, {"url", "https://orama.test/"}}).ok);
    ASSERT_TRUE(shortcut("Churn", {{"kind", "project"}, {"name", "churn"}}).ok);
    EXPECT_EQ(
        shortcut("Gone", {{"kind", "transcript"}, {"id", lake::control::new_uuid_v4()}}).error.code,
        "not_found")
        << "a target lake can check must be there when the shortcut is made";
    EXPECT_EQ(shortcut("Data", {{"kind", "url"}, {"url", "https://x.test"}}).error.code,
              "conflict");
    EXPECT_EQ(shortcut("Bad", {{"kind", "url"}, {"url", "file:///etc/passwd"}}).error.path,
              "/target");

    const auto statuses = [&] {
        std::map<std::string, std::string> out;
        const ApiResult listed =
            s.in(first, "list_drive_entries", {{"drive_id", virtual_id}, {"folder_id", sources}});
        EXPECT_TRUE(listed.ok) << listed.error.message;
        for (const json& item : listed.value["items"]) {
            out[item["name"]] = item.value("status", "");
        }
        return out;
    };
    EXPECT_EQ(statuses(),
              (std::map<std::string, std::string>{
                  {"Data", "ok"}, {"Call", "ok"}, {"Site", "unknown"}, {"Churn", "unknown"}}));
    ASSERT_TRUE(
        s.in(first, "delete_drive_entry", {{"drive_id", files.value["id"]}, {"id", data_id}}).ok);
    ASSERT_TRUE(s.in(first, "delete_transcript", {{"id", transcript.value["id"]}}).ok);
    EXPECT_EQ(
        statuses(),
        (std::map<std::string, std::string>{
            {"Data", "broken"}, {"Call", "broken"}, {"Site", "unknown"}, {"Churn", "unknown"}}))
        << "a broken shortcut is listed, never the folder's failure";

    const ApiResult deleted =
        s.in(first, "delete_drive", {{"id", virtual_id}, {"confirm_name", "Links"}});
    ASSERT_TRUE(deleted.ok) << deleted.error.message;
    EXPECT_EQ(deleted.value["entries_removed"], 6U);
}

TEST(Drives, EveryDriveCapabilityIsAuditedInItsWorkspace) {
    for (const char* name : {"create_drive",
                             "list_drives",
                             "get_drive",
                             "rename_drive",
                             "delete_drive",
                             "list_drive_entries",
                             "get_drive_entry",
                             "create_drive_folder_entry",
                             "create_drive_shortcut",
                             "move_drive_entry",
                             "delete_drive_entry",
                             "begin_drive_upload",
                             "append_drive_upload",
                             "finish_drive_upload",
                             "upload_drive_entries",
                             "read_drive_file",
                             "recent_drive_entries"}) {
        EXPECT_EQ(lake::api::audit_log_of(name), lake::api::AuditLog::Workspace) << name;
    }
}

namespace {

// A whole lake over HTTP: a real repository store on disk, one Ready workspace, the operator's
// bearer.
struct Lake {
    Spaces spaces;
    std::filesystem::path root =
        lake::test::fixture_path("drives-lake-" + lake::control::new_uuid_v4());
    std::unique_ptr<lake::api::ApiServer> server;
    std::string url;

    Lake() {
        server = std::make_unique<lake::api::ApiServer>(
            lake::api::ServerOptions{.port = 0,
                                     .node_name = "drives-test",
                                     .store = spaces.store,
                                     .workspaces = spaces.service,
                                     .object_transport = spaces.objects,
                                     .admin_token = "drives-test",
                                     .instance_kek = std::string(kek),
                                     .store_root = root.string()});
        EXPECT_TRUE(server->start()) << server->reason();
        url = "http://127.0.0.1:" + std::to_string(server->port());
    }

    std::pair<unsigned, std::string>
    request(const std::string& method, const std::string& path, const std::string& body = {}) {
        std::vector<std::byte> bytes(body.size());
        std::memcpy(bytes.data(), body.data(), body.size());
        const auto response =
            lake::test::http_request({.url = url,
                                      .method = method,
                                      .suffix = path,
                                      .headers = {{"Authorization", "Bearer drives-test"},
                                                  {"Content-Type", "application/json"}},
                                      .body = std::move(bytes)});
        EXPECT_TRUE(response) << path;
        if (!response) {
            return {0, {}};
        }
        return {response->status,
                std::string(reinterpret_cast<const char*>(response->body.data()),
                            response->body.size())};
    }

    // An action; `scoped` adds the first space's first workspace, as drive capabilities take it.
    json call(const std::string& capability, json input, bool scoped = true) {
        const auto& space = spaces.spaces[0];
        if (!scoped) {
            const auto [status, body] = request("POST", "/v1/" + capability, input.dump());
            return json::parse(body, nullptr, false);
        }
        if (!input.contains("customer_id")) {
            input["customer_id"] = space.customer_id;
        }
        if (!input.contains("workspace_id")) {
            input["workspace_id"] = space.workspaces[0];
        }
        const auto [status, body] = request("POST", "/v1/" + capability, input.dump());
        return json::parse(body, nullptr, false);
    }

    // A read is a GET with its input in the query string.
    json read(const std::string& capability, const json& input) {
        std::string query;
        for (const auto& [key, value] : input.items()) {
            query += (query.empty() ? "?" : "&") + key + "=" +
                     (value.is_string() ? value.get<std::string>() : value.dump());
        }
        const auto [status, body] = request("GET", "/v1/" + capability + query);
        return json::parse(body, nullptr, false);
    }
};

} // namespace

TEST(DrivesHttp, ARepositoryDriveHasMainFromTheMomentItIsCreated) {
    Lake lake;
    // Without the drive's first change, a new repository has no `main` for the drive_* reads.
    const json bare = lake.call("create_repository", {{"name", "bare"}});
    ASSERT_TRUE(bare.value("ok", false)) << bare.dump();
    EXPECT_EQ(lake.read("drive_browse",
                        {{"repository_id", bare["value"]["id"]},
                         {"ref", "main"},
                         {"path", ""}})["error"]["code"],
              "not_found");
    const json drive = lake.call(
        "create_drive",
        {{"name", "Shared"}, {"storage", "repository"}, {"repository_name", "shared-drive"}});
    ASSERT_TRUE(drive.value("ok", false)) << drive.dump();
    const std::string repository_id = drive["value"]["repository_id"];
    EXPECT_FALSE(repository_id.empty());
    // A new repository's first change is the drive's empty-folder marker, so every drive_*
    // capability resolves `main` at once and the marker is never listed.
    const json browse = lake.read(
        "drive_browse", {{"repository_id", repository_id}, {"ref", "main"}, {"path", ""}});
    ASSERT_TRUE(browse.value("ok", false)) << browse.dump();
    EXPECT_TRUE(browse["value"]["items"].empty()) << browse.dump();
    const json folder = lake.call(
        "create_drive_folder",
        {{"repository_id", repository_id}, {"ref", "main"}, {"path", ""}, {"name", "Reports"}},
        false);
    ASSERT_TRUE(folder.value("ok", false)) << folder.dump();
    const json listed = lake.read(
        "drive_browse", {{"repository_id", repository_id}, {"ref", "main"}, {"path", ""}});
    ASSERT_EQ(listed["value"]["items"].size(), 1U) << listed.dump();
    EXPECT_EQ(listed["value"]["items"][0]["path"], "Reports");

    // Attaching an existing repository of the workspace writes nothing to it; deleting the drive
    // never deletes the repository.
    const json other =
        lake.call("create_drive",
                  {{"name", "Again"}, {"storage", "repository"}, {"repository_id", repository_id}});
    ASSERT_TRUE(other.value("ok", false)) << other.dump();
    const json deleted =
        lake.call("delete_drive", {{"id", other["value"]["id"]}, {"confirm_name", "Again"}});
    ASSERT_TRUE(deleted.value("ok", false)) << deleted.dump();
    EXPECT_EQ(deleted["value"]["repository_id"], repository_id);
    EXPECT_TRUE(lake.read("drive_browse",
                          {{"repository_id", repository_id}, {"ref", "main"}, {"path", "Reports"}})
                    .value("ok", false));
    const auto& space = lake.spaces.spaces[0];
    EXPECT_EQ(lake.read("list_drive_entries",
                        {{"customer_id", space.customer_id},
                         {"workspace_id", space.workspaces[0]},
                         {"drive_id", drive["value"]["id"]}})["error"]["code"],
              "invalid_input")
        << "a repository drive is browsed with drive_browse";
}

TEST(DrivesHttp, AWorkspaceFileDownloadStreamsItsBytes) {
    Lake lake;
    const json drive = lake.call("create_drive", {{"name", "Files"}, {"storage", "files"}});
    ASSERT_TRUE(drive.value("ok", false)) << drive.dump();
    const std::string drive_id = drive["value"]["id"];
    const std::string bytes = pattern(3 * MiB + 17, 'z');
    const json begun =
        lake.call("begin_drive_upload",
                  {{"drive_id", drive_id}, {"path", "big/model.bin"}, {"size", bytes.size()}});
    ASSERT_TRUE(begun.value("ok", false)) << begun.dump();
    const std::string id = begun["value"]["entry"]["id"];
    for (std::uint64_t offset = 0; offset < bytes.size(); offset += 2 * MiB) {
        const json appended = lake.call("append_drive_upload",
                                        {{"drive_id", drive_id},
                                         {"id", id},
                                         {"offset", offset},
                                         {"content", base64(bytes.substr(offset, 2 * MiB))}});
        ASSERT_TRUE(appended.value("ok", false)) << appended.dump();
    }
    const json finished = lake.call("finish_drive_upload", {{"drive_id", drive_id}, {"id", id}});
    ASSERT_TRUE(finished.value("ok", false)) << finished.dump();
    const auto& space = lake.spaces.spaces[0];
    const std::string query = "?customer_id=" + space.customer_id +
                              "&workspace_id=" + space.workspaces[0] + "&drive_id=" + drive_id +
                              "&id=" + id;
    const auto [status, body] = lake.request("GET", "/drive/files/download" + query);
    EXPECT_EQ(status, 200U);
    ASSERT_EQ(body.size(), bytes.size());
    EXPECT_TRUE(body == bytes) << "the download is the uploaded bytes";
    const auto [missing, error] =
        lake.request("GET",
                     "/drive/files/download?customer_id=" + space.customer_id +
                         "&workspace_id=" + space.workspaces[0] + "&drive_id=" + drive_id +
                         "&id=" + lake::control::new_uuid_v4());
    EXPECT_EQ(missing, 404U) << error;
}

// Drives (docs/api/drives.md, lake#561): drive records, workspace files sealed in the workspace
// Zetta's `lake_files` bucket, virtual drives of shortcuts, and a repository drive that works from
// the moment it is created. The in-process cases run the capabilities over a workspace service, a
// fake record store (StreamCaller) and a fake object store (FakeZettaObjects); the HTTP cases run
// a whole lake with a real repository store and stream a download.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "api/workspace_records_api.hpp"
#include "lake/api/audit_routing.hpp"
#include "lake/api/capabilities.hpp"
#include "lake/api/registry.hpp"
#include "lake/api/server.hpp"
#include "lake/api/usage_recorder.hpp"
#include "lake/audit/audit.hpp"
#include "lake/control/drive_files.hpp"
#include "lake/control/drives.hpp"
#include "lake/control/memory_store.hpp"
#include "lake/control/store_buckets.hpp"
#include "lake/control/store_contents.hpp"
#include "lake/control/store_kept.hpp"
#include "lake/control/zetta_workspace.hpp"
#include "lake/search/index.hpp"
#include "one_thread_io.hpp"
#include "store_provider_fakes.hpp"
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
    // Each call's usage (lake#658: drive uploads and reads are transfer), written on flush.
    std::unique_ptr<lake::api::UsageRecorder> usage;
    struct Space {
        lake::control::Principal principal;
        std::string customer_id;
        std::vector<std::string> workspaces;
    };
    std::vector<Space> spaces;
    // The store browser every call gets (lake#656); empty, as on a Lake that browses no provider.
    lake::control::StoreBrowser browser;

    Spaces() {
        std::filesystem::create_directories(dir);
        service = std::make_shared<lake::control::WorkspaceService>(
            lake::control::WorkspaceConfig{dir.string(), 0}, store, zetta);
        usage = std::make_unique<lake::api::UsageRecorder>(store.get(), service.get());
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
        context.store_browser = browser;
        context.authorized = true;
        context.session_credential = true;
        context.acting_user_id = space.principal.user_id;
        context.acting_identity = space.principal.taap_id;
        context.credential_session_id = space.principal.session_id;
        context.scopes = space.principal.scopes;
        context.request_id = lake::control::new_uuid_v4();
        context.usage_recorder = usage.get();
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

// A bucket's contents in memory (lake#656): what a provider's StoreContents does, keyed by path,
// with folders as their own entries.
class FakeBucket final : public lake::control::StoreContents {
public:
    using Item = lake::control::StoreItem;
    template <class T> using Result = lake::control::ZettaControlResult<T>;

    std::map<std::string, std::string> files;
    std::set<std::string> folders;

    liquid::runtime::task<Result<std::vector<Item>>> list(std::string folder) override {
        std::vector<Item> out;
        for (const std::string& path : folders) {
            if (lake::control::store_parent(path) == folder) {
                out.push_back({path, true});
            }
        }
        for (const auto& [path, bytes] : files) {
            if (lake::control::store_parent(path) == folder) {
                out.push_back(item(path));
            }
        }
        co_return Result<std::vector<Item>>(std::move(out));
    }
    liquid::runtime::task<Result<std::optional<Item>>> stat(std::string path) override {
        if (folders.contains(path)) {
            co_return Result<std::optional<Item>>(Item{path, true});
        }
        if (files.contains(path)) {
            co_return Result<std::optional<Item>>(item(path));
        }
        co_return Result<std::optional<Item>>(std::nullopt);
    }
    liquid::runtime::task<Result<std::string>>
    read(std::string path, std::uint64_t offset, std::uint64_t length) override {
        const auto found = files.find(path);
        if (found == files.end()) {
            co_return Result<std::string>(
                std::unexpected(lake::control::ZettaControlError{"not_found", "gone"}));
        }
        co_return Result<std::string>(found->second.substr(offset, length));
    }
    liquid::runtime::task<Result<Item>>
    write(std::string path, std::string bytes, std::string content_type) override {
        make(lake::control::store_parent(path));
        files[path] = std::move(bytes);
        types[path] = std::move(content_type);
        co_return Result<Item>(item(path));
    }
    liquid::runtime::task<Result<void>> make_folder(std::string path) override {
        make(path);
        co_return Result<void>();
    }
    liquid::runtime::task<Result<void>> move(std::string from, std::string to) override {
        const auto moved = [&](const std::string& path) {
            return path == from ? to : to + path.substr(from.size());
        };
        const auto inside = [&](const std::string& path) {
            return path == from || path.starts_with(from + "/");
        };
        std::map<std::string, std::string> next_files;
        for (auto& [path, bytes] : files) {
            next_files[inside(path) ? moved(path) : path] = std::move(bytes);
        }
        files = std::move(next_files);
        std::set<std::string> next_folders;
        for (const std::string& path : folders) {
            next_folders.insert(inside(path) ? moved(path) : path);
        }
        folders = std::move(next_folders);
        co_return Result<void>();
    }
    liquid::runtime::task<Result<void>> remove(std::string path) override {
        std::erase_if(files, [&](const auto& file) {
            return file.first == path || file.first.starts_with(path + "/");
        });
        std::erase_if(folders, [&](const std::string& folder) {
            return folder == path || folder.starts_with(path + "/");
        });
        co_return Result<void>();
    }

private:
    std::map<std::string, std::string> types;

    Item item(const std::string& path) const {
        const auto type = types.find(path);
        return {path,
                false,
                files.at(path).size(),
                type == types.end() ? "" : type->second,
                "2026-10-11T12:00:00Z",
                "v-" + std::to_string(files.at(path).size())};
    }
    void make(const std::string& path) {
        for (std::string level = path; !level.empty(); level = lake::control::store_parent(level)) {
            folders.insert(level);
        }
    }
};

// A store of the space's first workspace as the operator leaves it: Ready, with its connection
// delivered beside the services'. Written straight to the store, past create_store's checks of a
// bucket's location and secret, which test_store_providers.cpp covers.
lake::control::ManagedStore ready_store(Spaces& s,
                                        const Spaces::Space& space,
                                        std::string type,
                                        std::string provider,
                                        std::string name,
                                        bool ready = true) {
    lake::control::NewManagedStore store;
    store.customer_id = space.customer_id;
    store.workspace_id = space.workspaces[0];
    store.type = std::move(type);
    store.provider = std::move(provider);
    store.name = std::move(name);
    store.created_by_taap_id = space.principal.taap_id;
    const auto created = s.store->create_store(store);
    EXPECT_TRUE(created) << created.error().message;
    if (ready) {
        EXPECT_TRUE(s.store->set_store_status(created->id, "Ready", "", "bucket.test"));
        std::ofstream(s.dir / (created->slug + ".json")) << json{
            {"type", created->type},
            {"provider", created->provider},
            {"bucket", created->slug}}.dump();
    }
    return *created;
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

// lake#588: a chunk sent again after the last answers what the upload holds, and an upload left
// unfinished is listed apart until it expires, when the next upload of its name takes the name.
// lake#658: the bytes a workspace-files drive takes in and hands out are its workspace's
// transfer, `in` for uploads (whole files and chunks) and `out` for reads; another workspace's
// transfer is untouched.
TEST(Drives, UploadsAndReadsAreTheWorkspacesTransfer) {
    Spaces s;
    const auto& first = s.spaces[0];
    const std::string home = first.workspaces[0];
    const ApiResult drive = s.in(first, "create_drive", {{"name", "Shared"}, {"storage", "files"}});
    ASSERT_TRUE(drive.ok) << drive.error.message;
    const std::string id = drive.value["id"];
    const std::string small = pattern(3000);
    const ApiResult uploaded =
        s.in(first,
             "upload_drive_entries",
             {{"drive_id", id},
              {"files", json::array({{{"name", "a.txt"}, {"content", base64(small)}}})}});
    ASSERT_TRUE(uploaded.ok) << uploaded.error.message;
    const std::string bytes = pattern(MiB + 500);
    const ApiResult begun = s.in(
        first, "begin_drive_upload", {{"drive_id", id}, {"path", "b.bin"}, {"size", bytes.size()}});
    ASSERT_TRUE(begun.ok) << begun.error.message;
    const std::string file = begun.value["entry"]["id"];
    for (const std::uint64_t offset : {std::uint64_t{0}, MiB}) {
        ASSERT_TRUE(s.in(first,
                         "append_drive_upload",
                         {{"drive_id", id},
                          {"id", file},
                          {"offset", offset},
                          {"content", base64(bytes.substr(offset, offset == 0 ? MiB : 500))}})
                        .ok);
    }
    ASSERT_TRUE(s.in(first, "finish_drive_upload", {{"drive_id", id}, {"id", file}}).ok);
    const ApiResult read = s.in(first,
                                "read_drive_file",
                                {{"drive_id", id}, {"id", file}, {"offset", 0}, {"length", 1000}});
    ASSERT_TRUE(read.ok) << read.error.message;

    static_cast<void>(liquid::runtime::block_on(s.usage->flush()));
    auto& series = s.zetta->records[Spaces::url(home)];
    EXPECT_EQ(series.metric_total("lake_usage", {{"dimension", "transfer"}, {"kind", "in"}}),
              3000 + MiB + 500);
    EXPECT_EQ(series.metric_total("lake_usage", {{"dimension", "transfer"}, {"kind", "out"}}),
              1000);
    EXPECT_EQ(s.zetta->records[Spaces::url(first.workspaces[1])].metric_total("lake_usage"), 0)
        << "the other workspace moved nothing";
}

TEST(Drives, AResentChunkAnswersTheUploadAndAbandonedUploadsExpire) {
    Spaces s;
    const auto& first = s.spaces[0];
    const ApiResult drive = s.in(first, "create_drive", {{"name", "Big"}, {"storage", "files"}});
    ASSERT_TRUE(drive.ok) << drive.error.message;
    const std::string id = drive.value["id"];
    const std::string bytes = pattern(2 * MiB + MiB / 2);
    const ApiResult begun =
        s.in(first,
             "begin_drive_upload",
             {{"drive_id", id}, {"path", "Big/blob.bin"}, {"size", bytes.size()}});
    ASSERT_TRUE(begun.ok) << begun.error.message;
    const std::string file = begun.value["entry"]["id"];
    const auto append = [&](std::uint64_t offset, std::uint64_t length) {
        return s.in(first,
                    "append_drive_upload",
                    {{"drive_id", id},
                     {"id", file},
                     {"offset", offset},
                     {"content", base64(bytes.substr(offset, length))}});
    };
    const ApiResult one = append(0, MiB);
    ASSERT_TRUE(one.ok) << one.error.message;
    EXPECT_EQ(one.value["received"], MiB);
    EXPECT_EQ(one.value["complete"], false);
    ASSERT_TRUE(append(MiB, MiB).ok);
    ASSERT_TRUE(append(2 * MiB, MiB / 2).value["complete"]);
    const ApiResult resent = append(0, MiB);
    ASSERT_TRUE(resent.ok) << resent.error.message;
    EXPECT_EQ(resent.value["received"], bytes.size())
        << "what the upload holds, not the chunk's end";
    EXPECT_EQ(resent.value["complete"], true);
    ASSERT_TRUE(s.in(first, "finish_drive_upload", {{"drive_id", id}, {"id", file}}).ok);

    // An unfinished upload is listed apart, with its age, and holds its name.
    const ApiResult left = s.in(
        first, "begin_drive_upload", {{"drive_id", id}, {"path", "Big/left.bin"}, {"size", 10}});
    ASSERT_TRUE(left.ok) << left.error.message;
    const ApiResult folder = s.in(first, "list_drive_entries", {{"drive_id", id}, {"path", "Big"}});
    ASSERT_TRUE(folder.ok) << folder.error.message;
    ASSERT_EQ(folder.value["items"].size(), 1U) << "blob.bin only";
    ASSERT_EQ(folder.value["uploads"].size(), 1U);
    EXPECT_EQ(folder.value["uploads"][0]["name"], "left.bin");
    EXPECT_EQ(folder.value["uploads"][0]["state"], "uploading");
    EXPECT_EQ(folder.value["uploads"][0]["expired"], false);
    EXPECT_GE(folder.value["uploads"][0]["age_seconds"], 0);
    const std::string big = folder.value["folder_id"];

    // An upload begun more than a day ago, as one abandoned would be.
    auto context = s.context(first);
    auto records = lake::api::workspace_records(context, first.customer_id, first.workspaces[0]);
    ASSERT_TRUE(records) << records.error().message;
    const auto old_upload = [&](std::string name) {
        DriveEntry entry;
        entry.customer_id = first.customer_id;
        entry.drive_id = id;
        entry.parent_id = big;
        entry.kind = "file";
        entry.name = std::move(name);
        entry.size = 10;
        entry.content_type = "application/octet-stream";
        entry.object_key = lake::control::new_drive_file_key();
        entry.state = "uploading";
        entry.created_by = first.principal.user_id;
        entry.modified_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count() -
                            lake::control::drive_upload_expiry_us - 1'000'000;
        const auto stored = run(lake::control::record_drive_entry(*records, std::move(entry)));
        EXPECT_TRUE(stored) << stored.error().message;
        return stored ? stored->id : std::string();
    };
    const std::string stale = old_upload("old.bin");
    const std::string other = old_upload("other.bin");
    const ApiResult listed =
        s.in(first, "list_drive_entries", {{"drive_id", id}, {"folder_id", big}});
    std::map<std::string, bool> expired;
    for (const json& upload : listed.value["uploads"]) {
        expired[upload["name"]] = upload["expired"];
    }
    EXPECT_EQ(
        expired,
        (std::map<std::string, bool>{{"left.bin", false}, {"old.bin", true}, {"other.bin", true}}));
    EXPECT_EQ(
        s.in(first,
             "append_drive_upload",
             {{"drive_id", id}, {"id", other}, {"offset", 0}, {"content", base64("0123456789")}})
            .error.code,
        "conflict")
        << "an expired upload takes no more chunks";

    // The next upload of the name removes the expired one and takes its name.
    const ApiResult reused =
        s.in(first,
             "upload_drive_entries",
             {{"drive_id", id},
              {"files", json::array({{{"path", "Big/old.bin"}, {"content", base64("new")}}})}});
    ASSERT_TRUE(reused.ok) << reused.error.message;
    EXPECT_EQ(reused.value["items"][0]["name"], "old.bin");
    EXPECT_EQ(s.in(first, "get_drive_entry", {{"drive_id", id}, {"id", stale}}).error.code,
              "not_found");
    // upload_drive_entries takes `name` as upload_drive_files does (lake#587).
    const ApiResult named =
        s.in(first,
             "upload_drive_entries",
             {{"drive_id", id},
              {"parent_id", big},
              {"files", json::array({{{"name", "n.txt"}, {"content", base64("n")}}})}});
    ASSERT_TRUE(named.ok) << named.error.message;
    EXPECT_EQ(named.value["items"][0]["path"], "Big/n.txt");
    EXPECT_EQ(s.in(first,
                   "upload_drive_entries",
                   {{"drive_id", id},
                    {"files", json::array({{{"name", "a/b.txt"}, {"content", base64("n")}}})}})
                  .error.path,
              "/files/0/name");
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

TEST(Drives, SearchFindsDrivesFoldersFilesAndShortcutsByName) {
    Spaces s;
    const auto& first = s.spaces[0];
    const auto& second = s.spaces[1];
    const ApiResult team = s.in(first, "create_drive", {{"name", "Team"}, {"storage", "files"}});
    const ApiResult links =
        s.in(first, "create_drive", {{"name", "Report Links"}, {"storage", "virtual"}});
    ASSERT_TRUE(team.ok && links.ok);
    const std::string team_id = team.value["id"];
    const std::string links_id = links.value["id"];
    const ApiResult uploaded =
        s.in(first,
             "upload_drive_entries",
             {{"drive_id", team_id},
              {"files",
               json::array({{{"path", "Trip/Day 1/Q3 REPORT.pdf"}, {"content", base64("pdf")}},
                            {{"path", "Trip/notes.txt"}, {"content", base64("n")}}})}});
    ASSERT_TRUE(uploaded.ok) << uploaded.error.message;
    // An unfinished upload is never found.
    ASSERT_TRUE(s.in(first,
                     "begin_drive_upload",
                     {{"drive_id", team_id}, {"path", "report-draft.txt"}, {"size", 9}})
                    .ok);
    const ApiResult folder = s.in(
        first, "create_drive_folder_entry", {{"drive_id", links_id}, {"path", "Reports/2026"}});
    ASSERT_TRUE(folder.ok) << folder.error.message;
    ASSERT_TRUE(s.in(first,
                     "create_drive_shortcut",
                     {{"drive_id", links_id},
                      {"parent_id", folder.value["id"]},
                      {"name", "Board report"},
                      {"target", {{"kind", "url"}, {"url", "https://orama.test/report"}}}})
                    .ok);

    const ApiResult found = s.in(first, "search_drives", {{"q", "rEpOrT"}});
    ASSERT_TRUE(found.ok) << found.error.message;
    EXPECT_EQ(found.value["truncated"], false);
    EXPECT_TRUE(found.value["skipped"].empty());
    std::vector<std::string> names;
    for (const json& item : found.value["items"]) {
        names.push_back(item["kind"].get<std::string>() + " " +
                        item["drive"]["name"].get<std::string>() + ":" +
                        item["path"].get<std::string>());
    }
    EXPECT_EQ(names,
              (std::vector<std::string>{"drive Report Links:",
                                        "folder Report Links:Reports",
                                        "shortcut Report Links:Reports/2026/Board report",
                                        "file Team:Trip/Day 1/Q3 REPORT.pdf"}))
        << found.value.dump();
    const json& file = found.value["items"][3];
    EXPECT_EQ(file["drive"]["storage"], "files");
    EXPECT_EQ(file["folder"], "Trip/Day 1");
    EXPECT_EQ(file["size"], 3);
    EXPECT_EQ(file["content_type"], "application/pdf");
    EXPECT_TRUE(file["modified_at"].is_string());
    EXPECT_TRUE(file["parent_id"].is_string());
    const json& shortcut = found.value["items"][2];
    EXPECT_EQ(shortcut["status"], "unknown");
    EXPECT_EQ(shortcut["target"]["url"], "https://orama.test/report");
    EXPECT_EQ(shortcut["folder"], "Reports/2026");
    EXPECT_EQ(shortcut["parent_id"], folder.value["id"]);

    // A limit keeps the first matches and says more were found; a drive narrows to its own items.
    const ApiResult two = s.in(first, "search_drives", {{"q", "report"}, {"limit", 2}});
    ASSERT_TRUE(two.ok) << two.error.message;
    EXPECT_EQ(two.value["items"].size(), 2U);
    EXPECT_EQ(two.value["truncated"], true);
    const ApiResult one = s.in(first, "search_drives", {{"q", "day"}, {"drive_id", team_id}});
    ASSERT_TRUE(one.ok) << one.error.message;
    ASSERT_EQ(one.value["items"].size(), 1U);
    EXPECT_EQ(one.value["items"][0]["path"], "Trip/Day 1");
    EXPECT_TRUE(s.in(first, "search_drives", {{"q", "report"}, {"drive_id", links_id}})
                    .value["items"]
                    .size() == 3U);
    EXPECT_EQ(s.in(first, "search_drives", {{"q", "report"}}, 1).value["items"].size(), 0U)
        << "another workspace of the space has its own drives";
    EXPECT_EQ(s.in(first, "search_drives", {{"q", "report"}, {"drive_id", team_id}}, 1).error.code,
              "not_found");

    // Outside the workspace's space nothing is answered.
    EXPECT_EQ(s.call(second,
                     "search_drives",
                     {{"customer_id", first.customer_id},
                      {"workspace_id", first.workspaces[0]},
                      {"q", "report"}})
                  .error.code,
              "unauthorized");
    EXPECT_EQ(s.in(second, "search_drives", {{"q", "report"}, {"drive_id", team_id}}).error.code,
              "not_found");
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
                             "recent_drive_entries",
                             "load_drive_dataset",
                             "load_drive_model",
                             "drive_share_people",
                             "search_drives"}) {
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
                                     .search = std::make_shared<lake::search::Service>(root),
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

    // A GET's whole response, headers included, over a plain socket.
    std::string raw(const std::string& path) {
        const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<unsigned short>(server->port()));
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(socket_fd);
            return {};
        }
        const std::string request = "GET " + path +
                                    " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
                                    "Authorization: Bearer drives-test\r\n\r\n";
        static_cast<void>(::send(socket_fd, request.data(), request.size(), 0));
        std::string response;
        char buffer[4096];
        for (;;) {
            const ssize_t count = ::recv(socket_fd, buffer, sizeof(buffer), 0);
            if (count <= 0) {
                break;
            }
            response.append(buffer, static_cast<std::size_t>(count));
        }
        ::close(socket_fd);
        return response;
    }

    // The hits of a search over the first space, as paths.
    std::set<std::string> found(const std::string& text) {
        const json result =
            read("search", {{"query", text}, {"customer", spaces.spaces[0].customer_id}});
        EXPECT_TRUE(result.value("ok", false)) << result.dump();
        std::set<std::string> paths;
        for (const json& item : result["value"]["items"]) {
            paths.insert(item["path"].get<std::string>());
        }
        return paths;
    }
};

std::string lowercase_headers(std::string response) {
    const std::size_t end = response.find("\r\n\r\n");
    std::string head = response.substr(0, end);
    std::ranges::transform(
        head, head.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return head;
}

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
    EXPECT_NE(lowercase_headers(lake.raw("/drive/files/download" + query))
                  .find("content-disposition: attachment; filename=\"model.bin\""),
              std::string::npos)
        << "the file saves under its name";

    // A workspace file loads as a dataset too (lake#593).
    const json csv = lake.call(
        "upload_drive_entries",
        {{"drive_id", drive_id},
         {"files",
          json::array({{{"path", "data/people.jsonl"}, {"content", base64("{\"a\":1}\n")}}})}});
    ASSERT_TRUE(csv.value("ok", false)) << csv.dump();
    lake.spaces.zetta->answers["upload_dataset"] = {{"table", "people"}, {"rows", 1}};
    const json loaded = lake.call(
        "load_drive_dataset",
        {{"drive_id", drive_id}, {"id", csv["value"]["items"][0]["id"]}, {"table_name", "people"}});
    ASSERT_TRUE(loaded.value("ok", false)) << loaded.dump();
    EXPECT_EQ(loaded["value"]["source"]["name"], "people.jsonl");
    EXPECT_EQ(
        lake.call("load_drive_dataset",
                  {{"drive_id", drive_id}, {"id", id}, {"table_name", "big"}})["error"]["path"],
        "/format")
        << "model.bin is neither CSV nor JSONL";
    const auto [missing, error] =
        lake.request("GET",
                     "/drive/files/download?customer_id=" + space.customer_id +
                         "&workspace_id=" + space.workspaces[0] + "&drive_id=" + drive_id +
                         "&id=" + lake::control::new_uuid_v4());
    EXPECT_EQ(missing, 404U) << error;
}

// lake#584: a file written through a repository drive reaches search as a push does, and moves
// and deletions follow. lake#587: download_drive_file answers the bytes, a CSV is text/csv, and
// both download routes are in OpenAPI. lake#593: a CSV in a drive loads as a dataset.
TEST(DrivesHttp, ARepositoryDriveWriteIsSearchedDownloadedAndLoaded) {
    Lake lake;
    const json drive =
        lake.call("create_drive",
                  {{"name", "Docs"}, {"storage", "repository"}, {"repository_name", "docs-drive"}});
    ASSERT_TRUE(drive.value("ok", false)) << drive.dump();
    const std::string repository_id = drive["value"]["repository_id"];
    const json uploaded = lake.call(
        "upload_drive_files",
        {{"repository_id", repository_id},
         {"ref", "main"},
         {"path", ""},
         {"files",
          json::array({{{"path", "docs/readme.md"}, {"content", base64("zorblaxquux notes")}},
                       {{"name", "churn.csv"},
                        {"content", base64("id,churned\n1,true\n2,false\n")},
                        {"content_type", "text/csv"}}})}},
        false);
    ASSERT_TRUE(uploaded.value("ok", false)) << uploaded.dump();
    EXPECT_EQ(lake.found("zorblaxquux"), (std::set<std::string>{"docs/readme.md"}))
        << "a drive write is searched at once";
    const json moved = lake.call("move_drive_path",
                                 {{"repository_id", repository_id},
                                  {"ref", "main"},
                                  {"path", "docs/readme.md"},
                                  {"destination", "docs/moved.md"}},
                                 false);
    ASSERT_TRUE(moved.value("ok", false)) << moved.dump();
    EXPECT_EQ(lake.found("zorblaxquux"), (std::set<std::string>{"docs/moved.md"}));

    // The bytes in the answer, the type from the name, and where to stream them from.
    const json file =
        lake.read("download_drive_file",
                  {{"repository_id", repository_id}, {"ref", "main"}, {"path", "churn.csv"}});
    ASSERT_TRUE(file.value("ok", false)) << file.dump();
    EXPECT_EQ(file["value"]["content_base64"], base64("id,churned\n1,true\n2,false\n"));
    EXPECT_EQ(file["value"]["media_type"], "text/csv");
    const std::string url = file["value"]["download_url"];
    EXPECT_TRUE(url.starts_with("/drive/download?repository_id=" + repository_id)) << url;
    const std::string streamed = lake.raw(url);
    const std::string head = lowercase_headers(streamed);
    EXPECT_NE(head.find(" 200 "), std::string::npos) << head;
    EXPECT_NE(head.find("content-type: text/csv"), std::string::npos) << head;
    EXPECT_NE(head.find("content-disposition: attachment; filename=\"churn.csv\""),
              std::string::npos)
        << head;
    EXPECT_TRUE(streamed.ends_with("id,churned\n1,true\n2,false\n"));
    EXPECT_EQ(lake.read("download_drive_file",
                        {{"repository_id", repository_id},
                         {"ref", "main"},
                         {"path", "churn.csv"},
                         {"sort", "name"}})["error"]["code"],
              "invalid_input")
        << "browse's ordering is not a download input";
    const auto [status, openapi] = lake.request("GET", "/v1/openapi.json");
    const json document = json::parse(openapi, nullptr, false);
    EXPECT_TRUE(document["paths"].contains("/drive/download"));
    EXPECT_TRUE(document["paths"].contains("/drive/files/download"));

    // Load as dataset: the file's bytes go to the workspace's upload_dataset.
    lake.spaces.zetta->answers["upload_dataset"] = {
        {"table", "churn"}, {"rows", 2}, {"database", "datasets"}};
    const json loaded = lake.call(
        "load_drive_dataset",
        {{"drive_id", drive["value"]["id"]}, {"id", "churn.csv"}, {"table_name", "churn"}});
    ASSERT_TRUE(loaded.value("ok", false)) << loaded.dump();
    EXPECT_EQ(loaded["value"]["table"], "churn");
    EXPECT_EQ(loaded["value"]["source"]["name"], "churn.csv");
    const auto sent = std::ranges::find_if(
        lake.spaces.zetta->calls, [](const auto& call) { return call.first == "upload_dataset"; });
    ASSERT_NE(sent, lake.spaces.zetta->calls.end());
    EXPECT_EQ(sent->second["format"], "csv");
    EXPECT_EQ(sent->second["content"], "id,churned\n1,true\n2,false\n");
    EXPECT_EQ(lake.call("load_drive_dataset",
                        {{"drive_id", drive["value"]["id"]},
                         {"id", "docs/moved.md"},
                         {"table_name", "notes"}})["error"]["path"],
              "/format")
        << "only CSV and JSONL load";

    // Bring a model file (lake#654): its text goes to the engine's register_model, with where it
    // came from; the engine's refusal of the file points at the file.
    const std::string weights = R"({"objective":"binary_logistic","trees":[]})";
    const json kept = lake.call(
        "upload_drive_files",
        {{"repository_id", repository_id},
         {"ref", "main"},
         {"path", ""},
         {"files", json::array({{{"path", "models/churn.json"}, {"content", base64(weights)}}})}},
        false);
    ASSERT_TRUE(kept.value("ok", false)) << kept.dump();
    lake.spaces.zetta->answers["register_model"] = {
        {"model_id", "m-9"}, {"name", "partner churn"}, {"standalone", true}};
    const json brought = lake.call("load_drive_model",
                                   {{"drive_id", drive["value"]["id"]},
                                    {"id", "models/churn.json"},
                                    {"name", "partner churn"},
                                    {"metrics", {{"held_out_auc", 0.8}}}});
    ASSERT_TRUE(brought.value("ok", false)) << brought.dump();
    EXPECT_EQ(brought["value"]["model_id"], "m-9");
    EXPECT_EQ(brought["value"]["source"]["name"], "churn.json");
    const auto registered = std::ranges::find_if(
        lake.spaces.zetta->calls, [](const auto& call) { return call.first == "register_model"; });
    ASSERT_NE(registered, lake.spaces.zetta->calls.end());
    EXPECT_EQ(registered->second["weights"], weights);
    EXPECT_EQ(registered->second["name"], "partner churn");
    EXPECT_EQ(registered->second["metrics"]["held_out_auc"], 0.8);
    EXPECT_EQ(registered->second["source"], "drive Docs: churn.json");
    EXPECT_EQ(lake.call("load_drive_model",
                        {{"drive_id", drive["value"]["id"]}, {"id", "models/churn.json"}})["error"]
                       ["path"],
              "/name")
        << "a new model is named";

    // A virtual drive's shortcut to the repository path is followed (lake#682).
    const json links = lake.call("create_drive", {{"name", "Links"}, {"storage", "virtual"}});
    ASSERT_TRUE(links.value("ok", false)) << links.dump();
    const json shortcut = lake.call("create_drive_shortcut",
                                    {{"drive_id", links["value"]["id"]},
                                     {"name", "Churn weights"},
                                     {"target",
                                      {{"kind", "repository_path"},
                                       {"repository_id", repository_id},
                                       {"path", "models/churn.json"}}}});
    ASSERT_TRUE(shortcut.value("ok", false)) << shortcut.dump();
    const json via = lake.call("load_drive_model",
                               {{"drive_id", links["value"]["id"]},
                                {"id", shortcut["value"]["id"]},
                                {"name", "via shortcut"}});
    ASSERT_TRUE(via.value("ok", false)) << via.dump();
    EXPECT_EQ(via["value"]["source"]["repository_id"], repository_id);
    EXPECT_EQ(via["value"]["source"]["path"], "models/churn.json");
    EXPECT_EQ(via["value"]["source"]["shortcut"]["name"], "Churn weights");
    const auto last = std::ranges::find_if(
        lake.spaces.zetta->calls.rbegin(), lake.spaces.zetta->calls.rend(), [](const auto& call) {
            return call.first == "register_model";
        });
    ASSERT_NE(last, lake.spaces.zetta->calls.rend());
    EXPECT_EQ(last->second["weights"], weights);
    EXPECT_EQ(last->second["source"], "repository docs-drive@main: models/churn.json")
        << last->second.dump();
    lake.spaces.zetta->responder = [](const std::string& capability, const json&)
        -> std::optional<lake::control::ZettaControlResult<json>> {
        if (capability != "register_model") {
            return std::nullopt;
        }
        return std::unexpected(lake::control::ZettaControlError{
            "not_supported", "/weights: the engine reads gradient-boosted tree models", "/weights"});
    };
    const json refused = lake.call("load_drive_model",
                                   {{"drive_id", drive["value"]["id"]},
                                    {"id", "models/churn.json"},
                                    {"name", "other"}});
    EXPECT_EQ(refused["error"]["code"], "not_supported") << refused.dump();
    EXPECT_EQ(refused["error"]["path"], "/id") << refused.dump();
    lake.spaces.zetta->responder = nullptr;

    const json deleted =
        lake.call("delete_drive_path",
                  {{"repository_id", repository_id}, {"ref", "main"}, {"path", "docs/moved.md"}},
                  false);
    ASSERT_TRUE(deleted.value("ok", false)) << deleted.dump();
    EXPECT_TRUE(lake.found("zorblaxquux").empty());
}

TEST(DrivesHttp, SearchFindsTheNamesOfARepositoryDrivesPaths) {
    Lake lake;
    const json drive = lake.call(
        "create_drive",
        {{"name", "Archive"}, {"storage", "repository"}, {"repository_name", "archive-drive"}});
    ASSERT_TRUE(drive.value("ok", false)) << drive.dump();
    const std::string repository_id = drive["value"]["repository_id"];
    const json folder = lake.call(
        "create_drive_folder",
        {{"repository_id", repository_id}, {"ref", "main"}, {"path", ""}, {"name", "Reports"}},
        false);
    ASSERT_TRUE(folder.value("ok", false)) << folder.dump();
    const json uploaded = lake.call(
        "upload_drive_files",
        {{"repository_id", repository_id},
         {"ref", "main"},
         {"path", "Reports"},
         {"files", json::array({{{"name", "q3-report.csv"}, {"content", base64("a,b\n")}}})}},
        false);
    ASSERT_TRUE(uploaded.value("ok", false)) << uploaded.dump();
    const auto& space = lake.spaces.spaces[0];
    const json found = lake.read("search_drives",
                                 {{"customer_id", space.customer_id},
                                  {"workspace_id", space.workspaces[0]},
                                  {"q", "REPORT"}});
    ASSERT_TRUE(found.value("ok", false)) << found.dump();
    const json& items = found["value"]["items"];
    ASSERT_EQ(items.size(), 2U) << found.dump();
    EXPECT_EQ(items[0]["kind"], "folder");
    EXPECT_EQ(items[0]["path"], "Reports");
    EXPECT_EQ(items[0]["drive"]["storage"], "repository");
    EXPECT_TRUE(items[0]["id"].is_null()) << "a repository item is addressed by its path";
    EXPECT_EQ(items[1]["kind"], "file");
    EXPECT_EQ(items[1]["path"], "Reports/q3-report.csv");
    EXPECT_EQ(items[1]["folder"], "Reports");
    EXPECT_EQ(items[1]["size"], 4);
    EXPECT_FALSE(items[1].contains("modified_at")) << "the tree carries no time";
    const json marker = lake.read("search_drives",
                                  {{"customer_id", space.customer_id},
                                   {"workspace_id", space.workspaces[0]},
                                   {"q", "lake-folder"}});
    EXPECT_TRUE(marker["value"]["items"].empty()) << "the empty-folder marker is never found";
}

TEST(Drives, AStoreDriveBrowsesItsStoreInPlaceAndLeavesItWhenDeleted) {
    Spaces s;
    const auto& first = s.spaces[0];
    const auto bucket = ready_store(s, first, "object", "r2", "media");
    auto contents = std::make_shared<FakeBucket>();
    contents->files["Reports/q3.csv"] = "account,plan\n1,pro\n";
    contents->folders.insert("Reports");
    std::vector<std::string> opened;
    s.browser = [&](const lake::control::StoreOpening& opening)
        -> lake::control::ZettaControlResult<std::shared_ptr<lake::control::StoreContents>> {
        opened.push_back(opening.store.id);
        EXPECT_EQ(opening.connection.value("bucket", ""), opening.store.slug)
            << "the operator's connection";
        EXPECT_TRUE(opening.credential.empty()) << "the store names no secret";
        return std::shared_ptr<lake::control::StoreContents>(contents);
    };

    EXPECT_EQ(s.in(first, "create_drive", {{"name", "Media"}, {"storage", "store"}}).error.path,
              "/store_id");
    EXPECT_EQ(s.in(first,
                   "create_drive",
                   {{"name", "Media"}, {"storage", "files"}, {"store_id", bucket.id}})
                  .error.path,
              "/store_id");
    EXPECT_EQ(s.in(first,
                   "create_drive",
                   {{"name", "Media"}, {"storage", "store"}, {"store_id", bucket.id}},
                   1)
                  .error.code,
              "not_found")
        << "a store of another workspace";
    const ApiResult drive = s.in(
        first, "create_drive", {{"name", "Media"}, {"storage", "store"}, {"store_id", bucket.id}});
    ASSERT_TRUE(drive.ok) << drive.error.message;
    EXPECT_EQ(drive.value["storage"], "store");
    EXPECT_EQ(drive.value["store_id"], bucket.id);
    EXPECT_TRUE(drive.value["repository_id"].is_null());
    const std::string id = drive.value["id"];
    EXPECT_TRUE(contents->files.size() == 1) << "creating the drive copies nothing";

    // Browse: the store's own folders and files, each addressed by its path.
    const ApiResult top = s.in(first, "list_drive_entries", {{"drive_id", id}});
    ASSERT_TRUE(top.ok) << top.error.message;
    ASSERT_EQ(top.value["items"].size(), 1U);
    EXPECT_EQ(top.value["items"][0]["kind"], "folder");
    EXPECT_EQ(top.value["items"][0]["id"], "Reports");
    EXPECT_TRUE(top.value["folder_id"].is_null());
    const ApiResult reports =
        s.in(first, "list_drive_entries", {{"drive_id", id}, {"folder_id", "Reports"}});
    ASSERT_TRUE(reports.ok) << reports.error.message;
    ASSERT_EQ(reports.value["items"].size(), 1U);
    EXPECT_EQ(reports.value["items"][0]["path"], "Reports/q3.csv");
    EXPECT_EQ(reports.value["items"][0]["parent_id"], "Reports");
    EXPECT_EQ(reports.value["items"][0]["content_type"], "text/csv");
    EXPECT_EQ(reports.value["items"][0]["size"], 19);
    EXPECT_EQ(reports.value["ancestors"], json::array({{{"id", "Reports"}, {"name", "Reports"}}}));
    EXPECT_EQ(s.in(first, "list_drive_entries", {{"drive_id", id}, {"path", "Nope"}}).error.code,
              "not_found");
    EXPECT_EQ(s.in(first, "list_drive_entries", {{"drive_id", id}, {"path", "a/../b"}}).error.code,
              "invalid_input");

    // Upload, folders, read, move, search.
    const ApiResult uploaded = s.in(
        first,
        "upload_drive_entries",
        {{"drive_id", id},
         {"parent_id", "Reports"},
         {"files", json::array({{{"path", "2026/notes.txt"}, {"content", base64("hello")}}})}});
    ASSERT_TRUE(uploaded.ok) << uploaded.error.message;
    EXPECT_EQ(uploaded.value["items"][0]["id"], "Reports/2026/notes.txt");
    EXPECT_EQ(contents->files.at("Reports/2026/notes.txt"), "hello") << "written to the store";
    const ApiResult again = s.in(
        first,
        "upload_drive_entries",
        {{"drive_id", id},
         {"files", json::array({{{"path", "Reports/2026/notes.txt"}, {"content", base64("x")}}})}});
    ASSERT_TRUE(again.ok) << again.error.message;
    EXPECT_EQ(again.value["items"][0]["name"], "notes (2).txt") << "a taken name is renamed";
    EXPECT_EQ(
        s.in(first,
             "upload_drive_entries",
             {{"drive_id", id},
              {"on_conflict", "refuse"},
              {"files", json::array({{{"path", "Reports/q3.csv"}, {"content", base64("x")}}})}})
            .error.code,
        "conflict");
    EXPECT_EQ(
        s.in(first, "begin_drive_upload", {{"drive_id", id}, {"path", "big.bin"}, {"size", 10}})
            .error.path,
        "/drive_id")
        << "chunked uploads are for workspace-files drives";
    EXPECT_EQ(s.in(first,
                   "create_drive_shortcut",
                   {{"drive_id", id},
                    {"name", "Site"},
                    {"target", {{"kind", "url"}, {"url", "https://x.test"}}}})
                  .error.path,
              "/drive_id");
    const ApiResult folder =
        s.in(first, "create_drive_folder_entry", {{"drive_id", id}, {"path", "Archive/Old"}});
    ASSERT_TRUE(folder.ok) << folder.error.message;
    EXPECT_EQ(folder.value["id"], "Archive/Old");
    EXPECT_TRUE(contents->folders.contains("Archive"));
    EXPECT_EQ(
        s.in(first, "create_drive_folder_entry", {{"drive_id", id}, {"path", "Reports/q3.csv/x"}})
            .error.code,
        "conflict");
    const ApiResult read =
        s.in(first, "read_drive_file", {{"drive_id", id}, {"id", "Reports/2026/notes.txt"}});
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value["content_base64"], base64("hello"));
    const ApiResult got = s.in(first, "get_drive_entry", {{"drive_id", id}, {"id", "Archive"}});
    ASSERT_TRUE(got.ok) << got.error.message;
    EXPECT_EQ(got.value["kind"], "folder");
    const ApiResult moved = s.in(
        first, "move_drive_entry", {{"drive_id", id}, {"id", "Reports"}, {"parent_id", "Archive"}});
    ASSERT_TRUE(moved.ok) << moved.error.message;
    EXPECT_EQ(moved.value["id"], "Archive/Reports");
    EXPECT_TRUE(contents->files.contains("Archive/Reports/q3.csv")) << "moved in the store";
    EXPECT_EQ(s.in(first,
                   "move_drive_entry",
                   {{"drive_id", id}, {"id", "Archive"}, {"parent_id", "Archive/Old"}})
                  .error.path,
              "/parent_id")
        << "a folder cannot move into itself";
    const ApiResult renamed = s.in(
        first, "move_drive_entry", {{"drive_id", id}, {"id", "Archive/Old"}, {"name", "Older"}});
    ASSERT_TRUE(renamed.ok) << renamed.error.message;
    EXPECT_EQ(renamed.value["path"], "Archive/Older");
    const ApiResult found = s.in(first, "search_drives", {{"q", "Q3"}});
    ASSERT_TRUE(found.ok) << found.error.message;
    ASSERT_EQ(found.value["items"].size(), 1U);
    EXPECT_EQ(found.value["items"][0]["path"], "Archive/Reports/q3.csv");
    EXPECT_EQ(found.value["items"][0]["drive"]["store_id"], bucket.id);
    EXPECT_EQ(found.value["items"][0]["folder"], "Archive/Reports");
    EXPECT_TRUE(s.in(first, "recent_drive_entries", json::object()).value["items"].empty())
        << "recent files are workspace files";

    // Delete an item, then the drive: the store and what is in it stay.
    const ApiResult removed =
        s.in(first, "delete_drive_entry", {{"drive_id", id}, {"id", "Archive/Older"}});
    ASSERT_TRUE(removed.ok) << removed.error.message;
    EXPECT_FALSE(contents->folders.contains("Archive/Older"));
    const ApiResult gone = s.in(first, "delete_drive", {{"id", id}, {"confirm_name", "Media"}});
    ASSERT_TRUE(gone.ok) << gone.error.message;
    EXPECT_EQ(gone.value["store_id"], bucket.id);
    EXPECT_EQ(gone.value["entries_removed"], 0);
    EXPECT_TRUE(contents->files.contains("Archive/Reports/q3.csv"))
        << "deleting the drive never deletes the store's contents";
    const ApiResult stores = s.in(first, "list_stores", json::object());
    ASSERT_TRUE(stores.ok) << stores.error.message;
    ASSERT_EQ(stores.value["stores"].size(), 1U);
    EXPECT_EQ(stores.value["stores"][0]["id"], bucket.id);
    EXPECT_EQ(stores.value["stores"][0]["desired"], "present") << "the store survives";
    EXPECT_FALSE(opened.empty());
}

// Bring one → A drive (lake#682): a model file is brought from every drive kind and any depth,
// and a virtual drive's shortcut to a file is followed once.
TEST(Drives, AModelIsBroughtFromEveryDriveKindAndAShortcutIsFollowed) {
    Spaces s;
    const auto& first = s.spaces[0];
    const std::string weights = R"({"objective":"binary_logistic","trees":[]})";
    s.zetta->answers["register_model"] = {{"model_id", "m-1"}, {"standalone", true}};
    const auto registered = [&] {
        std::vector<json> out;
        for (const auto& [capability, input] : s.zetta->calls) {
            if (capability == "register_model") {
                out.push_back(input);
            }
        }
        return out;
    };

    // A storage drive's file, three folders down, by its path.
    const auto bucket = ready_store(s, first, "object", "r2", "models");
    auto contents = std::make_shared<FakeBucket>();
    contents->folders.insert("2026");
    contents->folders.insert("2026/q3");
    contents->folders.insert("2026/q3/churn");
    contents->files["2026/q3/churn/model.json"] = weights;
    s.browser = [&](const lake::control::StoreOpening&)
        -> lake::control::ZettaControlResult<std::shared_ptr<lake::control::StoreContents>> {
        return std::shared_ptr<lake::control::StoreContents>(contents);
    };
    const ApiResult store_drive = s.in(
        first, "create_drive", {{"name", "Models"}, {"storage", "store"}, {"store_id", bucket.id}});
    ASSERT_TRUE(store_drive.ok) << store_drive.error.message;
    const ApiResult from_store = s.in(first,
                                      "load_drive_model",
                                      {{"drive_id", store_drive.value["id"]},
                                       {"id", "2026/q3/churn/model.json"},
                                       {"name", "churn"}});
    ASSERT_TRUE(from_store.ok) << from_store.error.message;
    EXPECT_EQ(from_store.value["model_id"], "m-1");
    EXPECT_EQ(from_store.value["source"]["name"], "model.json");
    ASSERT_EQ(registered().size(), 1U);
    EXPECT_EQ(registered()[0]["weights"], weights);
    EXPECT_EQ(registered()[0]["source"], "drive Models: model.json");
    EXPECT_EQ(s.in(first,
                   "load_drive_model",
                   {{"drive_id", store_drive.value["id"]}, {"id", "2026/q3"}, {"name", "x"}})
                  .error.code,
              "not_found")
        << "a folder is not a model file";

    // A workspace-files drive's file in a subfolder, by its entry id.
    const ApiResult files = s.in(first, "create_drive", {{"name", "Files"}, {"storage", "files"}});
    ASSERT_TRUE(files.ok) << files.error.message;
    const ApiResult uploaded =
        s.in(first,
             "upload_drive_entries",
             {{"drive_id", files.value["id"]},
              {"files", json::array({{{"path", "a/b/c/model.json"}, {"content", base64(weights)}}})}});
    ASSERT_TRUE(uploaded.ok) << uploaded.error.message;
    const std::string file_id = uploaded.value["items"][0]["id"];
    const ApiResult from_files = s.in(
        first,
        "load_drive_model",
        {{"drive_id", files.value["id"]}, {"id", file_id}, {"name", "deep"}});
    ASSERT_TRUE(from_files.ok) << from_files.error.message;
    EXPECT_EQ(from_files.value["source"]["id"], file_id);

    // A virtual drive: its shortcut to the file is followed to the file's own drive; a shortcut to
    // something that is not a file, a folder, and a broken shortcut are refused at /id.
    const ApiResult links = s.in(first, "create_drive", {{"name", "Links"}, {"storage", "virtual"}});
    ASSERT_TRUE(links.ok) << links.error.message;
    const std::string virtual_id = links.value["id"];
    const auto shortcut = [&](std::string name, json target) {
        return s.in(first,
                    "create_drive_shortcut",
                    {{"drive_id", virtual_id}, {"name", std::move(name)}, {"target", std::move(target)}});
    };
    const ApiResult to_file = shortcut(
        "Churn model",
        {{"kind", "drive_entry"}, {"drive_id", files.value["id"]}, {"entry_id", file_id}});
    ASSERT_TRUE(to_file.ok) << to_file.error.message;
    const ApiResult followed =
        s.in(first,
             "load_drive_model",
             {{"drive_id", virtual_id}, {"id", to_file.value["id"]}, {"name", "linked"}});
    ASSERT_TRUE(followed.ok) << followed.error.message;
    EXPECT_EQ(followed.value["source"]["drive_id"], files.value["id"]) << "the file's own drive";
    EXPECT_EQ(followed.value["source"]["id"], file_id);
    EXPECT_EQ(followed.value["source"]["shortcut"]["drive_id"], virtual_id);
    EXPECT_EQ(followed.value["source"]["shortcut"]["name"], "Churn model");
    EXPECT_EQ(registered().back()["source"], "drive Files: model.json");
    EXPECT_EQ(registered().back()["name"], "linked");

    const ApiResult to_site = shortcut("Site", {{"kind", "url"}, {"url", "https://orama.test/"}});
    ASSERT_TRUE(to_site.ok) << to_site.error.message;
    const ApiResult site = s.in(
        first, "load_drive_model", {{"drive_id", virtual_id}, {"id", to_site.value["id"]}, {"name", "x"}});
    EXPECT_EQ(site.error.code, "invalid_input");
    EXPECT_EQ(site.error.path, "/id");
    const ApiResult folder =
        s.in(first, "create_drive_folder_entry", {{"drive_id", virtual_id}, {"path", "Q3"}});
    ASSERT_TRUE(folder.ok) << folder.error.message;
    EXPECT_EQ(s.in(first,
                   "load_drive_model",
                   {{"drive_id", virtual_id}, {"id", folder.value["id"]}, {"name", "x"}})
                  .error.path,
              "/id");
    const ApiResult to_links = shortcut(
        "Loop", {{"kind", "drive_entry"}, {"drive_id", virtual_id}, {"entry_id", to_file.value["id"]}});
    ASSERT_TRUE(to_links.ok) << to_links.error.message;
    EXPECT_EQ(s.in(first,
                   "load_drive_model",
                   {{"drive_id", virtual_id}, {"id", to_links.value["id"]}, {"name", "x"}})
                  .error.code,
              "invalid_input")
        << "a shortcut is followed once";
    ASSERT_TRUE(
        s.in(first, "delete_drive_entry", {{"drive_id", files.value["id"]}, {"id", file_id}}).ok);
    EXPECT_EQ(s.in(first,
                   "load_drive_model",
                   {{"drive_id", virtual_id}, {"id", to_file.value["id"]}, {"name", "x"}})
                  .error.code,
              "not_found")
        << "a broken shortcut";
    EXPECT_EQ(registered().size(), 3U) << "nothing more was kept";
}

TEST(Drives, AVolumeIsShownNotBrowsedAndAStoreLakeCannotBrowseIsRefused) {
    Spaces s;
    const auto& first = s.spaces[0];
    const auto volume = ready_store(s, first, "volume", "cluster", "scratch");
    const auto bucket = ready_store(s, first, "object", "s3", "archive");
    const auto waiting = ready_store(s, first, "object", "r2", "later", false);
    const auto create = [&](const std::string& name, const std::string& store_id) {
        return s.in(
            first, "create_drive", {{"name", name}, {"storage", "store"}, {"store_id", store_id}});
    };
    // No store browser, as on a Lake not configured to reach external buckets.
    const ApiResult refused = create("Archive", bucket.id);
    EXPECT_EQ(refused.error.code, "not_supported");
    EXPECT_EQ(refused.error.path, "/store_id");
    EXPECT_NE(refused.error.message.find("Bucket — your AWS S3 bucket"), std::string::npos)
        << refused.error.message;

    s.browser = [](const lake::control::StoreOpening&)
        -> lake::control::ZettaControlResult<std::shared_ptr<lake::control::StoreContents>> {
        return std::shared_ptr<lake::control::StoreContents>(std::make_shared<FakeBucket>());
    };
    const ApiResult shown = create("Scratch", volume.id);
    EXPECT_EQ(shown.error.code, "invalid_input");
    EXPECT_NE(shown.error.message.find("shown, not browsed"), std::string::npos);
    EXPECT_EQ(create("Later", waiting.id).error.code, "workspace_not_ready");
    EXPECT_EQ(create("Nothing", "00000000-0000-4000-8000-000000000000").error.code, "not_found");
    EXPECT_TRUE(s.in(first, "list_drives", json::object()).value["drives"].empty());
    EXPECT_EQ(lake::control::store_kind_refusal("filesystem"), "");
    EXPECT_FALSE(lake::control::valid_store_path("/a"));
    EXPECT_FALSE(lake::control::valid_store_path("a//b"));
    EXPECT_FALSE(lake::control::valid_store_path("a/"));
    EXPECT_TRUE(lake::control::valid_store_path("a b/c.txt"));
    EXPECT_EQ(lake::control::join_store_path("", "a"), "a");
    EXPECT_EQ(lake::control::store_parent("a/b/c"), "a/b");
    EXPECT_EQ(lake::control::store_name("a/b/c"), "c");
}

// The operator's side of a store it reconciles: Ready, with its connection delivered.
void deliver(Spaces& s, const json& store) {
    EXPECT_TRUE(s.store->set_store_status(store["id"], "Ready", "", ""));
    std::ofstream(s.dir / (store["slug"].get<std::string>() + ".json"))
        << json{{"type", store["type"]}, {"provider", store["provider"]}}.dump();
}

TEST(Drives, YourOwnBucketIsNamedWithAWorkspaceSecretAndOpensAsADrive) {
    Spaces s;
    const auto& first = s.spaces[0];
    lake::test::stores::FakeS3 r2;
    s.browser = lake::control::cloud_store_browser(
        r2.http(), [] { return lake::test::stores::at(2026, 10, 11, 12); });
    const ApiResult mount = s.in(first, "create_secret_mount", {{"name", "buckets"}});
    ASSERT_TRUE(mount.ok) << mount.error.message;
    const std::string mount_id = mount.value["id"];
    ASSERT_TRUE(s.call(first,
                       "put_secret",
                       {{"mount_id", mount_id},
                        {"name", "r2-keys"},
                        {"value",
                         {{"access_key_id", r2.access_key_id},
                          {"secret_access_key", r2.secret_access_key}}}})
                    .ok);
    ASSERT_TRUE(s.call(first,
                       "put_secret",
                       {{"mount_id", mount_id}, {"name", "half"}, {"value", {{"access_key_id", "x"}}}})
                    .ok);
    const ApiResult elsewhere = s.in(first, "create_secret_mount", {{"name", "lab-buckets"}}, 1);
    ASSERT_TRUE(elsewhere.ok) << elsewhere.error.message;

    const json good{{"type", "object"},
                    {"provider", "r2"},
                    {"name", "media"},
                    {"bucket", "media"},
                    {"endpoint", "https://account.r2.cloudflarestorage.com/"},
                    {"credential", {{"mount_id", mount_id}, {"name", "r2-keys"}}}};
    const auto refused_at = [&](json change, std::string remove = {}) {
        json input = good;
        if (change.is_object()) {
            input.update(change);
        }
        if (!remove.empty()) {
            input.erase(remove);
        }
        const ApiResult refused = s.in(first, "create_store", input);
        EXPECT_FALSE(refused.ok);
        return refused.error.path;
    };
    EXPECT_EQ(refused_at(json::object(), "credential"), "/credential");
    EXPECT_EQ(refused_at({{"size_gi", 10}}), "/size_gi") << "only a volume has a size";
    EXPECT_EQ(refused_at(json::object(), "endpoint"), "/endpoint") << "an R2 bucket needs its account";
    EXPECT_EQ(refused_at({{"endpoint", "http://account.r2.cloudflarestorage.com"}}), "/endpoint");
    EXPECT_EQ(refused_at({{"bucket", "Media!"}}), "/bucket");
    EXPECT_EQ(refused_at({{"prefix", "/top"}}), "/prefix");
    EXPECT_EQ(refused_at({{"credential", {{"mount_id", elsewhere.value["id"]}, {"name", "r2-keys"}}}}),
              "/credential/mount_id")
        << "the secret is this workspace's";
    EXPECT_EQ(refused_at({{"credential", {{"mount_id", mount_id}, {"name", "half"}}}}),
              "/credential/name");
    EXPECT_EQ(refused_at({{"credential", {{"mount_id", mount_id}, {"name", "missing"}}}}),
              "/credential/name");
    EXPECT_EQ(s.in(first,
                   "create_store",
                   {{"type", "volume"}, {"provider", "cluster"}, {"name", "disk"}, {"bucket", "media"}})
                  .error.path,
              "/bucket");

    const ApiResult created = s.in(first, "create_store", good);
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(created.value["size_gi"], 0);
    EXPECT_EQ(created.value["location"]["bucket"], "media");
    EXPECT_EQ(created.value["location"]["endpoint"], "https://account.r2.cloudflarestorage.com");
    EXPECT_EQ(created.value["location"]["credential"],
              json({{"mount_id", mount_id}, {"name", "r2-keys"}}));
    EXPECT_EQ(created.value.dump().find(r2.secret_access_key), std::string::npos)
        << "a secret is named, never copied";
    const ApiResult early = s.in(first,
                                 "create_drive",
                                 {{"name", "Media"}, {"storage", "store"}, {"store_id", created.value["id"]}});
    EXPECT_EQ(early.error.code, "workspace_not_ready") << "until the operator has it Ready";
    deliver(s, created.value);

    const ApiResult drive = s.in(first,
                                 "create_drive",
                                 {{"name", "Media"}, {"storage", "store"}, {"store_id", created.value["id"]}});
    ASSERT_TRUE(drive.ok) << drive.error.message;
    const std::string id = drive.value["id"];
    const ApiResult uploaded = s.in(first,
                                    "upload_drive_entries",
                                    {{"drive_id", id},
                                     {"files",
                                      json::array({{{"path", "2026/q3.csv"},
                                                    {"content", base64("account,plan\n")}}})}});
    ASSERT_TRUE(uploaded.ok) << uploaded.error.message;
    EXPECT_EQ(r2.objects["2026/q3.csv"].bytes, "account,plan\n") << "the bytes are in the bucket";
    const ApiResult listed = s.in(first, "list_drive_entries", {{"drive_id", id}, {"folder_id", "2026"}});
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value["items"].size(), 1U);
    EXPECT_EQ(listed.value["items"][0]["path"], "2026/q3.csv");
    const ApiResult read = s.in(first, "read_drive_file", {{"drive_id", id}, {"id", "2026/q3.csv"}});
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value["content_base64"], base64("account,plan\n"));
    EXPECT_TRUE(r2.signed_failures.empty());

    // Deleting the storage forgets the bucket; what is in it stays in the customer's account.
    ASSERT_TRUE(s.in(first, "delete_store", {{"store_id", created.value["id"]}, {"confirm", "media"}}).ok);
    EXPECT_TRUE(r2.objects.contains("2026/q3.csv"));
}

TEST(Drives, AStoreKeptInTheWorkspaceZettaIsDeletedWithItsBytes) {
    Spaces s;
    const auto& first = s.spaces[0];
    const ApiResult created = s.in(first,
                                   "create_store",
                                   {{"type", "filesystem"}, {"provider", "zetta"}, {"name", "scratch"}});
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_TRUE(created.value["location"].is_null());
    EXPECT_EQ(s.in(first,
                   "create_store",
                   {{"type", "object"}, {"provider", "zetta"}, {"name", "other"}, {"size_gi", 5}})
                  .error.path,
              "/size_gi");
    deliver(s, created.value);
    const ApiResult drive = s.in(first,
                                 "create_drive",
                                 {{"name", "Scratch"}, {"storage", "store"}, {"store_id", created.value["id"]}});
    ASSERT_TRUE(drive.ok) << drive.error.message;
    ASSERT_TRUE(s.in(first,
                     "upload_drive_entries",
                     {{"drive_id", drive.value["id"]},
                      {"files", json::array({{{"path", "a/b.txt"}, {"content", base64("kept")}}})}})
                    .ok);
    const std::string bucket = lake::control::kept_store_bucket(created.value["slug"].get<std::string>());
    EXPECT_EQ(s.objects->objects(Spaces::url(first.workspaces[0]), bucket).size(), 1U);
    const ApiResult read =
        s.in(first, "read_drive_file", {{"drive_id", drive.value["id"]}, {"id", "a/b.txt"}});
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value["content_base64"], base64("kept"));

    ASSERT_TRUE(
        s.in(first, "delete_store", {{"store_id", created.value["id"]}, {"confirm", "scratch"}}).ok);
    EXPECT_TRUE(s.objects->objects(Spaces::url(first.workspaces[0]), bucket).empty())
        << "Lake deletes the bytes it keeps";
}

TEST(Drives, ALakeStoreNamesARepositoryOfItsWorkspace) {
    Spaces s;
    const auto& first = s.spaces[0];
    const ApiResult refused = s.in(first,
                                   "create_store",
                                   {{"type", "object"},
                                    {"provider", "lake"},
                                    {"name", "models"},
                                    {"repository_id", "00000000-0000-4000-8000-000000000000"}});
    EXPECT_EQ(refused.error.code, "not_found");
    EXPECT_EQ(refused.error.path, "/repository_id");
    EXPECT_EQ(s.in(first,
                   "create_store",
                   {{"type", "object"}, {"provider", "zetta"}, {"name", "x"}, {"repository_id", "r"}})
                  .error.path,
              "/repository_id");
}

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <liquid/runtime.hpp>
#include <nlohmann/json.hpp>

#include "lake/control/workspace_records.hpp"

namespace lake::control {

// A tenant image's manifests and tags (lake#383 phase A, decision 54 S4-S6,
// docs/api/registry.md#where-a-tenant-image-lives), kept in the workspace Zetta of the
// repository's route (oci_repository_routes) as sealed records
// (docs/architecture/workspace-records.md). Since phase B a blob's bytes are an object in the
// workspace's liquid store (lake#380), found through its lake_oci_blobs record.
//
// lake_oci_manifests: one record per manifest of the space, keyed by the blind index of its
// digest and found by the space (`customer:<id>`). It is content-addressed, so it is written
// `if_absent` and never changes: a second push of the same manifest finds it present.
//
// lake_oci_tags: one record per tag of a repository, keyed by the blind index of the repository
// and tag, found by the repository (`customer:<id>\nrepository:<name>`), so one list_records reads
// every tag of a repository. A tag moves to another manifest with a write conditional on the
// revision read (`if_absent` for a new tag), reread on a conflict: a tag is never torn and every
// move is applied whole, but two pushes of one tag still end with the last one, as the OCI
// protocol expects. Each tag carries what its manifest references, so a blob read and a size count
// need only the tags, and says whether the one-shot copy wrote it (`copied`).
//
// lake_oci_blobs: one record per blob the space pushed into this workspace, keyed by the blind
// index of its digest and written `if_absent` when an upload into one of the workspace's
// repositories completes, or by a copy. A manifest pushed into the workspace may reference only
// blobs recorded here, so a space never publishes another space's (or another workspace's) layer
// by knowing its digest. Since phase B (decision 54 S5) the record also maps the OCI digest to the
// blob's identity in the workspace's liquid store (`object`, a liquid digest): the store is
// encrypted and liquid addresses an object by its own hash, so the SHA-256 a client sends finds it
// only through this record. A record without `object` was written before phase B, or while the
// workspace was still served from its space's store: its bytes are in the instance registry until
// copy_workspace_records (`oci_blobs`) names its object, and once it does it is read only from
// the workspace store.

// A digest a manifest references (its config or a layer), with the size the registry holds.
struct OciReference {
    std::string digest;
    std::uint64_t size = 0;
};

struct OciManifest {
    std::string customer_id;
    std::string digest;       // sha256:<hex> of `document`
    std::string content_type; // the media type it was pushed with
    std::string document;     // the manifest's exact bytes (schema version 2 JSON)
    std::vector<OciReference> references;
    std::int64_t at_us = 0;
    std::int64_t revision = 0;
};

struct OciTag {
    std::string customer_id;
    std::string repository; // the route's name below the space: `<image>` or `cache/<image>`
    std::string tag;
    std::string digest; // the manifest it names
    std::string content_type;
    std::uint64_t manifest_size = 0;
    std::vector<OciReference> references; // the manifest's, copied when the tag moves
    bool copied = false; // written by the one-shot copy, not by a push
    std::string updated_at;
    std::int64_t at_us = 0;
    std::int64_t revision = 0;
    std::int64_t lake_revision = 1;
};

struct OciManifestCodec {
    using Value = OciManifest;
    static constexpr RecordCollection collection{"oci_manifests", 1};
    static std::string lookup(const OciManifest& manifest);
    static nlohmann::json encode(const OciManifest& manifest);
    static std::optional<OciManifest> decode(const StoredRecord& record);
};

// A blob the space pushed into this workspace.
struct OciBlob {
    std::string customer_id;
    std::string digest;
    std::uint64_t size = 0;
    // The blob's identity in the workspace's liquid store (a liquid digest, `blake3:<hex>`);
    // empty while its bytes are in the instance registry.
    std::string object;
    std::int64_t revision = 0;
    std::int64_t lake_revision = 1;
};

struct OciBlobCodec {
    using Value = OciBlob;
    static constexpr RecordCollection collection{"oci_blobs", 1};
    static std::string lookup(const OciBlob& blob);
    static nlohmann::json encode(const OciBlob& blob);
    static std::optional<OciBlob> decode(const StoredRecord& record);
};

struct OciTagCodec {
    using Value = OciTag;
    static constexpr RecordCollection collection{"oci_tags", 1};
    static std::string lookup(const OciTag& tag);
    static nlohmann::json encode(const OciTag& tag);
    static std::optional<OciTag> decode(const StoredRecord& record);
};

// The lookup of one repository's tags.
std::string oci_tags_lookup(const std::string& customer_id, const std::string& repository);

// A manifest of the space by digest; nullopt when this workspace holds none (or it does not open
// here, or its bytes do not match its digest).
liquid::runtime::task<ZettaControlResult<std::optional<OciManifest>>>
get_oci_manifest(WorkspaceRecords& records, std::string customer_id, std::string digest);

// Stores a manifest where it is absent; an existing one with that digest is the same bytes and is
// answered as it is. `manifest.at_us` 0 is now.
liquid::runtime::task<ZettaControlResult<OciManifest>>
put_oci_manifest(WorkspaceRecords& records, OciManifest manifest);

// One tag of a repository; nullopt when it has none.
liquid::runtime::task<ZettaControlResult<std::optional<OciTag>>>
get_oci_tag(WorkspaceRecords& records, std::string customer_id, std::string repository,
            std::string tag);

// Points a tag at a manifest: a new tag is written `if_absent`, an existing one at the revision
// read, rereading on a conflict up to `attempts` times. `at_us` (a copy's instant) is used only
// for a new tag; 0 is now.
liquid::runtime::task<ZettaControlResult<OciTag>>
set_oci_tag(WorkspaceRecords& records, OciTag tag, int attempts = 5);

// Writes a copied tag only where none exists: `conflict` otherwise (the copy counts it present).
liquid::runtime::task<ZettaControlResult<OciTag>> create_oci_tag(WorkspaceRecords& records,
                                                                 OciTag tag);

// Records that the space pushed `blob` into this workspace; an existing record is kept.
liquid::runtime::task<ZettaControlResult<void>> record_oci_blob(WorkspaceRecords& records,
                                                                OciBlob blob);

// Whether the space pushed the blob into this workspace.
liquid::runtime::task<ZettaControlResult<bool>>
has_oci_blob(WorkspaceRecords& records, std::string customer_id, std::string digest);

// The space's record of one blob in this workspace; nullopt when it has none.
liquid::runtime::task<ZettaControlResult<std::optional<OciBlob>>>
get_oci_blob(WorkspaceRecords& records, std::string customer_id, std::string digest);

// Records a blob whose bytes are now the store object `blob.object` (lake#383 phase B): a new
// record is written `if_absent`; one without an object (its bytes were in the instance registry)
// gains it at the revision read; one that already names an object keeps it, since the object holds
// the same bytes. Answers the record as stored.
liquid::runtime::task<ZettaControlResult<OciBlob>> store_oci_blob(WorkspaceRecords& records,
                                                                  OciBlob blob);

// Every blob record of the space in this workspace, by digest.
liquid::runtime::task<ZettaControlResult<std::vector<OciBlob>>>
list_oci_blobs(WorkspaceRecords& records, std::string customer_id);

// Every tag of a repository, by tag.
liquid::runtime::task<ZettaControlResult<std::vector<OciTag>>>
list_oci_tags(WorkspaceRecords& records, std::string customer_id, std::string repository);

} // namespace lake::control

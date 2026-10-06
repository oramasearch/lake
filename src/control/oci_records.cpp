#include "lake/control/oci_records.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "lake/control/identity_crypto.hpp"

namespace lake::control {
namespace {

template <class T> ZettaControlResult<T> failed(const ZettaControlError& error) {
    return ZettaControlResult<T>(std::unexpected(error));
}

std::int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string manifest_name(const std::string& customer_id, const std::string& digest) {
    return "customer:" + customer_id + "\ndigest:" + digest;
}

std::string tag_name(const std::string& customer_id, const std::string& repository,
                     const std::string& tag) {
    return oci_tags_lookup(customer_id, repository) + "\ntag:" + tag;
}

nlohmann::json references_json(const std::vector<OciReference>& references) {
    nlohmann::json values = nlohmann::json::array();
    for (const OciReference& reference : references) {
        values.push_back({{"digest", reference.digest}, {"size", reference.size}});
    }
    return values;
}

std::optional<std::vector<OciReference>> references_of(const nlohmann::json& body) {
    std::vector<OciReference> references;
    const auto found = body.find("references");
    if (found == body.end() || !found->is_array()) {
        return std::nullopt;
    }
    for (const nlohmann::json& value : *found) {
        if (!value.is_object() || !value.contains("digest") || !value.at("digest").is_string() ||
            !value.contains("size") || !value.at("size").is_number_unsigned()) {
            return std::nullopt;
        }
        references.push_back(
            {value.at("digest").get<std::string>(), value.at("size").get<std::uint64_t>()});
    }
    return references;
}

ZettaControlResult<OciTag> tag_or_error(const StoredRecord& record) {
    auto decoded = OciTagCodec::decode(record);
    if (!decoded) {
        return failed<OciTag>(ZettaControlError{"store_error", "the stored image tag did not decode"});
    }
    return ZettaControlResult<OciTag>(std::move(*decoded));
}

} // namespace

std::string oci_tags_lookup(const std::string& customer_id, const std::string& repository) {
    return "customer:" + customer_id + "\nrepository:" + repository;
}

std::string OciManifestCodec::lookup(const OciManifest& manifest) {
    return "customer:" + manifest.customer_id;
}

nlohmann::json OciManifestCodec::encode(const OciManifest& manifest) {
    return {{"customer_id", manifest.customer_id},
            {"digest", manifest.digest},
            {"content_type", manifest.content_type},
            {"document", manifest.document},
            {"references", references_json(manifest.references)}};
}

std::optional<OciManifest> OciManifestCodec::decode(const StoredRecord& record) {
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    const nlohmann::json& body = record.body;
    OciManifest manifest;
    manifest.customer_id = body.value("customer_id", std::string());
    manifest.digest = body.value("digest", std::string());
    manifest.content_type = body.value("content_type", std::string());
    manifest.document = body.value("document", std::string());
    auto references = references_of(body);
    // Content-addressed: bytes that are not their digest are not this manifest.
    if (manifest.customer_id.empty() || !references ||
        manifest.digest != "sha256:" + identity_crypto::sha256_hex(manifest.document)) {
        return std::nullopt;
    }
    manifest.references = std::move(*references);
    manifest.at_us = record.at_us;
    manifest.revision = record.revision;
    return manifest;
}

std::string OciTagCodec::lookup(const OciTag& tag) {
    return oci_tags_lookup(tag.customer_id, tag.repository);
}

nlohmann::json OciTagCodec::encode(const OciTag& tag) {
    return {{"customer_id", tag.customer_id},
            {"repository", tag.repository},
            {"tag", tag.tag},
            {"digest", tag.digest},
            {"content_type", tag.content_type},
            {"manifest_size", tag.manifest_size},
            {"references", references_json(tag.references)},
            {"copied", tag.copied},
            {"updated_at", tag.updated_at}};
}

std::optional<OciTag> OciTagCodec::decode(const StoredRecord& record) {
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    const nlohmann::json& body = record.body;
    OciTag tag;
    tag.customer_id = body.value("customer_id", std::string());
    tag.repository = body.value("repository", std::string());
    tag.tag = body.value("tag", std::string());
    tag.digest = body.value("digest", std::string());
    tag.content_type = body.value("content_type", std::string());
    tag.manifest_size = body.value("manifest_size", std::uint64_t{0});
    auto references = references_of(body);
    if (tag.customer_id.empty() || tag.repository.empty() || tag.tag.empty() ||
        tag.digest.empty() || !references) {
        return std::nullopt;
    }
    tag.references = std::move(*references);
    tag.copied = body.value("copied", false);
    tag.at_us = record.at_us;
    tag.updated_at = body.value("updated_at", record_instant(record.at_us));
    tag.revision = record.revision;
    tag.lake_revision = record.lake_revision;
    return tag;
}

liquid::runtime::task<ZettaControlResult<std::optional<OciManifest>>>
get_oci_manifest(WorkspaceRecords& records, std::string customer_id, std::string digest) {
    using Found = std::optional<OciManifest>;
    const std::string key =
        records.index_of(OciManifestCodec::collection, manifest_name(customer_id, digest));
    auto stored = settled(co_await records.get(OciManifestCodec::collection, key));
    if (!stored) {
        co_return failed<Found>(stored.error());
    }
    if (!*stored) {
        co_return ZettaControlResult<Found>(Found{});
    }
    auto manifest = OciManifestCodec::decode(**stored);
    if (!manifest || manifest->customer_id != customer_id || manifest->digest != digest) {
        co_return ZettaControlResult<Found>(Found{});
    }
    co_return ZettaControlResult<Found>(std::move(manifest));
}

liquid::runtime::task<ZettaControlResult<OciManifest>>
put_oci_manifest(WorkspaceRecords& records, OciManifest manifest) {
    const std::int64_t at_us = manifest.at_us > 0 ? manifest.at_us : now_us();
    const std::string key = records.index_of(OciManifestCodec::collection,
                                             manifest_name(manifest.customer_id, manifest.digest));
    auto stored = settled(co_await records.put(
        OciManifestCodec::collection, key, OciManifestCodec::lookup(manifest),
        OciManifestCodec::encode(manifest), RecordCondition{.if_absent = true}, 1, at_us));
    if (!stored && stored.error().code == "conflict") {
        // Content-addressed: the manifest already there is these bytes.
        auto present = settled(co_await get_oci_manifest(records, manifest.customer_id,
                                                         manifest.digest));
        if (!present) {
            co_return failed<OciManifest>(present.error());
        }
        if (!*present) {
            co_return failed<OciManifest>(ZettaControlError{
                "store_error", "a record holds this manifest's key but does not open as it"});
        }
        co_return ZettaControlResult<OciManifest>(std::move(**present));
    }
    if (!stored) {
        co_return failed<OciManifest>(stored.error());
    }
    auto decoded = OciManifestCodec::decode(*stored);
    if (!decoded) {
        co_return failed<OciManifest>(
            ZettaControlError{"store_error", "the stored image manifest did not decode"});
    }
    co_return ZettaControlResult<OciManifest>(std::move(*decoded));
}

liquid::runtime::task<ZettaControlResult<std::optional<OciTag>>>
get_oci_tag(WorkspaceRecords& records, std::string customer_id, std::string repository,
            std::string tag) {
    using Found = std::optional<OciTag>;
    const std::string key =
        records.index_of(OciTagCodec::collection, tag_name(customer_id, repository, tag));
    auto stored = settled(co_await records.get(OciTagCodec::collection, key));
    if (!stored) {
        co_return failed<Found>(stored.error());
    }
    if (!*stored) {
        co_return ZettaControlResult<Found>(Found{});
    }
    auto decoded = OciTagCodec::decode(**stored);
    if (!decoded || decoded->customer_id != customer_id || decoded->repository != repository ||
        decoded->tag != tag) {
        co_return ZettaControlResult<Found>(Found{});
    }
    co_return ZettaControlResult<Found>(std::move(decoded));
}

liquid::runtime::task<ZettaControlResult<OciTag>>
set_oci_tag(WorkspaceRecords& records, OciTag tag, int attempts) {
    const std::string key =
        records.index_of(OciTagCodec::collection, tag_name(tag.customer_id, tag.repository, tag.tag));
    ZettaControlError last{"conflict", "the image tag kept changing"};
    for (int attempt = 0; attempt < std::max(attempts, 1); ++attempt) {
        auto found =
            settled(co_await get_oci_tag(records, tag.customer_id, tag.repository, tag.tag));
        if (!found) {
            co_return failed<OciTag>(found.error());
        }
        const std::int64_t now = now_us();
        tag.updated_at = record_instant(now);
        const bool absent = !*found;
        auto written = settled(co_await records.put(
            OciTagCodec::collection, key, OciTagCodec::lookup(tag), OciTagCodec::encode(tag),
            absent ? RecordCondition{.if_absent = true}
                   : RecordCondition{.if_revision = (*found)->revision},
            absent ? 1 : (*found)->lake_revision + 1,
            absent ? (tag.at_us > 0 ? tag.at_us : now) : (*found)->at_us));
        if (written) {
            co_return tag_or_error(*written);
        }
        if (written.error().code != "conflict") {
            co_return failed<OciTag>(written.error());
        }
        last = written.error(); // another push moved it first: reread and move it again
    }
    co_return failed<OciTag>(last);
}

liquid::runtime::task<ZettaControlResult<OciTag>> create_oci_tag(WorkspaceRecords& records,
                                                                 OciTag tag) {
    const std::int64_t at_us = tag.at_us > 0 ? tag.at_us : now_us();
    if (tag.updated_at.empty()) {
        tag.updated_at = record_instant(at_us);
    }
    const std::string key =
        records.index_of(OciTagCodec::collection, tag_name(tag.customer_id, tag.repository, tag.tag));
    auto written = settled(co_await records.put(OciTagCodec::collection, key,
                                                OciTagCodec::lookup(tag), OciTagCodec::encode(tag),
                                                RecordCondition{.if_absent = true}, 1, at_us));
    if (!written) {
        co_return failed<OciTag>(written.error());
    }
    co_return tag_or_error(*written);
}

std::string OciBlobCodec::lookup(const OciBlob& blob) {
    return "customer:" + blob.customer_id;
}

nlohmann::json OciBlobCodec::encode(const OciBlob& blob) {
    nlohmann::json body{{"customer_id", blob.customer_id}, {"digest", blob.digest}, {"size", blob.size}};
    if (!blob.object.empty()) {
        body["object"] = blob.object;
    }
    return body;
}

std::optional<OciBlob> OciBlobCodec::decode(const StoredRecord& record) {
    if (record.schema_version != 1 || !record.body.is_object()) {
        return std::nullopt;
    }
    OciBlob blob{record.body.value("customer_id", std::string()),
                 record.body.value("digest", std::string()),
                 record.body.value("size", std::uint64_t{0}),
                 record.body.value("object", std::string())};
    if (blob.customer_id.empty() || blob.digest.empty()) {
        return std::nullopt;
    }
    blob.revision = record.revision;
    blob.lake_revision = record.lake_revision;
    return blob;
}

liquid::runtime::task<ZettaControlResult<void>> record_oci_blob(WorkspaceRecords& records,
                                                                OciBlob blob) {
    const std::string key =
        records.index_of(OciBlobCodec::collection, manifest_name(blob.customer_id, blob.digest));
    auto written = settled(co_await records.put(OciBlobCodec::collection, key,
                                                OciBlobCodec::lookup(blob), OciBlobCodec::encode(blob),
                                                RecordCondition{.if_absent = true}));
    if (!written && written.error().code != "conflict") {
        co_return failed<void>(written.error());
    }
    co_return ZettaControlResult<void>();
}

liquid::runtime::task<ZettaControlResult<bool>>
has_oci_blob(WorkspaceRecords& records, std::string customer_id, std::string digest) {
    auto blob = settled(co_await get_oci_blob(records, std::move(customer_id), std::move(digest)));
    if (!blob) {
        co_return failed<bool>(blob.error());
    }
    co_return ZettaControlResult<bool>(blob->has_value());
}

liquid::runtime::task<ZettaControlResult<std::optional<OciBlob>>>
get_oci_blob(WorkspaceRecords& records, std::string customer_id, std::string digest) {
    using Found = std::optional<OciBlob>;
    const std::string key =
        records.index_of(OciBlobCodec::collection, manifest_name(customer_id, digest));
    auto stored = settled(co_await records.get(OciBlobCodec::collection, key));
    if (!stored) {
        co_return failed<Found>(stored.error());
    }
    if (!*stored) {
        co_return ZettaControlResult<Found>(Found{});
    }
    auto blob = OciBlobCodec::decode(**stored);
    if (!blob || blob->customer_id != customer_id || blob->digest != digest) {
        co_return ZettaControlResult<Found>(Found{});
    }
    co_return ZettaControlResult<Found>(std::move(blob));
}

liquid::runtime::task<ZettaControlResult<OciBlob>> store_oci_blob(WorkspaceRecords& records,
                                                                  OciBlob blob) {
    const std::string key =
        records.index_of(OciBlobCodec::collection, manifest_name(blob.customer_id, blob.digest));
    ZettaControlError last{"conflict", "the image blob record kept changing"};
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto found = settled(co_await get_oci_blob(records, blob.customer_id, blob.digest));
        if (!found) {
            co_return failed<OciBlob>(found.error());
        }
        if (*found && (*found)->object == blob.object && (*found)->size == blob.size) {
            co_return ZettaControlResult<OciBlob>(std::move(**found));
        }
        const bool absent = !*found;
        auto written = settled(co_await records.put(
            OciBlobCodec::collection, key, OciBlobCodec::lookup(blob), OciBlobCodec::encode(blob),
            absent ? RecordCondition{.if_absent = true}
                   : RecordCondition{.if_revision = (*found)->revision},
            absent ? 1 : (*found)->lake_revision + 1));
        if (written) {
            auto decoded = OciBlobCodec::decode(*written);
            if (!decoded) {
                co_return failed<OciBlob>(
                    ZettaControlError{"store_error", "the stored image blob record did not decode"});
            }
            co_return ZettaControlResult<OciBlob>(std::move(*decoded));
        }
        if (written.error().code != "conflict") {
            co_return failed<OciBlob>(written.error());
        }
        last = written.error(); // written meanwhile: reread
    }
    co_return failed<OciBlob>(last);
}

liquid::runtime::task<ZettaControlResult<std::vector<OciBlob>>>
list_oci_blobs(WorkspaceRecords& records, std::string customer_id) {
    using Blobs = std::vector<OciBlob>;
    auto stored = settled(
        co_await records.list(OciBlobCodec::collection, "customer:" + customer_id));
    if (!stored) {
        co_return failed<Blobs>(stored.error());
    }
    Blobs blobs;
    for (const StoredRecord& record : *stored) {
        auto decoded = OciBlobCodec::decode(record);
        if (decoded && decoded->customer_id == customer_id &&
            record.id == records.index_of(OciBlobCodec::collection,
                                          manifest_name(customer_id, decoded->digest))) {
            blobs.push_back(std::move(*decoded));
        }
    }
    std::ranges::sort(blobs, {}, &OciBlob::digest);
    co_return ZettaControlResult<Blobs>(std::move(blobs));
}

liquid::runtime::task<ZettaControlResult<std::vector<OciTag>>>
list_oci_tags(WorkspaceRecords& records, std::string customer_id, std::string repository) {
    using Tags = std::vector<OciTag>;
    auto stored = settled(co_await records.list(OciTagCodec::collection,
                                                oci_tags_lookup(customer_id, repository)));
    if (!stored) {
        co_return failed<Tags>(stored.error());
    }
    Tags tags;
    for (const StoredRecord& record : *stored) {
        auto decoded = OciTagCodec::decode(record);
        if (decoded && decoded->customer_id == customer_id && decoded->repository == repository &&
            record.id == records.index_of(OciTagCodec::collection,
                                          tag_name(customer_id, repository, decoded->tag))) {
            tags.push_back(std::move(*decoded));
        }
    }
    std::ranges::sort(tags, {}, &OciTag::tag);
    co_return ZettaControlResult<Tags>(std::move(tags));
}

} // namespace lake::control

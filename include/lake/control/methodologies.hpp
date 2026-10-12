#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// A workspace's own methodologies from Python code (docs/api/methodologies.md, lake#655): the pure
// parts. A workspace methodology is Python in one of the workspace's repositories, described by a
// manifest (`methodology.json`, interface 1) and run as untrusted code in its own container
// (oramasearch/methodology-runtime, the Orama operator's stage, run and collect pods). The engine
// keeps its registry entry (zetta#1261, ids `W-<8 hex>`); lake checks the code, builds its image,
// dispatches its runs and keeps each run's record in the workspace Zetta.
namespace lake::control {

// What a methodology may use when it runs: CPU in millicores, memory in bytes, GPUs (and their
// class), and how long it may run. The defaults are what a methodology gets when its manifest
// names none; the bounds are the most a run may ask for.
struct MethodologyResources {
    std::int64_t cpu_millis = 1000;
    std::int64_t memory_bytes = std::int64_t{2} << 30;
    int gpus = 0;
    std::string gpu_class;
    int timeout_seconds = 3600;
    friend bool operator==(const MethodologyResources&, const MethodologyResources&) = default;
};
inline constexpr std::int64_t max_methodology_cpu_millis = 16000;
inline constexpr std::int64_t max_methodology_memory_bytes = std::int64_t{64} << 30;
inline constexpr int max_methodology_gpus = 8;
inline constexpr int max_methodology_timeout_seconds = 24 * 3600;

// One parameter a study may search over: its type, default and bounds or choices.
struct MethodologyParameter {
    std::string name;
    std::string kind; // int | float | categorical | bool
    nlohmann::json default_value;
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::vector<nlohmann::json> choices;
    bool log = false;
};

// The manifest, `methodology.json` (interface 1). docs/api/methodologies.md#the-manifest.
struct MethodologyManifest {
    int interface = 1;
    std::string name;
    std::string description;
    std::string entrypoint = "methodology.py";
    // What it needs: the kinds of input it reads, its target (or none) and required features.
    std::vector<std::string> input_kinds;
    bool target_required = false;
    std::string target_kind = "none"; // binary | multiclass | regression | none
    std::vector<std::string> required_features;
    std::vector<MethodologyParameter> parameters;
    // Base metric names; a run reports each as in_sample_<name> and held_out_<name>.
    std::vector<std::string> metrics;
    bool explain = false;
    // The pip requirements file: every requirement pinned with its hashes; may be empty.
    std::string packages = "requirements.lock";
    MethodologyResources resources;
};

// One thing Validate checked: what (`check`, such as `manifest.metrics` or `entrypoint.score`),
// whether it passed, why, and the file it is about.
struct MethodologyFinding {
    std::string check;
    bool ok = true;
    std::string message;
    std::string file;
};
nlohmann::json finding_json(const MethodologyFinding& finding);
bool all_ok(const std::vector<MethodologyFinding>& findings);

inline constexpr std::string_view methodology_manifest_file = "methodology.json";
inline const std::vector<std::string> methodology_input_kinds{
    "tabular", "text", "time_series", "graph", "image", "audio", "document"};
inline const std::vector<std::string> methodology_target_kinds{
    "binary", "multiclass", "regression", "none"};

// The manifest a file holds, with a finding for each thing missing or wrong. `manifest` is set
// whenever the text is a JSON object, even with failed findings, so a page can show what it says.
struct ManifestRead {
    std::optional<MethodologyManifest> manifest;
    std::vector<MethodologyFinding> findings;
};
ManifestRead read_manifest(std::string_view text);
nlohmann::json manifest_json(const MethodologyManifest& manifest);

// "1", "500m", "0.5" CPUs as millicores; "2Gi", "512Mi", "1G", bytes as bytes. Nothing when the
// text is not one.
std::optional<std::int64_t> cpu_millis_of(const nlohmann::json& value);
std::optional<std::int64_t> memory_bytes_of(const nlohmann::json& value);

// The lock file's findings: every requirement pinned `name==version` with at least one
// `--hash=sha256:<64 hex>`, no index or link options, no editable or local paths.
std::vector<MethodologyFinding> check_lock(std::string_view text, const std::string& file);

// The entrypoint's findings, read without running it: `fit`, `predict` and `score` defined at
// module level (and `explain` when the manifest declares it).
std::vector<MethodologyFinding> check_entrypoint(std::string_view source,
                                                 const MethodologyManifest& manifest,
                                                 const std::string& file);

// A path inside a methodology's folder, refusing absolute paths and `..`.
std::optional<std::string> methodology_path(std::string_view folder, std::string_view file);

// --- Drafts and duplicates -----------------------------------------------------------------------

struct MethodologyFile {
    std::string path; // relative to the methodology's folder
    std::string contents;
    friend bool operator==(const MethodologyFile&, const MethodologyFile&) = default;
};

// A first version of a methodology's files: the manifest, the Python and an empty lock file (the
// templates use the standard library only). `template_name` is the template chosen: `binary`,
// `multiclass`, `regression` or `clustering`.
struct MethodologyDraft {
    MethodologyManifest manifest;
    std::vector<MethodologyFile> files;
    std::string template_name;
    std::string reason; // why that template, in a sentence
};

// A draft from a person's description of what the methodology should do: the template follows
// the kind of problem the description names (clusters or segments, an amount, one of several
// categories, otherwise a yes-or-no outcome), or `target_kind` when given.
MethodologyDraft draft_methodology(std::string_view description,
                                   std::string_view name,
                                   std::string_view target_kind = {});

// A draft from one of Orama's catalogue methodologies (the engine's list_methodologies entry): its
// name, description (what it makes, needs and is measured by), metrics and search space, with the
// Python template its target fits. Orama's methodologies run natively in the engine and have no
// Python source; the draft says so.
MethodologyDraft draft_from_catalogue(const nlohmann::json& entry, std::string_view name);

// The name a duplicate takes when the person gives none: "<name> (copy)", within 200 bytes.
std::string methodology_copy_name(std::string_view name);

// --- The engine's registry entry -----------------------------------------------------------------

// Whether `id` names a workspace methodology (`W-<8 lowercase hex>`); Orama's are `M-<n>`.
bool workspace_methodology_id(std::string_view id);

// What register_methodology / update_methodology carry for a manifest: the description fields,
// what it needs, its parameters, metrics and resources. The caller adds `code` and the rest.
nlohmann::json registry_fields(const MethodologyManifest& manifest);

// A workspace entry's validation state as the engine keeps it, and whether the entry is usable
// for its current code revision.
bool validated_for_revision(const nlohmann::json& entry);

// --- The image
// ------------------------------------------------------------------------------------

// The Dockerfile Lake's build path uses for a methodology's image: the methodology-runtime image
// with the folder's files at /methodology and the lock file installed with hashes required.
std::string methodology_dockerfile(std::string_view packages);

// --- Runs
// -----------------------------------------------------------------------------------------

// The engine keeps an artifact as a model's weights and takes at most 24 MiB of it in one call
// (zetta#1261: its request bodies are bounded at 33 MiB, and base64 grows a payload by a third).
inline constexpr std::size_t max_methodology_artifact_bytes = std::size_t{24} << 20;
// A larger artifact (lake#690) is sent in chunks of at most methodology_artifact_chunk_bytes, staged
// in the workspace's engine, up to max_methodology_staged_artifact_bytes; a served model's artifact
// is read back in chunks of the same size.
inline constexpr std::size_t methodology_artifact_chunk_bytes = std::size_t{8} << 20;
inline constexpr std::size_t max_methodology_staged_artifact_bytes = std::size_t{1} << 30;
// What a served model takes in one predict call (lake#690), as the runtime's serve mode does.
inline constexpr std::size_t max_methodology_predict_rows = 10'000;
inline constexpr std::size_t max_methodology_log_bytes = 64 * 1024;
inline constexpr std::int64_t max_methodology_train_rows = 1'000'000;
inline constexpr std::int64_t methodology_validation_rows = 200;
inline constexpr int max_methodology_input_page = 5000;
// How long a run may wait for the Orama operator to report on it (its image build included)
// before Lake withdraws it.
inline constexpr std::int64_t methodology_run_start_timeout_seconds = 1800;
inline constexpr std::string_view methodology_run_start_timeout_reason =
    "Timed out: the Orama operator did not start this run, or build its image, within 30 "
    "minutes.";

// The deterministic sample Validate runs on when no dataset is given: `rows` rows with three
// numeric features, a text column and a target of the manifest's kind.
std::vector<nlohmann::json> validation_sample(const MethodologyManifest& manifest,
                                              std::int64_t rows = methodology_validation_rows);

// What a person is told about a run from its dispatch row and its record: the phase, and a
// message in Lake's words.
struct MethodologyRunProgress {
    std::string phase, message;
};

// The metrics a result may carry: numbers only, named in_sample_<m> or held_out_<m>; anything
// else is dropped and named in `dropped`.
nlohmann::json numeric_metrics(const nlohmann::json& metrics, std::vector<std::string>* dropped);

} // namespace lake::control

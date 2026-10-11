// A workspace's own methodologies from Python code (docs/api/methodologies.md, lake#655): the pure
// parts. What a manifest must say, the lock file and entrypoint checks Validate makes before
// anything runs, the drafts and the copies of Orama's, the image's Dockerfile, the validation
// sample, and the metrics a result may carry. The capabilities against a workspace are in
// test_workspace_records.cpp (MethodologiesAreTheWorkspacesOwnAndOramasStayReadOnly); the
// sandbox's own refusals (network, credentials) are oramasearch/methodology-runtime's and
// orama-operator's tests.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "lake/control/methodologies.hpp"
#include "temporary_directory.hpp"

namespace {

using json = nlohmann::json;
namespace control = lake::control;

json good_manifest() {
    return {
        {"interface", 1},
        {"name", "Late payers"},
        {"description", "Who pays late."},
        {"entrypoint", "methodology.py"},
        {"inputs",
         {{"kinds", {"tabular"}},
          {"target", {{"required", true}, {"kind", "binary"}}},
          {"required_features", json::array()}}},
        {"parameters",
         json::array({{{"name", "learning_rate"},
                       {"kind", "float"},
                       {"default", 0.1},
                       {"minimum", 0.001},
                       {"maximum", 1},
                       {"log", true}},
                      {{"name", "depth"}, {"kind", "int"}, {"default", 6}, {"minimum", 1}},
                      {{"name", "loss"},
                       {"kind", "categorical"},
                       {"default", "log_loss"},
                       {"choices", {"log_loss", "exponential"}}},
                      {{"name", "balanced"}, {"kind", "bool"}, {"default", false}}})},
        {"metrics", {"auc", "log_loss"}},
        {"explain", false},
        {"packages", "requirements.lock"},
        {"resources", {{"cpu", "500m"}, {"memory", "2Gi"}, {"gpu", 0}, {"timeout_seconds", 600}}}};
}

std::vector<std::string> failed_checks(const std::vector<control::MethodologyFinding>& findings) {
    std::vector<std::string> out;
    for (const auto& finding : findings) {
        if (!finding.ok) {
            out.push_back(finding.check);
        }
    }
    return out;
}

bool contains(const std::vector<std::string>& names, const std::string& name) {
    return std::ranges::find(names, name) != names.end();
}

TEST(Methodologies, AGoodManifestReadsWhole) {
    const control::ManifestRead read = control::read_manifest(good_manifest().dump());
    ASSERT_TRUE(read.manifest);
    EXPECT_TRUE(control::all_ok(read.findings));
    const control::MethodologyManifest& manifest = *read.manifest;
    EXPECT_EQ(manifest.name, "Late payers");
    EXPECT_EQ(manifest.input_kinds, std::vector<std::string>{"tabular"});
    EXPECT_TRUE(manifest.target_required);
    EXPECT_EQ(manifest.target_kind, "binary");
    ASSERT_EQ(manifest.parameters.size(), 4U);
    EXPECT_TRUE(manifest.parameters[0].log);
    EXPECT_EQ(manifest.metrics, (std::vector<std::string>{"auc", "log_loss"}));
    EXPECT_EQ(manifest.resources.cpu_millis, 500);
    EXPECT_EQ(manifest.resources.memory_bytes, std::int64_t{2} << 30);
    EXPECT_EQ(manifest.resources.timeout_seconds, 600);
    // The manifest written back reads the same.
    const control::ManifestRead again =
        control::read_manifest(control::manifest_json(manifest).dump());
    ASSERT_TRUE(again.manifest);
    EXPECT_TRUE(control::all_ok(again.findings));
    EXPECT_EQ(control::manifest_json(*again.manifest), control::manifest_json(manifest));
}

TEST(Methodologies, ValidateNamesEveryMissingPartOfAManifest) {
    EXPECT_EQ(failed_checks(control::read_manifest("not json").findings),
              std::vector<std::string>{"manifest"});
    json manifest = good_manifest();
    manifest.erase("interface");
    manifest["name"] = "";
    manifest["inputs"]["kinds"] = {"spreadsheet"};
    manifest["inputs"]["target"] = {{"required", false}, {"kind", "binary"}};
    manifest["metrics"] = json::array();
    manifest["entrypoint"] = "../escape.py";
    manifest["packages"] = "/etc/passwd";
    manifest["resources"] = {{"cpu", "64"}, {"memory", "1Ti"}, {"gpu", 9}, {"timeout_seconds", 5}};
    const auto failed = failed_checks(control::read_manifest(manifest.dump()).findings);
    for (const char* check : {"manifest.interface",
                              "manifest.name",
                              "manifest.inputs.kinds",
                              "manifest.inputs.target.required",
                              "manifest.metrics",
                              "manifest.entrypoint",
                              "manifest.packages",
                              "manifest.resources.cpu",
                              "manifest.resources.memory",
                              "manifest.resources.gpu",
                              "manifest.resources.timeout_seconds"}) {
        EXPECT_TRUE(contains(failed, check)) << check;
    }
    EXPECT_FALSE(contains(failed, "manifest")) << "the file itself reads";
}

TEST(Methodologies, ParametersKeepTheirKindDefaultAndBounds) {
    const auto check = [](json parameter) {
        json manifest = good_manifest();
        manifest["parameters"] = json::array({std::move(parameter)});
        return failed_checks(control::read_manifest(manifest.dump()).findings);
    };
    EXPECT_TRUE(check({{"name", "rate"}, {"kind", "float"}, {"default", 0.5}}).empty());
    EXPECT_FALSE(check({{"name", "Rate"}, {"kind", "float"}, {"default", 0.5}}).empty());
    EXPECT_FALSE(check({{"name", "rate"}, {"kind", "number"}, {"default", 0.5}}).empty());
    EXPECT_FALSE(check({{"name", "rate"}, {"kind", "float"}}).empty()) << "a default is required";
    EXPECT_FALSE(check({{"name", "n"}, {"kind", "int"}, {"default", 1.5}}).empty());
    EXPECT_FALSE(check({{"name", "n"}, {"kind", "int"}, {"default", 0}, {"minimum", 1}}).empty());
    EXPECT_FALSE(check({{"name", "c"}, {"kind", "categorical"}, {"default", "a"}}).empty())
        << "a categorical one lists its choices";
    EXPECT_FALSE(
        check({{"name", "c"}, {"kind", "categorical"}, {"default", "z"}, {"choices", {"a"}}})
            .empty());
    EXPECT_FALSE(check({{"name", "r"}, {"kind", "float"}, {"default", 1}, {"log", true}}).empty())
        << "a log scale needs a minimum above zero";
    json twice = good_manifest();
    twice["parameters"] = json::array({{{"name", "a"}, {"kind", "bool"}, {"default", true}},
                                       {{"name", "a"}, {"kind", "bool"}, {"default", false}}});
    EXPECT_EQ(failed_checks(control::read_manifest(twice.dump()).findings),
              std::vector<std::string>{"manifest.parameters[1]"});
}

TEST(Methodologies, MetricsAreBaseNamesLakePrefixes) {
    json manifest = good_manifest();
    manifest["metrics"] = {"held_out_auc"};
    EXPECT_TRUE(contains(failed_checks(control::read_manifest(manifest.dump()).findings),
                         "manifest.metrics"));
    manifest["metrics"] = {"auc", "auc"};
    EXPECT_TRUE(contains(failed_checks(control::read_manifest(manifest.dump()).findings),
                         "manifest.metrics"));
}

TEST(Methodologies, TheLockFileIsPinnedAndHashed) {
    const std::string hash(64, 'a');
    EXPECT_TRUE(control::all_ok(control::check_lock("", "requirements.lock")));
    EXPECT_TRUE(control::all_ok(control::check_lock("# nothing\n\n", "requirements.lock")));
    EXPECT_TRUE(control::all_ok(
        control::check_lock("numpy==2.1.3 \\\n    --hash=sha256:" + hash +
                                " \\\n    --hash=sha256:" + std::string(64, 'b') + "\n",
                            "requirements.lock")));
    for (const std::string line : {"numpy\n",
                                   "numpy>=2\n",
                                   "numpy==2.1.3\n",
                                   "numpy==2.1.3 --hash=md5:abc\n",
                                   "numpy==2.1.3 --hash=sha256:ABC\n",
                                   "--index-url https://example.invalid/simple\n",
                                   "-e ./local\n",
                                   "pkg @ https://example.invalid/pkg.whl\n",
                                   "./wheels/pkg.whl\n"}) {
        EXPECT_FALSE(control::all_ok(control::check_lock(line, "requirements.lock"))) << line;
    }
    const auto findings = control::check_lock("numpy==2.1.3\n", "requirements.lock");
    ASSERT_EQ(findings.size(), 1U);
    EXPECT_NE(findings[0].message.find("line 1"), std::string::npos);
    EXPECT_NE(findings[0].message.find("--hash=sha256"), std::string::npos);
}

TEST(Methodologies, TheEntrypointDefinesFitPredictAndScore) {
    control::MethodologyManifest manifest;
    const std::string good = "import math\n\ndef fit(data, params):\n    return 1\n\n"
                             "def predict(artifact, data):\n    return []\n\n"
                             "def score(artifact, data):\n    return {}\n";
    EXPECT_TRUE(control::all_ok(control::check_entrypoint(good, manifest, "methodology.py")));
    const std::string nested = "def fit(data, params):\n    def score(a, d):\n        pass\n"
                               "def predict(artifact, data):\n    return []\n";
    EXPECT_EQ(failed_checks(control::check_entrypoint(nested, manifest, "methodology.py")),
              std::vector<std::string>{"entrypoint.score"})
        << "only module-level functions count";
    manifest.explain = true;
    EXPECT_EQ(failed_checks(control::check_entrypoint(good, manifest, "methodology.py")),
              std::vector<std::string>{"entrypoint.explain"});
}

TEST(Methodologies, PathsStayInsideTheFolder) {
    EXPECT_EQ(control::methodology_path("", "methodology.py"), "methodology.py");
    EXPECT_EQ(control::methodology_path("models/churn/", "lib/a.py"), "models/churn/lib/a.py");
    EXPECT_EQ(control::methodology_path(".", "a.py"), "a.py");
    for (const char* bad : {"", "/etc/passwd", "../a.py", "a/../../b", "a//b", "a\\b"}) {
        EXPECT_FALSE(control::methodology_path("x", bad)) << bad;
    }
}

TEST(Methodologies, ResourcesReadAsKubernetesWritesThem) {
    EXPECT_EQ(control::cpu_millis_of("1"), 1000);
    EXPECT_EQ(control::cpu_millis_of("500m"), 500);
    EXPECT_EQ(control::cpu_millis_of(0.25), 250);
    EXPECT_FALSE(control::cpu_millis_of("one"));
    EXPECT_FALSE(control::cpu_millis_of("-1"));
    EXPECT_EQ(control::memory_bytes_of("2Gi"), std::int64_t{2} << 30);
    EXPECT_EQ(control::memory_bytes_of("512Mi"), std::int64_t{512} << 20);
    EXPECT_EQ(control::memory_bytes_of("1G"), 1000000000);
    EXPECT_FALSE(control::memory_bytes_of("lots"));
}

TEST(Methodologies, ADraftFollowsTheProblemTheDescriptionNames) {
    EXPECT_EQ(
        control::draft_methodology("Which customers will churn next month?", "").template_name,
        "binary");
    EXPECT_EQ(control::draft_methodology("Segment our customers into groups", "").template_name,
              "clustering");
    EXPECT_EQ(
        control::draft_methodology("Forecast how much each store will sell", "").template_name,
        "regression");
    EXPECT_EQ(control::draft_methodology("Route each ticket to one of several categories", "")
                  .template_name,
              "multiclass");
    EXPECT_EQ(control::draft_methodology("anything", "", "regression").template_name, "regression");
    for (const char* description : {"Which customers will churn?",
                                    "Segment customers",
                                    "Forecast revenue",
                                    "Classify into categories"}) {
        const control::MethodologyDraft draft = control::draft_methodology(description, "");
        ASSERT_EQ(draft.files.size(), 3U);
        std::map<std::string, std::string> files;
        for (const auto& file : draft.files) {
            files[file.path] = file.contents;
        }
        const control::ManifestRead read = control::read_manifest(files.at("methodology.json"));
        ASSERT_TRUE(read.manifest) << description;
        EXPECT_TRUE(control::all_ok(read.findings)) << description;
        EXPECT_TRUE(control::all_ok(control::check_entrypoint(
            files.at("methodology.py"), *read.manifest, "methodology.py")))
            << description;
        EXPECT_TRUE(control::all_ok(
            control::check_lock(files.at("requirements.lock"), "requirements.lock")))
            << description;
        EXPECT_FALSE(draft.manifest.name.empty());
    }
    const auto named = control::draft_methodology("predict churn", "Churn v2");
    EXPECT_EQ(named.manifest.name, "Churn v2");
    EXPECT_NE(named.files[1].contents.find("Churn v2"), std::string::npos);
    // A description cannot end the docstring early.
    const auto quoted = control::draft_methodology("say \"\"\" then \\ code", "");
    EXPECT_EQ(quoted.files[1].contents.find("\"\"\" then"), std::string::npos);
}

TEST(Methodologies, ACopyOfOramasCarriesItsDescriptionAndSearchSpace) {
    const json entry{{"id", "M-1"},
                     {"name", "Tabular supervised learning"},
                     {"family", "The tabular decision core"},
                     {"model_kinds", {"gbdt", "classifier"}},
                     {"inputs",
                      json::array({{{"role", "entity"}, {"required", true}, {"description", ""}},
                                   {{"role", "label"}, {"required", true}, {"description", ""}}})},
                     {"outputs", {"model", "scores"}},
                     {"metrics", {"held_out_auc"}},
                     {"study_search_space",
                      {{"parameters",
                        json::array({{{"name", "max_depth"},
                                      {"kind", "int"},
                                      {"minimum", 2},
                                      {"maximum", 12},
                                      {"default", 6},
                                      {"log", false},
                                      {"step", 1},
                                      {"choices", json::array()}}})}}}};
    const control::MethodologyDraft copy = control::draft_from_catalogue(entry, "");
    EXPECT_EQ(copy.manifest.name, "Tabular supervised learning");
    EXPECT_EQ(copy.template_name, "binary") << "a required label is a target";
    EXPECT_NE(copy.manifest.description.find("Duplicated from Orama's M-1"), std::string::npos);
    EXPECT_NE(copy.manifest.description.find("no Python source"), std::string::npos);
    EXPECT_NE(copy.manifest.description.find("held_out_auc"), std::string::npos);
    EXPECT_TRUE(std::ranges::any_of(copy.manifest.parameters, [](const auto& p) {
        return p.name == "max_depth";
    })) << "its search space becomes parameters";
    EXPECT_TRUE(control::all_ok(control::read_manifest(copy.files[0].contents).findings));
    const json segments{{"id", "M-3"},
                        {"name", "Segmentation / clustering"},
                        {"inputs",
                         json::array({{{"role", "entity"}, {"required", true}},
                                      {{"role", "label"}, {"required", false}}})}};
    EXPECT_EQ(control::draft_from_catalogue(segments, "Ours").template_name, "clustering");
    EXPECT_EQ(control::draft_from_catalogue(segments, "Ours").manifest.name, "Ours");
}

TEST(Methodologies, IdsCopiesAndValidation) {
    EXPECT_TRUE(control::workspace_methodology_id("W-0a1b2c3d"));
    for (const char* id : {"M-1", "W-0A1B2C3D", "W-0a1b2c3", "W-0a1b2c3dd", "w-0a1b2c3d", ""}) {
        EXPECT_FALSE(control::workspace_methodology_id(id)) << id;
    }
    EXPECT_EQ(control::methodology_copy_name("Churn"), "Churn (copy)");
    EXPECT_LE(control::methodology_copy_name(std::string(300, 'a')).size(), 200U);
    const json passed{{"code", {{"revision", "c2"}}},
                      {"validation", {{"status", "passed"}, {"revision", "c2"}}}};
    EXPECT_TRUE(control::validated_for_revision(passed));
    json stale = passed;
    stale["code"]["revision"] = "c3";
    EXPECT_FALSE(control::validated_for_revision(stale)) << "a new revision is validated again";
    json failed = passed;
    failed["validation"]["status"] = "failed";
    EXPECT_FALSE(control::validated_for_revision(failed));
}

TEST(Methodologies, TheImageIsTheRuntimeWithTheLockInstalled) {
    const std::string dockerfile = control::methodology_dockerfile("requirements.lock");
    EXPECT_TRUE(dockerfile.starts_with("inline:ARG METHODOLOGY_RUNTIME_IMAGE\n"));
    EXPECT_NE(dockerfile.find("FROM ${METHODOLOGY_RUNTIME_IMAGE}"), std::string::npos);
    EXPECT_NE(dockerfile.find("install-locked /methodology \"requirements.lock\""),
              std::string::npos);
    EXPECT_TRUE(dockerfile.ends_with("USER 10001:10001\n")) << "the code never runs as root";
    EXPECT_EQ(control::methodology_dockerfile("").find("install-locked"), std::string::npos);
}

TEST(Methodologies, TheValidationSampleIsTheSameEveryTime) {
    control::MethodologyManifest manifest;
    manifest.target_kind = "binary";
    manifest.target_required = true;
    manifest.required_features = {"tenure"};
    const auto first = control::validation_sample(manifest);
    EXPECT_EQ(first.size(), 200U);
    EXPECT_EQ(first, control::validation_sample(manifest));
    EXPECT_TRUE(first[0].contains("tenure"));
    EXPECT_TRUE(first[0].at("target").is_number_integer());
    int positives = 0;
    for (const json& row : first) {
        positives += row.at("target").get<int>();
    }
    EXPECT_GT(positives, 40);
    EXPECT_LT(positives, 160);
    manifest.target_kind = "none";
    manifest.target_required = false;
    EXPECT_FALSE(control::validation_sample(manifest, 3)[0].contains("target"));
}

TEST(Methodologies, AResultsMetricsAreNumbersNamedInSampleOrHeldOut) {
    std::vector<std::string> dropped;
    const json kept = control::numeric_metrics({{"in_sample_auc", 0.9},
                                                {"held_out_auc", 0.8},
                                                {"auc", 0.8},
                                                {"held_out_note", "good"},
                                                {"held_out_nan", nullptr}},
                                               &dropped);
    EXPECT_EQ(kept, (json{{"in_sample_auc", 0.9}, {"held_out_auc", 0.8}}));
    EXPECT_EQ(dropped.size(), 3U);
}

// Each template, run through the interface as the runtime's harness calls it: fit on the sample,
// predict one value per row, score every manifest metric as a number. Needs python3 on the path;
// the image's own tests (oramasearch/methodology-runtime) run a methodology end to end.
TEST(Methodologies, EveryTemplateRunsThroughTheInterface) {
    if (std::system("python3 -c 'import sys' > /dev/null 2>&1") != 0) {
        GTEST_SKIP() << "python3 is not on the path";
    }
    const std::filesystem::path root =
        lake::test::fixture_path("methodology-templates-" + std::to_string(std::rand()));
    for (const char* description : {"Which customers will churn?",
                                    "Segment customers",
                                    "Forecast revenue",
                                    "Classify into categories"}) {
        const control::MethodologyDraft draft = control::draft_methodology(description, "");
        const std::filesystem::path folder = root / draft.template_name;
        std::filesystem::create_directories(folder);
        for (const auto& file : draft.files) {
            std::ofstream(folder / file.path) << file.contents;
        }
        std::ofstream sample(folder / "sample.jsonl");
        for (const json& row : control::validation_sample(draft.manifest, 120)) {
            sample << row.dump() << "\n";
        }
        sample.close();
        std::ofstream(folder / "check.py")
            << "import json, math, pickle, sys\n"
               "sys.path.insert(0, sys.argv[1])\n"
               "import methodology\n"
               "manifest = json.load(open(sys.argv[1] + '/methodology.json'))\n"
               "rows = [json.loads(line) for line in open(sys.argv[1] + '/sample.jsonl')]\n"
               "target = 'target' if manifest['inputs']['target']['required'] else None\n"
               "features = [c for c in rows[0] if c != 'target']\n"
               "class Data:\n"
               "    def __init__(self, rows):\n"
               "        self.rows, self.columns, self.target, self.features = rows, list(rows[0]), "
               "target, features\n"
               "    def X(self):\n"
               "        return [[r.get(f) for f in self.features] for r in self.rows]\n"
               "    def y(self):\n"
               "        return None if self.target is None else [r.get(self.target) for r in "
               "self.rows]\n"
               "params = {p['name']: p['default'] for p in manifest['parameters']}\n"
               "train, held = Data(rows[:90]), Data(rows[90:])\n"
               "artifact = methodology.fit(train, params)\n"
               "pickle.loads(pickle.dumps(artifact))\n"
               "assert len(methodology.predict(artifact, held)) == len(held.rows)\n"
               "scores = methodology.score(artifact, held)\n"
               "for name in manifest['metrics']:\n"
               "    assert isinstance(scores[name], (int, float)) and math.isfinite(scores[name]), "
               "name\n"
               "if manifest['explain']:\n"
               "    assert len(methodology.explain(artifact, held)) == len(held.rows)\n"
               "print('ok')\n";
        const std::string command = "python3 '" + (folder / "check.py").string() + "' '" +
                                    folder.string() + "' > '" + (folder / "out.txt").string() +
                                    "' 2>&1";
        const int status = std::system(command.c_str());
        std::ifstream out(folder / "out.txt");
        const std::string printed((std::istreambuf_iterator<char>(out)),
                                  std::istreambuf_iterator<char>());
        EXPECT_EQ(status, 0) << draft.template_name << ": " << printed;
    }
}

} // namespace

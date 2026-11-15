// SPDX-License-Identifier: MIT

#include "lake/control/methodologies.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <sstream>
#include <utility>

#include "lake/control/store.hpp"
#include "methodology_templates.hpp"

namespace lake::control {
namespace {

using json = nlohmann::json;

std::string folded(std::string_view text) {
    std::string out(text);
    std::ranges::transform(
        out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

// Cuts `text` to at most `bytes`, never inside a UTF-8 sequence.
std::string bounded(std::string text, std::size_t bytes) {
    if (text.size() <= bytes) {
        return text;
    }
    std::size_t end = bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    text.resize(end);
    return text;
}

bool identifier(std::string_view name) {
    if (name.empty() || name.size() > 64 ||
        std::isalpha(static_cast<unsigned char>(name[0])) == 0 ||
        std::isupper(static_cast<unsigned char>(name[0])) != 0) {
        return false;
    }
    return std::ranges::all_of(name, [](unsigned char c) {
        return (std::isalnum(c) != 0 && std::isupper(c) == 0) || c == '_';
    });
}

bool listed(const std::vector<std::string>& names, std::string_view name) {
    return std::ranges::find(names, name) != names.end();
}

MethodologyFinding failed(std::string check, std::string message, std::string file) {
    return {std::move(check), false, std::move(message), std::move(file)};
}

MethodologyFinding passed(std::string check, std::string message, std::string file) {
    return {std::move(check), true, std::move(message), std::move(file)};
}

const std::string manifest_file(methodology_manifest_file);

std::optional<double> number_of(const json& value) {
    if (value.is_number()) {
        return value.get<double>();
    }
    return std::nullopt;
}

// A parameter's default must have its kind and keep to its bounds or choices.
std::optional<std::string> default_problem(const MethodologyParameter& parameter) {
    const json& value = parameter.default_value;
    if (parameter.kind == "int") {
        if (!value.is_number_integer()) {
            return "its default must be a whole number";
        }
    } else if (parameter.kind == "float") {
        if (!value.is_number()) {
            return "its default must be a number";
        }
    } else if (parameter.kind == "bool") {
        if (!value.is_boolean()) {
            return "its default must be true or false";
        }
        return std::nullopt;
    } else if (parameter.kind == "categorical") {
        if (std::ranges::find(parameter.choices, value) == parameter.choices.end()) {
            return "its default must be one of its choices";
        }
        return std::nullopt;
    }
    const double number = value.get<double>();
    if (parameter.minimum && number < *parameter.minimum) {
        return "its default is below its minimum";
    }
    if (parameter.maximum && number > *parameter.maximum) {
        return "its default is above its maximum";
    }
    return std::nullopt;
}

void read_parameters(const json& value,
                     MethodologyManifest& manifest,
                     std::vector<MethodologyFinding>& findings) {
    if (value.is_null()) {
        return;
    }
    if (!value.is_array()) {
        findings.push_back(failed("manifest.parameters",
                                  "parameters must be a list of {name, kind, default, …}",
                                  manifest_file));
        return;
    }
    if (value.size() > 64) {
        findings.push_back(failed("manifest.parameters", "at most 64 parameters", manifest_file));
        return;
    }
    std::set<std::string> seen;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const json& item = value[index];
        const std::string check = "manifest.parameters[" + std::to_string(index) + "]";
        if (!item.is_object()) {
            findings.push_back(failed(check, "a parameter is an object", manifest_file));
            continue;
        }
        MethodologyParameter parameter;
        parameter.name = item.value("name", std::string());
        parameter.kind = item.value("kind", std::string());
        parameter.default_value = item.value("default", json());
        parameter.minimum = number_of(item.value("minimum", json()));
        parameter.maximum = number_of(item.value("maximum", json()));
        if (const json choices = item.value("choices", json()); choices.is_array()) {
            parameter.choices.assign(choices.begin(), choices.end());
        }
        parameter.log = item.value("log", false);
        std::string problem;
        if (!identifier(parameter.name)) {
            problem = "a parameter's name is lowercase letters, digits and _, starting with a "
                      "letter";
        } else if (!seen.insert(parameter.name).second) {
            problem = parameter.name + " is listed twice";
        } else if (parameter.kind != "int" && parameter.kind != "float" &&
                   parameter.kind != "categorical" && parameter.kind != "bool") {
            problem = parameter.name + ": kind is int, float, categorical or bool";
        } else if (!item.contains("default")) {
            problem = parameter.name + ": a parameter needs a default";
        } else if (parameter.kind == "categorical" && parameter.choices.empty()) {
            problem = parameter.name + ": a categorical parameter lists its choices";
        } else if (parameter.minimum && parameter.maximum &&
                   *parameter.minimum > *parameter.maximum) {
            problem = parameter.name + ": minimum is above maximum";
        } else if (parameter.log && (parameter.kind == "categorical" || parameter.kind == "bool" ||
                                     !parameter.minimum || *parameter.minimum <= 0)) {
            problem = parameter.name + ": a log scale needs a numeric kind and a minimum above 0";
        } else if (auto bad = default_problem(parameter)) {
            problem = parameter.name + ": " + *bad;
        }
        if (!problem.empty()) {
            findings.push_back(failed(check, problem, manifest_file));
            continue;
        }
        manifest.parameters.push_back(std::move(parameter));
    }
}

void read_resources(const json& value,
                    MethodologyManifest& manifest,
                    std::vector<MethodologyFinding>& findings) {
    if (value.is_null()) {
        return;
    }
    if (!value.is_object()) {
        findings.push_back(failed("manifest.resources",
                                  "resources is {cpu, memory, gpu, timeout_seconds}",
                                  manifest_file));
        return;
    }
    MethodologyResources& resources = manifest.resources;
    if (value.contains("cpu")) {
        const auto millis = cpu_millis_of(value.at("cpu"));
        if (!millis || *millis <= 0 || *millis > max_methodology_cpu_millis) {
            findings.push_back(failed("manifest.resources.cpu",
                                      "cpu is between 0.001 and 16 CPUs, such as \"1\" or \"500m\"",
                                      manifest_file));
        } else {
            resources.cpu_millis = *millis;
        }
    }
    if (value.contains("memory")) {
        const auto bytes = memory_bytes_of(value.at("memory"));
        if (!bytes || *bytes < (std::int64_t{64} << 20) || *bytes > max_methodology_memory_bytes) {
            findings.push_back(failed("manifest.resources.memory",
                                      "memory is between 64Mi and 64Gi, such as \"2Gi\"",
                                      manifest_file));
        } else {
            resources.memory_bytes = *bytes;
        }
    }
    if (value.contains("gpu")) {
        const json& gpu = value.at("gpu");
        if (!gpu.is_number_integer() || gpu.get<int>() < 0 ||
            gpu.get<int>() > max_methodology_gpus) {
            findings.push_back(failed(
                "manifest.resources.gpu", "gpu is a whole number from 0 to 8", manifest_file));
        } else {
            resources.gpus = gpu.get<int>();
        }
    }
    if (value.contains("gpu_class")) {
        const std::string gpu_class = value.value("gpu_class", std::string());
        const bool valid =
            gpu_class.size() <= 32 &&
            std::ranges::all_of(gpu_class,
                                [](unsigned char c) {
                                    return std::islower(c) != 0 || std::isdigit(c) != 0 || c == '-';
                                }) &&
            (gpu_class.empty() || gpu_class.front() != '-');
        if (!valid) {
            findings.push_back(failed("manifest.resources.gpu_class",
                                      "gpu_class is a GPU class such as l4 or a100",
                                      manifest_file));
        } else {
            resources.gpu_class = gpu_class;
        }
    }
    if (value.contains("timeout_seconds")) {
        const json& timeout = value.at("timeout_seconds");
        if (!timeout.is_number_integer() || timeout.get<int>() < 60 ||
            timeout.get<int>() > max_methodology_timeout_seconds) {
            findings.push_back(failed("manifest.resources.timeout_seconds",
                                      "timeout_seconds is between 60 and 86400",
                                      manifest_file));
        } else {
            resources.timeout_seconds = timeout.get<int>();
        }
    }
}

std::string replaced(std::string text, std::string_view from, std::string_view to) {
    for (std::size_t at = text.find(from); at != std::string::npos;
         at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

// A docstring line: no triple quotes or backslashes can end it early.
std::string docstring_text(std::string_view text) {
    std::string out = replaced(std::string(text), "\\", "/");
    out = replaced(std::move(out), "\"\"\"", "'''");
    return out;
}

json parameter_json(const MethodologyParameter& parameter) {
    json out{
        {"name", parameter.name}, {"kind", parameter.kind}, {"default", parameter.default_value}};
    if (parameter.minimum) {
        out["minimum"] = *parameter.minimum;
    }
    if (parameter.maximum) {
        out["maximum"] = *parameter.maximum;
    }
    if (!parameter.choices.empty()) {
        out["choices"] = parameter.choices;
    }
    if (parameter.log) {
        out["log"] = true;
    }
    return out;
}

MethodologyParameter numeric(std::string name,
                             std::string kind,
                             json value,
                             double minimum,
                             double maximum,
                             bool log = false) {
    MethodologyParameter parameter;
    parameter.name = std::move(name);
    parameter.kind = std::move(kind);
    parameter.default_value = std::move(value);
    parameter.minimum = minimum;
    parameter.maximum = maximum;
    parameter.log = log;
    return parameter;
}

struct Template {
    std::string_view name;
    std::string_view target_kind;
    std::string_view body;
    std::vector<std::string> metrics;
    bool explain = false;
    std::string_view summary;
};

const Template& template_for(std::string_view name) {
    static const std::vector<Template> templates{
        {"binary",
         "binary",
         methodology_templates::binary,
         {"auc", "log_loss", "accuracy"},
         true,
         "logistic regression fitted by gradient descent on the numeric features, for a yes-or-no "
         "outcome"},
        {"multiclass",
         "multiclass",
         methodology_templates::multiclass,
         {"accuracy", "macro_f1"},
         false,
         "multinomial logistic regression, for one of several categories"},
        {"regression",
         "regression",
         methodology_templates::regression,
         {"rmse", "mae", "r2"},
         true,
         "linear regression fitted by gradient descent, for an amount"},
        {"clustering",
         "none",
         methodology_templates::clustering,
         {"inertia", "largest_cluster_share", "clusters"},
         false,
         "k-means on the numeric features, for groups with no target"},
    };
    for (const Template& entry : templates) {
        if (entry.name == name) {
            return entry;
        }
    }
    return templates.front();
}

std::string template_for_target(std::string_view target_kind) {
    if (target_kind == "multiclass") {
        return "multiclass";
    }
    if (target_kind == "regression") {
        return "regression";
    }
    if (target_kind == "none") {
        return "clustering";
    }
    return "binary";
}

bool mentions(std::string_view text, std::initializer_list<std::string_view> words) {
    return std::ranges::any_of(
        words, [&](std::string_view word) { return text.find(word) != std::string_view::npos; });
}

MethodologyDraft
draft_from(const Template& chosen, std::string name, std::string description, std::string reason) {
    MethodologyDraft draft;
    draft.template_name = std::string(chosen.name);
    draft.reason = std::move(reason);
    MethodologyManifest& manifest = draft.manifest;
    manifest.name = bounded(std::move(name), 200);
    manifest.description = bounded(std::move(description), 4000);
    manifest.input_kinds = {"tabular"};
    manifest.target_kind = std::string(chosen.target_kind);
    manifest.target_required = manifest.target_kind != "none";
    manifest.metrics = chosen.metrics;
    manifest.explain = chosen.explain;
    if (chosen.name == "clustering") {
        manifest.parameters = {numeric("clusters", "int", 4, 2, 50),
                               numeric("iterations", "int", 50, 1, 500),
                               numeric("seed", "int", 42, 0, 1000000)};
    } else {
        manifest.parameters = {numeric("learning_rate", "float", 0.1, 0.001, 1.0, true),
                               numeric("epochs", "int", 200, 1, 5000),
                               numeric("l2", "float", 0.0, 0.0, 1.0)};
    }
    const std::string docstring =
        "\"\"\"" + docstring_text(manifest.name) + "\n\n" +
        (manifest.description.empty() ? std::string()
                                      : docstring_text(manifest.description) + "\n\n") +
        "A first version from Lake's " + std::string(chosen.name) +
        " template: " + std::string(chosen.summary) +
        ". Standard library only.\n"
        "Lake runs it as untrusted code: fit(data, params) returns the artifact, predict and "
        "score\nuse it. data.X() is the feature values row by row, data.y() the target's. "
        "Change anything,\nthen Validate.\n\"\"\"\n";
    std::string source =
        docstring + std::string(methodology_templates::helpers) + "\n\n" + std::string(chosen.body);
    draft.files = {
        {std::string(methodology_manifest_file), manifest_json(manifest).dump(2) + "\n"},
        {manifest.entrypoint, std::move(source)},
        {manifest.packages,
         "# Pinned requirements, each with --hash=sha256:… (pip install --require-hashes)."
         "\n# This methodology uses the standard library only.\n"}};
    return draft;
}

} // namespace

json finding_json(const MethodologyFinding& finding) {
    json out{{"check", finding.check}, {"ok", finding.ok}, {"message", finding.message}};
    if (!finding.file.empty()) {
        out["file"] = finding.file;
    }
    return out;
}

bool all_ok(const std::vector<MethodologyFinding>& findings) {
    return std::ranges::all_of(findings, [](const MethodologyFinding& f) { return f.ok; });
}

std::optional<std::int64_t> cpu_millis_of(const json& value) {
    if (value.is_number()) {
        const double cpus = value.get<double>();
        if (!std::isfinite(cpus) || cpus <= 0) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(std::llround(cpus * 1000));
    }
    if (!value.is_string()) {
        return std::nullopt;
    }
    std::string text = trimmed(value.get<std::string>());
    bool milli = false;
    if (text.ends_with('m')) {
        milli = true;
        text.pop_back();
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::size_t used = 0;
    double number = 0;
    try {
        number = std::stod(text, &used);
    } catch (...) {
        return std::nullopt;
    }
    if (used != text.size() || !std::isfinite(number) || number <= 0) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(std::llround(milli ? number : number * 1000));
}

std::optional<std::int64_t> memory_bytes_of(const json& value) {
    if (value.is_number_integer()) {
        return value.get<std::int64_t>() > 0 ? std::optional(value.get<std::int64_t>())
                                             : std::nullopt;
    }
    if (!value.is_string()) {
        return std::nullopt;
    }
    const std::string text = trimmed(value.get<std::string>());
    static const std::vector<std::pair<std::string_view, std::int64_t>> units{
        {"Ki", std::int64_t{1} << 10},
        {"Mi", std::int64_t{1} << 20},
        {"Gi", std::int64_t{1} << 30},
        {"K", 1000},
        {"M", 1000 * 1000},
        {"G", 1000 * 1000 * 1000}};
    std::int64_t scale = 1;
    std::string number = text;
    for (const auto& [suffix, factor] : units) {
        if (text.ends_with(suffix)) {
            scale = factor;
            number = text.substr(0, text.size() - suffix.size());
            break;
        }
    }
    if (number.empty() || !std::ranges::all_of(number, [](unsigned char c) {
            return std::isdigit(c) != 0 || c == '.';
        })) {
        return std::nullopt;
    }
    double amount = 0;
    try {
        amount = std::stod(number);
    } catch (...) {
        return std::nullopt;
    }
    if (!std::isfinite(amount) || amount <= 0 || amount * static_cast<double>(scale) > 1e15) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(std::llround(amount * static_cast<double>(scale)));
}

ManifestRead read_manifest(std::string_view text) {
    ManifestRead read;
    auto& findings = read.findings;
    const json value = json::parse(text, nullptr, false);
    if (!value.is_object()) {
        findings.push_back(failed("manifest",
                                  std::string(methodology_manifest_file) +
                                      " is not a JSON object; start from a draft to see the shape",
                                  manifest_file));
        return read;
    }
    MethodologyManifest manifest;
    if (value.value("interface", json()) != json(1)) {
        findings.push_back(failed("manifest.interface",
                                  "interface must be 1, the version of the methodology interface",
                                  manifest_file));
    }
    manifest.name = trimmed(value.value("name", std::string()));
    if (manifest.name.empty() || manifest.name.size() > 200) {
        findings.push_back(failed("manifest.name", "a name of 1 to 200 bytes", manifest_file));
    }
    if (value.contains("description") && !value.at("description").is_string()) {
        findings.push_back(failed("manifest.description", "description is text", manifest_file));
    }
    manifest.description = bounded(trimmed(value.value("description", std::string())), 4000);
    manifest.entrypoint = value.value("entrypoint", std::string("methodology.py"));
    if (!manifest.entrypoint.ends_with(".py") || !methodology_path("", manifest.entrypoint)) {
        findings.push_back(failed("manifest.entrypoint",
                                  "entrypoint is a .py file inside the methodology's folder",
                                  manifest_file));
    }
    const json inputs = value.value("inputs", json());
    if (!inputs.is_object()) {
        findings.push_back(failed("manifest.inputs",
                                  "inputs says what it needs: {kinds, target, required_features}",
                                  manifest_file));
    } else {
        const json kinds = inputs.value("kinds", json());
        if (!kinds.is_array() || kinds.empty()) {
            findings.push_back(failed("manifest.inputs.kinds",
                                      "inputs.kinds lists at least one of tabular, text, "
                                      "time_series, graph, image, audio, document",
                                      manifest_file));
        } else {
            for (const json& kind : kinds) {
                const std::string name = kind.is_string() ? kind.get<std::string>() : std::string();
                if (!listed(methodology_input_kinds, name)) {
                    findings.push_back(failed("manifest.inputs.kinds",
                                              "'" + (kind.is_string() ? name : kind.dump()) +
                                                  "' is not an input kind: tabular, text, "
                                                  "time_series, graph, image, audio or document",
                                              manifest_file));
                } else if (!listed(manifest.input_kinds, name)) {
                    manifest.input_kinds.push_back(name);
                }
            }
        }
        const json target = inputs.value("target", json());
        if (!target.is_object()) {
            findings.push_back(failed("manifest.inputs.target",
                                      "inputs.target is {required, kind}; kind none when it has "
                                      "no target",
                                      manifest_file));
        } else {
            manifest.target_kind = target.value("kind", std::string());
            manifest.target_required = target.value("required", false);
            if (!listed(methodology_target_kinds, manifest.target_kind)) {
                findings.push_back(failed("manifest.inputs.target.kind",
                                          "target.kind is binary, multiclass, regression or none",
                                          manifest_file));
            } else if (manifest.target_required != (manifest.target_kind != "none")) {
                findings.push_back(failed("manifest.inputs.target.required",
                                          "target.required is true for a target of a kind, and "
                                          "false for kind none",
                                          manifest_file));
            }
        }
        if (const json features = inputs.value("required_features", json::array());
            !features.is_array()) {
            findings.push_back(failed("manifest.inputs.required_features",
                                      "required_features lists column names",
                                      manifest_file));
        } else {
            for (const json& feature : features) {
                if (!feature.is_string() || feature.get<std::string>().empty()) {
                    findings.push_back(failed("manifest.inputs.required_features",
                                              "required_features lists column names",
                                              manifest_file));
                    break;
                }
                manifest.required_features.push_back(feature.get<std::string>());
            }
        }
    }
    read_parameters(value.value("parameters", json()), manifest, findings);
    const json metrics = value.value("metrics", json());
    if (!metrics.is_array() || metrics.empty()) {
        findings.push_back(failed("manifest.metrics",
                                  "metrics lists at least one metric score() reports, such as "
                                  "auc or rmse; a run reports each in sample and held out",
                                  manifest_file));
    } else {
        for (const json& metric : metrics) {
            const std::string name = metric.is_string() ? metric.get<std::string>() : std::string();
            if (!identifier(name) || name.starts_with("in_sample_") ||
                name.starts_with("held_out_")) {
                findings.push_back(failed("manifest.metrics",
                                          "'" + (metric.is_string() ? name : metric.dump()) +
                                              "': a metric is a base name in lowercase letters, "
                                              "digits and _ (Lake adds in_sample_ and held_out_)",
                                          manifest_file));
            } else if (listed(manifest.metrics, name)) {
                findings.push_back(
                    failed("manifest.metrics", name + " is listed twice", manifest_file));
            } else {
                manifest.metrics.push_back(name);
            }
        }
    }
    if (value.contains("explain") && !value.at("explain").is_boolean()) {
        findings.push_back(failed("manifest.explain", "explain is true or false", manifest_file));
    }
    manifest.explain = value.value("explain", false) == true;
    manifest.packages = value.value("packages", std::string("requirements.lock"));
    if (!manifest.packages.empty() && (!methodology_path("", manifest.packages) ||
                                       !std::ranges::all_of(manifest.packages, [](unsigned char c) {
                                           return std::isalnum(c) != 0 || c == '.' || c == '_' ||
                                                  c == '-' || c == '/';
                                       }))) {
        findings.push_back(
            failed("manifest.packages",
                   "packages is the lock file's path inside the methodology's folder",
                   manifest_file));
    }
    read_resources(value.value("resources", json()), manifest, findings);
    if (all_ok(findings)) {
        findings.insert(findings.begin(),
                        passed("manifest", "the manifest describes interface 1", manifest_file));
    }
    read.manifest = std::move(manifest);
    return read;
}

json manifest_json(const MethodologyManifest& manifest) {
    json parameters = json::array();
    for (const MethodologyParameter& parameter : manifest.parameters) {
        parameters.push_back(parameter_json(parameter));
    }
    json resources{{"cpu", std::to_string(manifest.resources.cpu_millis) + "m"},
                   {"memory", std::to_string(manifest.resources.memory_bytes >> 20) + "Mi"},
                   {"gpu", manifest.resources.gpus},
                   {"timeout_seconds", manifest.resources.timeout_seconds}};
    if (manifest.resources.cpu_millis % 1000 == 0) {
        resources["cpu"] = std::to_string(manifest.resources.cpu_millis / 1000);
    }
    if (manifest.resources.memory_bytes % (std::int64_t{1} << 30) == 0) {
        resources["memory"] = std::to_string(manifest.resources.memory_bytes >> 30) + "Gi";
    }
    if (!manifest.resources.gpu_class.empty()) {
        resources["gpu_class"] = manifest.resources.gpu_class;
    }
    return {{"interface", manifest.interface},
            {"name", manifest.name},
            {"description", manifest.description},
            {"entrypoint", manifest.entrypoint},
            {"inputs",
             {{"kinds", manifest.input_kinds},
              {"target", {{"required", manifest.target_required}, {"kind", manifest.target_kind}}},
              {"required_features", manifest.required_features}}},
            {"parameters", std::move(parameters)},
            {"metrics", manifest.metrics},
            {"explain", manifest.explain},
            {"packages", manifest.packages},
            {"resources", std::move(resources)}};
}

std::vector<MethodologyFinding> check_lock(std::string_view text, const std::string& file) {
    std::vector<MethodologyFinding> findings;
    std::istringstream lines{std::string(text)};
    std::string line;
    std::string logical;
    int number = 0;
    int start = 0;
    int requirements = 0;
    const auto check = [&](const std::string& requirement, int at) {
        const std::string where = file + " line " + std::to_string(at);
        const std::string first = requirement.substr(0, requirement.find_first_of(" \t;"));
        if (requirement.starts_with("-")) {
            findings.push_back(failed("packages",
                                      where + ": options such as " + first +
                                          " are not allowed; list pinned requirements only",
                                      file));
            return;
        }
        if (requirement.find("://") != std::string::npos || requirement.starts_with(".") ||
            requirement.starts_with("/") || requirement.find(" @ ") != std::string::npos) {
            findings.push_back(failed("packages",
                                      where + ": " + first +
                                          " is a link or a path; use name==version from the index",
                                      file));
            return;
        }
        const auto pin = first.find("==");
        if (pin == std::string::npos || pin == 0 || pin + 2 >= first.size() ||
            first.find_first_of("<>!~*", pin) != std::string::npos) {
            findings.push_back(failed(
                "packages", where + ": " + first + " is not pinned; write name==version", file));
            return;
        }
        bool hashed = false;
        for (std::size_t at = requirement.find("--hash="); at != std::string::npos;
             at = requirement.find("--hash=", at + 7)) {
            const std::string rest = requirement.substr(at + 7);
            if (rest.starts_with("sha256:") && rest.size() >= 7 + 64 &&
                std::all_of(rest.begin() + 7,
                            rest.begin() + 7 + 64,
                            [](unsigned char c) {
                                return std::isxdigit(c) != 0 && std::isupper(c) == 0;
                            }) &&
                (rest.size() == 71 || rest[71] == ' ' || rest[71] == '\t')) {
                hashed = true;
            } else {
                hashed = false;
                break;
            }
        }
        if (!hashed) {
            findings.push_back(failed("packages",
                                      where + ": " + first +
                                          " needs --hash=sha256:<64 lowercase hex> for each file "
                                          "it may install (pip-compile --generate-hashes writes "
                                          "them)",
                                      file));
            return;
        }
        ++requirements;
    };
    while (std::getline(lines, line)) {
        ++number;
        if (const auto comment = line.find('#');
            comment != std::string::npos &&
            (comment == 0 || line[comment - 1] == ' ' || line[comment - 1] == '\t')) {
            line.resize(comment);
        }
        std::string piece = trimmed(line);
        const bool continues = piece.ends_with('\\');
        if (continues) {
            piece.pop_back();
        }
        if (logical.empty()) {
            start = number;
        }
        if (!piece.empty()) {
            logical += (logical.empty() ? "" : " ") + trimmed(piece);
        }
        if (!continues && !logical.empty()) {
            check(logical, start);
            logical.clear();
        }
    }
    if (!logical.empty()) {
        check(logical, start);
    }
    if (findings.empty()) {
        findings.push_back(passed(
            "packages",
            requirements == 0 ? "no packages: the standard library only"
                              : std::to_string(requirements) + " pinned requirement" +
                                    (requirements == 1 ? "" : "s") + ", each with its hashes",
            file));
    }
    return findings;
}

std::vector<MethodologyFinding> check_entrypoint(std::string_view source,
                                                 const MethodologyManifest& manifest,
                                                 const std::string& file) {
    std::vector<MethodologyFinding> findings;
    std::set<std::string> defined;
    std::istringstream lines{std::string(source)};
    std::string line;
    while (std::getline(lines, line)) {
        // Only module-level definitions: `def name(` or `async def` at the start of a line.
        std::string_view view(line);
        if (view.starts_with("def ")) {
            view.remove_prefix(4);
        } else {
            continue;
        }
        const auto open = view.find('(');
        if (open == std::string_view::npos) {
            continue;
        }
        defined.insert(trimmed(view.substr(0, open)));
    }
    const auto need =
        [&](const std::string& name, const std::string& signature, const std::string& purpose) {
            if (defined.contains(name)) {
                findings.push_back(passed("entrypoint." + name, signature + " is defined", file));
            } else {
                findings.push_back(
                    failed("entrypoint." + name,
                           file + " defines no module-level " + signature + ": " + purpose,
                           file));
            }
        };
    need("fit", "fit(data, params)", "it trains on the data and returns the artifact");
    need("predict", "predict(artifact, data)", "it answers one prediction per row");
    need("score",
         "score(artifact, data)",
         "it answers the manifest's metrics as numbers, in sample and held out");
    if (manifest.explain) {
        need("explain",
             "explain(artifact, data)",
             "the manifest says it explains, so it answers each row's feature contributions");
    }
    return findings;
}

std::optional<std::string> methodology_path(std::string_view folder, std::string_view file) {
    if (file.empty() || file.starts_with('/') || file.find('\\') != std::string_view::npos ||
        file.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view rest = file;
    while (!rest.empty()) {
        const auto slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        if (part.empty() || part == "." || part == "..") {
            return std::nullopt;
        }
        rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash + 1);
    }
    std::string base = trimmed(folder);
    while (base.ends_with('/')) {
        base.pop_back();
    }
    while (base.starts_with('/')) {
        base.erase(0, 1);
    }
    if (base == ".") {
        base.clear();
    }
    return base.empty() ? std::string(file) : base + "/" + std::string(file);
}

MethodologyDraft draft_methodology(std::string_view description,
                                   std::string_view name,
                                   std::string_view target_kind) {
    const std::string text = folded(description);
    std::string chosen;
    std::string reason;
    if (!target_kind.empty()) {
        chosen = template_for_target(target_kind);
        reason = "You chose a target of kind " + std::string(target_kind) + ".";
    } else if (mentions(text, {"cluster", "segment", "group similar", "groups of", "personas"})) {
        chosen = "clustering";
        reason = "The description asks for groups with no target, so the draft clusters.";
    } else if (mentions(text,
                        {"forecast",
                         "how much",
                         "how many",
                         "amount",
                         "price",
                         "revenue",
                         "value of",
                         "estimate",
                         "regression",
                         "spend",
                         "cost"})) {
        chosen = "regression";
        reason = "The description asks for an amount, so the draft predicts a number.";
    } else if (mentions(text,
                        {"which of",
                         "category",
                         "categories",
                         "classify into",
                         "multiclass",
                         "one of several",
                         "type of",
                         "route to"})) {
        chosen = "multiclass";
        reason = "The description asks for one of several categories.";
    } else {
        chosen = "binary";
        reason = "The description asks whether something happens, so the draft predicts a "
                 "yes-or-no outcome.";
    }
    std::string title = trimmed(name);
    if (title.empty()) {
        std::string first = trimmed(description);
        if (const auto stop = first.find_first_of(".\n"); stop != std::string::npos) {
            first.resize(stop);
        }
        title = first.empty() ? "My methodology" : bounded(first, 80);
        if (!title.empty()) {
            title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
        }
    }
    return draft_from(template_for(chosen), title, trimmed(description), std::move(reason));
}

MethodologyDraft draft_from_catalogue(const json& entry, std::string_view name) {
    const auto text = [&](const char* key) { return entry.value(key, std::string()); };
    const auto joined = [&](const char* key) {
        std::string out;
        if (const json values = entry.value(key, json::array()); values.is_array()) {
            for (const json& value : values) {
                std::string item;
                if (value.is_string()) {
                    item = value.get<std::string>();
                } else if (value.is_object()) {
                    item = value.value("role", value.value("name", std::string()));
                    if (value.value("required", false)) {
                        item += " (required)";
                    }
                }
                if (!item.empty()) {
                    out += (out.empty() ? "" : ", ") + item;
                }
            }
        }
        return out;
    };
    // The target the catalogue's inputs imply: a required label means a target. Its kind follows
    // the family's words; clustering and other label-free ones have none.
    bool label = false;
    if (const json inputs = entry.value("inputs", json::array()); inputs.is_array()) {
        for (const json& input : inputs) {
            if (input.is_object() && input.value("role", std::string()) == "label" &&
                input.value("required", false)) {
                label = true;
            }
        }
    }
    const std::string kinds = folded(joined("model_kinds") + " " + text("name"));
    std::string target_kind = "binary";
    if (!label) {
        target_kind = "none";
    } else if (mentions(kinds, {"forecast", "regression", "time_series", "survival"})) {
        target_kind = "regression";
    } else if (mentions(kinds, {"multiclass", "vision", "language", "nlp"})) {
        target_kind = "multiclass";
    }
    const std::string id = text("id");
    std::string description = "Duplicated from Orama's " + id + " " + text("name") + ".";
    if (const std::string family = text("family"); !family.empty()) {
        description += " Family: " + family + ".";
    }
    if (const std::string makes = joined("outputs"); !makes.empty()) {
        description += " It makes " + makes + ".";
    }
    if (const std::string needs = joined("inputs"); !needs.empty()) {
        description += " It needs " + needs + ".";
    }
    if (const std::string measured = joined("metrics"); !measured.empty()) {
        description += " Orama measures it by " + measured + ".";
    }
    description += " Orama's runs natively in the engine and has no Python source, so this copy "
                   "starts from Lake's " +
                   template_for_target(target_kind) + " template.";
    std::string title = trimmed(name);
    if (title.empty()) {
        title = text("name").empty() ? id : text("name");
    }
    MethodologyDraft draft = draft_from(template_for(template_for_target(target_kind)),
                                        title,
                                        description,
                                        "Orama's " + id + " has no Python source.");
    // Its search space, where the engine publishes one, becomes the copy's parameters.
    const json space = entry.value("study_search_space", json());
    if (space.is_object() && space.value("parameters", json()).is_array()) {
        std::vector<MethodologyFinding> ignored;
        MethodologyManifest probe;
        json parameters = json::array();
        for (const json& parameter : space.at("parameters")) {
            if (!parameter.is_object()) {
                continue;
            }
            json kept{{"name", parameter.value("name", std::string())},
                      {"kind", parameter.value("kind", std::string())},
                      {"default", parameter.value("default", json())}};
            for (const char* key : {"minimum", "maximum", "log"}) {
                if (parameter.contains(key) && !parameter.at(key).is_null()) {
                    kept[key] = parameter.at(key);
                }
            }
            if (const json choices = parameter.value("choices", json::array());
                choices.is_array() && !choices.empty()) {
                kept["choices"] = choices;
            }
            parameters.push_back(std::move(kept));
        }
        read_parameters(parameters, probe, ignored);
        for (MethodologyParameter& parameter : probe.parameters) {
            const bool known =
                std::ranges::any_of(draft.manifest.parameters, [&](const MethodologyParameter& p) {
                    return p.name == parameter.name;
                });
            if (!known) {
                draft.manifest.parameters.push_back(std::move(parameter));
            }
        }
        draft.files[0].contents = manifest_json(draft.manifest).dump(2) + "\n";
    }
    return draft;
}

std::string methodology_copy_name(std::string_view name) {
    constexpr std::string_view suffix = " (copy)";
    std::string base = trimmed(name);
    if (base.empty()) {
        base = "Methodology";
    }
    return bounded(std::move(base), 200 - suffix.size()) + std::string(suffix);
}

bool workspace_methodology_id(std::string_view id) {
    return id.size() == 10 && id.starts_with("W-") &&
           std::all_of(id.begin() + 2, id.end(), [](unsigned char c) {
               return std::isdigit(c) != 0 || (c >= 'a' && c <= 'f');
           });
}

json registry_fields(const MethodologyManifest& manifest) {
    json parameters = json::array();
    for (const MethodologyParameter& parameter : manifest.parameters) {
        parameters.push_back(parameter_json(parameter));
    }
    return {{"name", manifest.name},
            {"description", manifest.description},
            {"input_kinds", manifest.input_kinds},
            {"target", {{"required", manifest.target_required}, {"kind", manifest.target_kind}}},
            {"required_features", manifest.required_features},
            {"parameters", std::move(parameters)},
            {"metrics", manifest.metrics},
            {"explain", manifest.explain},
            // The engine keeps resources as Kubernetes writes them (zetta#1261); the GPU class
            // stays in the manifest, which Lake reads for every run.
            {"resources",
             {{"cpu", std::to_string(manifest.resources.cpu_millis) + "m"},
              {"memory", std::to_string(manifest.resources.memory_bytes >> 20) + "Mi"},
              {"gpu", manifest.resources.gpus},
              {"timeout_seconds", manifest.resources.timeout_seconds}}}};
}

bool validated_for_revision(const json& entry) {
    if (!entry.is_object()) {
        return false;
    }
    const json validation = entry.value("validation", json::object());
    const json code = entry.value("code", json::object());
    return validation.is_object() && code.is_object() &&
           validation.value("status", std::string()) == "passed" &&
           !code.value("revision", std::string()).empty() &&
           validation.value("revision", std::string()) == code.value("revision", std::string());
}

std::string methodology_dockerfile(std::string_view packages) {
    // oramasearch/methodology-runtime's templates/methodology.Dockerfile, inline: the folder's
    // files at /methodology, the lock file installed into /methodology/.packages with hashes
    // required, wheels only and no dependency resolution beyond the lock (an empty file installs
    // nothing), and /methodology owned by root and read-only to the uid the code runs as.
    std::string dockerfile = "inline:ARG METHODOLOGY_RUNTIME_IMAGE\n"
                             "FROM ${METHODOLOGY_RUNTIME_IMAGE}\n"
                             "USER root\n"
                             "COPY . /methodology\n";
    dockerfile +=
        packages.empty()
            ? std::string("RUN chown -R 0:0 /methodology && chmod -R a+rX,go-w /methodology\n")
            : "RUN /opt/orama/bin/install-locked /methodology \"" + std::string(packages) +
                  "\" && chown -R 0:0 /methodology && chmod -R a+rX,go-w /methodology\n";
    dockerfile += "USER 10001:10001\n";
    return dockerfile;
}

std::vector<json> validation_sample(const MethodologyManifest& manifest, std::int64_t rows) {
    // A fixed linear congruential sequence, so a sample is the same on every run and platform.
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    const auto next = [&] {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((state >> 11) & ((std::uint64_t{1} << 53) - 1)) /
               static_cast<double>(std::uint64_t{1} << 53);
    };
    static const std::vector<std::string> words{"red", "green", "blue", "amber"};
    std::vector<json> out;
    out.reserve(static_cast<std::size_t>(std::max<std::int64_t>(rows, 0)));
    for (std::int64_t index = 0; index < rows; ++index) {
        const double a = next() * 2 - 1;
        const double b = next() * 2 - 1;
        const double c = next();
        const double noise = (next() - 0.5) * 0.4;
        json row{{"row_id", index + 1},
                 {"feature_a", a},
                 {"feature_b", b},
                 {"feature_c", c},
                 {"label_text", words[static_cast<std::size_t>(index) % words.size()]}};
        for (const std::string& feature : manifest.required_features) {
            if (!row.contains(feature)) {
                row[feature] = next();
            }
        }
        const double signal = 1.5 * a - b + 0.5 * c + noise;
        if (manifest.target_kind == "binary") {
            row["target"] = signal > 0 ? 1 : 0;
        } else if (manifest.target_kind == "multiclass") {
            row["target"] = signal < -0.5 ? "low" : (signal < 0.5 ? "middle" : "high");
        } else if (manifest.target_kind == "regression") {
            row["target"] = 10 * signal + 50;
        }
        out.push_back(std::move(row));
    }
    return out;
}

json numeric_metrics(const json& metrics, std::vector<std::string>* dropped) {
    json out = json::object();
    if (!metrics.is_object()) {
        return out;
    }
    for (const auto& [name, value] : metrics.items()) {
        const bool named =
            (name.starts_with("in_sample_") || name.starts_with("held_out_")) && name.size() <= 96;
        const bool number = value.is_number() && std::isfinite(value.get<double>());
        if (named && number) {
            out[name] = value;
        } else if (dropped != nullptr) {
            dropped->push_back(name);
        }
    }
    return out;
}

bool methodology_deployment_endpoint_valid(std::string_view endpoint) {
    // http://md-<12 hex>.<namespace>.svc.cluster.local[:port], nothing else (lake#690).
    constexpr std::string_view scheme = "http://md-";
    constexpr std::string_view suffix = ".svc.cluster.local";
    if (!endpoint.starts_with(scheme)) {
        return false;
    }
    std::string_view rest = endpoint.substr(scheme.size());
    if (rest.size() < 13 || rest[12] != '.' ||
        !std::ranges::all_of(rest.substr(0, 12), [](unsigned char c) {
            return std::isdigit(c) != 0 || (c >= 'a' && c <= 'f');
        })) {
        return false;
    }
    rest = rest.substr(13);
    const auto at = rest.find(suffix);
    if (at == std::string_view::npos || at == 0) {
        return false;
    }
    const std::string_view space = rest.substr(0, at);
    if (!std::ranges::all_of(space, [](unsigned char c) {
            return std::islower(c) != 0 || std::isdigit(c) != 0 || c == '-';
        })) {
        return false;
    }
    const std::string_view port = rest.substr(at + suffix.size());
    if (port.empty()) {
        return true;
    }
    return port.size() >= 2 && port.size() <= 6 && port[0] == ':' &&
           std::ranges::all_of(port.substr(1), [](unsigned char c) { return std::isdigit(c) != 0; });
}

} // namespace lake::control

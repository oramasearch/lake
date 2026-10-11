#include "lake/api/model_gateway.hpp"

#include <algorithm>
#include <format>
#include <optional>

namespace lake::api {
namespace {

// A model can think for minutes before it answers.
constexpr std::chrono::seconds model_timeout{600};

std::optional<std::uint64_t> count(const nlohmann::json& usage, const char* name) {
    const auto found = usage.find(name);
    if (found == usage.end()) {
        return std::nullopt;
    }
    if (found->is_number_unsigned()) {
        return found->get<std::uint64_t>();
    }
    if (found->is_number_integer() && found->get<std::int64_t>() >= 0) {
        return static_cast<std::uint64_t>(found->get<std::int64_t>());
    }
    return std::nullopt;
}

// `usage` as one answer reports it, whichever of the two spellings it uses.
bool read_usage(const nlohmann::json& usage, ModelUsage& out) {
    if (!usage.is_object()) {
        return false;
    }
    const auto input = count(usage, "input_tokens") ? count(usage, "input_tokens")
                                                    : count(usage, "prompt_tokens");
    const auto output = count(usage, "output_tokens") ? count(usage, "output_tokens")
                                                      : count(usage, "completion_tokens");
    if (!input && !output) {
        return false;
    }
    out.input_tokens = input.value_or(0);
    out.output_tokens = output.value_or(0);
    out.reported = true;
    return true;
}

void read_model(const nlohmann::json& object, ModelUsage& out) {
    if (object.is_object() && object.contains("model") && object.at("model").is_string() &&
        !object.at("model").get<std::string>().empty()) {
        out.model = object.at("model").get<std::string>();
    }
}

// What one answer (or one event of a stream) says about its usage.
void read_answer(const nlohmann::json& answer, ModelUsage& out) {
    if (!answer.is_object()) {
        return;
    }
    read_model(answer, out);
    if (answer.contains("usage") && read_usage(answer.at("usage"), out)) {
        return;
    }
    // The Responses API's stream ends with `response.completed` (or `.incomplete`, `.failed`),
    // whose `response` carries the usage.
    if (const auto response = answer.find("response"); response != answer.end()) {
        read_model(*response, out);
        if (response->is_object() && response->contains("usage")) {
            read_usage(response->at("usage"), out);
        }
    }
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

// The provider's own explanation, `error.message`, cut to a sentence's length.
std::string provider_message(std::string_view body) {
    const nlohmann::json parsed = nlohmann::json::parse(body, nullptr, false);
    std::string message;
    if (parsed.is_object()) {
        const nlohmann::json error = parsed.value("error", nlohmann::json());
        if (error.is_object() && error.contains("message") && error.at("message").is_string()) {
            message = error.at("message").get<std::string>();
        } else if (error.is_string()) {
            message = error.get<std::string>();
        }
    }
    if (message.size() > 500) {
        message = message.substr(0, 500) + "...";
    }
    return message;
}

} // namespace

bool is_model_gateway_endpoint(std::string_view endpoint) {
    return std::ranges::find(model_gateway_endpoints, endpoint) != model_gateway_endpoints.end();
}

std::expected<nlohmann::json, ApiError> prepare_model_request(std::string_view endpoint,
                                                              nlohmann::json request) {
    if (!is_model_gateway_endpoint(endpoint)) {
        return std::unexpected(
            ApiError{"invalid_input",
                     std::format("endpoint: {} is not one the model gateway forwards", endpoint),
                     "/endpoint"});
    }
    if (!request.is_object()) {
        return std::unexpected(
            ApiError{"invalid_input", "request: must be a JSON object", "/request"});
    }
    const auto model = request.find("model");
    if (model == request.end() || !model->is_string() || model->get<std::string>().empty()) {
        return std::unexpected(ApiError{
            "invalid_input", "model: name the model, such as gpt-5.5", "/request/model"});
    }
    const auto stream = request.find("stream");
    if (stream != request.end() && !stream->is_boolean() && !stream->is_null()) {
        return std::unexpected(
            ApiError{"invalid_input", "stream: must be true or false", "/request/stream"});
    }
    const bool streamed = stream != request.end() && stream->is_boolean() && stream->get<bool>();
    if (streamed && endpoint == "embeddings") {
        return std::unexpected(
            ApiError{"invalid_input", "stream: embeddings do not stream", "/request/stream"});
    }
    if (streamed && endpoint == "chat/completions") {
        nlohmann::json options = request.value("stream_options", nlohmann::json::object());
        if (!options.is_object()) {
            options = nlohmann::json::object();
        }
        options["include_usage"] = true;
        request["stream_options"] = std::move(options);
    }
    return request;
}

ModelUsage model_usage(std::string_view body, bool stream) {
    ModelUsage out;
    if (!stream) {
        read_answer(nlohmann::json::parse(body, nullptr, false), out);
        return out;
    }
    // Server-sent events: each `data:` line is one JSON event, and the last one that reports
    // usage is the answer's total.
    std::size_t start = 0;
    while (start < body.size()) {
        std::size_t end = body.find('\n', start);
        if (end == std::string_view::npos) {
            end = body.size();
        }
        std::string_view line = trim(body.substr(start, end - start));
        start = end + 1;
        if (!line.starts_with("data:")) {
            continue;
        }
        line = trim(line.substr(5));
        if (line.empty() || line == "[DONE]") {
            continue;
        }
        ModelUsage event;
        read_answer(nlohmann::json::parse(line, nullptr, false), event);
        if (!event.model.empty()) {
            out.model = event.model;
        }
        if (event.reported) {
            out.input_tokens = event.input_tokens;
            out.output_tokens = event.output_tokens;
            out.reported = true;
        }
    }
    return out;
}

ApiError model_provider_failure(int status, std::string_view body, std::string_view transport) {
    if (!transport.empty()) {
        return ApiError{"store_unavailable",
                        "could not reach the model provider: " + std::string(transport)};
    }
    if (status == 401 || status == 403) {
        return ApiError{"store_unavailable",
                        "the model provider refused this Lake's key; an administrator should "
                        "check LAKE_OPENAI_API_KEY"};
    }
    const std::string message = provider_message(body);
    const std::string said = message.empty() ? std::format(" (HTTP {})", status) : ": " + message;
    if (status == 429) {
        return ApiError{"rate_limited", "the model provider is busy or over its quota" + said};
    }
    if (status >= 500 || status < 200) {
        return ApiError{"store_unavailable", "the model provider could not answer" + said};
    }
    return ApiError{"engine_error", "the model provider refused the request" + said};
}

ModelProvider openai_model_provider(std::string api_key, std::string base_url, ModelPost post) {
    if (base_url.empty()) {
        base_url = std::string(default_model_provider_url);
    }
    while (!base_url.empty() && base_url.back() == '/') {
        base_url.pop_back();
    }
    return [api_key = std::move(api_key), base_url = std::move(base_url), post = std::move(post)](
               const ModelProviderRequest& request) -> ModelProviderResponse {
        return post(base_url + "/" + request.endpoint,
                    request.body,
                    {{"Authorization", "Bearer " + api_key},
                     {"Content-Type", "application/json"},
                     {"Accept", request.stream ? "text/event-stream" : "application/json"},
                     {"User-Agent", "lake-model-gateway/1"}},
                    model_timeout);
    };
}

} // namespace lake::api

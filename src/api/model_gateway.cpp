#include "lake/api/model_gateway.hpp"

#include <algorithm>
#include <format>
#include <optional>

namespace lake::api {
namespace {

// A model can think for minutes before it answers.
constexpr std::chrono::seconds model_timeout{600};

using Json = nlohmann::json;

Json closed_object(Json properties, Json required = Json::array()) {
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
            {"additionalProperties", false}};
}

// These are request schemas, not the provider's entire API. New provider features stay disabled
// until their charges can be counted. Schemas supplied as function parameters or output formats
// describe data only and cannot launch provider work.
Json token_request_schema(std::string_view endpoint) {
    const Json string{{"type", "string"}};
    const Json boolean{{"type", {"boolean", "null"}}};
    const Json number{{"type", {"number", "null"}}};
    const Json integer{{"type", {"integer", "null"}}};
    const Json disabled{{"enum", {false, nullptr}}};
    const auto array = [](Json items) {
        return Json{{"type", "array"}, {"items", std::move(items)}};
    };
    const Json function = closed_object({{"name", string},
                                         {"description", string},
                                         {"parameters", {{"type", "object"}}},
                                         {"strict", boolean}},
                                        {"name"});
    const Json format = closed_object({{"type", {{"enum", {"text", "json_object", "json_schema"}}}},
                                       {"name", string},
                                       {"description", string},
                                       {"schema", {{"type", "object"}}},
                                       {"strict", boolean}},
                                      {"type"});
    const Json chat_format =
        closed_object({{"type", {{"enum", {"text", "json_object", "json_schema"}}}},
                       {"json_schema",
                        closed_object({{"name", string},
                                       {"description", string},
                                       {"schema", {{"type", "object"}}},
                                       {"strict", boolean}},
                                      {"name", "schema"})}},
                      {"type"});
    Json properties{{"model", string}, {"stream", boolean}};
    if (endpoint == "embeddings") {
        properties["input"] = {
            {"type", {"string", "array"}},
            {"items",
             {{"type", {"string", "integer", "array"}}, {"items", {{"type", "integer"}}}}}};
        properties["encoding_format"] = {{"enum", {"float", "base64"}}};
        properties["dimensions"] = integer;
        properties["user"] = string;
        return closed_object(std::move(properties));
    }
    properties.update({{"background", disabled},
                       {"store", disabled},
                       {"temperature", number},
                       {"top_p", number},
                       {"parallel_tool_calls", boolean}});
    Json tool = function;
    tool["properties"]["type"] = {{"enum", {"function"}}};
    tool["required"].push_back("type");
    if (endpoint == "chat/completions") {
        tool = closed_object({{"type", {{"enum", {"function"}}}}, {"function", function}},
                             {"type", "function"});
        const Json part =
            closed_object({{"type", {{"enum", {"text"}}}}, {"text", string}}, {"type", "text"});
        const Json call = closed_object(
            {{"id", string},
             {"type", {{"enum", {"function"}}}},
             {"function",
              closed_object({{"name", string}, {"arguments", string}}, {"name", "arguments"})}},
            {"id", "type", "function"});
        properties.update(
            {{"messages",
              array(closed_object(
                  {{"role",
                    {{"enum", {"system", "developer", "user", "assistant", "tool", "function"}}}},
                   {"content", {{"type", {"string", "array", "null"}}, {"items", part}}},
                   {"name", string},
                   {"tool_call_id", string},
                   {"tool_calls", array(call)},
                   {"function_call",
                    closed_object({{"name", string}, {"arguments", string}},
                                  {"name", "arguments"})}},
                  {"role"}))},
             {"stream_options", closed_object({{"include_usage", boolean}})},
             {"response_format", chat_format},
             {"max_tokens", integer},
             {"max_completion_tokens", integer},
             {"reasoning_effort", string},
             {"verbosity", string},
             {"frequency_penalty", number},
             {"presence_penalty", number},
             {"seed", integer},
             {"n", integer},
             {"logprobs", boolean},
             {"top_logprobs", integer},
             {"logit_bias", {{"type", {"object", "null"}}}},
             {"stop", {{"type", {"string", "array", "null"}}, {"items", string}}},
             {"user", string},
             {"functions", array(function)}});
    } else {
        properties.update({{"input", {{"type", {"string", "array"}}}},
                           {"instructions", string},
                           {"max_output_tokens", integer},
                           {"max_tool_calls", integer},
                           {"reasoning", closed_object({{"effort", string}})},
                           {"text", closed_object({{"format", format}, {"verbosity", string}})},
                           {"truncation", {{"enum", {"auto", "disabled"}}}}});
    }
    properties["tools"] = array(tool);
    // Object tool choices are checked below; the provider must never choose a hosted tool.
    properties["tool_choice"] = {{"type", {"string", "object"}}};
    if (endpoint == "chat/completions") {
        properties["function_call"] = {{"type", {"string", "object"}}};
    }
    return closed_object(std::move(properties));
}

std::optional<ApiError> token_request_error(std::string_view endpoint, const Json& request) {
    const auto validate = [](const Json& schema,
                             const Json& value,
                             const std::string& path) -> std::optional<ApiError> {
        const Capability rule{.input_schema = closed_object({{"value", schema}}, {"value"})};
        const auto result = validate_input(rule, {{"value", value}});
        if (!result.ok) {
            return ApiError{"invalid_input",
                            result.error.message,
                            path + result.error.path.substr(std::string("/value").size())};
        }
        return std::nullopt;
    };
    static const Json chat = token_request_schema("chat/completions");
    static const Json responses = token_request_schema("responses");
    static const Json embeddings = token_request_schema("embeddings");
    const auto& schema = endpoint == "chat/completions" ? chat
                         : endpoint == "responses"      ? responses
                                                        : embeddings;
    if (auto error = validate(schema, request, "/request")) {
        return error;
    }
    if (const auto bias = request.find("logit_bias"); bias != request.end() && bias->is_object()) {
        for (auto entry = bias->begin(); entry != bias->end(); ++entry) {
            if (!entry->is_number()) {
                const auto path = Json::json_pointer("/request/logit_bias") / entry.key();
                return ApiError{
                    "invalid_input", "logit_bias: values must be numbers", path.to_string()};
            }
        }
    }
    for (const char* key : {"tool_choice", "function_call"}) {
        const auto choice = request.find(key);
        if (choice == request.end()) {
            continue;
        }
        Json choice_schema;
        if (choice->is_string()) {
            choice_schema = {{"enum", {"auto", "none", "required"}}};
        } else if (std::string_view(key) == "function_call") {
            choice_schema = closed_object({{"name", {{"type", "string"}}}}, {"name"});
        } else {
            Json fields{{"type", {{"enum", {"function"}}}}};
            const Json name = closed_object({{"name", {{"type", "string"}}}}, {"name"});
            if (endpoint == "chat/completions") {
                fields["function"] = name;
                choice_schema = closed_object(fields, {"type", "function"});
            } else {
                fields["name"] = {{"type", "string"}};
                choice_schema = closed_object(fields, {"type", "name"});
            }
        }
        if (auto error = validate(choice_schema, *choice, std::string("/request/") + key)) {
            return error;
        }
    }
    // Responses input is a tagged union. Only inline text and caller-executed function history
    // are accepted: item references and provider-owned files/conversations are never resolved.
    if (endpoint == "responses" && request.contains("input") && request.at("input").is_array()) {
        const Json string{{"type", "string"}};
        const auto& input = request.at("input");
        for (std::size_t i = 0; i < input.size(); ++i) {
            const auto& item = input[i];
            const Json type =
                item.is_object() && item.contains("type") ? item.at("type") : Json("message");
            Json fields{{"type", {{"enum", {"message"}}}},
                        {"role", {{"enum", {"system", "developer", "user", "assistant"}}}},
                        {"content",
                         {{"type", {"string", "array"}},
                          {"items",
                           closed_object({{"type", {{"enum", {"input_text", "output_text"}}}},
                                          {"text", string}},
                                         {"type", "text"})}}}};
            Json required{"role", "content"};
            if (type == "function_call") {
                fields = {{"type", {{"enum", {"function_call"}}}},
                          {"call_id", string},
                          {"name", string},
                          {"arguments", string}};
                required = {"type", "call_id", "name", "arguments"};
            } else if (type == "function_call_output") {
                fields = {{"type", {{"enum", {"function_call_output"}}}},
                          {"call_id", string},
                          {"output", string}};
                required = {"type", "call_id", "output"};
            }
            if (auto error = validate(
                    closed_object(fields, required), item, "/request/input/" + std::to_string(i))) {
                return error;
            }
        }
    }
    return std::nullopt;
}

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
    // Anything but false or null: a background answer is generated after it is answered, so its
    // usage never passes the gateway (lake#939).
    const auto background = request.find("background");
    if (background != request.end() && !background->is_null() && *background != false) {
        return std::unexpected(ApiError{
            "invalid_input",
            "background: the gateway counts an answer's tokens as it is made; leave it out",
            "/request/background"});
    }
    if (streamed && endpoint == "embeddings") {
        return std::unexpected(
            ApiError{"invalid_input", "stream: embeddings do not stream", "/request/stream"});
    }
    if (auto error = token_request_error(endpoint, request)) {
        return std::unexpected(*error);
    }
    if (endpoint != "embeddings") {
        // Responses otherwise stores answers by default under the shared platform account.
        request["store"] = false;
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
    if (!stream) {
        ModelUsage out;
        read_answer(nlohmann::json::parse(body, nullptr, false), out);
        return out;
    }
    ModelStreamUsage reader;
    reader.feed(body);
    reader.finish();
    return reader.usage();
}

std::uint64_t estimated_model_tokens(std::size_t bytes) {
    return (static_cast<std::uint64_t>(bytes) + 3) / 4;
}

void ModelStreamUsage::feed(std::string_view piece) {
    bytes_ += piece.size();
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = piece.find('\n', start);
        if (end == std::string_view::npos) {
            partial_.append(piece.substr(start));
            return;
        }
        if (partial_.empty()) {
            line(piece.substr(start, end - start));
        } else {
            partial_.append(piece.substr(start, end - start));
            line(partial_);
            partial_.clear();
        }
        start = end + 1;
    }
}

void ModelStreamUsage::finish() {
    if (!partial_.empty()) {
        const std::string last = std::move(partial_);
        partial_.clear();
        line(last);
    }
}

// Server-sent events: each `data:` line is one JSON event, and the last one that reports usage is
// the answer's total.
void ModelStreamUsage::line(std::string_view text) {
    text = trim(text);
    if (!text.starts_with("data:")) {
        return;
    }
    text = trim(text.substr(5));
    if (text.empty() || text == "[DONE]") {
        return;
    }
    ModelUsage event;
    read_answer(nlohmann::json::parse(text, nullptr, false), event);
    if (!event.model.empty()) {
        usage_.model = event.model;
    }
    if (event.reported) {
        usage_.input_tokens = event.input_tokens;
        usage_.output_tokens = event.output_tokens;
        usage_.reported = true;
    }
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
                    model_timeout,
                    request.on_body);
    };
}

} // namespace lake::api

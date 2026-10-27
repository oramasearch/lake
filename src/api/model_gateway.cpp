#include "lake/api/model_gateway.hpp"

#include "lake/api/billing.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <set>

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
    // Only the standard tier is ever asked for (lake#1230): Fast, Ultrafast, priority and the rest
    // cost a multiple of the price list's rates. Absent or null is the default as well; the gateway
    // writes `default` either way, so the account's own provider setting never chooses for it.
    properties.update({{"background", disabled},
                       {"store", disabled},
                       {"service_tier", {{"enum", {"default", nullptr}}}},
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

// An agent sends the provider's last assistant message back as history, as the SDK dumped it.
// OpenAI's chat messages carry fields that only describe an answer: `annotations` (citations,
// `[]` without a hosted tool), `refusal` (the refusal text) and `audio` (null without audio
// output). They launch no provider work, so they are dropped here rather than refused, and are
// never forwarded. A non-null `audio` names audio the provider stored, so it stays and is
// refused by the closed message schema. Only assistant messages are touched.
void drop_output_only_message_fields(Json& request) {
    const auto messages = request.find("messages");
    if (messages == request.end() || !messages->is_array()) {
        return;
    }
    for (auto& message : *messages) {
        if (!message.is_object()) {
            continue;
        }
        const auto role = message.find("role");
        if (role == message.end() || *role != "assistant") {
            continue;
        }
        message.erase("annotations");
        message.erase("refusal");
        if (const auto audio = message.find("audio"); audio != message.end() && audio->is_null()) {
            message.erase(audio);
        }
    }
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

std::vector<GatewayModel> default_gateway_models() {
    return {{"gpt-6.1-sol"},
            {"gpt-6.1-terra"},
            {"gpt-6-astra"},
            {"gpt-6-luna"},
            {"gpt-5.5"},
            {"gpt-5"},
            {"gpt-4o"},
            {"text-embedding-3-small"}};
}

bool is_chat_model(std::string_view name) {
    return !name.empty() && !name.starts_with("text-embedding");
}

std::vector<std::string> gateway_chat_models(const std::vector<GatewayModel>& models) {
    std::vector<std::string> out;
    for (const GatewayModel& model : models) {
        if (model.provider == "openai" && is_chat_model(model.name)) {
            out.push_back(model.name);
        }
    }
    return out;
}

bool has_long_context_rate(std::string_view model) {
    return model.starts_with("gpt-6") || model == "gpt-5.5" || model.starts_with("gpt-5.5-");
}

std::expected<std::vector<GatewayModel>, std::string> parse_gateway_models(std::string_view text) {
    if (text.empty()) {
        return default_gateway_models();
    }
    const auto parsed = nlohmann::json::parse(text, nullptr, false);
    if (!parsed.is_array()) {
        return std::unexpected(
            "LAKE_MODEL_GATEWAY_MODELS must be an array of model/provider objects");
    }
    std::vector<GatewayModel> models;
    std::set<std::string> seen;
    for (const auto& entry : parsed) {
        if (!entry.is_object() || entry.size() != 2 || !entry.contains("model") ||
            !entry.at("model").is_string() || !entry.contains("provider") ||
            entry.at("provider") != "openai") {
            return std::unexpected("each gateway model needs model and provider: openai");
        }
        const std::string name = entry.at("model");
        if (name.empty() || name.size() > 128 ||
            !std::ranges::all_of(name, [](unsigned char c) { return c > 32 && c < 127; }) ||
            !seen.insert(name).second) {
            return std::unexpected(
                "gateway model names must be distinct printable names (1-128 bytes)");
        }
        models.push_back({name, "openai"});
    }
    return models;
}

bool gateway_model_allowed(const std::vector<GatewayModel>& models, std::string_view name) {
    return std::ranges::any_of(models, [name](const GatewayModel& model) {
        return model.provider == "openai" && model.name == name && !name.empty();
    });
}

std::string model_token_class(std::string_view model, std::string_view kind) {
    return std::format("{}/{}", model, kind);
}

std::string
model_token_class(std::string_view model, std::string_view kind, std::uint64_t prompt_tokens) {
    if (prompt_tokens > long_context_prompt_tokens && has_long_context_rate(model)) {
        return std::format("{}@long/{}", model, kind);
    }
    return model_token_class(model, kind);
}

std::expected<void, ApiError>
validate_model_prices(std::string_view model, ApiContext& context, std::string_view path) {
    const auto refused = [path] {
        return std::unexpected(
            ApiError{"invalid_input",
                     "model: this Lake requires positive input and output prices for this model",
                     std::string(path)});
    };
    if (!billing_registered(context.registry)) {
        return {};
    }
    if (!context.store) {
        return std::unexpected(ApiError{"store_unavailable", "model prices could not be read", ""});
    }
    const auto stored = context.store->billing_plans();
    if (!stored) {
        return std::unexpected(ApiError{"store_unavailable", "model prices could not be read", ""});
    }
    const std::chrono::year_month_day today{
        std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
    const auto list =
        price_list_for(price_lists(*stored),
                       std::format("{:04}-{:02}", int(today.year()), unsigned(today.month())));
    if (!list) {
        return refused();
    }
    // A model whose long prompts cost more needs its long-context rates too: the gateway learns a
    // prompt's length only from the provider's answer, after the call is made.
    std::vector<std::string> keys{model_token_class(model, "input"),
                                  model_token_class(model, "output")};
    if (has_long_context_rate(model)) {
        keys.push_back(model_token_class(model, "input", long_context_prompt_tokens + 1));
        keys.push_back(model_token_class(model, "output", long_context_prompt_tokens + 1));
    }
    for (const std::string& key : keys) {
        std::size_t matches = 0;
        for (const auto& price : list->prices) {
            if (price.dimension == "model_tokens" && price.class_name == key) {
                if (price.unit_micros == 0 || price.per == 0) {
                    return refused();
                }
                ++matches;
            }
        }
        if (matches != 1) {
            return refused();
        }
    }
    return {};
}

std::expected<void, ApiError> validate_gateway_model(std::string_view model, ApiContext& context) {
    if (!gateway_model_allowed(context.model_gateway_models, model)) {
        return std::unexpected(ApiError{
            "invalid_input", "model: not in this Lake's configured model list", "/request/model"});
    }
    return validate_model_prices(model, context);
}

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
    const auto tier = request.find("service_tier");
    if (tier != request.end() && !tier->is_null() && *tier != "default") {
        return std::unexpected(
            ApiError{"invalid_input",
                     "service_tier: Lake asks only for the standard tier (default); faster or "
                     "priority processing costs more than its price list covers",
                     "/request/service_tier"});
    }
    if (streamed && endpoint == "embeddings") {
        return std::unexpected(
            ApiError{"invalid_input", "stream: embeddings do not stream", "/request/stream"});
    }
    if (endpoint == "chat/completions") {
        drop_output_only_message_fields(request);
    }
    if (auto error = token_request_error(endpoint, request)) {
        return std::unexpected(*error);
    }
    if (endpoint != "embeddings") {
        // Responses otherwise stores answers by default under the shared platform account.
        request["store"] = false;
        // The standard tier, never the account's own default (which could be a costlier one).
        request["service_tier"] = "default";
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

// lake#1245: tool calls on the gpt-6.x models go through OpenAI's Responses API.

bool needs_responses_for_tools(std::string_view model) {
    return std::ranges::any_of(responses_for_tools_models, [model](std::string_view listed) {
        return model == listed || (model.starts_with(listed) && model.size() > listed.size() &&
                                   model[listed.size()] == '-');
    });
}

bool chat_goes_to_responses(const nlohmann::json& chat) {
    if (!chat.is_object() || !chat.contains("model") || !chat.at("model").is_string() ||
        !needs_responses_for_tools(chat.at("model").get<std::string>())) {
        return false;
    }
    const auto tools = chat.find("tools");
    if (tools == chat.end() || !tools->is_array() || tools->empty()) {
        return false;
    }
    const auto effort = chat.find("reasoning_effort");
    return effort == chat.end() || *effort != "none";
}

namespace {

// A field Chat Completions has and the Responses API does not.
ApiError no_responses_equivalent(std::string_view field, std::string_view model) {
    return ApiError{
        "invalid_input",
        std::format("{}: {} answers calls with tools through OpenAI's Responses API, which has no "
                    "such field; leave it out",
                    field,
                    model),
        std::format("/request/{}", field)};
}

// A chat message's content as Responses content: a string stays a string; text parts become
// `input_text` parts, or `output_text` for the assistant's own earlier answer.
Json responses_content(const Json& content, std::string_view part_type) {
    if (!content.is_array()) {
        return content;
    }
    Json parts = Json::array();
    for (const Json& part : content) {
        parts.push_back({{"type", part_type}, {"text", part.value("text", std::string())}});
    }
    return parts;
}

// A tool result's content as the text of a `function_call_output`.
std::string content_text(const Json& content) {
    if (content.is_string()) {
        return content.get<std::string>();
    }
    std::string text;
    if (content.is_array()) {
        for (const Json& part : content) {
            text += part.value("text", std::string());
        }
    }
    return text;
}

bool is_unset(const Json& request, const char* field) {
    const auto found = request.find(field);
    return found == request.end() || found->is_null();
}

Json chat_usage(const Json& usage) {
    if (!usage.is_object()) {
        return nullptr;
    }
    const std::uint64_t input = count(usage, "input_tokens").value_or(0);
    const std::uint64_t output = count(usage, "output_tokens").value_or(0);
    Json out{{"prompt_tokens", input},
             {"completion_tokens", output},
             {"total_tokens", count(usage, "total_tokens").value_or(input + output)}};
    if (const auto details = usage.find("input_tokens_details");
        details != usage.end() && details->is_object()) {
        if (const auto cached = count(*details, "cached_tokens")) {
            out["prompt_tokens_details"] = {{"cached_tokens", *cached}};
        }
    }
    if (const auto details = usage.find("output_tokens_details");
        details != usage.end() && details->is_object()) {
        if (const auto reasoning = count(*details, "reasoning_tokens")) {
            out["completion_tokens_details"] = {{"reasoning_tokens", *reasoning}};
        }
    }
    return out;
}

// Why a Responses answer stopped, as Chat Completions says it.
std::string chat_finish_reason(const Json& answer, bool called_tools) {
    if (called_tools) {
        return "tool_calls";
    }
    if (answer.value("status", std::string()) == "incomplete") {
        const Json details = answer.value("incomplete_details", Json());
        const std::string reason =
            details.is_object() ? details.value("reason", std::string()) : std::string();
        return reason == "content_filter" ? "content_filter" : "length";
    }
    return "stop";
}

std::string provider_failure_message(const Json& error) {
    if (error.is_object() && error.contains("message") && error.at("message").is_string()) {
        return error.at("message").get<std::string>();
    }
    return "no reason given";
}

} // namespace

std::expected<nlohmann::json, ApiError> chat_request_as_responses(const nlohmann::json& chat) {
    const std::string model = chat.value("model", std::string());
    for (const char* field :
         {"logit_bias", "stop", "seed", "top_logprobs", "functions", "function_call"}) {
        if (!is_unset(chat, field)) {
            return std::unexpected(no_responses_equivalent(field, model));
        }
    }
    if (const auto n = chat.find("n"); n != chat.end() && !n->is_null() && *n != 1) {
        return std::unexpected(no_responses_equivalent("n", model));
    }
    if (const auto logprobs = chat.find("logprobs");
        logprobs != chat.end() && !logprobs->is_null() && *logprobs != false) {
        return std::unexpected(no_responses_equivalent("logprobs", model));
    }
    for (const char* field : {"frequency_penalty", "presence_penalty"}) {
        if (const auto penalty = chat.find(field);
            penalty != chat.end() && !penalty->is_null() && *penalty != 0) {
            return std::unexpected(no_responses_equivalent(field, model));
        }
    }

    Json input = Json::array();
    const Json& messages = chat.at("messages");
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const Json& message = messages[i];
        const std::string role = message.at("role");
        const std::string path = "/request/messages/" + std::to_string(i);
        if (role == "function" || message.contains("function_call")) {
            return std::unexpected(ApiError{
                "invalid_input",
                std::format("messages: {} answers calls with tools through OpenAI's Responses "
                            "API; send tool calls as tool_calls and their results as role tool, "
                            "not the deprecated function call fields",
                            model),
                path});
        }
        if (role == "tool") {
            const auto call_id = message.find("tool_call_id");
            if (call_id == message.end() || !call_id->is_string()) {
                return std::unexpected(ApiError{
                    "invalid_input", "messages: a tool message needs its tool_call_id", path});
            }
            input.push_back({{"type", "function_call_output"},
                             {"call_id", *call_id},
                             {"output", content_text(message.value("content", Json()))}});
            continue;
        }
        const Json content = message.value("content", Json());
        const bool has_text = content.is_string() ? !content.get<std::string>().empty()
                                                  : content.is_array() && !content.empty();
        const bool has_calls = role == "assistant" && message.contains("tool_calls") &&
                               message.at("tool_calls").is_array() &&
                               !message.at("tool_calls").empty();
        if (has_text || !has_calls) {
            input.push_back(
                {{"role", role},
                 {"content",
                  content.is_null()
                      ? Json("")
                      : responses_content(content,
                                          role == "assistant" ? "output_text" : "input_text")}});
        }
        if (has_calls) {
            for (const Json& call : message.at("tool_calls")) {
                input.push_back({{"type", "function_call"},
                                 {"call_id", call.at("id")},
                                 {"name", call.at("function").at("name")},
                                 {"arguments", call.at("function").at("arguments")}});
            }
        }
    }

    Json tools = Json::array();
    for (const Json& tool : chat.at("tools")) {
        const Json& function = tool.at("function");
        Json out{{"type", "function"},
                 {"name", function.at("name")},
                 // Responses makes a function strict unless told otherwise; Chat Completions does
                 // not, and a schema written for it may not satisfy strict mode.
                 {"strict",
                  function.contains("strict") && function.at("strict").is_boolean()
                      ? function.at("strict")
                      : Json(false)}};
        if (function.contains("description") && function.at("description").is_string()) {
            out["description"] = function.at("description");
        }
        out["parameters"] = function.contains("parameters")
                                ? function.at("parameters")
                                : Json{{"type", "object"}, {"properties", Json::object()}};
        tools.push_back(std::move(out));
    }

    Json request{{"model", model},
                 {"input", std::move(input)},
                 {"tools", std::move(tools)},
                 {"store", false},
                 {"service_tier", "default"}};
    if (const auto choice = chat.find("tool_choice"); choice != chat.end() && !choice->is_null()) {
        request["tool_choice"] =
            choice->is_object()
                ? Json{{"type", "function"}, {"name", choice->at("function").at("name")}}
                : *choice;
    }
    for (const char* field : {"parallel_tool_calls", "temperature", "top_p", "user", "stream"}) {
        if (!is_unset(chat, field)) {
            request[field] = chat.at(field);
        }
    }
    if (!is_unset(chat, "reasoning_effort")) {
        request["reasoning"] = {{"effort", chat.at("reasoning_effort")}};
    }
    if (!is_unset(chat, "max_completion_tokens")) {
        request["max_output_tokens"] = chat.at("max_completion_tokens");
    } else if (!is_unset(chat, "max_tokens")) {
        request["max_output_tokens"] = chat.at("max_tokens");
    }
    Json text = Json::object();
    if (const auto format = chat.find("response_format");
        format != chat.end() && format->is_object()) {
        Json out{{"type", format->at("type")}};
        if (format->contains("json_schema")) {
            out.update(format->at("json_schema"));
        }
        text["format"] = std::move(out);
    }
    if (!is_unset(chat, "verbosity")) {
        text["verbosity"] = chat.at("verbosity");
    }
    if (!text.empty()) {
        request["text"] = std::move(text);
    }
    return request;
}

std::expected<nlohmann::json, ApiError> responses_answer_as_chat(const nlohmann::json& answer) {
    if (!answer.is_object()) {
        return std::unexpected(
            ApiError{"engine_error", "the model provider answered without a JSON object"});
    }
    if (answer.value("status", std::string()) == "failed") {
        return std::unexpected(
            ApiError{"engine_error",
                     "the model provider could not answer: " +
                         provider_failure_message(answer.value("error", Json()))});
    }
    std::string text;
    std::string refusal;
    Json calls = Json::array();
    const Json output = answer.value("output", Json::array());
    for (const Json& item : output.is_array() ? output : Json::array()) {
        const std::string type = item.value("type", std::string());
        if (type == "function_call") {
            calls.push_back({{"id", item.value("call_id", std::string())},
                             {"type", "function"},
                             {"function",
                              {{"name", item.value("name", std::string())},
                               {"arguments", item.value("arguments", std::string())}}}});
        } else if (type == "message") {
            const Json content = item.value("content", Json::array());
            for (const Json& part : content.is_array() ? content : Json::array()) {
                if (part.value("type", std::string()) == "output_text") {
                    text += part.value("text", std::string());
                } else if (part.value("type", std::string()) == "refusal") {
                    refusal += part.value("refusal", std::string());
                }
            }
        }
        // Reasoning items are the model's own; Chat Completions never shows them.
    }
    Json message{
        {"role", "assistant"},
        {"content",
         text.empty() && (!calls.empty() || !refusal.empty()) ? Json(nullptr) : Json(text)}};
    if (!refusal.empty()) {
        message["refusal"] = refusal;
    }
    if (!calls.empty()) {
        message["tool_calls"] = calls;
    }
    Json out{{"id", answer.value("id", std::string())},
             {"object", "chat.completion"},
             {"created", answer.value("created_at", std::int64_t{0})},
             {"model", answer.value("model", std::string())},
             {"choices",
              Json::array({{{"index", 0},
                            {"message", std::move(message)},
                            {"logprobs", nullptr},
                            {"finish_reason", chat_finish_reason(answer, !calls.empty())}}})}};
    if (const Json usage = chat_usage(answer.value("usage", Json())); !usage.is_null()) {
        out["usage"] = usage;
    }
    if (answer.contains("service_tier") && answer.at("service_tier").is_string()) {
        out["service_tier"] = answer.at("service_tier");
    }
    return out;
}

std::string ResponsesChatStream::feed(std::string_view piece) {
    std::string out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = piece.find('\n', start);
        if (end == std::string_view::npos) {
            partial_.append(piece.substr(start));
            return out;
        }
        if (partial_.empty()) {
            out += line(piece.substr(start, end - start));
        } else {
            partial_.append(piece.substr(start, end - start));
            const std::string whole = std::exchange(partial_, std::string());
            out += line(whole);
        }
        start = end + 1;
    }
}

std::string ResponsesChatStream::finish() {
    if (partial_.empty()) {
        return {};
    }
    const std::string last = std::exchange(partial_, std::string());
    return line(last);
}

std::string ResponsesChatStream::chunk(nlohmann::json delta, nlohmann::json finish_reason) const {
    const Json event{{"id", id_},
                     {"object", "chat.completion.chunk"},
                     {"created", created_},
                     {"model", model_},
                     {"choices",
                      Json::array({{{"index", 0},
                                    {"delta", std::move(delta)},
                                    {"finish_reason", std::move(finish_reason)}}})}};
    return "data: " + event.dump() + "\n\n";
}

std::string ResponsesChatStream::line(std::string_view text) {
    text = trim(text);
    if (done_ || !text.starts_with("data:")) {
        return {};
    }
    text = trim(text.substr(5));
    const Json event = Json::parse(text, nullptr, false);
    if (!event.is_object()) {
        return {};
    }
    const std::string type = event.value("type", std::string());
    const Json response = event.value("response", Json());
    if (response.is_object()) {
        id_ = response.value("id", id_);
        created_ = response.value("created_at", created_);
        model_ = response.value("model", model_);
    }
    if (type == "response.created") {
        return chunk({{"role", "assistant"}, {"content", ""}});
    }
    if (type == "response.output_text.delta") {
        return chunk({{"content", event.value("delta", std::string())}});
    }
    if (type == "response.refusal.delta") {
        return chunk({{"refusal", event.value("delta", std::string())}});
    }
    if (type == "response.output_item.added") {
        const Json item = event.value("item", Json());
        if (!item.is_object() || item.value("type", std::string()) != "function_call") {
            return {};
        }
        tool_outputs_.push_back(event.value("output_index", std::int64_t{-1}));
        return chunk({{"tool_calls",
                       Json::array({{{"index", tool_outputs_.size() - 1},
                                     {"id", item.value("call_id", std::string())},
                                     {"type", "function"},
                                     {"function",
                                      {{"name", item.value("name", std::string())},
                                       {"arguments", ""}}}}})}});
    }
    if (type == "response.function_call_arguments.delta") {
        const auto at =
            std::ranges::find(tool_outputs_, event.value("output_index", std::int64_t{-1}));
        if (at == tool_outputs_.end()) {
            return {};
        }
        return chunk({{"tool_calls",
                       Json::array({{{"index", at - tool_outputs_.begin()},
                                     {"function",
                                      {{"arguments", event.value("delta", std::string())}}}}})}});
    }
    if (type == "response.completed" || type == "response.incomplete") {
        done_ = true;
        std::string out =
            chunk(Json::object(), chat_finish_reason(response, !tool_outputs_.empty()));
        if (const Json usage = chat_usage(response.value("usage", Json())); !usage.is_null()) {
            const Json last{{"id", id_},
                            {"object", "chat.completion.chunk"},
                            {"created", created_},
                            {"model", model_},
                            {"choices", Json::array()},
                            {"usage", usage}};
            out += "data: " + last.dump() + "\n\n";
        }
        return out + "data: [DONE]\n\n";
    }
    if (type == "response.failed" || type == "error") {
        done_ = true;
        const Json error = type == "error" ? event : response.value("error", Json());
        const Json failure{
            {"error",
             {{"message",
               "the model provider could not answer: " + provider_failure_message(error)},
              {"type", "engine_error"},
              {"code", "engine_error"},
              {"param", nullptr}}}};
        return "data: " + failure.dump() + "\n\ndata: [DONE]\n\n";
    }
    return {};
}

} // namespace lake::api

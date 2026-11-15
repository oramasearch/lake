# The model gateway

**Issue:** [lake#248](https://github.com/oramasearch/lake/issues/248), part of the metering epic
[lake#658](https://github.com/oramasearch/lake/issues/658) · **Code:** `include/lake/api/model_gateway.hpp`,
`src/api/model_gateway.cpp`, `model_completion` in `src/api/usage_capabilities.cpp`, the
`/openai/v1/*` routes in `src/api/http_server.cpp`; the workspace's model in
`src/api/workspace_settings_capabilities.cpp` ([lake#1230](https://github.com/oramasearch/lake/issues/1230))
· **Tests:** `tests/test_model_gateway.cpp`

Every model call an agent makes for a workspace goes through Lake. Lake sends it to the model
provider with Orama's own key, counts the tokens the provider reports as the workspace's
`model_tokens`, and refuses the call once the account's monthly `model_tokens` limit is used up.
Agents hold no provider key, so there is no other way for them to reach a model, and nothing
depends on an agent reporting what it spent.

## Why one point in Lake

The alternative was for each agent to report its own tokens (`record_model_usage`). That trusts every
agent, including the code people write for their own agents on agent-runtime, to report every call,
and it leaves Orama's provider key in every agent pod, where a person's agent code can read it. The
gateway counts at the one place every call must pass:

- the caller is identified by the credential Lake already gives each run (below), so the workspace,
  agent and run are Lake's own records, not something the agent says;
- the tokens are the provider's own `usage` field in the answer Lake relays;
- the provider key is a platform environment variable of Lake's (`LAKE_OPENAI_API_KEY`), never in an
  agent pod or a user's secret store.

## Calling it

The gateway speaks the OpenAI API, so an agent uses the OpenAI SDK unchanged, with two settings:

```python
from openai import OpenAI

client = OpenAI(base_url=f"{lake_url}/openai/v1", api_key=lake_token)
client.chat.completions.create(model="gpt-6.1-sol", messages=[...])
```

For an agent turn (a task credential) the `model` asked for is replaced by the
[workspace's model](#the-workspaces-model).

| Route | Body | Answer |
|---|---|---|
| `POST /openai/v1/chat/completions` | OpenAI's chat completion request | the provider's answer |
| `POST /openai/v1/responses` | OpenAI's Responses request | the provider's answer |
| `POST /openai/v1/embeddings` | OpenAI's embeddings request | the provider's answer |

`lake_url` is Lake's address (an agent pod's `LAKE_URL`). `lake_token` is the run's own credential:

| Credential | Who holds it | Counted in |
|---|---|---|
| task, `lat.<task>.<secret>` | a library agent, for one turn: the `lake_token` in the claimed task's body (Ask Orama, the Project Agent, Application Agent, Experiments Agent, Auto-Suggest Agent, Agent Creator Agent, the Eval Generator Agent and the two corpus generators) | the task's workspace, labelled with the agent and the task id |
| launch, `lal.<launch>.<secret>` | an agent launched on agent-runtime (`LAKE_TOKEN`) | the launch's workspace, labelled with the agent's name and the launch id |
| schedule, `las.<schedule>.<secret>` | a scheduled agent's runs | the schedule's workspace, labelled with the agent's name and the schedule id |

Anything else is refused before the provider is called: a person's key or session and the operator
credential are `unauthorized` (403), and so is a library agent's bus credential, which names no turn.
No credential is `unauthenticated` (401). A task credential stops working when its task finishes, and
each call renews the task's lease, as every call with it does.

**Under a workspace path.** The same routes answer at `/w/<workspace>/openai/v1/<endpoint>`. An agent
launched on agent-runtime runs in its workspace's namespace with `LAKE_URL` set to lake followed by
`/w/<workspace>` (orama-operator `docs/lake/agents.md`), so its SDK's base URL `<LAKE_URL>/openai/v1`
lands there. The workspace in the path must be the run's own: a call naming another is `unauthorized`
(403) before the provider is called, and one naming a workspace that does not exist is `not_found`
(404). That is the only way such an agent reaches a model: its pod holds no provider key
([orama-operator#779](https://github.com/oramasearch/orama-operator/issues/779)).

The path is `/openai/v1/`, not `/v1/`, because `/v1/<name>` is the capability registry's
([README](README.md)). Like the OCI registry at `/v2/` and the Hub routes at `/api/models/`, the
gateway is a protocol route named for its protocol.

### Supported requests

The gateway accepts a closed set of synchronous, token-metered fields (lake#960).
Unknown fields are `invalid_input` before the provider is called, including unknown fields inside
messages, content parts, tools and stream options. Chat and Responses accept text and function
tools executed by the caller. Embeddings accept text or token arrays. Function parameter schemas
and structured-output schemas remain application data; their property names are unrestricted.

Provider-hosted tools (search, file search, code execution, image generation and remote MCP),
audio/image/file inputs or outputs, saved prompts, conversations and stored-response references are
refused. See [The processing tier](#the-processing-tier) for `service_tier`. `store` must be false or null and is sent as false on
generation endpoints, including when omitted. Background generation remains refused; false and
null are accepted for SDK compatibility. There is no stored-response retrieval route and no
customer ownership registry for provider objects, so their identifiers cannot be reused here.

An agent sends the provider's assistant message back as history on its next tool round, as the
OpenAI SDK dumps it (`message.model_dump(exclude_none=True)`). OpenAI's chat answer carries fields
that only describe the answer: `annotations` (`[]` without a hosted tool), `refusal` and `audio`.
On an `assistant` message the gateway drops `annotations` and `refusal`, and `audio` when it is
null, before validating; they are never forwarded. A non-null `audio` names audio the provider
stored and is refused, as is any other unknown field, and the same fields on another role are
refused. Verify with `ModelCompletion.AcceptsTheProvidersAssistantMessageBackAsHistory`, which
replays a chat completion with a tool call as OpenAI returns it.

This deliberately narrows OpenAI compatibility: a provider adding a field does not enable it on
Lake. Extend the endpoint schemas in `src/api/model_gateway.cpp` only after verifying that its
charges are represented in workspace usage, with a refusal test and a supported-request test.
Model eligibility and pricing are handled separately by lake#961.

Verify with `ModelCompletion.RefusesUnmeteredFeaturesBeforeCallingTheProvider` and
`ModelGateway.AcceptsTextAndCallerExecutedFunctions`, plus the OpenAI route rejection checks in
`ModelGatewayRoute.AnswersTheOpenAIShapeForATaskCredential`. REST, MCP and both OpenAI route
forms call the same preparation function before any provider work.

### The processing tier

The gateway never asks the provider for a costlier processing mode (lake#1230). OpenAI's Fast mode
costs 2x, Ultrafast 6x, priority more again, and regional processing adds 10%; none of them is
covered by the [price list](billing.md#rates). So:

- `service_tier` may only be `default` or null on `chat/completions` and `responses`. Any other
  value (`priority`, `flex`, `auto`, `scale`, a Fast or Ultrafast tier) is `invalid_input` at
  `/request/service_tier` before the provider is called. `auto` is refused too, because it lets the
  account's provider setting choose.
- Every chat or responses request is sent with `service_tier: "default"`, also when the caller left
  it out, so the account's own default can never choose a costlier tier.
- Lake never asks for regional processing. That matters for cache writes: they cost 1.25x input and
  are billed as input at 1.3x, which covers them, but with the 10% regional surcharge
  (1.25 x 1.1 = 1.375) they would not be covered.
- Batch and Flex (half price) are not used either.

Verify with `ModelGateway.RefusesACostlierServiceTierAndAlwaysAsksForTheStandardOne` and the
`service_tier: priority` case of `ModelCompletion.RefusesUnmeteredFeaturesBeforeCallingTheProvider`.

### Streaming

`stream: true` is accepted. For a chat completion Lake adds `stream_options.include_usage: true`, so
the provider reports the stream's usage in its last event (the Responses API always does, in
`response.completed`). The answer is `text/event-stream` with the provider's events, unchanged, and
Lake relays them as they arrive (lake#665): the outbound client hands the provider's body on piece by
piece, and the route writes each piece to the caller in chunked transfer, so an agent that shows
tokens as they come gets them as the provider sends them, not all at once at the end.

- Lake reads the usage from the events as they pass and counts it from the event that carries it,
  once the stream ends. A stream that ends without one is counted by
  [estimate](#an-answer-without-usage) and audited `usage_reported: false`, as an answer without
  `usage` is.
- A refusal before the first event (Lake's or the provider's) is answered as any other, with its
  status and [OpenAI's error shape](#errors). Once the first event is sent the status is 200, so a
  failure after it (the provider drops the connection, the deadline passes) ends the stream early;
  the call is audited with its failure and counted by estimate, since the provider charges for what
  it generated (lake#939).
- A caller that stops reading is sent nothing more: its stream ends when it takes no bytes for the
  server's stream idle timeout, or falls more than 8 MiB behind the provider, or when it drops the
  connection. The provider is not stopped. Lake reads the rest of its stream, without relaying it,
  up to the provider's 600-second deadline, and counts the usage in its last event (lake#939). The
  whole answer is billed to the account, so ending a stream early never makes its tokens free.

`POST /v1/model_completion` and the MCP tool answer the whole stream at once, as `events`, after the
provider has finished it. None of the agents stream today.

### Tools on the gpt-6 models

**Issue:** [lake#1245](https://github.com/oramasearch/lake/issues/1245) · **Code:**
`responses_for_tools_models`, `chat_goes_to_responses`, `chat_request_as_responses`,
`responses_answer_as_chat` and `ResponsesChatStream` in `src/api/model_gateway.cpp`; the call in
`model_completion` (`src/api/usage_capabilities.cpp`).

OpenAI refuses a Chat Completions request that carries function tools for the gpt-6.x models:
*"Function tools with reasoning_effort are not supported for gpt-6.1-sol in /v1/chat/completions. To
use function tools, use /v1/responses or set reasoning_effort to 'none'."* The refusal comes whether
or not the request names `reasoning_effort`, because these models reason by default. The library
agents call Chat Completions with tools (the OpenAI SDK pointed at Lake), so every agent turn with
tools failed on gpt-6.1-sol in lake v0.7.2, and v0.7.3 made gpt-5.5 the default as a hotfix. The
gateway now sends such a call to the provider's Responses API and answers it as a chat completion,
so the agents work on the gpt-6 models without a change of their own, and gpt-6.1-sol is the
default again.

**When.** A `chat/completions` request, from any caller (an agent turn, `model_completion` over REST
or MCP, either OpenAI route), whose model (after the [workspace's choice](#the-workspaces-model)) is
in `responses_for_tools_models` and which carries at least one tool. A request that asks for
`reasoning_effort: "none"` stays on Chat Completions, which accepts tools then on the models that
offer `none` (gpt-6.1-terra, gpt-6-luna; gpt-6.1-sol and gpt-6-astra refuse `none` and the provider
says so). The table, one line per model, is:

| Model | Why |
|---|---|
| `gpt-6.1-sol`, `gpt-6.1-terra`, `gpt-6-astra`, `gpt-6-luna` | each refuses tools on Chat Completions (checked against the live API, 2026-10-26) |

A dated snapshot of a listed model (`gpt-6.1-sol-2026-...`) is included. **gpt-5.5 is not listed**: it
accepts tools on Chat Completions, with and without `reasoning_effort` (checked live the same day),
so it stays there. A call without tools, a Responses request and an embedding are never touched.

**The request** sent to `POST /v1/responses`:

| Chat Completions | Responses |
|---|---|
| `messages`: `system`, `developer`, `user`, `assistant` text | `input` message items, in order, with the same role (text parts become `input_text`, or `output_text` for the assistant) |
| an assistant message's `tool_calls` | one `function_call` item each (`call_id`, `name`, `arguments`), after the message's text if it has any |
| a `role: tool` message | a `function_call_output` item (`call_id` from `tool_call_id`, `output` the content's text) |
| `tools[].function` | `{type: "function", name, description, parameters, strict}`; **`strict` is false unless the tool says otherwise**, because Responses makes a function strict by default and rewrites its schema (every property required), which Chat Completions does not; a tool without `parameters` gets an empty object schema |
| `tool_choice` | the same string, or `{type: "function", name}` |
| `reasoning_effort` | `reasoning: {effort}` |
| `max_completion_tokens`, else `max_tokens` | `max_output_tokens` |
| `response_format` | `text.format` (`json_schema`'s name, description, schema and strict moved up a level) |
| `verbosity` | `text.verbosity` |
| `parallel_tool_calls`, `temperature`, `top_p`, `user`, `stream` | the same |
| `store`, `service_tier` | `false` and `default`, [as always](#the-processing-tier) |

Fields Responses has no equivalent for are `invalid_input` at their path before the provider is
called: `n` other than 1, `logprobs: true`, `top_logprobs`, `logit_bias`, `stop`, `seed`, a non-zero
`frequency_penalty` or `presence_penalty`, and the deprecated `functions`, `function_call` and
`role: function`. Null values are ignored. `temperature` and `top_p` are passed on as asked: the
gpt-6 models (and gpt-5.5) refuse them on Responses, and the provider's own message says so.

**The answer** is a chat completion: `id` and `model` the provider's, `created` its `created_at`,
`choices[0].message` with the answer's text as `content` (null when the answer is only tool calls)
and its `tool_calls` (`id` the `call_id`, `function.name`, `function.arguments`), and
`finish_reason` `tool_calls` when it called a tool, `length` when it stopped at `max_output_tokens`,
`content_filter` when it was filtered, else `stop`. `usage` is `prompt_tokens` (input tokens),
`completion_tokens` (output tokens, reasoning included), `total_tokens`, and
`prompt_tokens_details.cached_tokens` and `completion_tokens_details.reasoning_tokens` when the
provider reports them. Reasoning items are not shown, as Chat Completions never shows them. An answer
whose `status` is `failed` is `engine_error` with the provider's message, after its tokens are counted.

**Streaming.** A streamed call is sent as a Responses stream and rewritten, as it arrives, into Chat
Completions chunks: the role chunk on `response.created`, a `content` delta per
`response.output_text.delta`, a `tool_calls` delta (`index`, `id`, `name`) per function call added and
an `arguments` delta per `response.function_call_arguments.delta`, the finishing chunk and the usage
chunk (`choices: []`) on `response.completed` or `response.incomplete`, then `data: [DONE]`. A
`response.failed` or `error` event becomes `data: {"error": {...}}`, which the OpenAI SDK raises, then
`[DONE]`. The usage is read from the provider's own events, as for any stream. No library agent
streams today (checked across every `agent-*` repo's `dev` on 2026-10-26).

**Counting and audit.** The tokens are the provider's `usage`, counted exactly as for a chat
completion under the model called, long prompts included ([What is counted](#what-is-counted)). The
audit keeps `endpoint: chat/completions` and adds `provider_endpoint: responses`.

**Verify** with `ModelGateway.AToolCallingTurnOnGpt6RoundTripsThroughTheResponsesAPI` (request, tool
call, tool result and final answer, both ways, byte for byte),
`ModelGateway.TranslatesEveryChatFieldResponsesHasAndRefusesTheRest`,
`ModelGateway.OnlyToolCallsOnTheListedModelsGoToTheResponsesAPI`,
`ModelGateway.RewritesAResponsesStreamAsChatChunksHoweverItsPiecesFall`,
`ModelCompletion.AnAgentTurnWithToolsOnGpt61SolIsSentToResponsesAndCounted`,
`ModelCompletion.Gpt55AndCallsWithoutToolsStayOnChatCompletions`,
`ModelCompletion.PassesOnTheResponsesAPIsRefusalAndCountsNothing`,
`ModelCompletion.RelaysATranslatedStreamAsChatChunksAndCountsItsUsage`, and the tools call at the end
of `ModelGatewayRoute.AnswersTheOpenAIShapeForATaskCredential`. On a Lake with a provider key, an
agent turn with tools on a workspace that chose no model answers instead of failing in under a second.

**To add a model** that refuses tools on Chat Completions, add its line to
`responses_for_tools_models` (`include/lake/api/model_gateway.hpp`) and to the table above.

### Errors

A refusal is OpenAI's error shape, with Lake's documented code ([errors.md](errors.md)) as `type`
and `code`, and the JSON pointer of the field at fault as `param`:

```json
{"error": {"message": "model: name the model, such as gpt-5.5", "type": "invalid_input",
           "code": "invalid_input", "param": "/request/model"}}
```

| Code | Status | When |
|---|---|---|
| `invalid_input` | 400 | an unsupported field, content or tool; no allowed/priced `model`; a `service_tier` other than `default`; a workspace model this Lake no longer serves; streamed embeddings; background work or stored references |
| `unauthenticated`, `unauthorized` | 401, 403 | no credential, or one that is not a run's (above), or a `/w/<workspace>/` path naming another workspace than the run's |
| `not_found` | 404 | a `/w/<workspace>/` path naming a workspace that does not exist |
| `usage_limit_reached` | 429 | the account's monthly `model_tokens` limit is used up ([Limits](#limits)) |
| `engine_error` | 422 | the provider refused the request; the message is the provider's own |
| `rate_limited` | 429 | the provider is busy or over its quota |
| `store_unavailable` | 503 | the provider is down, unreachable, or refused Lake's key (its message, which may echo part of the key, is not passed on) |
| `not_supported` | 501 | this Lake has no provider key (`LAKE_OPENAI_API_KEY`) |

The OpenAI SDK retries 429 and 5xx answers itself.

## The capability: `model_completion`

The routes are a rendering of one registry capability, as `/workspaces/<customer>/zetta/v1/*` is of
`workspace_query` and `workspace_act`. REST is `POST /v1/model_completion`; MCP lists it as a tool.

| Field | Type | Meaning |
|---|---|---|
| `endpoint` (input) | `chat/completions`, `responses` or `embeddings` | the OpenAI API path |
| `request` (input) | object | the OpenAI request body |
| `workspace_id`, `agent`, `launch_id` | string | the run the call was counted for |
| `model` | string | the model the provider says answered (else the one called) |
| `input_tokens`, `output_tokens` | integer | the provider's `usage` |
| `usage_reported` | boolean | false when the provider's answer carried no `usage`: the tokens are an [estimate](#an-answer-without-usage), and audited so |
| `recorded` | boolean | the tokens were added to the workspace's usage |
| `streamed` | boolean | the request streamed |
| `response` | object | the provider's answer, when not streamed |
| `events` | string | the provider's whole event stream, when streamed (REST and MCP; the `/openai/v1/*` route relays the stream itself, [Streaming](#streaming)) |

It requires no grant (`requires_auth` false, like `record_model_usage`): the handler authorizes it by
the credential's kind and finds the run from it.

## What is counted

Each answered call adds two values to the run's workspace's `lake_usage` counter
([metering.md](metering.md#usage-over-time)): `model_tokens` with `kind: input` and with
`kind: output`, each labelled `agent`, `launch_id` (the task, launch or schedule id) and `model`. Those
are the labels `usage_over_time` breaks `model_tokens` down by (`by_agent`, `by_launch`, `by_model`),
which the Usage dashboard and an agent's page show. `model` is the exact name of the model called:
for an agent turn, the [workspace's model](#the-workspaces-model), not the one the agent asked for.
Provider snapshot names are audit detail (`answered_model`). Each value also carries a billing
`class`: `<model>/input` or `<model>/output`, exposed in `by_class` and priced separately
([billing.md](billing.md#price-list)).

### Long prompts

OpenAI prices a request whose prompt is longer than 272,000 input tokens at 2x input and 1.5x output
**for the whole request**, on the gpt-6.x models and gpt-5.5. Billing such a request at the short
rate (1.3x) would not cover it. So when the provider reports more than 272,000 input tokens
(`long_context_prompt_tokens` in `model_gateway.hpp`), both values of the call are recorded under the
long-context classes `<model>@long/input` and `<model>@long/output`, priced at 2x input and 1.5x
output plus 30% ([billing.md](billing.md#rates)). At exactly 272,000 the plain classes apply. A model
without long-context rates (gpt-5, gpt-4o, the embedding model) keeps its one pair of classes
(`has_long_context_rate`: names starting `gpt-6`, and `gpt-5.5`). With billing on, a model that has
long-context rates needs them published as well as its plain ones before it is served, because the
prompt's length is known only from the provider's answer. `record_model_usage` classes a report the
same way, by its `input_tokens`.

Verify with `ModelCompletion.ALongPromptIsCountedUnderTheLongContextClasses` (272,000 and 272,001
prompt tokens) and the long-context cases of
`ModelCompletion.BillingRequiresBothPositiveExactModelPricesBeforeDispatch`. Raw token counts and monthly token limits are unchanged.
A refused call, by Lake or by the provider, counts nothing.
A call the provider answered counts even when its answer did not arrive whole (lake#939).
A transport failure without an HTTP status is also counted by estimate against the originating
run: a lost response cannot establish that the provider did no billable work (lake#960).
An explicit non-success HTTP response counts nothing.

### An answer without usage

The provider has charged for every answer it began, so Lake never counts one as zero. When the
answer reports no `usage` (it failed part way, its stream ended before its last event, or an
OpenAI-compatible provider sends none), Lake counts one token for every four bytes, rounded up: the
request as sent to the provider as `input`, and the answer's bytes that arrived as `output`. That is
about what OpenAI's tokenizer makes of English text; an answer's bytes include its JSON and, for a
stream, each event's framing, so the estimate of an answer is above the text it carried. The audit
says `usage_reported: false` (`estimated_model_tokens` in `model_gateway.hpp`).

A Responses request with `background` set is refused `invalid_input` before the provider is called:
its answer would come back before the model has generated anything and report no usage, and the
gateway has no route to read the result.

Verify: `ModelGatewayRoute.CountsAStreamItsCallerAbandoned` drops a caller's connection after the
first event over the real server and finds the provider's usage counted;
`ModelCompletion.CountsAStreamWhoseCallerStoppedReading`, `CountsAnAnswerThatFailedPartWay`,
`CountsARelayedStreamWithoutUsageByEstimate`, `HoldsAnEstimateOfTheInputWhileTheProviderAnswers` and
`ModelGateway.RefusesABackgroundAnswer` cover the rest (`tests/test_model_gateway.cpp`).

## Limits

`set_usage_limit {customer_id, dimension: "model_tokens", limit}` is the account's monthly limit, and
the gateway enforces it: once this UTC month's `model_tokens` across the account's workspaces reach
the limit, every model call is refused with `usage_limit_reached` before the provider is called.
`usage_limits` reports the limit as `enforced: true`. The month's use is read from the workspaces at
most once a minute per account and Lake process, and the tokens a process counts in between are added
to it, so a limit can be passed by about a minute of the account's calls on each Lake replica. While a
call waits on the provider, an [estimate](#an-answer-without-usage) of its input is held against the
limit, so calls made together cannot all pass a limit only the first should reach (lake#939); the
call's own count replaces it when it ends.

## Audit

Each call is one event in the run's workspace audit ([audit.md](audit.md#workspace-audit)), object
`agent_usage/<launch_id>`, action `model_completion`, with `endpoint`, the model called (`model`), the
one the agent asked for when the workspace's model replaced it (`agent_asked_model`), the one that
answered (`answered_model`), `streamed`, `provider_status`, `input_tokens`, `output_tokens` and `usage_reported`.
The prompt and the answer are never in the audit.

## Configuration

| Variable | Meaning |
|---|---|
| `LAKE_OPENAI_API_KEY` | The operator's provider key, a platform environment variable of the Lake pod (for example `spec.env` from a Secret). Unset, the gateway answers `not_supported` |
| `LAKE_MODEL_PROVIDER_URL` | an OpenAI-compatible base URL; default `https://api.openai.com/v1` |
| `LAKE_MODEL_GATEWAY_MODELS` | JSON array of exact model/provider objects, such as `[{"model":"gpt-5.5","provider":"openai"}]`; absent defaults to `gpt-6.1-sol`, `gpt-6.1-terra`, `gpt-6-astra`, `gpt-6-luna`, `gpt-5.5`, `gpt-5`, `gpt-4o` and `text-embedding-3-small`; `[]` disables all model calls |

Only the `openai` provider protocol is supported, using the configured base URL and key.
Malformed configuration, duplicate model names and unsupported providers stop startup. Names are
case-sensitive and never trimmed, decoded or expanded to snapshot or fine-tuning aliases. An
unlisted name returns `invalid_input` at `/request/model` before any provider call, through the
OpenAI routes (including workspace paths), REST and MCP alike (lake#961).

With billing off, the configured allowlist is sufficient. With billing on, the current UTC month's
published price list must also have exactly one positive `model_tokens` price for each of
`<model>/input` and `<model>/output`, with positive `per`, and for a model with
[long-context rates](#long-prompts) also `<model>@long/input` and `<model>@long/output`. A model with
no price in force is never served. A missing list, blended rate alone,
missing direction or zero rate refuses the model with `invalid_input`; a failed price read refuses
with `store_unavailable`. Even an embedding model needs both rates, although it normally reports no
output tokens. Publish approved rates with `set_price_list` before enabling a model. Choose rates
for the configured provider; an allowlist entry never derives prices from its name.

**The default models are priced by the schema** (the provider's published
per-token price plus 30%). Migration `0095_model_token_prices` seeds them once, on a fresh and an
existing control plane alike, in millionths of a US dollar per 1,000,000 tokens, from
OpenAI's published standard price ([pricing](https://developers.openai.com/api/docs/pricing), read 2026-10-23):

| Model | OpenAI input / output (per 1M) | Lake input / output (per 1M) | `unit_micros` input / output |
|---|---|---|---|
| `gpt-5.5` | $5.00 / $30.00 (under 272K context) | $6.50 / $39.00 | 6500000 / 39000000 |
| `gpt-5` | $1.25 / $10.00 | $1.625 / $13.00 | 1625000 / 13000000 |
| `gpt-4o` | $2.50 / $10.00 | $3.25 / $13.00 | 3250000 / 13000000 |
| `text-embedding-3-small` | $0.02 / none | $0.026 / $0.026 | 26000 / 26000 |

Migration `0105_agent_model_prices` (lake#1230) adds the gpt-6.x models and the long-context rates the same way, from
the same page and the model pages, read 2026-10-26:

| Model | OpenAI input / output (per 1M) | Lake input / output (per 1M) | `unit_micros` input / output | `@long` input / output |
|---|---|---|---|---|
| `gpt-6.1-sol` | $2.00 / $10.00 | $2.60 / $13.00 | 2600000 / 13000000 | 5200000 / 19500000 |
| `gpt-6.1-terra` | $1.00 / $5.00 | $1.30 / $6.50 | 1300000 / 6500000 | 2600000 / 9750000 |
| `gpt-6-astra` | $10.00 / $50.00 | $13.00 / $65.00 | 13000000 / 65000000 | 26000000 / 97500000 |
| `gpt-6-luna` | $0.10 / $0.50 | $0.13 / $0.65 | 130000 / 650000 | 260000 / 975000 |
| `gpt-5.5` | (0095's rates above) | | | 13000000 / 58500000 |

An embedding reports no output tokens, so its output rate bills nothing; it exists because the
gateway needs both. Cached input (5% of input at OpenAI) is billed at the full input rate, and a cache
write (1.25x input) as input at 1.3x, so both are covered; see [The processing tier](#the-processing-tier)
for why regional processing would break that. Before 0105, a `gpt-5.5` prompt over 272K tokens was
billed at the short rate, which did not cover it; it is now billed at its `@long` rates. The seed writes a new
publication for the UTC month it runs in, holding the rates of the list then in force (none on a
fresh instance) plus these, and adds them to any list published for a later month; a rate already
published for one of these classes is kept, and a list in another currency is left alone. 0105 follows the same rules (a
fresh instance therefore has two publications for its first month, 0095's and 0105's). Each runs
only while its own migration is not recorded, so re-applying the schema never re-adds a
rate an operator later removed. Change a rate with `set_price_list`; when OpenAI's prices move,
publish the new rates (provider price × 1.3) and update this table.

The operator can change the allowlist by setting `LAKE_MODEL_GATEWAY_MODELS` and restarting Lake.
Publish model/direction rates through `set_price_list`; the gateway reads the effective list on
every call. Migration `0095_model_token_prices` extends the price and invoice class checks for
model/direction names while retaining GPU restrictions. Before deployment the operator must
vendor `db/schema.sql` and select `model_token_prices_guard`; this change does not edit the operator.
For 0105 the operator must vendor `db/schema.sql` again and select `agent_model_prices_guard` before
an image with lake#1230 rolls: without the 0105 rates, a Lake with billing on refuses the gpt-6.x
models and gpt-5.5 (its long-context rates are missing).
The default list deliberately restricts previously
accepted arbitrary names; add other approved models explicitly.

Agent pods need neither: they reach the model through Lake. The operator's
`spec.agents.openaiSecretName`, which used to put the key into every agent pod, is left unset once the
agents use the gateway.

## The workspace's model

People choose the model their agents use (lake#1230), with `gpt-6.1-sol` as the
default. Each workspace has a setting `agent_model`, one of the chat models this Lake
serves (the configured list without embedding models), read with `get_workspace_settings` and set with
`set_workspace_settings` ([workspaces.md](workspaces.md#settings)). **Unset means `gpt-6.1-sol`**
(`default_agent_model`). Its tool calls go through the provider's Responses API
([Tools on the gpt-6 models](#tools-on-the-gpt-6-models)); v0.7.3 defaulted to gpt-5.5 for a day
until that was built (lake#1245, [v0.7.3](../releases/v0.7.3.md)).

When an agent turn's task credential calls `chat/completions` or `responses`, the gateway first checks
that the model the agent asked for is a served one (an unlisted name is still `invalid_input`), then
reads the workspace's setting from its Zetta and sends the call with **that** model, whatever the agent
asked for. Every library agent therefore follows the choice without a change of its own, and the
operator's `spec.agents.model` (the `AGENT_MODEL` each agent pod is given) only says what the agents
ask for. The call is then checked, priced, counted and audited under the model called.

What the setting does not touch:

- `embeddings` keep the model asked for;
- a launched or scheduled agent (a launch or schedule credential) runs the workspace's own code, which
  names its model itself, and calls what it names;
- `record_model_usage` reports, which name the model that was used.

A chosen model this Lake no longer serves (the operator narrowed `LAKE_MODEL_GATEWAY_MODELS`) is
refused with `invalid_input`, naming the model and saying to choose another; Lake never swaps in a
different, differently priced model. A workspace whose settings cannot be read refuses the call with
that error rather than guess.

Verify with `ModelCompletion.AnAgentTurnCallsItsWorkspacesModelAndUnsetMeansGpt61Sol` (the default,
a choice, the Responses API, embeddings and a launched agent left alone, usage under the model called,
a model no longer served) and `Stores/WorkspaceRecordStores.AWorkspaceChoosesItsAgentModelAmongTheServedChatModels`.

## Lake's own model calls

Lake itself calls a model only for speech-to-text (`transcribe`, and `create_transcript` from audio),
which is metered as `speech` by the minute in the workspace it is used in, dictation included
(lake#613), and never also as `model_tokens`: a transcription model's tokens are audit detail
only, so no recording is billed twice ([transcripts.md](transcripts.md#how-long-a-recording-is),
lake#658). Lake makes no other model call (no clean-up or naming of its own).

## `record_model_usage`

`record_model_usage` stays for a model call made outside Lake, such as a model an agent runs itself.
A call through the gateway is already counted and must not be reported again. New reports use the
same model/direction billing classes; they do not dispatch a provider call or grant model access.
With billing on, a report without positive exact input and output prices is refused with
`invalid_input` at `/model_name` before any tokens are recorded. This prevents an agent from
inserting an unpriced class that blocks the account's invoice. Reporting a priced outside model
does not require or change the gateway allowlist, because it grants no provider access. With
billing off, outside reports are accepted as before.

## Verifying model entitlement and pricing

Run `ModelCompletion.RejectsUnlistedModelsBeforeEveryEndpointCallsTheProvider`,
`ModelCompletion.BillingRequiresBothPositiveExactModelPricesBeforeDispatch`,
`ModelGateway.ParsesAnExactModelAllowlistAndRejectsBadConfiguration`, and
`ModelGatewayRoute.AnswersTheOpenAIShapeForATaskCredential`. They cover unknown and altered names,
configuration replacement and disabling, each endpoint, protocol and REST routes, missing or
incomplete prices, and raw usage labelled by the requested model despite a provider snapshot.
`Billing.ModelInputAndOutputHaveExactPricesAndCannotFallBackToTheBlendedRate` verifies separate
input/output invoice amounts and refuses bills after a used model's prices are removed.
`PostgresBilling.ModelDirectionPricesAndInvoiceLinesRoundTrip` verifies the migration, persisted
rates and invoice lines, and continued rejection of model/direction classes on GPU prices.
`PostgresBilling.ModelTokenPricesAreSeededOnceOnFreshAndExistingInstances` checks the seeded rates
on a fresh database, and on a simulated existing one that the list in force is kept, a new one
carries its rates plus the seeded ones (an operator's own model rate kept), a later list gets them,
and a second run adds nothing. `ModelCompletion.ASeededInstanceCallsTheDefaultModelsWithBillingOnAtTheSeededPrices`
makes a model call with billing on against a freshly seeded PostgreSQL control plane and checks
the bill prices its input and output at the seeded rates.

`PostgresBilling.AgentModelPricesAreSeededOnceOnFreshAndExistingInstances` checks 0105 the same
way, with an operator's own `gpt-6.1-sol/input` rate kept and a list in another currency left alone.
`ModelGateway.ServesTheGpt6ModelsAndOnlyChatModelsAreChoosable` checks the default list.

`RecordModelUsage.UnpricedReportsCannotPoisonABilledAccountsLedger` verifies that rejected reports
record no model tokens, priced outside reports grant no gateway access, and billing-off reports
still work.

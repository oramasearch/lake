# The model gateway

**Issue:** [lake#248](https://github.com/oramasearch/lake/issues/248), part of the metering epic
[lake#658](https://github.com/oramasearch/lake/issues/658) · **Code:** `include/lake/api/model_gateway.hpp`,
`src/api/model_gateway.cpp`, `model_completion` in `src/api/usage_capabilities.cpp`, the
`/openai/v1/*` routes in `src/api/http_server.cpp` · **Tests:** `tests/test_model_gateway.cpp`

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
client.chat.completions.create(model="gpt-5.5", messages=[...])
```

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

The gateway accepts a closed set of synchronous, token-metered fields (lake#960, LSR-094).
Unknown fields are `invalid_input` before the provider is called, including unknown fields inside
messages, content parts, tools and stream options. Chat and Responses accept text and function
tools executed by the caller. Embeddings accept text or token arrays. Function parameter schemas
and structured-output schemas remain application data; their property names are unrestricted.

Provider-hosted tools (search, file search, code execution, image generation and remote MCP),
audio/image/file inputs or outputs, priority service tiers, saved prompts, conversations and
stored-response references are refused. `store` must be false or null and is sent as false on
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
Model eligibility and pricing are handled separately by lake#961 (LSR-095).

Verify with `ModelCompletion.RefusesUnmeteredFeaturesBeforeCallingTheProvider` and
`ModelGateway.AcceptsTextAndCallerExecutedFunctions`, plus the OpenAI route rejection checks in
`ModelGatewayRoute.AnswersTheOpenAIShapeForATaskCredential`. REST, MCP and both OpenAI route
forms call the same preparation function before any provider work.

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

### Errors

A refusal is OpenAI's error shape, with Lake's documented code ([errors.md](errors.md)) as `type`
and `code`, and the JSON pointer of the field at fault as `param`:

```json
{"error": {"message": "model: name the model, such as gpt-5.5", "type": "invalid_input",
           "code": "invalid_input", "param": "/request/model"}}
```

| Code | Status | When |
|---|---|---|
| `invalid_input` | 400 | an unsupported field, content or tool; no `model`; streamed embeddings; background work or stored references |
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
| `model` | string | the model the provider says answered (else the one asked for) |
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
which the Usage dashboard and an agent's page show, and the bill prices `model_tokens` at one rate
([billing.md](billing.md#price-list)). A refused call, by Lake or by the provider, counts nothing.
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
`agent_usage/<launch_id>`, action `model_completion`, with `endpoint`, the model asked for and the one
that answered, `streamed`, `provider_status`, `input_tokens`, `output_tokens` and `usage_reported`.
The prompt and the answer are never in the audit.

## Configuration

| Variable | Meaning |
|---|---|
| `LAKE_OPENAI_API_KEY` | Orama's provider key, a platform environment variable of the Lake pod (on the dev cluster, `spec.env` from Secret `lake-prod-openai`). Unset, the gateway answers `not_supported` |
| `LAKE_MODEL_PROVIDER_URL` | an OpenAI-compatible base URL; default `https://api.openai.com/v1` |

Agent pods need neither: they reach the model through Lake. The operator's
`spec.agents.openaiSecretName`, which used to put the key into every agent pod, is left unset once the
agents use the gateway ([deployment/aks.md](../deployment/aks.md#speech-to-text-key)).

## Lake's own model calls

Lake itself calls a model only for speech-to-text (`transcribe`, and `create_transcript` from audio),
which is metered as `speech` by the minute in the workspace it is used in, dictation included
(lake#613), and never also as `model_tokens`: a transcription model's tokens are audit detail
only, so no recording is billed twice ([transcripts.md](transcripts.md#how-long-a-recording-is),
lake#658). Lake makes no other model call (no clean-up or naming of its own).

## `record_model_usage`

`record_model_usage` stays for a model call made outside Lake, such as a model an agent runs itself.
A call through the gateway is already counted and must not be reported again.

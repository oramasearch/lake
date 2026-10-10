# Ask Orama

Ask Orama is the assistant at the top of every page. Ask it anything about your lake in plain words:
your repositories, projects, data, runs, models, services and agents. It answers from what you are
allowed to read, proposes every change for you to confirm, and brings in another agent when that
agent does the job better.

## Concepts

- **It works as you, and never on its own.** Each turn gets a key made from your own credential for
  that turn only. The key carries every read and nothing that changes anything, and is revoked when
  the turn ends. Ask Orama can read only what you can read.
- **Changes are proposals.** When Ask Orama wants to change something (create a repository, start
  an agent, …) it shows a **proposal** with the capability it would call. Nothing happens until you
  press **Confirm**; lake then runs it as you, with your permissions, and records it in the audit.
  Only a proposal lake stored in your own conversation can be confirmed.
- **It delegates.** Ask Orama hands work to the right agent: the Project Agent for pipelines,
  ApplicationAgent for apps, ExperimentsAgent for training studies, Auto-Suggest for ideas, or an
  agent you built. Their proposals are reviewed the same way before anything is written.
- **Conversations live in the workspace.** Every message is kept in the workspace Zetta of the
  workspace you asked in, sealed with its key. Another workspace never shows it, and only you can
  read your conversation back.
- **Agents work in the background.** After you send a message the page returns at once and shows
  who is working. You can leave and come back; the answer is kept.

## Open Ask Orama

- Press **Ask Orama** in the top bar. It opens beside the page, in the current workspace, and knows
  which page you asked from. Drag its edge to resize it (or use the arrow keys on the handle;
  double-click resets it).
- The **Full view** button opens the same conversation as a full page (`/ask`).
- The **Start a new conversation** button begins another.
- On a phone, Ask Orama fills the screen ([Using Lake on a Phone](/docs/phone)).

Ask Orama needs a workspace. With none selected it says so and links to **Ship → Workspaces**.

## Ask a Question

1. Type in the message box (*What should I look at first?*). On a keyboard, Enter sends and
   Shift+Enter starts a new line; on a touch screen, Return starts a new line and **Send** sends.
2. Or press the microphone (**Dictate: speak your message**) and speak; your words are added to
   the box to edit before you send ([Transcripts and Speech-to-Text](/docs/transcripts)).
3. Press **Send**.

While the turn runs, the conversation says *Waiting for Ask Orama to start…* and then *Ask Orama is
working…*, how long it has worked, its latest progress, and *Attempt 2 of 3* when lake retried it.
When it brings in another agent you see that too (*Ask Orama asked ExperimentsAgent: …*), then that
agent's progress. **View this turn in Streams** opens the turn's events in **Train → Streams**.

Through the API, a turn is a few calls. Send the message with `POST /v1/send_ask_message`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "message": "What do I have?", "page": {"path": "/design/project", "name": "churn"}}
```

Leave out `conversation_id` to start a new conversation, or send it to continue one. `page` (the
page you asked from, as its path and query values) is optional. The answer names the
`conversation_id`, your `message_id` and the agent's `task_id`.

Follow the task with `GET /v1/get_agent_task?customer_id=<customer id>&workspace_id=<workspace id>&task_id=<task id>`
until its status is final. Then store the answer in the conversation once with
`POST /v1/settle_ask_turn`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "conversation_id": "<conversation id>", "task_id": "<task id>", "kind": "ask"}
```

Read the conversation back with
`GET /v1/get_ask_conversation?customer_id=<customer id>&workspace_id=<workspace id>&conversation_id=<conversation id>`.
Through MCP the tools have the same names and inputs: `send_ask_message`, `get_agent_task`,
`settle_ask_turn`, `get_ask_conversation`.

## Talk to Another Agent

The **Talk to** selector under the message box picks who answers: Ask Orama first, then the Agent
Library's other running chat agents, then the chat agents your workspace built. Internal tooling
agents are never offered. An agent of your workspace answers at once; a library agent answers in
the background, as above.

Through the API, send `agent` with the agent's id in `send_ask_message`, or start a turn directly
with `POST /v1/agent_turn`:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "agent": "agent-ask-orama", "message": "Which of my runs scored best?"}
```

Through MCP, call `agent_turn` with the same input. `GET /v1/list_agent_library` lists the library's
agents and whether each runs on your lake ([Agents](/docs/agents)).

## Confirm a Proposal

An answer that wants to change something shows **Proposed:** with the capability and its impact,
and a **Confirm** button.

1. Read the proposal: what it calls and with which input.
2. Press **Confirm**. lake runs it as you and adds *Done: …* or *That did not work: …* to the
   conversation. A confirmed proposal shows **Confirmed** and is never offered again.

A change to an existing pipeline goes to the Project Agent instead, and the answer shows **Review
the proposed change**, which opens the pipeline page where you apply or discard it
([Projects and Pipelines](/docs/design)). An app goes to ApplicationAgent, whose full proposal you
review before **Confirm and deploy** ([Apps](/docs/apps)); a training study goes to ExperimentsAgent,
whose study you edit and confirm before it starts ([Experiments](/docs/experiments)).

Through the API, confirm with `POST /v1/confirm_ask_proposal`, naming the stored message and the
proposal's position in it:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "conversation_id": "<conversation id>", "message_id": "<message id>", "index": 0}
```

Through MCP, call `confirm_ask_proposal`. A capability that refuses is still a successful confirm,
answered with `ok` false and the error.

## Ask About Something on the Page

Some pages open Ask Orama about what they show. On a transcript, **Ask Orama about this** opens it
with a chip naming the transcript and its text attached to your next message
([Transcripts and Speech-to-Text](/docs/transcripts)). The chip's close button asks without that
context. After one message, later messages are about the page as usual.

## Permissions

- Ask Orama reads what you may read, in the workspace you asked in. Sending a message needs `write`
  on the account that owns the workspace, because it stores your message there.
- Confirming a proposal needs whatever that capability needs; a proposal you are not allowed to run
  is refused like any other call.
- A key you make for an AI agent can be limited to reads ([Tokens and Keys](/docs/tokens)).

## Limits

- A turn is retried at most twice; after the third failure it ends with *That did not finish: …*.
- The panel keeps the current conversation for the browser tab; the full page's address names the
  conversation, so it can be linked and reopened.
- A transcript sent as context is cut at 20,000 characters, and says so.

## Troubleshooting

| You see | What to do |
|---|---|
| *Ask Orama works in a workspace, and you have none selected* | Choose a workspace in the top bar, or start one on **Ship → Workspaces**. |
| An agent is not offered in **Talk to** | It is not deployed on your lake, is internal tooling, or does not chat. **Design → Agent Library** says which agents run. |
| *That proposal is not in this conversation* (`not_found`) | Only proposals lake stored in your conversation can be confirmed. |
| `workspace_not_ready` | The workspace is not ready; conversations live in it. |
| `invalid_input` at `/workspace_id` | The workspace id is unknown or not one of the account's. |
| *That did not finish: …* | The agent failed after its retries; send the message again. |

## Related Pages

- [Agents](/docs/agents)
- [Transcripts and Speech-to-Text](/docs/transcripts)
- [Using Lake on a Phone](/docs/phone)
- [Streams](/docs/streams)
- [Security Model](/docs/security)

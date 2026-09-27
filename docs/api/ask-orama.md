# Ask Orama

**Status: implemented 2026-09-27 (lake#236, epic lake#228).** Ask Orama is the general agent for all
of lake and the first entry of the Agent Library. Its code is
[`oramasearch/agent-ask-orama`](https://github.com/oramasearch/agent-ask-orama); this page is how lake
shows it and keeps it honest.

## What a person sees

- **Ask Orama** in the top bar of every signed-in page. It opens `/ask?from=<the page>`, so the
  conversation knows where it started.
- `/ask`: the conversation, a **Talk to** selector (Ask Orama first, then the library's other running
  chat agents, then the space's own chat-capable agents), and a message box. *Start a new
  conversation* begins another.
- Each of Ask Orama's **proposals** (a change it wants to make) is shown with its capability and its
  impact, and a **Confirm** button. A confirmed proposal shows *Confirmed* and is never offered again.

## How it stays honest

- **It works as the person.** A turn goes through `agent_turn` ([agent-launches.md](agent-launches.md)),
  which hands Ask Orama a key for that turn only, derived from the person's own credential and
  carrying **every read capability and `agent_turn`, never an action**. The key is revoked when the
  turn ends.
- **It never changes anything itself.** A change comes back as a proposal. **Confirm** runs it in
  lake, as the person, through the ordinary registry path with its own authorization and audit.
  Only a proposal lake stored in that person's conversation can be confirmed: a forged index or
  another person's message is refused.
- **A change to an existing pipeline** goes to the Project Agent with `{project, graph}`; its answer is a
  proposal and the conversation shows *Review the proposed change*, a link to the pipeline page where
  it is applied (lake#249).
- **It delegates to any agent** (`delegate` → `agent_turn`): the Project Agent, Auto-Suggest, and any
  agent the person built; an unattended agent is started by proposing `launch_agent`.
- **Conversations are lake's.** Every message is an `ask_messages` row (migration `0030`): the
  person's text, and for Ask Orama's the reply with its proposals, reads, delegations and trace id.
  Only the person who had a conversation reads it back, from any replica.

## Routes

| Route | What it does |
|---|---|
| `GET /ask?customer_id=&conversation=&from=&agent=` | the conversation |
| `POST /ask/send` | stores the message, sends one `agent_turn` to the chosen agent with `page.path`, stores the answer |
| `POST /ask/confirm` | runs one stored proposal (`message_id`, `index`) as the person and records the outcome |

## Verify

`HttpServer/HttpResponseIsolation.AskOramaAnswersProposesAndRunsOnlyWhatThePersonConfirms`: the link
on every page, a turn carrying the page and a turn key, the proposal shown, nothing created before
confirming, a forged confirm refused, the real one creating the repository as the person, and
*Done* plus *Confirmed* afterwards.

# When a resource fails: notifications, repair, change and auto-fix

lake#1207. Steve, 2026-10-25: *"let's build a skill to notify the user and allow them to try and fix
the issue, with additionally auto commits/drips, or additional instructions to modify/update a app
with more instructions"*.

When something a person deployed fails, Lake tells them where they will see it, offers an agent to
fix it, and, if they opt in, lets the agent fix it by itself a bounded number of times. The same
agent can also change the resource from more instructions.

The mechanism is **general over the resource kind**. A notification and a repair record name a
`resource_kind` and a `resource_id`. Apps (`app`, repaired by the Application Agent) are the only
kind today; custom agents built in Agent Factory (`agent`, repaired by agent-creator) are
[lake#1209](https://github.com/oramasearch/lake/issues/1209), which plugs into the same records,
capabilities and pass ([below](#adding-a-resource-kind-lake1209)).

## Where it lives

Both are workspace data (umbrella rule: everything goes in the workspace Zetta), so both are sealed
records in the workspace Zetta of the resource's workspace, found by the resource's account
([workspace records](../architecture/workspace-records.md)). Lake's Postgres gains nothing; there
is no migration and nothing for the operator to vendor.

| Collection | Key | What |
|---|---|---|
| `lake_resource_notifications` | `<event>.<kind>.<resource id>.<generation or build id>` | one notification: `event` (`rollout_failed`, `build_failed`, `app_errors`, `auto_fix_stopped`), the reason in plain words, what the resource reported (`detail`), the phase and generation, the person it is for (`recipient_user_id`, who deployed it), `resolved_at`, and the people who dismissed it |
| `lake_resource_repairs` | `<kind>.<resource id>` | one resource's repair settings: who deployed it (`deployed_by`), the source it was last deployed from, `auto_fix`, `max_attempts`, `attempts`, the generations already attempted, the running attempt's task, whether auto-fix stopped and why, and the repair and change conversations |

The key of a notification is derived from what it is about, so the same failure is recorded once
however many times the pass sees it.

`create_app` and `deploy_app` (and `rollback_app`) record who deployed the app and its source in its
repair record. An app deployed from a built image keeps no source in its desired state; this is
where the agent finds its repository. A workspace that cannot answer leaves the deployment as it
is.

## The pass

The store root's owner runs one pass with the MCP connection streams (`run_resource_repair_pass`,
`src/api/repair_capabilities.cpp`), so two lakes never notify twice or start two repairs. For
every running app of every workspace:

- **A failed rollout** is one that the operator reports `Degraded`, `Failed` or `Error`, or a message
  naming `CrashLoopBackOff`, `ImagePullBackOff` or `ErrImagePull`, at the generation it was asked
  for (a rollout still in progress is not judged). It records `rollout_failed` for that generation.
- **A failed build** is the app's newest image build ending `Failed`. It records `build_failed`; a
  newer build that succeeds resolves it.
- **Repeated script errors in a healthy app** (lake#1228). A Streamlit or FastAPI app whose code
  raises on every page stays Running and Ready, so the rollout says nothing. For a running app that
  is Ready **and has auto-fix on** (and only then, so other apps cost no log read), the pass reads
  its runtime log through `app_logs`' source: the newest 500 lines of the last five minutes
  (`app_errors_line_limit`, `app_errors_window_seconds`). A failure is Streamlit's
  `Uncaught app execution` or Uvicorn's `Exception in ASGI application`, each followed by a
  traceback. At least two in the window (`app_errors_threshold`; one bad request is not a failure)
  record `app_errors` for that generation: its reason is the newest traceback's exception line
  (`AttributeError: 'NoneType' object has no attribute 'sort_values'`), its `detail` that traceback.
  Auto-fix then starts its one attempt for the generation, exactly as for a failed rollout. Once a
  read finds fewer than two, the notification is resolved. Without a log source (lake#1206) or when
  the read fails, nothing is recorded or resolved. The repair turn reads `app_logs` itself and sees
  the traceback. `app_errors_in_logs` (`include/lake/api/resource_repairs.hpp`) is the detector.
- **An app Ready again** at its generation, with no repeated script errors, resolves its
  `rollout_failed` and `auto_fix_stopped` notifications.
- **Auto-fix**, when on and not stopped, starts one `repair` turn per failed generation, as the person
  who turned it on (their own permissions, balance gate, audit and metering). It waits while that
  turn runs. It stops when the attempts are spent (`Auto-fix tried 3 times and it still fails`) or
  when an attempt finished without producing a new generation, and records `auto_fix_stopped`.

The reasons are plain words: a crash loop says the app "starts and then stops again, over and over"
and names the usual causes (an error at start, a missing setting or secret, the wrong port);
`ImagePullBackOff` says Lake cannot fetch the image; a failed probe says the app does not answer on
its port. The operator's message is kept beside it as `detail`.

The pass's counters: `lake_repair_passes_total`, `lake_notifications_total`,
`lake_auto_fix_attempts_total`, `lake_auto_fix_stopped_total`, `lake_repair_problems_total`, and
`lake_repair_log_reads_total` (app logs read for script errors).

## Capabilities

REST, MCP and OpenAPI expose them like every capability; their audit goes to the workspace.

| Capability | Kind | Input beyond `customer_id`, `workspace_id` | Answer |
|---|---|---|---|
| `list_notifications` | read | `resource_kind?`, `resource_id?`, `include_dismissed?`, `include_resolved?` | `{notifications, open}`, newest first; each with `for_you` when it is for the caller |
| `dismiss_notification` | action (read on the account) | `notification_id` | the notification, dismissed for the caller only |
| `get_auto_fix` | read | `resource_kind`, `resource_id` | the repair settings |
| `set_auto_fix` | action (write) | `resource_kind`, `resource_id`, `enabled`, `max_attempts?` (1 to 10, default 3) | the repair settings; turning it on resets the attempts and records the caller as the person attempts act as |
| `repair_app` | action (write, paid work) | `app_id`, `message?`, `notification_id?`, `conversation_id?` | the `agent_turn` answer (`task_id`) of a `repair` turn |
| `change_app` | action (write, paid work) | `app_id`, `message`, `conversation_id?` | the `agent_turn` answer of a `change` turn |
| `get_app_conversation` | read | `app_id` | `{app_id, conversation_id, turns, truncated}`: the caller's own change conversation about the app, oldest turn first, each turn as `get_agent_task` answers it plus `request` (what the person wrote) and `confirmation` (lake#1240) |

`repair_app` and `change_app` start their turn through `agent_turn`, so the turn carries the caller's
own permissions, the balance gate, audit (`app.repair.start`, `app.change.start`, then
`agent.turn`) and metering. Without a `conversation_id` they continue the app's last repair
conversation, or the caller's own change conversation, so later instructions go on in the same one.

### A change is a conversation (lake#1240)

- **Each person has their own change conversation per app.** The repair record keeps
  `change_conversations` (user id → conversation id) in the workspace Zetta beside
  `change_conversation_id` (the newest, for records written before). `agent_turn` refuses to continue
  someone else's conversation, so `change_app` continues only the caller's own; a record from before
  is continued only by the person whose turn is its newest.
- **The thread is the conversation's chain of turns.** `agent_turn` records each turn's previous turn
  of the same agent and kind as `prior_turn` in its request (sealed in the workspace with the turn,
  lake#374), so the thread is followed back from the newest turn with no new index or table. The walk
  stops at another person's turn and at 50 turns (`truncated`). A confirmation is an ordinary turn of
  the thread (`agent_turn` kind `change` with the conversation id), and the conversation goes on after
  it.
- **The agent is handed the thread.** A `repair` or `change` turn of the Application Agent with
  earlier turns carries `history` in its request, oldest first, at most 20 turns: each with
  `task_id`, `message` (what the person wrote), `reply`, `status`, `outcome` (the result's kind:
  `question`, `proposal`, …), `diff` (the proposed diff, cut at 8,000 bytes) and `confirmed` on a
  confirmation. `prior_turn` stays as it was.
- **agent-application does not read `history` yet.** It keeps a conversation's model history in its
  process memory only (lost on restart, not shared between replicas), and reads neither `prior_turn`
  nor `history`. Seeding its history from `history` when its memory has none is the change it needs
  (agent-application `service.py` `State.history` / `run_turn`).

To verify: `AgentRuns.AChangeConversationKeepsItsTurnsAndHandsThemToTheAgent` and
`ResourceRepairs.EachPersonContinuesTheirOwnChangeConversation`.

## The turns

An Application Agent turn of kind `repair` or `change` names its app as `context.app_id`
(`agent_turn` with `agent: agent-application`, `kind: repair|change`). Lake checks the app is a live
app of the workspace and hands the turn `context.app` (as `get_app` answers it) and
`context.repair` (its repair record). The turn reads with the person's credential: the app, its
events and logs (`app_logs`, which may answer nothing until lake#1206), its builds and build logs,
its repository, and `get_auto_fix`.

It writes only in two cases, and then only `commit_repository_files`, `build_app`, `set_app_env`
(for the `APP_DEPS` a changed manifest needs) and `deploy_app`; never `create_app`,
`create_repository` or `set_app_auth`:

1. **Confirmed.** The person reviewed the proposal (the diagnosis and the diff) and Lake checked
   it as for any Application Agent proposal ([apps.md](apps.md#application-agent-and-ship-apps)):
   `proposal_task_id` is a finished turn of the same kind, `proposal_sha256` its digest, and
   `context.confirmed_proposal` equal to it. Its `app.id` must be the `context.app_id` (else
   `invalid_input` at `/context/confirmed_proposal/app/id`). A repair or change keeps the app's
   access, so it asks nothing about public access.
2. **Auto-fix.** `context.auto_fix: true` on a `repair` turn, accepted only when the app's auto-fix is
   on, not stopped and attempts remain (else `invalid_input` or `conflict` at `/context/auto_fix`).
   Lake counts the attempt before the turn starts, records the generation and the task, audits it as
   `app.auto_fix.attempt` and hands the agent `context.auto_fix: {attempt, max_attempts}`.

`get_agent_task` answers a repair or change proposal's `review_url` as the app page
(`/ship/app?…&task=<id>#proposal`) and its `proposal_sha256`.

### How the fix is written: a drip

The agent commits the changed files to the app's repository on `main` with `commit_repository_files`
in one commit, `Repair the <name> App: <diagnosis>` (`Change …`, `Auto-fix …`). Each file written
is a liquid drip (a captured, signed write) cut into one change, attributed to the person whose turn
it is, so every repair can be seen in the repository's history and reverted. App source stays in its
workspace repository; nothing about it is copied anywhere else. Then the agent rebuilds the image
when new packages need it, deploys the new revision, follows the rollout and reports in the same
conversation. The agent's side is agent-application's `docs/building/repair.md`.

## Adding a resource kind (lake#1209)

Custom agents plug in without a second system:

1. `control::known_resource_kind` and `resource_kind_schema()` accept `agent`.
2. `repairer_of("agent")` names agent-creator and the context key (`agent_id`).
3. `owned_resource` resolves an agent of the workspace.
4. The pass gains an agent detector beside `observe_app`: the agent's failure signals (a launch or
   run that failed, its error) as an `Observed` with a generation, and when it is healthy again.
5. `agent_turn` gives agent-creator `repair` and `change` turns that write only that agent (its
   own `prepare_*_turn`, as `prepare_app_turn` does for apps), with the same confirm and auto-fix
   checks against the same repair record.
6. `repair_agent` and `change_agent`, beside `repair_app` and `change_app`; the agent page uses the
   same `NotificationBanner` and auto-fix card.

`list_notifications`, `dismiss_notification`, `get_auto_fix`, `set_auto_fix`, the records and the
console's notification list need no change.

## How to verify

```sh
LAKE_TEST_SKIP_STALE_SWEEP=1 ./build/tests/lake-tests \
  --gtest_filter='AgentRuns.AFailedRollout*:AgentRuns.ARepairTurn*:AgentRuns.AutoFix*:AgentRuns.AChangeTurn*:AgentRuns.AHealthyApp*:AppErrors.*'
```

- `AFailedRolloutRecordsOneNotificationForThePersonWhoDeployedIt`: a crash-looping rollout records
  one notification for the deployer, a failed build another, dismissal hides it for that person, and
  Ready resolves the rollout's.
- `ARepairTurnProposesThenWritesOnlyThatAppOnceConfirmed`: the first turn reads only; a confirmation
  for another app or of another kind is refused; the confirmed turn may commit and redeploy that app,
  and never create one.
- `AutoFixRepairsWithoutAConfirmAndStopsAfterItsAttempts`: off by default; on with two attempts,
  two repair turns run as the person, then auto-fix stops and says so, refuses a third, and turning
  it on again resets it.
- `AChangeTurnTurnsInstructionsIntoAProposalAndContinuesTheConversation`.
- `AHealthyAppWhoseScriptKeepsFailingIsRepairedWhenAutoFixIsOn` (lake#1228): with auto-fix off the
  log is not read; one traceback starts nothing; two record one `app_errors` notification with the
  exception line and start one repair; the same generation is not attempted twice; the notification
  resolves when the errors stop.
- `AppErrors.RepeatedScriptFailuresAreCountedAndTheNewestExceptionIsNamed`: Streamlit and Uvicorn
  markers, newest-first reads, multi-line entries and chained tracebacks.

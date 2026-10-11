# Getting Started

lake is where you **design, train and ship AI**. You build an **AI solution** in an **AI project**,
whose pipeline is the workflow from your data to a result; lake keeps the data it learns from and
runs what you ship ([Concepts and Glossary](/docs/concepts)). This page takes you from your first sign-in to a model you can call from your own code.
Each step says where to click, then gives the same step as a REST call and an MCP tool, for when
you automate it.

## What You Need

- **An account.** Open lake and choose **Sign in** in the top bar (or **Create account** where your
  lake allows sign-up). [Your Account and Sign-In](/docs/account) covers every way in.
- **A workspace.** Everything you design, train and ship lives in a workspace: an isolated Orama
  Zetta cluster of its own. Your account can have many workspaces, one per environment for example.
- **A key, for scripts and agents.** The site needs no key. Scripts, `curl` and AI agents use a lake
  API key made on **Developer → API** or **Developer → MCP** ([Tokens and Keys](/docs/tokens)).

## Find Your Way Around

Signing in, or opening lake while signed in, lands you on **Ask Orama**, full page, in your current
workspace. Ask it anything about your lake in plain words; while the conversation is empty it
suggests questions about your own datasets, projects and runs. It is the same conversation the
**Ask Orama** button opens beside any other page ([Ask Orama](/docs/ask-orama)).

The rail on the left groups lake's pages:

| Group | Pages | What they are for |
|---|---|---|
| **Home** | Ask Orama, Dashboard | Ask about anything in your lake; the whole platform at a glance |
| **Data** | Repositories, Shared with Me, Search, Datasets, Transcripts | Where your data lives and is found |
| **Design** | Projects, Opportunities, Solutions, Methodologies, Agent Library | Deciding what AI to build, and designing its workflow |
| **Train** | Agents, Runs, Experiments, Models, Streams | Training models and agents, and watching them work |
| **Ship** | Models, Search Indexes, Packages, Apps, Deployments, Services, Workspaces | Putting what you built to work |
| **Organization** | Organizations, Secrets, Usage, Activity | People, credentials, usage and history |
| **Developer** | API, MCP, Docs | Calling lake from code and from AI agents |

The top bar holds the workspace picker, **Go to…** (press ⌘K or `/`), the theme switch, **Ask
Orama** and your account menu. On a phone the rail is a drawer behind the menu button
([Using Lake on a Phone](/docs/phone)).

## 1. Start Your Workspace

1. Open **Design → Projects**. If you have no workspace yet, the page shows **Start Your Workspace**.
2. Press **Start your workspace**. lake asks for a Zetta cluster of your own; the page shows
   *Preparing Your Workspace* and checks again every 15 seconds until it is ready.
3. To add another workspace later, open **Ship → Workspaces** and press **New workspace**.

The page address now starts with `/w/<workspace id>/`. That id is the `workspace_id` every Design,
Train and Ship call names. Every call also names `customer_id`: the id of the account that owns your
workspaces (yours, or your organization's). Replace `<customer id>` and `<workspace id>` below with
yours; `list_workspaces` lists both.

- REST: `POST /v1/create_workspace` with
  `{"customer_id": "<customer id>", "name": "analytics", "environment": "dev"}`. The answer's `id` is
  the workspace id. Then poll
  `GET /v1/workspace_status?customer_id=<customer id>&workspace_id=<workspace id>` until `ready` is
  true.
- MCP: `create_workspace`, then `workspace_status`, with the same inputs.

A call without `workspace_id` is refused with `workspace_id is required`: lake never guesses which
workspace you mean. Details: [Workspaces](/docs/workspaces).

## 2. Load Your Data

1. Open **Data → Datasets** and press **Load data**.
2. Keep **Upload**, choose a CSV or JSONL file of up to 16 MiB, and name the dataset, for example
   `accounts` (letters, digits and underscores, starting with a letter).
3. Press **Upload**. The page says how many rows arrived and links to the new dataset.

- REST: `POST /v1/workspace_act` with this body:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "upload_dataset", "input": {"table_name": "accounts", "format": "csv", "content": "account,plan,churned\na1,pro,false\n"}}
```

- MCP: `workspace_act` with the same input.

Other ways in (a database you connect, generated data, read-only SQL): [Datasets and Loading
Data](/docs/load-data). Recordings and meeting notes go to **Data → Transcripts**
([Transcripts and Speech-to-Text](/docs/transcripts)).

## 3. Create a Project

1. Open **Design → Projects** and press **New project**.
2. Give it a **Name** (lowercase letters, digits, `-` and `_`), a **Description**, and **The
   decision it should improve**, for example *which accounts customer success calls first each
   week*.
3. Press **Create project**. The project opens.

- REST: `POST /v1/workspace_act` with this body:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "create_project", "input": {"name": "retention", "description": "Keep accounts from churning", "decision": "Which accounts customer success calls first each week"}}
```

- MCP: `workspace_act` with the same input.

A name already in use answers `conflict`. More: [AI Projects and Pipelines](/docs/design).

## 4. Build and Approve a Pipeline

1. On the project, press **Build pipeline**.
2. Under **What should it predict or answer?** write the goal, for example *Predict the churned
   column of accounts*. Under **From data** choose `accounts`. Leave **Methodology** to let Zetta
   choose, or pick one the workspace runs.
3. Press **Build pipeline**. The pipeline opens, drawn as steps joined by arrows.
4. Press **Approve**. A pipeline must be approved before it runs, and again after any change.

Through the API the same steps are `workspace_act` calls: `capture_intent` with `{project, text}`,
then `compile_intent` with `{project, input_id}`, then `approve_plan` with `{project, graph}`. For
example, approving:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "capability": "approve_plan", "input": {"project": "retention", "graph": "<pipeline name>"}}
```

To change the pipeline, use **Ask the Project Agent** on its page: it proposes edits you review,
then apply or discard ([AI Projects and Pipelines](/docs/design)).

## 5. Train It

1. On the pipeline page, press **Check** to run the correctness checks without training anything.
2. Under **Train on** choose `accounts`, keep **Hold out for measuring** at 0.2 (a fifth of the rows
   is kept out of training to measure the model honestly), and keep or change **Model name**.
3. Press **Check and run**. The run opens in **Train → Runs**.

- REST: `POST /v1/run_pipeline` with this body:

```json
{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "project": "retention", "graph": "<pipeline name>", "dataset_path": "datasets.accounts", "holdout_fraction": 0.2, "model_name": "retention_churn"}
```

- MCP: `run_pipeline` with the same input. It answers `gate` (the checks) and `run`, or `gate` alone
  when a check failed.

The run page leads with the score on rows the model did not see, for example *Held-out AUC 0.708 on
412 rows the model did not see*. Then it shows what the model learned from, what it leans on, the
rows most at risk and what the run cannot tell you ([Train a Model](/docs/train)).

## 6. Publish and Call the Model

1. Open **Ship → Models** and press **Publish v1** on your model (the button names the version it
   publishes). **Train → Models** is where you compare its versions and retrain it.
2. Press **Call** on it. The drawer shows the call with your account, workspace and model filled in.
3. Call it from your own code with a lake key in `LAKE_KEY`:

```
curl -s https://lake.oramadev.net/v1/predict \
  -H "Authorization: Bearer $LAKE_KEY" -H 'Content-Type: application/json' \
  -d '{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "model_id": "<model id>", "rows": [{"entity": "a1", "support_tickets": 11}]}'
```

From an AI agent it is the MCP tool `predict`. **Unpublish** stops every outside call at once
([Models and Search Indexes](/docs/ship)).

## Where to Go Next

| To | Read |
|---|---|
| Keep code and files in lake | [Repositories](/docs/repositories), [Sharing and Shared with Me](/docs/sharing) |
| Find anything | [Search](/docs/search) |
| Turn recordings into searchable text | [Transcripts and Speech-to-Text](/docs/transcripts) |
| Start from a ready-made business solution | [AI Solutions, Methodologies and Opportunities](/docs/solutions) |
| Build agents and tune models | [Agents](/docs/agents), [Experiments](/docs/experiments), [Streams](/docs/streams) |
| Ship apps, images and databases | [Apps](/docs/apps), [Packages, Builds and the Registry](/docs/registry), [Deployments and Environments](/docs/deployments), [Services and Storage](/docs/services) |
| Ask in plain words | [Ask Orama](/docs/ask-orama) |
| Bring in colleagues | [Organizations, Members and Teams](/docs/organizations), [Grants and Effective Access](/docs/access), [Single Sign-On and SCIM](/docs/sso) |
| Keep credentials and watch usage | [Secrets and Encryption Keys](/docs/secrets), [Activity and Audit](/docs/activity), [Usage and Metering](/docs/usage) |
| Automate | [REST API and OpenAPI](/docs/developer), [MCP Setup](/docs/mcp), [The liquid CLI](/docs/cli) |
| Understand the guarantees | [Security Model](/docs/security) |

## Troubleshooting

| You see | What it means and what to do |
|---|---|
| *Start Your Workspace* on every Design, Train and Ship page | The workspace was never started. Press **Start your workspace**. |
| *Preparing Your Workspace* | The workspace Zetta is being provisioned. The page checks again every 15 seconds. |
| *Your Workspace Could Not Be Prepared* | The operator could not provision it; the banner gives the reason. **Cancel workspace** withdraws the request. |
| `workspace_not_ready` | A call reached a workspace that is not ready yet. Poll `workspace_status`. |
| `workspace_id is required` | Every Design, Train and Ship call names its workspace. |
| `unauthenticated` (401) | Your key is missing, wrong or expired. Make a new one on **Developer → API**. |
| `unauthorized` (403) | Your key is valid but may not do this here. Ask an administrator of the organization ([Grants and Effective Access](/docs/access)). |

More answers: [FAQ and Troubleshooting](/docs/faq).

## Related Pages

- [Concepts and Glossary](/docs/concepts)
- [Workspaces](/docs/workspaces)
- [Your Account and Sign-In](/docs/account)
- [REST API and OpenAPI](/docs/developer)

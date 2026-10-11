# Concepts and Glossary

Checked against lake's source on 2026-10-10.

lake is where you **design, train and ship AI**. What you build is an **AI solution**: an AI
system that answers one business problem end to end. You build it in an **AI project**, and the
project's **pipeline** is the workflow that builds it: Design decides what to build, Train builds
and measures it, Ship puts it in front of the people and systems that use it.

## How an AI Solution Is Built

| Step | What happens | Where |
|---|---|---|
| **1. Pick the problem** | Choose the business question and the decision the AI should improve. Start from a ready-made AI solution in Orama's catalogue, an opportunity Auto-Suggest found in your data, or a blank project. | **Design → Solutions**, **Opportunities**, **Projects** |
| **2. Design the workflow** | The AI project's pipeline lays out the steps from your data to a result: connect, clean, model, index, serve, act. You review and approve it. | **Design → Projects** |
| **3. Train and measure** | Running the pipeline trains the models and measures them on held-out data. Experiments try many settings to find the best. | **Train → Runs**, **Experiments** |
| **4. Ship** | Publish the model or search index, and put it in an app or behind an API, for people and systems to use. | **Ship** |

Agents help at every step: Ask Orama answers and hands work on; the Project Agent designs and
changes pipelines; ExperimentsAgent and ApplicationAgent propose experiments and apps. Nothing they
propose changes anything until you confirm it.

## Glossary

The words lake uses, grouped by where you meet them. Each entry says what the thing is and which
page covers it. Ids named in code (`customer_id`, `workspace_id`, …) are the inputs the REST API and
MCP tools take.

### Accounts and Workspaces

| Term | What it is |
|---|---|
| **Account** | Who owns workspaces, repositories, keys and apps: you, or an organization. Its id is the `customer_id` of every call. When you sign up, lake makes one for you; an organization has its own. |
| **Workspace** | An isolated environment with an Orama Zetta cluster of its own, where datasets, projects, runs, models, transcripts and Ask Orama conversations live. An account can have many workspaces, for example one per environment (`dev`, `staging`, `prod`). Its id is the `workspace_id` every Design, Train and Ship call names; lake never picks one for you. See [Workspaces](/docs/workspaces). |
| **Workspace Zetta** | The Zetta cluster that is a workspace's data plane. Everything that belongs to a workspace is kept in it, sealed with the workspace's own key, apart from every other customer's. |
| **Workspace picker** | The pill in the top bar that names the current workspace. Choosing another keeps the page you are on. |
| **Environment** | A workspace's label, such as `dev`, `staging` or `prod` (any lowercase DNS label). |
| **Organization** | A group of people with its own workspaces, repositories and apps. Members join by invitation or through single sign-on. See [Organizations, Members and Teams](/docs/organizations). |
| **Team** | A group inside an organization, used to grant access to many people at once. |
| **Member roles** | `owner` and `admin` administer the organization and its workspaces; `member` reads them. Anything more is a grant. |
| **Grant** | A role (`read`, `write`, `share`, `review`, `deploy` or `admin`) given to a person, team or agent on one object: an account, organization, repository, folder, environment or secret mount. Grants on a parent reach its children. See [Grants and Effective Access](/docs/access). |
| **Effective access** | Everyone who can reach an object and why: directly, or inherited from a parent. Shown before anything is shared. |
| **Invitation** | An offer to join an organization as owner, admin or member, sent to an email or a handle, valid for 1 to 90 days. |
| **SSO and SCIM** | Signing in through your company's identity provider (SAML or OIDC), and keeping members in step with it (SCIM). See [Single Sign-On and SCIM](/docs/sso). |

### Data

| Term | What it is |
|---|---|
| **Repository** | Versioned files and history, kept by liquid. Every repository belongs to one workspace, chosen when it is created. See [Repositories](/docs/repositories). |
| **Change** | One recorded step in a repository's history. liquid captures each signed write as a *drip*; a change is what a review, an issue or a release points at. |
| **Review** | A change proposed for a branch, with comments anchored to lines, before it lands. |
| **Issue** | A tracked problem or task in a repository, with labels, assignees and discussion. |
| **Release** | A tagged change with notes and the build results attached to it. |
| **Branch protection** | Rules on a reference (such as `main`): who may push, whether a review is needed. Refused pushes are recorded. |
| **Drive** | A repository seen as folders and files: upload, rename, move, preview, history and restore, without the command line. |
| **Share link** | An expiring, revocable link to a folder, opened without signing in. Revoking stops the next open; a copy already downloaded stays with its recipient. See [Sharing and Shared with Me](/docs/sharing). |
| **Shared with Me** | The folders other people shared with you directly. |
| **Dataset** | A table in a workspace, from an uploaded file, a connection or a generator. Its id is `<database>/<name>`. Tables a run writes are listed apart as *Run outputs*. See [Datasets and Loading Data](/docs/load-data). |
| **Connection** | A database a workspace reads from, such as a Postgres service lake runs for you. Its password reaches the workspace only and is never shown again. |
| **Transcript** | The text of a meeting, call or voice memo, kept in a workspace and searchable. Audio sent for speech-to-text is never kept. See [Transcripts and Speech-to-Text](/docs/transcripts). |
| **Search** | One query across repositories, code, datasets, transcripts, projects, agents, models and these Docs. See [Search](/docs/search). |

### Design and Train

| Term | What it is |
|---|---|
| **AI project** | Where you build an AI solution: the business question, the decision it should improve, and the pipeline that builds it, with its runs and models. Called a *project* on the pages. See [AI Projects and Pipelines](/docs/design). |
| **Pipeline** | The AI project's workflow, an execution graph: the steps that turn data into a result: connect, clean, model, index, serve, act, joined by arrows. It must be **approved** before it runs, and again after any change. |
| **Run** | One execution of a pipeline, with its report, lineage, log and trace. See [Train a Model](/docs/train). |
| **Hold-out** | The share of rows kept out of training to measure the model. A score without one is *in-sample* and flatters the model. |
| **Held-out metrics** | Scores measured on the held-out rows, named as the run report names them: `held_out_auc`, `held_out_log_loss`, `held_out_accuracy`, `held_out_rmse`, `held_out_mae`. A run's report leads with its held-out score. |
| **Methodology** | A modelling method the engine runs, such as segmentation, forecasting or anomaly detection. **Design → Methodologies** lists each one, what it needs and whether your workspace runs it. |
| **AI solution** | An AI system that answers one business problem end to end: the decision it drives, the methodologies it composes and how its return is measured. You build one in an AI project; **Design → Solutions** is Orama's catalogue of ready-made ones to start from. See [AI Solutions, Methodologies and Opportunities](/docs/solutions). |
| **Opportunity** | An AI project worth building that Auto-Suggest found by scanning your workspace's data. It is kept until you create a project from it or dismiss it. |
| **Model** | A learned function that turns inputs into a prediction, a score or a ranking. A model can be one step inside a pipeline, and one pipeline can hold several models (a segmentation feeding a churn classifier, for example). A model also stands on its own: you can upload one as a model repository (weights, card and history) without any pipeline. Each run that trains a model adds a **version** of it. **Train → Models** lists the models a workspace trains, with their versions and held-out scores; **Ship → Models** serves them: a **published** version can be called from outside lake with `predict`. See [Models and Search Indexes](/docs/ship). |
| **Search index** | A full-text index over a dataset's text columns. A published index answers outside searches with `search_index`. See [Models and Search Indexes](/docs/ship). |
| **Experiment and trial** | An AI model training experiment, as an experiment tracker records it: one experiment tries many settings for one objective, within budgets, and keeps the best model; each attempt is a trial with its own run and curves, ranked and compared live. See [Experiments](/docs/experiments). |
| **Stream and topic** | A topic is a named sequence of messages in a workspace Zetta (agent events, app events, build logs, your own). Streams browses and publishes to them. See [Streams](/docs/streams). |
| **Agent** | An AI helper that works for you. It reads what it is allowed to read and proposes changes for you to confirm. See [Agents](/docs/agents). |
| **Agent Library** | The agents Orama provides: Ask Orama, the Project Agent, ApplicationAgent, ExperimentsAgent, Auto-Suggest and AgentCreator. **Design → Agent Library** lists them and whether each runs on your lake. |
| **Agent Factory** | Building a new agent in a conversation with AgentCreator. |
| **Proposal** | A change an agent wants to make, shown in full. Nothing changes until you confirm it. |
| **Ask Orama** | The agent in the top bar of every page. It answers from what you can read and hands work to other agents. See [Ask Orama](/docs/ask-orama). |

### Ship

| Term | What it is |
|---|---|
| **App** | A running web application lake hosts for you, from a repository or a container image, with its own address, logs, environment and bindings. See [Apps](/docs/apps). |
| **Build** | Turning a repository into a container image inside the cluster. Its log streams live. |
| **Package and registry** | The images, npm and Python packages lake serves. **Ship → Packages** lists them with their install commands. See [Packages, Builds and the Registry](/docs/registry). |
| **Environment** | Where a repository's deployments run, with its domains and logs. See [Deployments and Environments](/docs/deployments). |
| **Deployment** | One version of a repository running in an environment. |
| **Service** | A database, stream or engine lake runs for you: Postgres, Zetta, Kafka, ClickHouse, TiDB, Cassandra, Valkey. See [Services and Storage](/docs/services). |
| **Storage** | A volume lake provisions for a workspace's services. |

### Credentials, Audit and Usage

| Term | What it is |
|---|---|
| **Session** | Your signed-in browser. Revoking it signs that browser out on its next request. |
| **Token or key** | A lake API credential for scripts and AI agents, limited to the capabilities you name. It acts as you. See [Tokens and Keys](/docs/tokens). |
| **Passkey** | A device-bound credential for signing in without a password. |
| **Secret mount** | A place for secrets and encryption keys, belonging to one workspace. Pages show metadata only, never values. See [Secrets and Encryption Keys](/docs/secrets). |
| **Audit** | A signed, hash-chained record of every call: who, what, on which object, and the outcome. Events about a workspace's data are kept in that workspace; the rest centrally. See [Activity and Audit](/docs/activity). |
| **Transparency log** | lake's public, append-only log of signed statements, checkable by anyone. See [Security Model](/docs/security). |
| **Usage** | What your account stores, transfers and computes, with its storage limit. See [Usage and Metering](/docs/usage). |
| **Capability** | One thing lake can do, such as `create_workspace`. The site, REST (`/v1/<capability>`) and MCP all call the same capabilities, with the same permissions and audit. See [REST API and OpenAPI](/docs/developer). |

## Related Pages

- [Getting Started](/docs)
- [Workspaces](/docs/workspaces)
- [Security Model](/docs/security)
- [FAQ and Troubleshooting](/docs/faq)

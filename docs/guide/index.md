# Getting started

lake is where you design, train and ship AI on your own data. This page takes you from a CSV file
to a model you can call from your own code. Each step shows where to click, and the same step
through the REST API and MCP for when you automate it.

## 1. Start your workspace

Your **workspace** is where your data, projects, pipelines, runs and models live. Open **Design →
Projects**; if the workspace is not running yet, press **Start workspace** and wait until the page
says it is ready.

Every REST and MCP call below names your space (`customer_id`) and your workspace
(`workspace_id`). A space can have several workspaces, so lake never guesses which one you mean: a
call without `workspace_id` is refused with `workspace_id is required`. On the site your workspace
id is in the address bar, `/w/<workspace id>/…`; `list_workspaces` lists them. Replace
`<customer id>` and `<workspace id>` below with yours.

- REST: `POST /v1/create_workspace` with
  `{"customer_id": "<customer id>", "name": "analytics", "environment": "dev"}`. The answer's `id`
  is the workspace id. Then poll
  `GET /v1/workspace_status?customer_id=<customer id>&workspace_id=<workspace id>` until `ready`
  is true.
- MCP: `create_workspace`, then `workspace_status`, with the same inputs.

## 2. Load your data

Open **Train → Data** and use **Load data → Upload a file** (CSV or JSONL, up to 16 MiB). Give the
dataset a name, for example `accounts`. The page confirms with the row count and links to it.

- REST: `POST /v1/workspace_act` with `{"customer_id": "<customer id>", "workspace_id":
  "<workspace id>", "capability": "upload_dataset", "input": {"table_name": "accounts", "format":
  "csv", "content": "<the file's text>"}}`.
- MCP: `workspace_act` with the same input.

More ways to bring data in (a database lake runs for you, generated data): [Load data](/docs/load-data).

## 3. Create a project

Open **Design → Projects → New Project**. Name it and say which decision it should improve, for
example *which accounts customer success calls first each week*.

## 4. Build and train a pipeline

On the project, describe what to predict under **Build a Pipeline**, for example *Predict the
churned column of accounts*. Open the pipeline, press **Approve**, choose your dataset under
**Train on**, keep **Hold out for measuring** at 0.2, and press **Check and run**. The run page
shows the report: the score on rows the model did not see, what the model leans on, and the rows
most at risk.

To change the pipeline, use **Ask the Project Agent** on the pipeline page: it proposes edits you
review and apply. Details: [Train a model](/docs/train).

## 5. Publish and call the model

Open **Ship → Models**, press **Publish** on your model, and copy its `curl`. The page fills in
your space, workspace and model:

```
curl -s https://lake.oramadev.net/v1/predict \
  -H "Authorization: Bearer $LAKE_KEY" -H 'Content-Type: application/json' \
  -d '{"customer_id": "<customer id>", "workspace_id": "<workspace id>", "model_id": "<model id>", "rows": [{"support_tickets": 11}]}'
```

Create the key on **Developer → MCP**. Details: [Ship](/docs/ship).

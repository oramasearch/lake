# Quickstart

**Quickstart** is Lake's home page: drop a dataset, and Lake takes you to a search box on your own
website, then to a model whose answers show beside each search result. Every step says where it
stands, and you can leave and come back: the page goes on from where you stopped.

## The Steps

1. **Drop a dataset.** Drag a CSV, JSON or JSON Lines file of up to 16 MiB onto the page and press
   **Add it and build search**. Lake adds it to your workspace. If you have no workspace yet, Lake
   makes one; keep the page open while it starts, and your dataset goes in as soon as it is ready.
2. **Search your data.** Lake builds a search index over every column and publishes it. Type in the
   search box: results appear as you type.
3. **Search on your website.** Press **Make a website key**, then copy the code for a plain web page
   or a React component. Lake's address, your account, workspace and index are already in it, and so
   is the key. Paste it into your site and search there.
4. **Train a model.** Choose the column to predict and the column that names a row (Lake guesses the
   last column and the first text column), then press **Train a model**. The page follows the
   training; you can leave and come back.
5. **Model answers on your website.** When the model is trained, Lake publishes it. The search box
   on the page shows its answer beside each result. Make a new website key (the first one can only
   search) and copy the code again: your site shows the answers too.

**Start another** begins a new quickstart; **Your quickstarts** opens an earlier one.

## What It Costs

Before a step that costs money (adding the dataset, training), the page says what it is billed by,
at Lake's published rates, and your balance. Training runs for as long as it takes, which depends
on the size of your dataset. Each search and each model answer your website asks for is billed too.
If your balance does not cover new work, the page shows **Add funds** instead of starting the step
([Billing](/docs/billing)).

## The Website Key

Anyone who opens your website can read the key in its code. The key can only search published
indexes and ask published models of the accounts you can read; it cannot change anything. It lasts
as long as the sign-in that made it, at most 30 days. Revoke it on **Tokens** at any time
([Tokens and Keys](/docs/tokens)).

## From Code and AI Agents

The quickstart is four tools, the same over REST and MCP:

| Tool | What it does |
|---|---|
| `start_quickstart` | Adds the dataset (`content` and `format`, or a `dataset_id`), making a workspace when there is none, and builds and publishes its search index |
| `quickstart_status` | Where each step stands, and what to call next; with `include_costs`, what the paid steps cost and your balance |
| `train_quickstart` | Trains a model on the same dataset; called again once it is trained, publishes it |
| `quickstart_snippets` | The website code, and the `create_token` call that makes its key |

```sh
curl -s -X POST https://<your lake>/v1/start_quickstart \
  -H "Authorization: Bearer $LAKE_KEY" -H "Content-Type: application/json" \
  -d '{"customer_id": "<account>", "format": "csv", "content": "title,genre\nAlien,horror\n"}'
```

An agent connected to Lake's MCP server finds the quickstart first in the tool list
([Use Lake from an AI Agent](/docs/mcp)).

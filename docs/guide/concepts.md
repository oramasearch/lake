# Concepts

The words lake uses, in the order you meet them.

| Term | What it is |
|---|---|
| **Space** | Your account's area in lake: its repositories, members, keys and workspace. |
| **Workspace** | Your own Orama Zetta cluster, where data is stored and models are trained. Design, Train and Ship all work in it. |
| **Dataset** | A table in your workspace, from an upload, a connection or a generator. |
| **Connection** | A database your workspace reads from, such as a Postgres service lake runs for you. |
| **Service** | A database, stream or engine lake runs for your space (Postgres, Kafka, ClickHouse, …). |
| **Storage** | A volume or bucket lake provisions for your services. |
| **Project** | A business question and the decision it should improve. It holds pipelines. |
| **Pipeline** | The steps that turn data into a result: connect, clean, model, index, serve, act, joined by arrows. A pipeline must be **approved** before it runs, and again after any change. |
| **Run** | One execution of a pipeline, with its report, lineage, log and timing. |
| **Hold-out** | The share of rows kept out of training to measure the model honestly. A score without one is *in-sample* and flatters. |
| **Model** | What a training run produced. **Published** models can be called from outside lake. |
| **Search index** | A searchable index over a dataset; published indexes answer outside searches. |
| **Agent** | An AI helper. The **Agent Library** holds the ones Orama provides; you can build your own in Train → Agents. |
| **Ask Orama** | The agent at the top of every page. It reads what you can read and proposes changes for you to confirm. |
| **Key** | A lake API token for scripts and AI agents (Developer → MCP). |

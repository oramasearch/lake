# Concepts

The words lake uses, in the order you meet them. Checked against the running service on 2026-10-01.

| Term | What it is |
|---|---|
| **Space** | Your account's area in lake: its repositories, members, keys and workspaces. Its id is the `customer_id` of every call. |
| **Workspace** | One of your space's own Orama Zetta clusters, where data is stored and models are trained. A space can have many, for example one per environment; the top-bar selector switches between them, **New workspace** adds one, and Design, Train and Ship work in the one selected. Each has an id, the `workspace_id` every Design, Train and Ship call names (`list_workspaces` lists them, `create_workspace` adds one). |
| **Dataset** | A table in your workspace, from an upload, a connection or a generator. |
| **Connection** | A database your workspace reads from, such as a Postgres service lake runs for you. |
| **Service** | A database, stream or engine lake runs for your space (Postgres, Kafka, ClickHouse, …). |
| **Storage** | A volume or bucket lake provisions for your services. |
| **Project** | A business question and the decision it should improve. It holds pipelines. |
| **Methodology** | A modelling method the engine runs, such as segmentation, forecasting or anomaly detection. **Design → Methodologies** lists all 21: what each needs, what it makes, how it is measured, and whether it runs in your workspace. |
| **Solution** | A module from Orama's catalogue that answers one business problem end to end: the methodologies it composes, the decision it drives, and how its return is measured. **Design → Solutions** lists them. **Start this solution** asks which data to use, where actions go and the economics, then a Project Agent proposes the technical foundation for you to edit and confirm. The project shows its policy, holdout, actions, monitor and ROI. |
| **Pipeline** | The steps that turn data into a result: connect, clean, model, index, serve, act, joined by arrows. A pipeline must be **approved** before it runs, and again after any change. |
| **Run** | One execution of a pipeline, with its report, lineage, log and timing. |
| **Hold-out** | The share of rows kept out of training to measure the model honestly. A score without one is *in-sample* and flatters. |
| **Model** | What a training run produced. **Published** models can be called from outside lake. |
| **Search index** | A searchable index over a dataset; published indexes answer outside searches. |
| **Agent** | An AI helper. The **Agent Library** holds the ones Orama provides; you can build your own in Train → Agents. |
| **Ask Orama** | The agent at the top of every page. It reads what you can read and proposes changes for you to confirm. |
| **Key** | A lake API token for scripts and AI agents (Developer → API or Developer → MCP). |

# MCP Connections

**Data → MCP Connections** connects a workspace to other MCP servers: a ticket tracker, a wiki, a
CRM, anything that speaks the Model Context Protocol over the web. Lake becomes their client. You
choose which of a server's tools the workspace may use; then people, Ask Orama and your agents use
them through Lake, which holds the credential, records every call in the workspace's activity and
counts it in the workspace's usage.

## Concepts

- **Connection.** A remote MCP server, by its URL, with how Lake signs in to it and what it offers:
  its tools, resources and prompts. It belongs to one workspace; other workspaces never see it.
- **Allowed.** No tool is usable until a person allows it. A tool you have not allowed is never
  called, whoever asks.
- **Read or change.** Each tool is a read or a change. The server says which it thinks; you decide.
  A **change** is something a person confirms: you can run it yourself, Ask Orama only proposes
  it, and an agent only proposes it unless you **auto-approve** that one tool for that one agent.
- **Credential.** A bearer token kept as a secret of the workspace, or a sign-in (OAuth) whose
  refresh token Lake keeps as a secret. Lake reads it for each call and never shows it to anyone,
  to a model or to an agent.
- **Data, never instructions.** What a server answers comes from outside Lake. Lake, Ask Orama and
  agents treat it as data: if an answer asks for something to be done, it is shown to you, never
  done.

Lake reaches servers on the public internet over `https`. A server that only runs as a program
(stdio), an npm or Python package, Lake runs for you as an App of the workspace: see
[A Server That Runs Here](#a-server-that-runs-here).

## Add a Connection

1. **Data → MCP Connections → New connection.**
2. Paste the server's **URL** and give it a **name**. The name gives the connection's prefix: its
   tools are named `<prefix>__<tool>` outside it (for example `ticket_desk__create_ticket`).
3. Choose **Authentication**:
   - **None**, for an open server;
   - **Bearer token from a secret**: first put the token in a secret mount of this workspace
     (**Organization → Secrets**), then pick the mount, the secret and its field;
   - **Sign in (OAuth)**: you sign in from the connection's page after adding it.
4. **Test** connects and lists the tools without keeping anything. **Add connection** keeps it and
   asks the server what it offers.

## A Server That Runs Here

Many MCP servers are packages you would normally run on your own computer. Lake can run one for the
workspace:

1. Put any credential the server needs in a secret mount of this workspace first.
2. **New connection → Here, from a package (stdio)**. Choose **npm package** or **Python package**,
   type the package with its version (for example
   `@modelcontextprotocol/server-everything@2026.8.31`), the **command** if it is not named after
   the package, the **arguments** one per line, the **secret mount**, and the **environment**: each
   variable a value, or a field of a secret in the mount.
3. **Add connection.** Lake keeps a token for the server in the mount, starts an App of this
   workspace that installs and runs the package, and connects to it inside the cluster. The page
   says **Starting** until the App is ready, then lists the server's tools.

**Runs in This Workspace** on the connection's page shows the App's state, **Start**, **Stop** and
**Restart**, and its logs, where an install that failed says why. The App is metered like any App.
The workspace needs internet access for the package to install. Deleting the connection deletes
its App.

## Decide Its Tools

On the connection's page, each tool shows its name, what the server says it does, and two switches:

- **Allowed**: whether this workspace may use it.
- **Changes something**: on for anything that writes, sends, deletes or spends.

**Try it** opens a form made from the tool's own inputs. A read runs at once; a change asks you to
confirm that you are making it, and the call is recorded under your name.

For an allowed change, **Auto-approve** lets one of your agents run it unattended. Choose the agent
and press **Auto-approve**; press its name to take it back. Every call it makes is recorded with
the agent and who granted it.

## Sign In with OAuth

Press **Sign in**, choose the secret mount the sign-in should be kept in, and continue: you sign in
at the server, which sends you back to Lake. Lake registers itself with the server's sign-in when it
can, uses PKCE, and keeps only the refresh token, as the secret `mcp-<prefix>`. If the server stops
accepting it, the connection says **Needs sign-in**; sign in again.

## Use It from Lake's MCP

**Through Lake's MCP** is off by default. Turned on, the connection's allowed tools appear in this
workspace's MCP endpoint (`/w/<workspace>/mcp`) as `<prefix>__<tool>`, so one MCP client set up for
Lake reaches them too. **Anyone who may call Lake's tools in this workspace can then call these
tools**, and Lake calls the server for them with this connection's credential; Lake's permission
on the workspace is the only gate. Lake asks before turning it on.

## Its Agent and Streams

A connection can have its own agent, whose only tools are the connection's:

- **Create an agent for this connection** with a sentence saying what it is for.
- An **input stream** (a topic of **Train → Streams**): each new event starts one run of the agent,
  with the event as its request.
- An **output stream**: each run's result is published there for other agents, apps and pipelines.
- A **notifications stream**, which needs no agent: Lake subscribes to the resources you tick and
  publishes each notification the server sends, so a server that pushes changes can drive the agent
  without polling.

The streams act with the permission of the person who last set them.

## Ask Orama and Agents

Ask Orama lists the allowed tools of the workspace's connections and reads with them; it proposes
every change for you to **Confirm**. An agent you build reaches a connection when its skills include
`mcp:<prefix>`; its changes are proposals unless you auto-approved them for it.

## Recent Calls and Usage

**Recent Calls** on the connection's page shows who called which tool, how (a person, an agent,
Lake's MCP) and the outcome, from the workspace's activity; arguments and results are never
recorded. Each call counts in **Organization → Usage** as API and MCP calls of the workspace.

## Questions

- **Why does a tool say "Not allowed"?** No one has allowed it yet; a person allows it on the
  connection's page.
- **My agent's change did nothing.** It was proposed. Confirm it, or auto-approve that tool for the
  agent.
- **The connection says Failing.** Lake could not reach the server or it answered something that is
  not MCP; the page says why. **Refresh** asks again.
- **A server that runs here stays Starting.** Its App is not ready yet: the logs on its page show
  the package installing, or why it could not.

Through the API and MCP: `create_mcp_connection`, `list_mcp_connections`, `get_mcp_connection`,
`update_mcp_connection`, `refresh_mcp_connection`, `test_mcp_connection`,
`list_mcp_connection_tools`, `read_mcp_tool`, `call_mcp_tool`, `set_mcp_tool_auto_approve`,
`list_mcp_connection_calls`, `begin_mcp_connection_sign_in`, `complete_mcp_connection_sign_in`,
`create_mcp_connection_agent` and `delete_mcp_connection` ([Developer → API](/developer/api)).

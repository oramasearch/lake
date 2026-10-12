# Data → MCP Connections

**Status: implemented 2026-10-11 in the source tree (lake#652, decision 60); not deployed.** The
pages for a workspace's MCP connections ([API](../api/mcp-connections.md)). They live under Data,
after Transcripts, and call only `/v1` capabilities.

## The list (`/w/<workspace>/mcp-connections`)

- Figures: connections (and how many need attention), tools allowed across them, calls this month.
- One card per connection: its name, status (Connected, Needs sign-in, Failing, New, Starting), the server's
  host (or, for a server run here, its package), tools allowed of offered, how it authenticates, whether it is exposed through Lake's MCP,
  whether it has an agent, calls this month and the last refresh; a failing connection says why.
- **New connection** (a `Modal sheet`, full screen on a phone): the server URL, a name (the prefix
  its tools get, `<prefix>__<tool>`, is shown as it is typed), the transport, and authentication:
  none, a bearer token held in a secret of this workspace (mount, secret, field; the token is never
  typed here), or OAuth. **Test** connects without keeping anything (`test_mcp_connection`); **Add
  connection** keeps it and discovers its tools (`create_mcp_connection`), then opens its page.
  **Where the server runs** comes first: **At a URL** (the fields above) or **Here, from a package
  (stdio)** (lake#659): the runtime (npm package through `npx`, or Python package through `uvx`),
  the package with its version, the command when it is not named after the package, the arguments
  one per line, the secret mount, and the environment, each variable a plain value or a field of a
  secret in that mount (names starting with `MCP_`, `APP_` or `LAKE_` are refused as they are
  typed). There is no Test for a package: it runs only once its App does.

## A connection (`/w/<workspace>/mcp-connection?id=<id>`)

- Header: **Sign in** / **Sign in again** for OAuth, **Refresh** (`refresh_mcp_connection`),
  **Delete** with the name typed (the secret stays in its mount).
- A banner when it is not connected, with the reason and, for OAuth, Sign in.
- Figures: status and server version, tools allowed and the prefix, authentication, last refresh
  and the transport in use.
- **Tools**: each with its title, `<prefix>__<tool>` name, description, Read or Change, and two
  switches, **Allowed** and **Changes something** (`update_mcp_connection {tools}`). **Try it**
  opens a drawer whose form is made from the tool's own input schema (`src/pages/mcp/logic.ts`
  `formFields`): text, numbers, choices, booleans, and JSON for anything else. A read runs through
  `read_mcp_tool`; a change through `call_mcp_tool`, only after the person ticks that they are
  making it. The answer is shown with the server's words marked as data, never instructions.
  An allowed change lists the agents **auto-approved** to run it unattended, from the workspace's
  Python agents (`set_mcp_tool_auto_approve`); pressing a grant takes it back. Tools Lake left out
  are listed with why.
- **Through Lake's MCP**: off by default. Turning it on asks first, in the decision's words:
  anyone who may call Lake's tools in this workspace will be able to call these tools, and Lake
  calls the server for them with this connection's credential; Lake's permission on the workspace
  is the only gate.
- **Agent and Streams**: create an agent whose only tools are this connection's
  (`create_mcp_connection_agent`), or open it; its input stream, output stream and the
  notifications stream, with topic names suggested from the workspace's streams, and the
  resources to subscribe to (`update_mcp_connection`).
- **Recent Calls** from the workspace audit (`list_mcp_connection_calls`): when, tool, through whom
  (a person, an agent, auto-approved or not, Lake's MCP) and the outcome. Arguments and results are
  never recorded.
- **Also Offered**: the server's resources and prompts.
- **Runs in This Workspace**, for a server Lake runs here (lake#659): the header shows its command
  line instead of a URL, and a **Starting** banner shows until the App is ready and its tools are
  discovered (the page asks `refresh_mcp_connection` once when `get_app` says ready). The section
  shows the App's state (`get_app`), the package, runtime, command line and environment names (a
  secret variable is marked, never shown), a link to the App's own page, **Start**, **Stop** and
  **Restart** (`start_app`, `stop_app`, `restart_app`) and the last 200 log lines (`app_logs`):
  what the package installer and the server print. Delete says the App goes with the connection.

## Signing in

**Sign in** asks for the secret mount the refresh token goes to, calls
`begin_mcp_connection_sign_in` and sends the browser to the server's sign-in. The server sends it
back to `/mcp-connections/oauth/callback?code&state` (outside any workspace: the state names the
account and workspace), which calls `complete_mcp_connection_sign_in` there and opens the
connection, or says why the sign-in did not finish.

## Verify

`frontend/tests/mcp.test.ts` covers the form a schema makes, the arguments its values make, the
prefix, the callback's state and the switch's words. `frontend/checks/mcp-connections.mjs` takes
screenshots of the list, New connection, a connection and Try it at 1440 and 390 wide against
mocked answers and reports sideways scrolling and page errors (none on 2026-10-11), and a stdio
connection's page with its App section.

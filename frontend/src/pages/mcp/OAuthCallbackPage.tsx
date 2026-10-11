/** Where an MCP server's sign-in sends the browser back (`/mcp-connections/oauth/callback?code&state`,
 * lake#652, docs/ui/mcp-connections.md). The state names the account and workspace Lake started the
 * sign-in in; the page finishes it there with `complete_mcp_connection_sign_in` (Lake exchanges the
 * code with its PKCE verifier and keeps the refresh token as a workspace secret), then opens the
 * connection. A refusal from the server (`error`) or Lake is said in words, with the way back. */
import { useEffect, useRef, useState } from "react";
import { useNavigate, useSearchParams } from "react-router-dom";
import { act } from "@/lib/api";
import { Banner, Button, Page, Skeleton } from "@/lib/ui";
import { signInState, type McpConnection } from "./logic";

export default function OAuthCallbackPage() {
  const [params] = useSearchParams();
  const navigate = useNavigate();
  const started = useRef(false);
  const [problem, setProblem] = useState<string | null>(null);
  const state = params.get("state");
  const code = params.get("code");
  const scope = signInState(state);
  const back = scope ? `/w/${encodeURIComponent(scope.workspaceId)}/mcp-connections` : "/mcp-connections";

  useEffect(() => {
    if (started.current) return;
    started.current = true;
    const refused = params.get("error");
    if (refused) {
      setProblem(`The server did not sign you in: ${params.get("error_description") || refused}.`);
      return;
    }
    if (!scope || !code) {
      setProblem("This page was opened without a sign-in to finish. Start again from the connection's page.");
      return;
    }
    act<McpConnection>(
      "complete_mcp_connection_sign_in",
      { customer_id: scope.customerId, workspace_id: scope.workspaceId, state, code },
      { workspace: scope.workspaceId },
    )
      .then((c) => navigate(`/w/${encodeURIComponent(scope.workspaceId)}/mcp-connection?id=${encodeURIComponent(c.id)}`, { replace: true }))
      .catch((e) => setProblem((e as Error).message));
  }, [code, navigate, params, scope, state]);

  return (
    <Page eyebrow="MCP Connections" title="Signing In">
      {problem ? (
        <Banner tone="warn" title="The sign-in did not finish" action={<Button to={back}>Back to MCP Connections</Button>}>
          {problem}
        </Banner>
      ) : (
        <>
          <p className="muted">Finishing the sign-in and asking the server what it offers…</p>
          <Skeleton lines={2} />
        </>
      )}
    </Page>
  );
}

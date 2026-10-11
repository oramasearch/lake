/** The old Design → Agent Library address (`/design/agent-library`, renamed lake#653): Design →
 * Agents, in the same workspace and with the same query. */
import { Navigate, useLocation } from "react-router-dom";

export default function AgentLibraryRedirect() {
  const { pathname, search } = useLocation();
  return <Navigate to={`${pathname.replace(/\/design\/agent-library$/, "/design/agents")}${search}`} replace />;
}

/** Design → Agent Library (lake's agent_library_content): the agents Orama provides. The library is
 * lake's, not the workspace's, so it shows whether the workspace is ready or not. */
import { Badge, Page, Query } from "@/lib/ui";
import { useRead } from "@/lib/api";
import { WorkspaceGate } from "@/components/WorkspaceGate";
import { text, type J } from "./logic";
import { useDesignTrail } from "./parts";

export default function AgentLibraryPage() {
  useDesignTrail({ label: "Agent Library", path: "/design/agent-library" }, [], "Agent Library");
  return (
    <Page eyebrow="Design" title="Agent Library" sub="Agents Orama provides, each for one job. Use them where they appear, or ask Orama and it brings in the right one.">
      <WorkspaceGate always>
        <AgentLibrary />
      </WorkspaceGate>
    </Page>
  );
}

/** The Agent Library panel (also shown on Train → Agents). */
export function AgentLibrary() {
  const library = useRead<{ agents?: J[] }>("list_agent_library", {});
  return (
    <section className="panel" id="agent-library">
      <h2>Agent Library</h2>
      <p className="sub">Agents Orama provides, each for one job. Use them where they appear below, or just ask Orama and it brings in the right one.</p>
      <Query q={library} what="the Agent Library">
        {(value) => {
          const agents = Array.isArray(value.agents) ? value.agents : [];
          if (!agents.length) return <p>The Agent Library is not available.</p>;
          return (
            <div className="table-wrap">
              <table>
                <thead>
                  <tr>
                    <th>Agent</th>
                    <th>What it does for you</th>
                    <th>Where to use it</th>
                    <th />
                  </tr>
                </thead>
                <tbody>
                  {agents.map((agent, index) => {
                    const available = text(agent, "status") === "Available";
                    // Orama's own tooling is listed for the record, not offered to talk to (lake#451).
                    const internal = agent.internal === true;
                    const label = internal ? "Orama internal" : available ? "Available" : text(agent, "runs_as") === "offline" ? "Not yet available to chat" : "Not available here";
                    return (
                      <tr key={text(agent, "key") || index}>
                        <td>
                          <strong>{text(agent, "name")}</strong>
                        </td>
                        <td>{text(agent, "purpose")}</td>
                        <td>{text(agent, "used_in")}</td>
                        <td>
                          <Badge tone={available && !internal ? "success" : "neutral"}>{label}</Badge>
                        </td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          );
        }}
      </Query>
    </section>
  );
}

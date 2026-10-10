/** What the Agents, Agent and Agent Factory pages say about an agent (web_ui.cpp agent_skills,
 * agent_skill_labels, agent_engagement, agent_model_label, agent_library_content). */
import type { Row } from "@/lib/api";
import { useRead } from "@/lib/api";
import { DataTable } from "@/lib/ui";
import { text } from "./workspace";

/** Studio's skills (orama-design studioAgents.js): granted, never assumed. */
export const AGENT_SKILLS: [string, string][] = [
  ["search", "Search data"],
  ["generate-data", "Generate a dataset"],
  ["train", "Train a Model"],
  ["summarize", "Summarize and explain"],
  ["call-api", "Call the API"],
];

export function skillLabels(skills: unknown): string {
  const labels = (Array.isArray(skills) ? skills : [])
    .filter((s): s is string => typeof s === "string")
    .map((s) => AGENT_SKILLS.find(([key]) => key === s)?.[1] ?? s);
  return labels.length ? labels.join(", ") : "—";
}

export function engagement(agent: Row): string {
  return text(agent, "trigger") ? "Conversation and unattended" : "Conversation";
}

export function modelLabel(agent: Row): string {
  return text(agent, "model_name") || text(agent, "model");
}

/** The Agent Library panel: every specialized agent Orama ships. Shown whether or not the
 * workspace is ready; the library is lake's, not the workspace's. */
export function AgentLibrary() {
  const library = useRead<{ agents?: Row[] }>("list_agent_library", undefined, { staleTime: 300_000 });
  const agents = library.data?.agents ?? [];
  return (
    <section className="panel" id="agent-library">
      <h2>Agent Library</h2>
      <p className="sub">Agents Orama provides, each for one job. Use them where they appear below, or just ask Orama and it brings in the right one.</p>
      {library.isError || (!library.isLoading && !agents.length) ? (
        // The page treated a failed library read as an absent library.
        <p>The Agent Library is not available.</p>
      ) : (
        <DataTable
          q={library}
          what="the Agent Library"
          rows={agents}
          rowKey={(a) => text(a, "key")}
          columns={[
            { key: "name", label: "Agent", render: (a) => <strong>{text(a, "name")}</strong> },
            { key: "purpose", label: "What it does for you", render: (a) => text(a, "purpose") },
            { key: "used_in", label: "Where to use it", render: (a) => text(a, "used_in") },
            {
              key: "badge",
              label: "",
              render: (a) => {
                const available = text(a, "status") === "Available";
                // Orama's own tooling is listed for the record, not offered to talk to (lake#451).
                const internal = !!a.internal;
                const words = internal ? "Orama internal" : available ? "Available" : text(a, "runs_as") === "offline" ? "Not yet available to chat" : "Not available here";
                return <span className={available && !internal ? "badge status-success" : "badge"}>{words}</span>;
              },
            },
          ]}
        />
      )}
    </section>
  );
}

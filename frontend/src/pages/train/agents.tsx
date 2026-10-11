/** What the Agents, Agent and Agent Factory pages say about an agent (web_ui.cpp agent_skills,
 * agent_skill_labels, agent_engagement, agent_model_label, agent_library_content). */
import { BookMarked } from "lucide-react";
import type { Row } from "@/lib/api";
import { useRead } from "@/lib/api";
import { Card, Chip, Empty, Section, Skeleton } from "@/lib/ui";
import { text } from "./workspace";
import agentSkills from "./agent-skills.json";

/** The skills a person may grant an agent, granted, never assumed: Studio's five (orama-design
 * studioAgents.js) and Lake's own areas (agent-runtime#7). Vendored from agent-runtime's
 * skills.json by tools/agents/vendor-agent-skills.py, which says what each one lets an agent do. */
export const AGENT_SKILLS: [string, string][] = agentSkills as [string, string][];

export function skillLabels(skills: unknown): string {
  const labels = (Array.isArray(skills) ? skills : [])
    .filter((s): s is string => typeof s === "string")
    .map((s) => AGENT_SKILLS.find(([key]) => key === s)?.[1] ?? s);
  return labels.length ? labels.join(", ") : "—";
}

export function engagement(agent: Row): string {
  return text(agent, "trigger") ? "Conversation and unattended" : "Conversation";
}

/** Where an agent runs, in words (web_ui.cpp agent_content's "Runs" row). */
export function runsWhere(agent: Row): string {
  const runtime = text(agent, "runtime");
  return runtime === "python" ? "On agent-runtime" : runtime === "service" ? "As its own service" : "In your workspace";
}

export function modelLabel(agent: Row): string {
  return text(agent, "model_name") || text(agent, "model");
}

/** What the library says about one of its agents (lake#451: Orama's own tooling is listed for the
 * record, not offered to talk to). */
export function libraryBadge(agent: Row): { words: string; tone: "ok" | "neutral" } {
  const available = text(agent, "status") === "Available";
  const internal = !!agent.internal;
  const words = internal ? "Orama internal" : available ? "Available" : text(agent, "runs_as") === "offline" ? "Not yet available to chat" : "Not available here";
  return { words, tone: available && !internal ? "ok" : "neutral" };
}

/** The Agent Library: every specialized agent Orama ships, as cards. Shown whether or not the
 * workspace is ready; the library is lake's, not the workspace's. */
export function AgentLibrary() {
  const library = useRead<{ agents?: Row[] }>("list_agent_library", undefined, { staleTime: 300_000 });
  const agents = library.data?.agents ?? [];
  return (
    <div className="mt-8">
      <Section
        id="agent-library"
        title="Agent Library"
        sub="Agents Orama provides, each for one job. Use them where they appear, or just ask Orama and it brings in the right one."
      >
        {library.isLoading ? (
          <Skeleton />
        ) : library.isError || !agents.length ? (
          // The page treated a failed library read as an absent library.
          <Empty compact icon={BookMarked} title="The Agent Library is not available" />
        ) : (
          <div className="grid grid-cols-1 gap-3 md:grid-cols-2 xl:grid-cols-3">
            {agents.map((agent) => {
              const badge = libraryBadge(agent);
              return (
                <Card key={text(agent, "key")} label="Orama agent" title={text(agent, "name")} actions={<Chip tone={badge.tone}>{badge.words}</Chip>} className="h-full">
                  <p className="m-0 text-[13px] text-ink-2">{text(agent, "purpose")}</p>
                  {text(agent, "used_in") && (
                    <p className="mt-2 mb-0 text-[12.5px] text-ink-3">
                      <span className="label mr-1 inline">Where</span>
                      {text(agent, "used_in")}
                    </p>
                  )}
                </Card>
              );
            })}
          </div>
        )}
      </Section>
    </div>
  );
}

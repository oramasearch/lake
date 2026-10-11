/** Design → Agent Library (lake's agent_library_content): the agents Orama provides, as cards with a
 * search and a filter by availability. The library is lake's, not the workspace's, so it shows
 * whether the workspace is ready or not. */
import { useState } from "react";
import { Bot, SearchX } from "lucide-react";
import { Card, Empty, FilterChips, Grid, Page, Query, SearchInput, Stat, StatRow, Status, Toolbar } from "@/lib/ui";
import { useRead } from "@/lib/api";
import { WorkspaceGate } from "@/components/WorkspaceGate";
import { text, type J } from "./logic";
import { matches } from "./catalogue";
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

/** What an agent's chip says (lake#451: Orama's own tooling is listed for the record, not offered
 * to talk to). */
function availability(agent: J): { label: string; available: boolean } {
  const available = text(agent, "status") === "Available";
  const internal = agent.internal === true;
  const label = internal ? "Orama internal" : available ? "Available" : text(agent, "runs_as") === "offline" ? "Not yet available to chat" : "Not available here";
  return { label, available: available && !internal };
}

/** The Agent Library as a card grid. */
export function AgentLibrary() {
  const library = useRead<{ agents?: J[] }>("list_agent_library", {});
  const [search, setSearch] = useState("");
  const [state, setState] = useState<string | null>(null);
  return (
    <Query q={library} what="the Agent Library">
      {(value) => {
        const agents = Array.isArray(value.agents) ? value.agents : [];
        if (!agents.length) {
          return (
            <Empty icon={Bot} title="No Agent Library">
              The Agent Library is not available.
            </Empty>
          );
        }
        const counts = new Map<string, number>();
        for (const a of agents) counts.set(availability(a).label, (counts.get(availability(a).label) ?? 0) + 1);
        const shown = agents.filter((a) => (!state || availability(a).label === state) && matches(a, search, ["name", "purpose", "used_in", "key"]));
        const available = agents.filter((a) => availability(a).available).length;
        return (
          <>
            <StatRow cols={2}>
              <Stat label="Agents" value={agents.length} icon={Bot} sub="Orama provides" />
              <Stat label="Available" value={available} tone={available ? "ok" : undefined} sub="to use here now" />
            </StatRow>
            <Toolbar>
              <SearchInput value={search} onChange={setSearch} placeholder="Search agents…" label="Search agents" />
              <FilterChips options={[...counts].map(([id, count]) => ({ id, label: id, count }))} value={state} onChange={setState} />
            </Toolbar>
            {!shown.length ? (
              <Empty compact icon={SearchX} title="No Agent Matches">
                Clear the search or the filter to see more.
              </Empty>
            ) : (
              <Grid className="grid-cols-1 gap-3 md:grid-cols-2 xl:grid-cols-3">
                {shown.map((agent, index) => {
                  const a = availability(agent);
                  return (
                    <Card
                      key={text(agent, "key") || index}
                      title={
                        <span className="inline-flex items-center gap-2">
                          <Bot size={15} className="text-ink-3" aria-hidden /> {text(agent, "name")}
                        </span>
                      }
                      actions={<Status value={a.label} tone={a.available ? "success" : "neutral"} />}
                      footer={
                        text(agent, "used_in") ? (
                          <>
                            <span className="text-ink-3">Where to use it </span>
                            {text(agent, "used_in")}
                          </>
                        ) : undefined
                      }
                      className="flex h-full flex-col"
                    >
                      <p className="m-0 text-[13px] text-ink-2">{text(agent, "purpose")}</p>
                    </Card>
                  );
                })}
              </Grid>
            )}
          </>
        );
      }}
    </Query>
  );
}

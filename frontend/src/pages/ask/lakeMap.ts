/** What you can do with Lake, on Ask Orama's home (lake#764), as pure rules so `npm test` runs them
 * (tests/home.test.ts). The groups are the rail's (Data, Design, Train, Ship, Network, Organization,
 * Developer); each item is a page the rail offers this person, what it is for in one line, and one
 * question to ask Ask Orama about it. It claims only what this Lake runs: an item shows only when
 * the rail shows its page (audience, billing), Deployments only when `server_info` does not say
 * they are off, and an item that needs a library agent only while that agent runs. No imports. */

/** The part of a rail item this reads (routes/nav.ts NavItem). */
type Item = { section: string; path: string; label: string };

/** One thing to do: the page, what it is for, and a question that goes into the message box. */
export type LakeThing = { path: string; label: string; does: string; ask: string };
export type LakeGroup = { key: string; title: string; line: string; things: LakeThing[] };

/** The groups, in the rail's order, and the one line that says what each is. */
export const LAKE_GROUPS: { key: string; title: string; line: string }[] = [
  { key: "data", title: "Data", line: "Bring in what you have." },
  { key: "design", title: "Design", line: "Decide what to build from it." },
  { key: "train", title: "Train", line: "Train on your own data." },
  { key: "ship", title: "Ship", line: "Put it to work." },
  { key: "network", title: "Network", line: "Control what reaches your workspace." },
  { key: "organization", title: "Organization", line: "Work as a team." },
  { key: "developer", title: "Developer", line: "Everything here is also an API." },
];

/** Each page: what it is for, the question to ask, and the library agent it needs to be useful. */
const THINGS: Record<string, { does: string; ask: string; needs?: string }> = {
  "/repositories": { does: "Code and files, with their full history.", ask: "Which of my repositories changed this week?" },
  "/drives": { does: "Folders of files to keep and share.", ask: "What is in my drives?" },
  "/train/data": { does: "Tables to explore, query and train on.", ask: "How do I load my first dataset?" },
  "/connections": { does: "The databases your workspace reads.", ask: "How do I connect a Postgres database?" },
  "/transcripts": { does: "Recordings and notes, made searchable.", ask: "Summarize my latest transcript." },
  "/mcp-connections": { does: "Remote tools your agents may call.", ask: "Which tools can my agents call?" },
  "/design/projects": { does: "Start from a business question and the decision it should improve.", ask: "Help me turn a business question into a project." },
  "/design/opportunities": { does: "What your data could be used for, best first.", ask: "What is worth building from my data?", needs: "agent-autosuggest" },
  "/design/solutions": { does: "Ready-made solutions to start a project from.", ask: "Which solution fits predicting customer churn?" },
  "/design/methodologies": { does: "The methods a pipeline trains with.", ask: "Which methodologies suit a sales forecast?" },
  "/design/agents": { does: "Agents that each do one job for you.", ask: "Help me design an agent that watches new orders." },
  "/train/runs": { does: "Every training run and what it produced.", ask: "How did my latest run go?" },
  "/train/experiments": { does: "Compare versions, prompts or models, then promote the best.", ask: "Propose an experiment to improve my model." },
  "/train/models": { does: "The models trained on your data.", ask: "Which of my models performs best?" },
  "/train/agents": { does: "Improve the agents that learn.", ask: "How is my agent's training going?" },
  "/ship/models": { does: "Call a model from your own code.", ask: "How do I call my model from Python?" },
  "/ship/indexes": { does: "Search over your own data.", ask: "Help me make a search index over my documents." },
  "/ship/packages": { does: "The images and packages your builds publish.", ask: "Which packages did my builds publish?" },
  "/ship/apps": { does: "Web and data apps that run in your workspace.", ask: "Build an app that lists this week's at-risk customers." },
  "/deployments": { does: "Where your repositories are deployed.", ask: "What is deployed right now?" },
  "/services": { does: "Databases and services for your team.", ask: "Start a Postgres database for my team." },
  "/workspaces": { does: "Workspaces of your own, each with its own data.", ask: "What is in this workspace?" },
  "/network/overview": { does: "What your workspace reaches, and what reaches it.", ask: "What can reach my workspace?" },
  "/organizations": { does: "Organizations and their members.", ask: "How do I invite someone to my organization?" },
  "/secret-mounts": { does: "Keys your apps and services use, kept sealed.", ask: "Which secrets do my apps use?" },
  "/usage": { does: "What you used, over time.", ask: "What did I use this month?" },
  "/billing": { does: "Your plan and invoices.", ask: "What will my next invoice be?" },
  "/activity": { does: "Who did what, and when.", ask: "What changed in my workspace today?" },
  "/developer/api": { does: "Every page here is an API call you can make with a token.", ask: "How do I call Lake's API with a token?" },
  "/developer/mcp": { does: "Connect your own AI assistant to Lake.", ask: "How do I connect my AI assistant to Lake over MCP?" },
};

/** What this Lake runs, as the home reads it: `server_info.deployments` (false: not set up) and
 * the library agents that run (`list_agent_library`, `deployed`). */
export type LakeRuns = { deployments?: boolean | null; runningAgents: ReadonlySet<string> };

/** The groups for this person: the rail's items they may open (`navFor`), in its order, that this
 * Lake runs and the map describes. Empty groups are left out. */
export function lakeMap(items: Item[], runs: LakeRuns): LakeGroup[] {
  const groups: LakeGroup[] = [];
  for (const group of LAKE_GROUPS) {
    const things: LakeThing[] = [];
    const seen = new Set<string>();
    for (const item of items) {
      const thing = THINGS[item.path];
      if (!thing || seen.has(item.path)) continue;
      // Datasets sit in the rail's Data group; the map lists each page once, in its rail group.
      if (item.section !== group.key) continue;
      if (item.path === "/deployments" && runs.deployments === false) continue;
      if (thing.needs && !runs.runningAgents.has(thing.needs)) continue;
      seen.add(item.path);
      things.push({ path: item.path, label: item.label, does: thing.does, ask: thing.ask });
    }
    if (things.length) groups.push({ key: group.key, title: group.title, line: group.line, things });
  }
  return groups;
}

/** The pages the map describes, for the test that every one is a rail page. */
export const MAPPED_PATHS: string[] = Object.keys(THINGS);

/** Ask Orama as the home (lake#600): what `/` shows each viewer, the rail's Home group and which of
 * its items is current, old `/ask` and Command Center links, what a link hands the shared
 * conversation, and the starting questions. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { askLinkTarget, confirmOutcomes, greeting, homeLink, homeView, isAskHome, landingAfterSignIn, linkThread, starterQuestions } from "../src/pages/ask/home.ts";
import { HOME_NAV, currentNavIn, localPath } from "../src/routes/nav.ts";

const person = { signed_in: true, administrator: false };
const operator = { signed_in: true, administrator: true };
const visitor = { signed_in: false };

test("a signed-in person's home is Ask Orama; the operator and visitors keep the landing", () => {
  assert.deepEqual(homeView(person, ""), { view: "ask" });
  assert.deepEqual(homeView(person, "?from=%2Ftrain%2Fruns"), { view: "ask" });
  assert.deepEqual(homeView(operator, ""), { view: "landing" });
  assert.deepEqual(homeView(visitor, ""), { view: "landing" });
  assert.deepEqual(homeView(visitor, "?ws=a"), { view: "landing" });
});

test("an old Command Center link opens the Dashboard with its query", () => {
  assert.deepEqual(homeView(person, "?ws=a,b"), { view: "redirect", to: "/dashboard?ws=a,b" });
});

test("the Ask Orama home is `/` for a signed-in person only", () => {
  assert.equal(isAskHome("/", person), true);
  assert.equal(isAskHome("/dashboard", person), false);
  assert.equal(isAskHome("/w/abc/design/projects", person), false);
  assert.equal(isAskHome("/", operator), false);
  assert.equal(isAskHome("/", visitor), false);
});

test("the rail's Home group: Ask Orama, then Dashboard, for a signed-in person", () => {
  const account = HOME_NAV.filter((i) => i.audience.includes("account"));
  assert.deepEqual(
    account.map((i) => [i.section, i.label, i.path]),
    [
      ["home", "Ask Orama", "/"],
      ["home", "Dashboard", "/dashboard"],
    ],
  );
  for (const who of ["visitor", "operator"] as const) {
    assert.deepEqual(
      HOME_NAV.filter((i) => i.audience.includes(who)).map((i) => [i.section, i.label, i.path]),
      [["top", "Home", "/"]],
    );
  }
});

test("each Home item is current on its own page only", () => {
  const items = HOME_NAV.filter((i) => i.audience.includes("account"));
  assert.equal(currentNavIn("/", items)?.label, "Ask Orama");
  assert.equal(currentNavIn("/dashboard", items)?.label, "Dashboard");
  assert.equal(currentNavIn("/dashboard/anything", items)?.label, "Dashboard");
  assert.equal(currentNavIn("/train/runs", items), undefined);
  assert.equal(currentNavIn("/w/abc/", items)?.label, "Ask Orama");
  assert.equal(localPath("/w/abc/train/runs"), "/train/runs");
  assert.equal(localPath("/w/abc"), "/");
});

test("old /ask links open the home with their query, without customer_id", () => {
  assert.equal(askLinkTarget(""), "/");
  assert.equal(askLinkTarget("?customer_id=c1"), "/");
  assert.equal(askLinkTarget("?customer_id=c1&conversation=k1&task=t1"), "/?conversation=k1&task=t1");
  assert.equal(askLinkTarget("?agent=agent-x&from=%2Ftrain%2Fruns"), "/?agent=agent-x&from=%2Ftrain%2Fruns");
});

test("a home link hands its conversation over and keeps only from", () => {
  assert.deepEqual(linkThread(""), { adopt: null, keep: "" });
  assert.deepEqual(linkThread("?from=%2Ftrain%2Fruns"), { adopt: null, keep: "?from=%2Ftrain%2Fruns" });
  assert.deepEqual(linkThread("?conversation=k1&task=t1"), { adopt: { conversation: "k1", task: "t1" }, keep: "" });
  assert.deepEqual(linkThread("?conversation=k1&agent=agent-x"), { adopt: { conversation: "k1", task: "", agent: "agent-x" }, keep: "" });
  // "Talk to this agent in Ask Orama": a new conversation with that agent.
  assert.deepEqual(linkThread("?agent=agent-x"), { adopt: { conversation: "", task: "", agent: "agent-x" }, keep: "" });
});

test("the panel's Open as a page goes to the home, about its page", () => {
  assert.equal(homeLink(""), "/");
  assert.equal(homeLink("/"), "/");
  assert.equal(homeLink("/train/run?id=r1"), "/?from=%2Ftrain%2Frun%3Fid%3Dr1");
  // The home reads it back as the same page.
  assert.equal(new URLSearchParams(homeLink("/train/run?id=r1").slice(1)).get("from"), "/train/run?id=r1");
});

test("starting questions come from the workspace's datasets, projects and runs", () => {
  const questions = starterQuestions({
    datasets: ["sales", "customers"],
    projects: ["churn"],
    runs: [
      { status: "failed", pipeline_name: "forecast" },
      { status: "succeeded", pipeline_name: "segments" },
    ],
  });
  assert.deepEqual(questions, [
    "Why did the latest run of forecast fail?",
    "What is in the sales dataset, and what could I predict from it?",
    "Where does the churn project stand, and what should I do next?",
    "What did the latest run of segments find?",
  ]);
});

test("an empty workspace is offered its first steps", () => {
  assert.deepEqual(starterQuestions({}), [
    "How do I load my first dataset?",
    "Help me turn a business question into a project.",
    "What is in this workspace, and what should I look at first?",
    "What can Lake do with my data?",
  ]);
  const some = starterQuestions({ datasets: ["sales"] });
  assert.equal(some.length, 4);
  assert.equal(some[0], "What is in the sales dataset, and what could I predict from it?");
  assert.equal(new Set(some).size, some.length);
  for (const q of [...some, ...starterQuestions({})]) assert.doesNotMatch(q, /\b(space|study|Zetta)\b/);
});

test("the empty home greets the person by first name, never by email", () => {
  assert.equal(greeting("Steve Sperandeo"), "Hello, Steve. What would you like to know?");
  assert.equal(greeting("steve@example.com"), "What would you like to know?");
  assert.equal(greeting(undefined), "What would you like to know?");
});

test("signing in lands a person on the home and the operator on Repositories", () => {
  assert.equal(landingAfterSignIn(false), "/");
  assert.equal(landingAfterSignIn(true), "/repositories");
});

test("the home says what Lake does, grouped as the rail is, claiming only what this Lake runs (lake#764)", async () => {
  const { lakeMap, LAKE_GROUPS, MAPPED_PATHS } = await import("../src/pages/ask/lakeMap.ts");
  const { readFileSync } = await import("node:fs");
  const routes = readFileSync(new URL("../src/routes.tsx", import.meta.url), "utf8") + readFileSync(new URL("../src/routes/nav.ts", import.meta.url), "utf8");
  // Every page the map describes is a rail page, so every link goes to a page that exists.
  for (const path of MAPPED_PATHS) assert.ok(routes.includes(`path: "${path}"`), `${path} is not a rail page`);
  assert.deepEqual(LAKE_GROUPS.map((g) => g.title), ["Data", "Design", "Train", "Ship", "Network", "Organization", "Developer"]);
  const items = [
    { section: "data", path: "/repositories", label: "Repositories" },
    { section: "data", path: "/train/data", label: "Datasets" },
    { section: "data", path: "/search", label: "Search" },
    { section: "design", path: "/design/opportunities", label: "Opportunities" },
    { section: "design", path: "/design/projects", label: "Projects" },
    { section: "ship", path: "/deployments", label: "Deployments" },
    { section: "ship", path: "/ship/apps", label: "Apps" },
    { section: "organization", path: "/billing", label: "Billing" },
    { section: "developer", path: "/developer/mcp", label: "MCP" },
  ];
  const all = lakeMap(items, { deployments: true, runningAgents: new Set(["agent-autosuggest"]) });
  assert.deepEqual(all.map((g) => g.key), ["data", "design", "ship", "organization", "developer"]);
  assert.deepEqual(all[0].things.map((t) => t.label), ["Repositories", "Datasets"], "a page the map does not describe is left out");
  for (const group of all) for (const thing of group.things) assert.ok(thing.ask.endsWith("?") || thing.ask.endsWith("."), thing.ask);
  // Deployments off in server_info, and the Auto-Suggest Agent not running: neither is claimed.
  const fewer = lakeMap(items, { deployments: false, runningAgents: new Set() });
  assert.deepEqual(fewer.find((g) => g.key === "design")!.things.map((t) => t.path), ["/design/projects"]);
  // MCP's Worked Flows follow Developer → MCP, and only where the rail offers it (lake#789).
  assert.deepEqual(all.find((g) => g.key === "developer")!.things.map((t) => t.path), ["/developer/mcp", "/developer/mcp?tab=tools", "/developer/mcp?tab=flows"]);
  // Every Network page the rail offers is on the home (lake#791).
  const network = ["/network/overview", "/network/connections", "/network/access", "/network/private-networks", "/network/egress", "/network/policies", "/network/activity"];
  const withNetwork = lakeMap(network.map((path) => ({ section: "network", path, label: path })), { runningAgents: new Set() });
  assert.deepEqual(withNetwork.find((g) => g.key === "network")!.things.map((t) => t.path), network);
  // Design is about AI (Steve, 2026-10-17): the home names its pages as AI.
  assert.equal(fewer.find((g) => g.key === "design")!.things[0].label, "AI Projects");
  assert.deepEqual(fewer.find((g) => g.key === "ship")!.things.map((t) => t.path), ["/ship/apps"]);
  // Billing shows only when the rail shows it (navFor drops it where LAKE_BILLING is off).
  assert.equal(lakeMap(items.filter((i) => i.path !== "/billing"), { runningAgents: new Set() }).find((g) => g.key === "organization"), undefined);
  // An older Lake that does not say whether deployments are set up keeps the page.
  assert.ok(lakeMap(items, { runningAgents: new Set() }).find((g) => g.key === "ship")!.things.some((t) => t.path === "/deployments"));
  for (const group of all) for (const thing of group.things) assert.doesNotMatch(`${thing.does} ${thing.ask}`, /\b(space|study|Zetta)\b/);
});

test("a confirm that did not work is not Confirmed and keeps its reason (lake#794)", () => {
  const outcomes = confirmOutcomes([
    { content: "I can do that.", payload: { proposals: [] } as never },
    { content: "That did not work: create_index needs `table`.", payload: { confirmed: { message_id: "m1", index: "0" }, ok: false } },
    { content: "Done: create_stream.", payload: { confirmed: { message_id: "m1", index: "1" }, ok: true } },
    { content: "That did not work: busy", payload: { confirmed: { message_id: "m2", index: "0" }, ok: false } },
    { content: "Done: create_repository.", payload: { confirmed: { message_id: "m2", index: "0" }, ok: true } },
  ]);
  assert.deepEqual([...outcomes.confirmed].sort(), ["m1/1", "m2/0"]);
  assert.deepEqual(outcomes.refused, { "m1/0": "That did not work: create_index needs `table`." });
});

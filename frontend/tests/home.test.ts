/** The home and Ask Orama's page: Quickstart is a signed-in person's home (lake#1128) and Ask Orama
 * has its own page, `/ask` (it was the home from lake#600); the rail's Home group and which of its
 * items is current, old home links with a conversation, `/w/<id>/ask` and Command Center links, what
 * a link hands the shared conversation, and the starting questions. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { askLinkTarget, confirmOutcomes, greeting, homeLink, homeView, isAskHome, landingAfterSignIn, linkThread, proposalCall, shownValue, starterQuestions, visibleText } from "../src/pages/ask/home.ts";
import { HOME_NAV, currentNavIn, localPath } from "../src/routes/nav.ts";
import { readFileSync } from "node:fs";
import { createElement } from "react";
import { renderToStaticMarkup } from "react-dom/server";
import { ProposalCallView, VALUE_PREVIEW_LINES, fieldValueLayout } from "../src/pages/ask/proposalCard.ts";

const read = (path: string) => readFileSync(new URL(`../${path}`, import.meta.url), "utf8");

const person = { signed_in: true, administrator: false };
const operator = { signed_in: true, administrator: true };
const visitor = { signed_in: false };

test("a signed-in person's home is Quickstart; the operator and visitors keep the landing", () => {
  assert.deepEqual(homeView(person, ""), { view: "quickstart" });
  assert.deepEqual(homeView(operator, ""), { view: "landing" });
  assert.deepEqual(homeView(visitor, ""), { view: "landing" });
  assert.deepEqual(homeView(visitor, "?ws=a"), { view: "landing" });
  assert.deepEqual(homeView(visitor, "?conversation=k1"), { view: "landing" });
});

test("an old Command Center link opens the Dashboard with its query", () => {
  assert.deepEqual(homeView(person, "?ws=a,b"), { view: "redirect", to: "/dashboard?ws=a,b" });
});

test("an old Ask Orama home link opens Ask Orama's page with its query", () => {
  assert.deepEqual(homeView(person, "?from=%2Ftrain%2Fruns"), { view: "redirect", to: "/ask?from=%2Ftrain%2Fruns" });
  assert.deepEqual(homeView(person, "?conversation=k1&task=t1"), { view: "redirect", to: "/ask?conversation=k1&task=t1" });
  assert.deepEqual(homeView(person, "?agent=agent-x"), { view: "redirect", to: "/ask?agent=agent-x" });
});

test("Ask Orama's page is `/ask` for a signed-in person only", () => {
  assert.equal(isAskHome("/ask", person), true);
  assert.equal(isAskHome("/", person), false, "the home is Quickstart now");
  assert.equal(isAskHome("/dashboard", person), false);
  assert.equal(isAskHome("/w/abc/design/projects", person), false);
  assert.equal(isAskHome("/ask", operator), false);
  assert.equal(isAskHome("/ask", visitor), false);
});

test("the rail's Home group: Quickstart, Ask Orama, then Dashboard, for a signed-in person", () => {
  const account = HOME_NAV.filter((i) => i.audience.includes("account"));
  assert.deepEqual(
    account.map((i) => [i.section, i.label, i.path]),
    [
      ["home", "Quickstart", "/"],
      ["home", "Ask Orama", "/ask"],
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
  assert.equal(currentNavIn("/", items)?.label, "Quickstart");
  assert.equal(currentNavIn("/ask", items)?.label, "Ask Orama");
  assert.equal(currentNavIn("/dashboard", items)?.label, "Dashboard");
  assert.equal(currentNavIn("/dashboard/anything", items)?.label, "Dashboard");
  assert.equal(currentNavIn("/train/runs", items), undefined);
  assert.equal(currentNavIn("/w/abc/", items)?.label, "Quickstart");
  assert.equal(localPath("/w/abc/train/runs"), "/train/runs");
  assert.equal(localPath("/w/abc"), "/");
});

test("Ask Orama links open /ask with their query, without customer_id", () => {
  assert.equal(askLinkTarget(""), "/ask");
  assert.equal(askLinkTarget("?customer_id=c1"), "/ask");
  assert.equal(askLinkTarget("?customer_id=c1&conversation=k1&task=t1"), "/ask?conversation=k1&task=t1");
  assert.equal(askLinkTarget("?agent=agent-x&from=%2Ftrain%2Fruns"), "/ask?agent=agent-x&from=%2Ftrain%2Fruns");
});

test("a home link hands its conversation over and keeps only from", () => {
  assert.deepEqual(linkThread(""), { adopt: null, keep: "" });
  assert.deepEqual(linkThread("?from=%2Ftrain%2Fruns"), { adopt: null, keep: "?from=%2Ftrain%2Fruns" });
  assert.deepEqual(linkThread("?conversation=k1&task=t1"), { adopt: { conversation: "k1", task: "t1" }, keep: "" });
  assert.deepEqual(linkThread("?conversation=k1&agent=agent-x"), { adopt: { conversation: "k1", task: "", agent: "agent-x" }, keep: "" });
  // "Talk to this agent in Ask Orama": a new conversation with that agent.
  assert.deepEqual(linkThread("?agent=agent-x"), { adopt: { conversation: "", task: "", agent: "agent-x" }, keep: "" });
});

test("the panel's Open as a page goes to Ask Orama's page, about its page", () => {
  assert.equal(homeLink(""), "/ask");
  assert.equal(homeLink("/"), "/ask");
  assert.equal(homeLink("/ask"), "/ask");
  assert.equal(homeLink("/train/run?id=r1"), "/ask?from=%2Ftrain%2Frun%3Fid%3Dr1");
  // The page reads it back as the same page.
  assert.equal(new URLSearchParams(homeLink("/train/run?id=r1").split("?")[1]).get("from"), "/train/run?id=r1");
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
  assert.deepEqual(outcomes.refused, { "m1/0": { reason: "That did not work: create_index needs `table`.", retry: true } });
});

test("Try again is offered only when Lake says trying again could work (lake#794)", () => {
  const outcomes = confirmOutcomes([
    {
      content: "That did not work: `topic` is not valid for create_stream: a concrete topic name is required. Ask Orama again with what to use and it will propose the change again.",
      payload: { confirmed: { message_id: "m1", index: "0" }, ok: false, retry: false, error: { code: "invalid_input" } } as never,
    },
    {
      content: "That did not work: the workspace is not reachable.",
      payload: { confirmed: { message_id: "m1", index: "1" }, ok: false, retry: true, error: { code: "store_unavailable" } } as never,
    },
  ]);
  assert.equal(outcomes.refused["m1/0"].retry, false);
  assert.match(outcomes.refused["m1/0"].reason, /Ask Orama again/);
  assert.equal(outcomes.refused["m1/1"].retry, true);
  assert.equal(outcomes.confirmed.size, 0);
});

test("Confirm shows the exact invitation Lake runs, not the model's impact (lake#942)", () => {
  const sha = "a".repeat(64);
  const message = {
    payload: { proposals: [{ capability: "create_invitation", impact: "Fixes sharing for your team; nothing else changes.", input: {} }] },
    proposal_calls: [
      {
        capability: "create_invitation",
        title: "Create invitation",
        kind: "action",
        input: { organization_id: "org-1", invitee_type: "email", invitee: "outsider@evil.test", role: "owner" },
        fields: [
          { name: "invitee", description: "", value: "outsider@evil.test" },
          { name: "invitee_type", description: "", value: "email" },
          { name: "organization_id", description: "", value: "org-1" },
          { name: "role", description: "", value: "owner" },
        ],
        input_sha256: sha,
      },
    ],
  };
  const call = proposalCall(message, 0);
  assert.ok(call);
  assert.equal(call.capability, "create_invitation");
  assert.equal(call.input_sha256, sha);
  const shown = Object.fromEntries(call.fields.map((f) => [f.name, shownValue(f.value)]));
  assert.deepEqual(shown, { invitee: '"outsider@evil.test"', invitee_type: '"email"', organization_id: '"org-1"', role: '"owner"' });
  assert.ok(!JSON.stringify(call).includes("Fixes sharing"), "the model's impact is no part of the call");
});

test("a proposal Lake states no exact call for offers no Confirm (lake#942)", () => {
  const good = { capability: "create_invitation", title: "", kind: "action", fields: [], input_sha256: "b".repeat(64) };
  assert.equal(proposalCall({}, 0), null, "a message stored without proposal_calls");
  assert.equal(proposalCall({ proposal_calls: [null] }, 0), null);
  assert.equal(proposalCall({ proposal_calls: [good] }, 1), null, "another index");
  assert.equal(proposalCall({ proposal_calls: [{ ...good, input_sha256: "" }] }, 0), null);
  assert.equal(proposalCall({ proposal_calls: [{ ...good, capability: 7 }] }, 0), null);
  assert.equal(proposalCall({ proposal_calls: [{ ...good, fields: "role: member" }] }, 0), null);
  assert.equal(proposalCall({ proposal_calls: [{ ...good, fields: [{ value: "x" }] }] }, 0), null, "a field without a name");
  assert.ok(proposalCall({ proposal_calls: [good] }, 0));
});

test("a shown value cannot hide characters (lake#942)", () => {
  assert.equal(shownValue("admin\u202e@evil.test"), '"admin\\u{202e}@evil.test"');
  assert.equal(shownValue("a\u200bb"), '"a\\u{200b}b"');
  assert.equal(shownValue(" padded "), '" padded "');
  assert.equal(shownValue("two\nlines"), '"two\\nlines"');
  assert.equal(shownValue({ public: true }), '{\n  "public": true\n}');
  assert.equal(shownValue(3), "3");
  assert.equal(visibleText("role\u2066"), "role\\u{2066}", "a field's name too");
});

test("the confirm card gives every value its own full-width row (lake#1192)", () => {
  const customer = "b267e8df-afb0-41ee-bcb7-02713e7a9fbe";
  const intent =
    "Segment customers from datasets.ux_review_20261018_customers into behavioural clusters and show each cluster's size, its defining columns and how it changed over the last quarter";
  const hints = { source: "datasets", table: "ux_review_20261018_customers", entity_column: "customer_id", columns: ["tenure", "spend", "plan", "region"] };
  const call = {
    capability: "compile_pipeline",
    title: "Build a pipeline from a description",
    kind: "action",
    fields: [
      { name: "customer_id", description: "the account whose workspace this is", value: customer },
      { name: "intent", description: "reserved: say what you are trying to do; recorded on the call's trace, stripped before schema validation", value: intent },
      { name: "hints", description: "known dataset, label, time column, and other shape hints", value: hints },
    ],
    input_sha256: "c".repeat(64),
  };
  const html = renderToStaticMarkup(createElement(ProposalCallView, { call }));
  // No two-column grid: the dl whose description column took the width is gone.
  assert.doesNotMatch(html, /<(dl|dt|dd)\b/);
  const rows = html.match(/<li class="ask-proposal-field">[\s\S]*?<\/li>/g) ?? [];
  assert.equal(rows.length, 3, html);
  // Each row is the name, then the value, then the description.
  const [uuidRow, intentRow, hintsRow] = rows;
  assert.match(uuidRow, /^<li class="ask-proposal-field"><code class="ask-proposal-field-name">customer_id<\/code><code class="ask-proposal-value">&quot;b267e8df-afb0-41ee-bcb7-02713e7a9fbe&quot;<\/code><p class="ask-proposal-field-about">the account whose workspace this is<\/p><\/li>$/);
  // A long sentence is a wrapped block, whole, and short enough to need no "Show all".
  assert.ok(intentRow.indexOf("ask-proposal-field-name") < intentRow.indexOf(intent.slice(0, 40)));
  assert.ok(intentRow.indexOf(intent.slice(-30)) < intentRow.indexOf("ask-proposal-field-about"));
  assert.match(intentRow, /<pre class="ask-proposal-value"><code>&quot;Segment customers/);
  assert.doesNotMatch(intentRow, /Show all/);
  // A JSON object is laid out over lines, every line in the page, and folds past a few lines.
  const layout = fieldValueLayout(hints);
  assert.ok(layout.block && layout.collapsible && layout.lines > VALUE_PREVIEW_LINES, JSON.stringify(layout));
  assert.match(hintsRow, /<pre class="ask-proposal-value ask-proposal-value-collapsed"><code>\{\n  &quot;source&quot;: &quot;datasets&quot;,/);
  assert.ok(hintsRow.includes("&quot;region&quot;\n  ]\n}"), "the last line is in the page, not cut");
  assert.match(hintsRow, new RegExp(`aria-expanded="false"[^>]*>Show all ${layout.lines} lines</button>`));
  // A short value stays inline; a long one-line value becomes a block.
  assert.equal(fieldValueLayout("owner").block, false);
  assert.equal(fieldValueLayout("x".repeat(120)).block, true);
  assert.equal(fieldValueLayout("x".repeat(120)).collapsible, false);
  // The page uses this card, and its value wraps at any character in the card's full width.
  assert.match(read("src/pages/ask/conversation.tsx"), /import \{ ProposalCallView \} from "\.\/proposalCard"/);
  const css = read("src/styles/lake.css");
  const rule = (selector: string) => css.match(new RegExp(`(^|\\n)${selector.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")} \\{([^}]*)\\}`))?.[2] ?? "";
  assert.match(rule(".ask-proposal-fields"), /flex-direction: column/);
  assert.match(rule(".ask-proposal-value"), /overflow-wrap: anywhere/);
  assert.match(rule(".ask-proposal-value"), /white-space: pre-wrap/);
  assert.match(rule(".ask-proposal-value-collapsed"), /overflow: hidden/);
});

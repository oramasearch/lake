/** Ask Orama as the home (lake#600): what `/` shows each viewer, the rail's Home group and which of
 * its items is current, old `/ask` and Command Center links, what a link hands the shared
 * conversation, and the starting questions. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { askLinkTarget, greeting, homeLink, homeView, isAskHome, landingAfterSignIn, linkThread, starterQuestions } from "../src/pages/ask/home.ts";
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

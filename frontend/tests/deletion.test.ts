/** What each delete and revoke dialog lists from its `dry_run: true` answer, and what the person
 * types to confirm (docs/ui/deletion.md, lake#645). `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import {
  accountBlockers,
  accountLines,
  accountTyped,
  conversationLines,
  datasetLines,
  grantLines,
  indexLines,
  invitationLines,
  modelLines,
  organizationBlockers,
  organizationLines,
  organizationTyped,
  runLines,
  secretMountLines,
} from "../src/lib/deletion.ts";

test("an organization's plan names the workspaces in the way, and only counts that are there", () => {
  const plan = {
    organization_id: "o-1",
    handle: "acme",
    workspaces: [{ id: "w-1", name: "analytics", environment: "prod" }, { name: "no id" }],
    counts: { memberships: 3, teams: 0, invitations: 2, grants: 5, sso_providers: 0, federation_issuers: 0 },
  };
  assert.deepEqual(organizationBlockers(plan), [{ id: "w-1", name: "analytics", environment: "prod" }]);
  const lines = organizationLines(plan);
  assert.equal(lines[0].count, null);
  assert.deepEqual(
    lines.slice(1).map((line) => [line.count, line.text]),
    [
      [3, "members, who lose access"],
      [2, "invitations"],
      [5, "access grants"],
    ],
  );
  assert.equal(organizationTyped(plan), "acme");
  assert.equal(organizationTyped({ organization_id: "o-2", handle: null }), "o-2", "without a handle, its id");
  assert.deepEqual(organizationBlockers({}), []);
});

test("a mount's plan counts its secrets with their versions, keys and leases", () => {
  const lines = secretMountLines({ counts: { secrets: 1, versions: 2, encryption_keys: 1, data_keys: 2, leases: 0 } });
  assert.deepEqual(lines[0], { count: 1, text: "secret with 2 versions" });
  assert.match(lines[1].text, /^encryption key\. Anything they encrypted/);
  assert.equal(lines[2].count, 2);
  assert.equal(lines.length, 4, "no line for leases it has none of");
  assert.equal(secretMountLines({ counts: {} })[0].count, 0, "the secrets line stays at zero");
});

test("engine deletions say what goes and what is kept", () => {
  assert.deepEqual(datasetLines({ removed: { rows: 1200, size_bytes: 2_500_000, partitions: 1, indexes: ["i-1"] } }), [
    { count: 1200, text: "rows (2.5 MB)" },
    { count: 1, text: "search index built on it" },
  ]);
  assert.deepEqual(datasetLines({}), [{ count: 0, text: "rows" }], "an answer without figures still reads");
  assert.equal(modelLines({ removed: { was_exposed: true }, kept: { run: "r-1" } }).length, 3);
  assert.equal(modelLines({ removed: {} }).length, 1);
  assert.equal(runLines({ kept: { models: ["m-1", "m-2"] } })[1].text, "Kept: the 2 models it trained.");
  assert.equal(runLines({ kept: { models: ["m-1"] } })[1].text, "Kept: the model it trained.");
  assert.equal(indexLines({ removed: { was_exposed: false } }).at(-1)?.text, "Kept: the dataset it indexes.");
});

test("a conversation shows its count and how it starts, shortened", () => {
  const long = "x".repeat(200);
  const lines = conversationLines({ messages: 4, first_message: long });
  assert.deepEqual(lines[0], { count: 4, text: "messages" });
  assert.equal(lines[1].text.length, "It starts: “”".length + 120);
  assert.equal(conversationLines({ messages: 1, first_message: "" }).length, 1);
});

test("a conversation counts the agent tasks that go with it (lake#673)", () => {
  const tasks = [{ task_id: "a", agent: "agent-ask-orama", status: "succeeded" }, { task_id: "b", agent: "agent-autosuggest", status: "running" }];
  const lines = conversationLines({ messages: 2, first_message: "hi", agent_tasks: tasks });
  assert.deepEqual(lines[2], { count: 2, text: "agent tasks it started, with what was asked and answered" });
  assert.deepEqual(conversationLines({ messages: 2, agent_tasks: [tasks[0]] })[1], { count: 1, text: "agent task it started, with what was asked and answered" });
  assert.equal(conversationLines({ messages: 2, agent_tasks: [] }).length, 1);
});

test("revokes name who and what", () => {
  assert.match(invitationLines({ invitee: "a@example.test", role: "admin" })[0].text, /a@example.test can no longer accept .* as admin/);
  assert.match(grantLines({ role: "read" })[0].text, /^The read role/);
});

test("an account's plan counts what goes, names the organizations in the way, and confirms with the handle", () => {
  const plan = {
    user_id: "u-1",
    handle: "brave-otter-7",
    workspaces: [{ id: "w-1", name: "dev" }],
    organizations: [
      { organization_id: "o-1", display_name: "Solo", action: "delete", workspaces: [{ id: "w-2" }], other_members: 0 },
      { organization_id: "o-2", display_name: "Shared", action: "leave", workspaces: [{ id: "w-3" }], other_members: 2 },
      { organization_id: "o-3", display_name: "Acme", handle: "acme", action: "blocked", workspaces: [], other_members: 4 },
    ],
    counts: { sessions: 2, credentials: 1, devices: 1, grants: 0, billing_contacts: 1 },
  };
  assert.deepEqual(accountBlockers(plan), [{ id: "o-3", name: "Acme", handle: "acme", members: 4 }]);
  const lines = accountLines(plan);
  assert.equal(lines[0].count, null);
  assert.equal(lines[lines.length - 1].count, null);
  assert.deepEqual(
    lines.slice(1, -1).map((line) => [line.count, line.text]),
    [
      [2, "workspaces, with everything in them"],
      [1, "organization you alone own, with their members and invitations"],
      [1, "organization you leave to their other owners"],
      [2, "sessions and tokens, signed out at once"],
      [2, "credentials and devices (passwords, passkeys, MFA)"],
      [1, "billing contact"],
    ],
  );
  assert.equal(accountTyped(plan), "brave-otter-7");
  assert.equal(accountTyped({ user_id: "u-2", handle: null }), "u-2", "without a handle, its id");
  assert.deepEqual(accountBlockers({}), []);
});

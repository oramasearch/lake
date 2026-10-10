/**
 * Developer → API and Developer → MCP (src/pages/developer/logic.ts): the `create_token` input a
 * key form builds (as the server's MCP key handler and Tokens built it), how a token's scopes
 * read, and the OpenAPI document as grouped, searchable operations with a curl line.
 */
import { test } from "node:test";
import assert from "node:assert/strict";
import {
  accessOf,
  curlFor,
  groupOperations,
  type KeyForm,
  keyFormProblem,
  keyInput,
  keyState,
  operationsOf,
  requestBodyOf,
  responseSchemaOf,
  skeleton,
} from "../src/pages/developer/logic.ts";

const scopes = { read_only: ["list_repositories", "viewer"], all: ["list_repositories", "viewer", "create_repository"] };
const form = (over: Partial<KeyForm> = {}): KeyForm => ({
  purpose: "mcp",
  name: "",
  access: "all",
  named: "",
  lifetime: "",
  seconds: "",
  isStatic: false,
  reason: "",
  ...over,
});

test("an every-tool key for the sign-in's lifetime is scoped * and names no expiry", () => {
  assert.deepEqual(keyInput(form(), scopes), { name: "MCP key", scopes: ["*"], static: false, reason: "created on Developer → MCP" });
});

test("a read-only key carries the read tools and a fixed lifetime in seconds", () => {
  const input = keyInput(form({ purpose: "api", access: "read", lifetime: "604800" }), scopes);
  assert.equal(input.name, "API key (read-only)");
  assert.deepEqual(input.scopes, scopes.read_only);
  assert.equal(input.expires_in_seconds, 604800);
  assert.equal(input.reason, "created on Developer → API");
});

test("a key that never expires names every capability instead of *, and keeps its reason", () => {
  const input = keyInput(form({ lifetime: "never", reason: " nightly sync " }), scopes);
  assert.equal(input.never_expires, true);
  assert.deepEqual(input.scopes, scopes.all);
  assert.equal(input.reason, "nightly sync");
  assert.equal("expires_in_seconds" in input, false);
});

test("named scopes are split on commas; custom seconds are sent as a number", () => {
  const input = keyInput(form({ access: "named", named: "refs, push,,pull ", lifetime: "custom", seconds: "3600", name: " CI " }), scopes);
  assert.deepEqual(input.scopes, ["refs", "push", "pull"]);
  assert.equal(input.expires_in_seconds, 3600);
  assert.equal(input.name, "CI");
});

test("the form says what is missing before it is sent", () => {
  assert.equal(keyFormProblem(form()), null);
  assert.match(keyFormProblem(form({ access: "named" }))!, /at least one capability/);
  assert.match(keyFormProblem(form({ lifetime: "custom", seconds: "0" }))!, /seconds/);
  assert.match(keyFormProblem(form({ lifetime: "never" }))!, /why this key exists/);
  assert.match(keyFormProblem(form({ isStatic: true }))!, /why this key exists/);
  assert.equal(keyFormProblem(form({ isStatic: true, reason: "build agent" })), null);
});

test("a token's scopes read as every tool, read-only or their names", () => {
  assert.equal(accessOf(["*"]).label, "Every tool");
  assert.equal(accessOf(["viewer", "list_repositories"], scopes).label, "Read-only");
  assert.equal(accessOf([...scopes.all].reverse(), scopes).label, "Every tool");
  assert.equal(accessOf(["refs", "push"], scopes).label, "refs, push");
  assert.equal(accessOf(["a", "b", "c", "d"], scopes).label, "4 capabilities");
});

test("a key is active until it is revoked or expires", () => {
  const now = Date.parse("2026-10-10T12:00:00Z");
  assert.equal(keyState({ expires_at: "2026-10-11T00:00:00Z" }, now), "active");
  assert.equal(keyState({ expires_at: "2026-10-09T00:00:00Z" }, now), "expired");
  assert.equal(keyState({ expires_at: null, revoked_at: "2026-10-01T00:00:00Z" }, now), "revoked");
  assert.equal(keyState({ expires_at: null }, now), "active");
});

/** The shape rest.cpp's openapi_document renders. */
const envelope = (value: object) => ({
  content: { "application/json": { schema: { type: "object", properties: { ok: { type: "boolean" }, value, duration_ms: { type: "number" } } } } },
});
const doc = {
  openapi: "3.1.0",
  paths: {
    "/v1/list_repositories": {
      get: {
        operationId: "list_repositories",
        summary: "List repositories",
        tags: ["read"],
        "x-lake-action": "read",
        parameters: [{ name: "limit", in: "query", required: false, schema: { type: "integer" } }],
        responses: { "200": envelope({ type: "object", properties: { items: { type: "array" } } }) },
      },
    },
    "/v1/create_repository": {
      post: {
        operationId: "create_repository",
        summary: "Create repository",
        tags: ["action"],
        "x-lake-action": "write",
        requestBody: {
          required: true,
          content: {
            "application/json": {
              schema: { type: "object", properties: { name: { type: "string" }, customer_id: { type: "string" }, visibility: { type: "string" } }, required: ["customer_id", "name"] },
            },
          },
        },
        responses: { "200": envelope({ type: "object" }) },
      },
    },
    "/v1/workspace_query": { post: { operationId: "workspace_query", summary: "Query a workspace", tags: ["action"], "x-lake-action": "read", responses: {} } },
    "/v1/server_info": { get: { operationId: "server_info", summary: "Server info", tags: ["read"], security: [], responses: {} } },
    "/v1/repositories/{repository_id}/sync/push": {
      post: {
        operationId: "push",
        tags: ["liquid-sync"],
        parameters: [{ name: "repository_id", in: "path", required: true, schema: { type: "string" } }],
        requestBody: { content: { "application/vnd.liquid.sync-v1": { schema: { type: "string", format: "binary" } } } },
      },
    },
    "/v1/repositories/{repository_id}/sync/pull": { post: { operationId: "pull", tags: ["liquid-sync"] } },
  },
};
const groups = [
  { title: "Repositories", tools: [{ name: "list_repositories" }, { name: "create_repository" }] },
  { title: "Workspace", tools: [{ name: "workspace_query" }] },
];

test("operations take their MCP tool's group, else one named for their tag", () => {
  const ops = operationsOf(doc, groups);
  const by = Object.fromEntries(ops.map((o) => [o.operationId, o]));
  assert.equal(by.list_repositories.group, "Repositories");
  assert.equal(by.workspace_query.group, "Workspace");
  assert.equal(by.push.group, "Liquid sync (binary)");
  assert.equal(by.server_info.group, "Other");
  assert.equal(by.server_info.public, true);
});

test("an action that only reads, and a sync pull, are not writes", () => {
  const by = Object.fromEntries(operationsOf(doc, groups).map((o) => [o.operationId, o]));
  assert.equal(by.create_repository.writes, true);
  assert.equal(by.push.writes, true);
  assert.equal(by.workspace_query.writes, false);
  assert.equal(by.pull.writes, false);
  assert.equal(by.list_repositories.writes, false);
});

test("groups keep mcp_setup's order and the search matches path, name, summary or group", () => {
  const ops = operationsOf(doc, groups);
  assert.deepEqual(
    groupOperations(ops, "", ["Repositories", "Workspace"]).map(([g]) => g),
    ["Repositories", "Workspace", "Liquid sync (binary)", "Other"],
  );
  assert.deepEqual(
    groupOperations(ops, "REPOSITOR", ["Repositories"]).map(([g, list]) => [g, list.map((o) => o.operationId)]),
    [
      ["Repositories", ["create_repository", "list_repositories"]],
      ["Liquid sync (binary)", ["pull", "push"]],
    ],
  );
  assert.deepEqual(groupOperations(ops, "workspace", []).flatMap(([, list]) => list.map((o) => o.operationId)), ["workspace_query"]);
});

test("the response shown is the value inside the envelope", () => {
  const op = doc.paths["/v1/list_repositories"].get;
  const response = responseSchemaOf(op)!;
  assert.equal(response.enveloped, true);
  assert.deepEqual(response.schema, { type: "object", properties: { items: { type: "array" } } });
  assert.equal(requestBodyOf(op), null);
  assert.equal(requestBodyOf(doc.paths["/v1/create_repository"].post)!.type, "application/json");
});

test("a skeleton body carries only the required fields, each a placeholder", () => {
  const schema = doc.paths["/v1/create_repository"].post.requestBody.content["application/json"].schema;
  assert.deepEqual(skeleton(schema), { customer_id: "<customer_id>", name: "<name>" });
  assert.deepEqual(skeleton({ type: "object", properties: { n: { type: "integer", minimum: 0 }, on: { type: "boolean" } }, required: ["n", "on"] }), { n: 0, on: false });
});

test("copy as curl: a read with its bearer, an action with a JSON body, sync with binary, public with none", () => {
  const by = Object.fromEntries(operationsOf(doc, groups).map((o) => [o.operationId, o]));
  assert.equal(curlFor(by.list_repositories, "https://lake.example", "$LAKE_KEY"), `curl -s https://lake.example/v1/list_repositories \\\n  -H "Authorization: Bearer $LAKE_KEY"`);
  assert.equal(
    curlFor(by.create_repository, "https://lake.example", "K"),
    `curl -s -X POST https://lake.example/v1/create_repository \\\n  -H "Authorization: Bearer K" \\\n  -H "Content-Type: application/json" \\\n  -d '{"customer_id":"<customer_id>","name":"<name>"}'`,
  );
  assert.match(curlFor(by.push, "https://lake.example", "K"), /\/v1\/repositories\/<repository_id>\/sync\/push[\s\S]*--data-binary @request\.bin/);
  assert.equal(curlFor(by.server_info, "https://lake.example", "K"), "curl -s https://lake.example/v1/server_info");
});

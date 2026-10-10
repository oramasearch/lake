/** A secret's value as the Secrets pages edit it (docs/ui/secrets.md). `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { accountLabel, administeredOrganizationIds, mountOwnerLabel, ownerOptions } from "../src/pages/admin/secretOwners.ts";
import { fieldText, fromBase64Url, parseJsonObject, rowsToValue, toBase64Url, valueToRows, versionState } from "../src/pages/admin/secretValue.ts";

test("rows make one JSON object, skipping blank rows", () => {
  const parsed = rowsToValue([
    { key: "username", value: "app" },
    { key: "", value: "" },
    { key: " password ", value: "s3cret" },
  ]);
  assert.deepEqual(parsed, { ok: true, value: { username: "app", password: "s3cret" } });
});

test("rows refuse a value without a key, a key used twice and no rows at all", () => {
  const orphan = rowsToValue([{ key: "", value: "x" }]);
  assert.equal(orphan.ok, false);
  assert.equal(!orphan.ok && orphan.row, 0);
  const twice = rowsToValue([
    { key: "a", value: "1" },
    { key: "a", value: "2" },
  ]);
  assert.equal(twice.ok, false);
  assert.equal(!twice.ok && twice.row, 1);
  assert.equal(rowsToValue([{ key: "", value: "" }]).ok, false);
});

test("raw JSON must be one object", () => {
  assert.deepEqual(parseJsonObject('{"password": "p", "port": 5432}'), { ok: true, value: { password: "p", port: 5432 } });
  for (const text of ["", "[1]", "null", '"text"', "{nope"]) assert.equal(parseJsonObject(text).ok, false, text);
});

test("an object becomes rows only when every field is a string", () => {
  assert.deepEqual(valueToRows({ a: "1", b: "2" }), [
    { key: "a", value: "1" },
    { key: "b", value: "2" },
  ]);
  assert.equal(valueToRows({ a: "1", port: 5432 }), null);
  assert.equal(fieldText("x"), "x");
  assert.equal(fieldText(5432), "5432");
  assert.equal(fieldText({ nested: true }), '{"nested":true}');
});

test("a version's state follows its destruction, then its deletion", () => {
  assert.equal(versionState({}), "active");
  assert.equal(versionState({ deleted_at: "2026-10-10T00:00:00Z" }), "deleted");
  assert.equal(versionState({ deleted_at: "2026-10-10T00:00:00Z", destroyed_at: "2026-10-10T00:00:00Z" }), "destroyed");
});

test("text travels to the transit capabilities as unpadded base64url and back", () => {
  assert.equal(toBase64Url("hello"), "aGVsbG8");
  assert.equal(toBase64Url("é?>"), "w6k_Pg");
  assert.equal(fromBase64Url("aGVsbG8"), "hello");
  assert.equal(fromBase64Url("aGVsbG8="), "hello");
  assert.equal(fromBase64Url("w6k_Pg"), "é?>");
  assert.equal(fromBase64Url("not base64!"), null);
  assert.equal(fromBase64Url("_w"), null, "bytes that are not UTF-8");
});

test("a personal workspace's mount defaults to the account, and organizations of other spaces are not offered", () => {
  const options = ownerOptions({
    space: { label: "Your workspaces", personal: true },
    workspaceCustomerId: "c-personal",
    ownerOrganizationId: null,
    administered: [
      { id: "o-side", display_name: "Side project", customer_id: null },
      { id: "o-orama", display_name: "Orama", customer_id: "c-orama" },
    ],
  });
  assert.deepEqual(options, [
    { organizationId: "", label: "Your account" },
    { organizationId: "o-side", label: "Side project" },
  ]);
});

test("an organization's workspace defaults to that organization, listed once", () => {
  const options = ownerOptions({
    space: { label: "Orama", personal: false },
    workspaceCustomerId: "c-orama",
    ownerOrganizationId: "o-orama",
    administered: [{ id: "o-orama", display_name: "Orama", customer_id: "c-orama" }],
  });
  assert.deepEqual(options, [{ organizationId: "o-orama", label: "Orama (workspace owner)" }]);
  // Someone else's account: named by its space, never blocked.
  assert.deepEqual(ownerOptions({ space: { label: "dana", personal: false }, workspaceCustomerId: "c-dana", administered: [] }), [{ organizationId: "", label: "dana" }]);
});

test("only owner and admin memberships of the organization itself administer it", () => {
  const ids = administeredOrganizationIds([
    { organization_id: "a", role: "owner" },
    { organization_id: "b", role: "admin" },
    { organization_id: "c", role: "member" },
    { organization_id: "d", team_id: "t", role: "admin" },
  ]);
  assert.deepEqual([...ids].sort(), ["a", "b"]);
});

test("a mount's owner reads as the organization's name or the account", () => {
  const organizations = new Map([["o-1", { id: "o-1", display_name: "Payments" }]]);
  assert.equal(mountOwnerLabel({ owner_type: "organization", organization_id: "o-1" }, organizations, null), "Payments");
  assert.equal(mountOwnerLabel({ owner_type: "organization", organization_id: "o-2" }, organizations, null), "o-2");
  assert.equal(mountOwnerLabel({ owner_type: "customer", organization_id: null }, organizations, { label: "Your workspaces", personal: true }), "Your account");
  assert.equal(accountLabel({ label: "dana", personal: false }), "dana");
});

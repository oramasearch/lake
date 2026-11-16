// SPDX-License-Identifier: MIT

/** Data → Connections (lake#651): the route and its place in the rail, the old Datasets links, the
 * empty state's figures, and the add and delete flow's inputs. `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { DATA_NAV, currentNavIn } from "../src/routes/nav.ts";
import {
  CONNECTIONS_PATH,
  CONNECTION_PATH,
  connectionHost,
  connectionInput,
  connectionKind,
  connectionPath,
  connectionStatus,
  connectionsUsing,
  datasetNameFor,
  deleteConfirmed,
  kindLabel,
  movedFromDatasets,
  serviceFor,
  tableQuery,
  tablesFigure,
} from "../src/pages/connections/connections.ts";

test("Connections is a Data page between Datasets and Transcripts", () => {
  const paths = DATA_NAV.map((item) => item.path);
  const at = paths.indexOf(CONNECTIONS_PATH);
  assert.equal(CONNECTIONS_PATH, "/connections");
  assert.ok(at > 0, "in the Data group");
  assert.equal(paths[at - 1], "/train/data", "after Datasets");
  assert.equal(paths[at + 1], "/transcripts", "before Transcripts");
  const item = DATA_NAV[at];
  assert.equal(item.label, "Connections");
  assert.equal(item.section, "data");
  assert.deepEqual(item.audience, ["account", "operator"]);
});

test("a connection's page marks Connections current in the rail", () => {
  const owned = (path: string) => (path === CONNECTIONS_PATH ? [CONNECTION_PATH] : []);
  assert.equal(currentNavIn("/w/ws-1/connections", DATA_NAV, owned)?.label, "Connections");
  assert.equal(currentNavIn(`/w/ws-1${connectionPath("warehouse")}`.split("?")[0], DATA_NAV, owned)?.label, "Connections");
  assert.equal(currentNavIn("/w/ws-1/train/data", DATA_NAV, owned)?.label, "Datasets");
  assert.equal(connectionPath("sales db"), "/connections/connection?name=sales%20db");
});

test("old Datasets links go to Connections", () => {
  assert.equal(movedFromDatasets("?customer_id=c&browse=warehouse", ""), "/connections/connection?name=warehouse");
  assert.equal(movedFromDatasets("?customer_id=c", "#connections"), "/connections");
  assert.equal(movedFromDatasets("", "#connection"), "/connections");
  assert.equal(movedFromDatasets("?customer_id=c&loaded=accounts", ""), null, "a Datasets link stays");
  assert.equal(movedFromDatasets("", ""), null);
});

test("the empty state's figures and the tables reachable", () => {
  assert.deepEqual(tablesFigure([]), { value: "0", failed: 0, warned: 0 }, "no connections: nothing reachable");
  assert.deepEqual(tablesFigure([{ tables: [1, 2] }, undefined]), { value: "—", failed: 0, warned: 0 }, "waits for every answer");
  assert.deepEqual(tablesFigure([{ tables: [1, 2] }, { tables: [1], truncated: true }]), { value: "3+", failed: 0, warned: 0 });
  assert.deepEqual(tablesFigure([{ tables: [1] }, "failed"]), { value: "1", failed: 1, warned: 0 });
  assert.deepEqual(tablesFigure(["failed"]), { value: "—", failed: 1, warned: 0 });
  // A connection whose safe mode asks first (lake#906) is neither counted nor failed.
  assert.deepEqual(tablesFigure([{ tables: [1, 2] }, "warned"]), { value: "2", failed: 0, warned: 1 });
  assert.deepEqual(tablesFigure(["warned"]), { value: "—", failed: 0, warned: 1 });
});

test("add: only the chosen kind's filled-in fields reach create_connection", () => {
  const postgres = connectionInput({ name: " warehouse ", type: "postgres", host: "db.example.com", port: "5432", user: "reader", sslmode: "require", account: "ignored", database: "sales", schema: "", token: "secret" });
  assert.deepEqual(postgres, { name: "warehouse", connector: { type: "postgres", host: "db.example.com", port: "5432", user: "reader", sslmode: "require", database: "sales", token: "secret" } });
  const snowflake = connectionInput({ name: "wh", type: "snowflake", host: "ignored", account: "ab12345.us-east-1", warehouse: "compute", role: "", database: "sales", token: "" });
  assert.deepEqual(snowflake, { name: "wh", connector: { type: "snowflake", account: "ab12345.us-east-1", warehouse: "compute", database: "sales" } });
});

test("delete: only once the connection's name is typed exactly", () => {
  assert.equal(deleteConfirmed("warehouse", "warehouse"), true);
  assert.equal(deleteConfirmed("warehouse", "Warehouse"), false);
  assert.equal(deleteConfirmed("warehouse", "warehouse "), false);
  assert.equal(deleteConfirmed("", ""), false);
});

test("a connection's kind, host and status in words", () => {
  const pg = { name: "sales", connector: { type: "postgres", host: "db", port: "5432", database: "sales" }, credential: "set", created: "2026-10-11T10:00:00Z" };
  assert.equal(connectionKind(pg), "postgres");
  assert.equal(kindLabel("postgres"), "Postgres");
  assert.equal(kindLabel(""), "Unknown");
  assert.equal(connectionHost(pg), "db:5432/sales");
  assert.equal(connectionHost({ connector: { type: "snowflake", account: "ab1", database: "D" } }), "ab1/D");
  assert.equal(connectionStatus(pg).label, "Ready");
  assert.equal(connectionStatus({ credential: "none" }).label, "No credential");
  assert.equal(connectionStatus({ credential: "unreadable" }).tone, "danger");
  assert.equal(connectionStatus({ credential: "plaintext" }).tone, "warning");
  assert.equal(connectionStatus({}).label, "Saved");
});

test("the service a connection came from", () => {
  const services = [
    { name: "orders", endpoint: "orders-pg.ws.svc:5432" },
    { name: "sales", endpoint: "other:5432" },
  ];
  const connection = { name: "orders", connector: { type: "postgres", host: "orders-pg.ws.svc" } };
  assert.equal(serviceFor(connection, services)?.name, "orders");
  assert.equal(serviceFor({ name: "sales", connector: { type: "postgres", host: "db" } }, services), undefined);
  assert.equal(serviceFor({ name: "orders", connector: { type: "snowflake" } }, services), undefined);
  assert.equal(connectionsUsing(services[0], [connection, { name: "x", connector: {} }]).length, 1);
});

test("Load into a dataset fills in the table's query and a dataset name", () => {
  assert.equal(tableQuery("public", "accounts", "postgres"), "SELECT * FROM public.accounts");
  assert.equal(tableQuery("Sales", "Order Lines", "postgres"), 'SELECT * FROM "Sales"."Order Lines"');
  assert.equal(tableQuery("", "accounts", "postgres"), "SELECT * FROM accounts");
  assert.equal(tableQuery("PUBLIC", "ACCOUNTS", "snowflake"), "SELECT * FROM PUBLIC.ACCOUNTS");
  assert.equal(datasetNameFor("Order Lines"), "Order_Lines");
  assert.equal(datasetNameFor("2024-sales"), "_sales", "SQL names start with a letter or underscore");
  assert.equal(datasetNameFor("2024"), "dataset");
  assert.equal(datasetNameFor("x".repeat(80)).length, 63);
  assert.equal(datasetNameFor("Order"), "Order_data", "lake refuses a SQL keyword, so it gets _data");
  assert.equal(datasetNameFor("group"), "group_data");
});

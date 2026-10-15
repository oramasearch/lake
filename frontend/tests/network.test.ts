/** Network (lake#729): the section's place in the rail, that its pages reach every network
 * capability (lake#728), and the words, inputs and sentences the pages use. `npm test`; the build
 * runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { DATA_NAV, NETWORK_NAV, currentNavIn } from "../src/routes/nav.ts";
import {
  CSR_COMMAND,
  NETWORK_PAGES,
  certificateFile,
  connectionInput,
  csrProblem,
  egressDestination,
  egressInput,
  expiresAt,
  serviceAccessHref,
  tunnelInstructions,
  pendingPoll,
  PENDING_POLL_MS,
  tunnelLocalPort,
  grantInput,
  grantReady,
  grantState,
  nameOf,
  parsePorts,
  portsText,
  present,
  recordColumns,
  recordState,
  ruleSentences,
  scopeText,
  type Names,
} from "../src/pages/network/network.ts";

/** Every network capability the registry exposes (docs/api/network.md). */
const CAPABILITIES = [
  "create_network_connection",
  "list_network_connections",
  "rotate_network_connection_credential",
  "remove_network_connection",
  "create_network_egress_rule",
  "list_network_egress_rules",
  "grant_network_tunnel",
  "issue_network_tunnel_certificate",
  "list_network_tunnel_grants",
  "revoke_network_tunnel",
  "list_network_tunnel_sessions",
  "list_network_peers",
  "network_rules",
  "network_activity",
];

test("every network capability is reachable from a page of the section", () => {
  const reached = new Set(NETWORK_PAGES.flatMap((p) => p.capabilities));
  for (const name of CAPABILITIES) assert.ok(reached.has(name), `${name} has a page`);
  for (const name of reached) assert.ok(CAPABILITIES.includes(name), `${name} is a network capability`);
});

test("the rail's Network group lists the section's pages in order", () => {
  assert.deepEqual(
    NETWORK_NAV.map((i) => [i.label, i.path]),
    NETWORK_PAGES.map((p) => [p.label, p.path]),
  );
  assert.deepEqual(
    NETWORK_NAV.map((i) => i.label),
    ["Overview", "Connections", "Developer Access", "Private Networks", "Egress and DNS", "Policies", "Activity"],
  );
  for (const item of NETWORK_NAV) {
    assert.equal(item.section, "network");
    assert.deepEqual(item.audience, ["account", "operator"]);
  }
});

test("each Network page marks only its own rail item current, and Data → Connections stays Data's", () => {
  for (const page of NETWORK_PAGES) assert.equal(currentNavIn(`/w/ws-1${page.path}`, NETWORK_NAV)?.label, page.label);
  assert.equal(currentNavIn("/w/ws-1/connections", [...DATA_NAV, ...NETWORK_NAV])?.section, "data");
  assert.equal(currentNavIn("/w/ws-1/network/connections", [...DATA_NAV, ...NETWORK_NAV])?.section, "network");
});

test("a desired row's state follows the cluster's observation", () => {
  assert.equal(recordState({ generation: 1, desired: "present", status: null }).label, "Waiting");
  assert.equal(recordState({ generation: 2, desired: "present", status: { phase: "Ready", observed_generation: 1, message: "" } }).label, "Updating");
  assert.equal(recordState({ generation: 2, desired: "present", status: { phase: "Ready", observed_generation: 2, message: "" } }).label, "Ready");
  assert.equal(recordState({ generation: 2, desired: "present", status: { phase: "Failed", observed_generation: 2, message: "no route" } }).detail, "no route");
  assert.equal(recordState({ generation: 3, desired: "absent", status: { phase: "Ready", observed_generation: 2, message: "" } }).label, "Removing");
  assert.equal(recordState({ generation: 3, desired: "absent", status: { phase: "Deleted", observed_generation: 3, message: "" } }).label, "Removed");
});

test("a grant is active, expired or revoked from its own fields, and ready only with relay trust", () => {
  const now = Date.parse("2026-10-14T12:00:00Z");
  const ready = { phase: "Ready", observed_generation: 1, message: "", endpoint: "relay:443", relay_ca_pem: "pem" };
  const grant = { generation: 1, expires_at: "2026-10-14T13:00:00Z", revoked_at: null, active: true, status: ready };
  assert.equal(grantState(grant, now).label, "Active");
  assert.equal(grantState({ ...grant, revoked_at: "2026-10-14T11:00:00Z" }, now).label, "Revoked");
  assert.equal(grantState({ ...grant, expires_at: "2026-10-14T11:00:00Z" }, now).label, "Expired");
  assert.equal(grantReady(grant, now), true);
  assert.equal(grantReady({ ...grant, generation: 2 }, now), false, "the relay has not caught up");
  assert.equal(grantReady({ ...grant, status: { ...ready, relay_ca_pem: "" } }, now), false, "no relay trust yet");
  assert.equal(grantReady({ ...grant, status: null }, now), false);
});

test("ports and credential scopes in words", () => {
  assert.equal(portsText([]), "Every port it serves");
  assert.equal(portsText([5432, 6432]), "5432, 6432");
  assert.equal(scopeText(null), "None");
  assert.equal(scopeText({ access: "read" }), "Read only");
  assert.equal(scopeText({ access: "write", schema: "analytics" }), "Read and write, schema analytics");
  assert.deepEqual(parsePorts("5432, 6432"), [5432, 6432]);
  assert.deepEqual(parsePorts(""), []);
  assert.throws(() => parsePorts("", { required: true }), /at least one port/);
  assert.throws(() => parsePorts("70000"), /1 to 65535/);
  assert.throws(() => parsePorts("http"), /whole numbers/);
});

test("Connect an app sends a credential only from an app to a service", () => {
  const base = { sourceKind: "app", sourceId: "a1", targetKind: "service", targetId: "s1", ports: "", access: "read" as const, schema: " analytics " };
  assert.deepEqual(connectionInput(base), { source_kind: "app", source_id: "a1", target_kind: "service", target_id: "s1", credential_scope: { access: "read", schema: "analytics" } });
  assert.deepEqual(connectionInput({ ...base, access: "", ports: "5432" }), { source_kind: "app", source_id: "a1", target_kind: "service", target_id: "s1", ports: [5432] });
  assert.equal(connectionInput({ ...base, targetKind: "app", targetId: "a2" }).credential_scope, undefined, "app to app carries no credential");
  assert.equal(connectionInput({ ...base, sourceKind: "service", sourceId: "s2" }).credential_scope, undefined, "service to service carries no credential");
  assert.throws(() => connectionInput({ ...base, sourceId: "" }), /what connects/);
  assert.throws(() => connectionInput({ ...base, targetKind: "app", targetId: "a1" }), /two different/);
});

test("Allow a destination sends exactly one destination, and a source only for one workload", () => {
  assert.deepEqual(egressInput({ appliesTo: "workspace", sourceId: "", destination: "dns", value: "API.Example.com", ports: "443" }), { ports: [443], dns_name: "api.example.com" });
  assert.deepEqual(egressInput({ appliesTo: "app", sourceId: "a1", destination: "cidr", value: "203.0.113.0/24", ports: "443, 8443" }), {
    ports: [443, 8443],
    cidr: "203.0.113.0/24",
    source_kind: "app",
    source_id: "a1",
  });
  assert.throws(() => egressInput({ appliesTo: "workspace", sourceId: "", destination: "dns", value: "not a host", ports: "443" }), /not a host name/);
  assert.throws(() => egressInput({ appliesTo: "workspace", sourceId: "", destination: "cidr", value: "10.0.0.1", ports: "443" }), /address range/);
  assert.throws(() => egressInput({ appliesTo: "app", sourceId: "", destination: "dns", value: "a.example.com", ports: "443" }), /workload/);
  assert.throws(() => egressInput({ appliesTo: "workspace", sourceId: "", destination: "dns", value: "a.example.com", ports: "" }), /port/);
  assert.deepEqual(egressDestination({ dns_name: "a.example.com", cidr: null }), { kind: "DNS name", value: "a.example.com" });
  assert.deepEqual(egressDestination({ dns_name: null, cidr: "10.0.0.0/8" }), { kind: "Address range", value: "10.0.0.0/8" });
});

test("Grant access sends one port and a whole-second UTC expiry", () => {
  const now = Date.parse("2026-10-14T12:00:00.750Z");
  assert.equal(expiresAt(8, now), "2026-10-14T20:00:00Z");
  assert.deepEqual(grantInput({ userId: "u1", targetKind: "service", targetId: "s1", port: "5432", hours: 1 }, now), {
    user_id: "u1",
    target_kind: "service",
    target_id: "s1",
    port: 5432,
    expires_at: "2026-10-14T13:00:00Z",
  });
  assert.throws(() => grantInput({ userId: "u1", targetKind: "service", targetId: "s1", port: "1, 2", hours: 1 }, now), /one port/);
  assert.throws(() => grantInput({ userId: "", targetKind: "service", targetId: "s1", port: "1", hours: 1 }, now), /who/);
});

test("the key stays local: only a certificate request is sent", () => {
  assert.match(CSR_COMMAND, /-keyout tunnel\.key -out tunnel\.csr/);
  const csr = "-----BEGIN CERTIFICATE REQUEST-----\nMIIB\n-----END CERTIFICATE REQUEST-----";
  assert.equal(csrProblem(csr), null);
  assert.match(csrProblem("") ?? "", /Paste/);
  assert.match(csrProblem("-----BEGIN PRIVATE KEY-----\nx\n-----END PRIVATE KEY-----") ?? "", /not a certificate request/);
  assert.match(csrProblem(`${csr}\n-----BEGIN PRIVATE KEY-----\nx\n-----END PRIVATE KEY-----\n${csr}`) ?? "", /private key/);
  assert.match(csrProblem(csr.replace("MIIB", "M".repeat(17000))) ?? "", /16 KiB/);
  assert.equal(certificateFile({ certificate_pem: "CERT\n", chain_pem: "CHAIN\n" }), "CERT\nCHAIN\n");
});

const names: Names = {
  app: new Map([["a1", "reporting"]]),
  service: new Map([["s1", "orders"]]),
  user: new Map([["u1", "You"]]),
};

test("names replace ids, and a tunnel is named for what it is, never a vendor", () => {
  assert.equal(nameOf(names, "app", "a1"), "reporting");
  assert.equal(nameOf(names, "service", "0b9d2c1e-aaaa"), "service 0b9d2c1e…");
  assert.equal(nameOf(names, "tunnel", "x"), "A developer tunnel");
});

test("the policy reads as sentences and ends with what is not asked for", () => {
  const now = Date.parse("2026-10-14T12:00:00Z");
  assert.deepEqual(ruleSentences({ connections: [], egress_rules: [], tunnel_grants: [], peers: [] }, names, now), ["Lake asks for no path into or out of this workspace."]);
  const sentences = ruleSentences(
    {
      connections: [{ source_kind: "app", source_id: "a1", target_kind: "service", target_id: "s1", ports: [5432], credential_scope: { access: "read" } }],
      egress_rules: [{ source_kind: null, source_id: null, dns_name: "api.example.com", cidr: null, ports: [443] }],
      tunnel_grants: [
        { user_id: "u1", target_kind: "service", target_id: "s1", port: 5432, expires_at: "2026-10-14T20:00:00Z", revoked_at: null, active: true },
        { user_id: "u1", target_kind: "service", target_id: "s1", port: 5432, expires_at: "2026-10-14T10:00:00Z", revoked_at: null, active: false },
      ],
      peers: [{ name: "aws-prod" }],
    },
    names,
    now,
  );
  assert.deepEqual(sentences, [
    "App reporting may reach service orders on TCP 5432, with its own read only credential.",
    "Every workload in the workspace may reach api.example.com on TCP 443.",
    "You may open a tunnel to service orders on TCP 5432 until 2026-10-14 20:00 UTC.",
    "The private network aws-prod is joined to this workspace.",
    "Lake asks for no other path into or out of this workspace.",
  ]);
  for (const s of sentences) assert.doesNotMatch(s, /cloudflare|zetta/i, "no vendor is named");
  const named = ruleSentences({ connections: [{ source_kind: "service", source_id: "s1", target_kind: "service", target_id: "s1", ports: [] }] }, { ...names, service: new Map([["s1", "ws-0a1b"]]) }, now);
  assert.equal(named[0], "Service ws-0a1b may reach service ws-0a1b.", "a name keeps its case");
});

test("history is hidden until asked for, and provider records read their common fields", () => {
  const rows = [{ desired: "present" }, { desired: "absent" }];
  assert.equal(present(rows, false).length, 1);
  assert.equal(present(rows, true).length, 2);
  assert.deepEqual(recordColumns({ at: "t", source: "a", target: "b", decision: "allowed" }), { when: "t", source: "a", destination: "b", outcome: "allowed" });
  assert.deepEqual(recordColumns({}), { when: "", source: "", destination: "", outcome: "" });
});

test("the credentials end in commands a developer pastes, with the endpoint and port filled in (lake#745)", () => {
  const steps = tunnelInstructions({
    grantId: "8a8636d3-9b18-41d6-8b9f-9aed08fbbb3b",
    endpoint: "20.246.181.73:443",
    port: 5432,
    targetKind: "service",
    serviceKind: "postgres",
    certificate: "-----BEGIN CERTIFICATE-----\nAAA\n-----END CERTIFICATE-----\n",
    relayCa: "-----BEGIN CERTIFICATE-----\nBBB\n-----END CERTIFICATE-----\n",
  });
  assert.equal(steps.folder, "~/orama-tunnel/8a8636d3");
  assert.equal(steps.localPort, 5433);
  // One paste writes both files, in the folder step 1 made, with the exact PEM text.
  assert.ok(steps.save.startsWith("cd ~/orama-tunnel/8a8636d3\ncat > tunnel.crt <<'END_OF_CERTIFICATE'\n-----BEGIN CERTIFICATE-----\nAAA"));
  assert.match(steps.save, /cat > relay-ca\.crt <<'END_OF_CERTIFICATE'\n-----BEGIN CERTIFICATE-----\nBBB\n-----END CERTIFICATE-----\nEND_OF_CERTIFICATE$/);
  assert.match(steps.start, /--relay 20\.246\.181\.73:443 --listen 127\.0\.0\.1:5433$/);
  assert.match(steps.start, /--cert tunnel\.crt --key tunnel\.key --ca relay-ca\.crt/);
  assert.equal(steps.connect, 'psql "postgresql://USER@127.0.0.1:5433/DATABASE"');
  assert.match(steps.connectNote, /Show connection/);
  // An app is reached over HTTP; an unknown kind gets the address and the same reminder.
  const app = tunnelInstructions({ grantId: "a", endpoint: "e:443", port: 80, targetKind: "app", certificate: "c", relayCa: "r" });
  assert.equal(app.connect, "curl http://127.0.0.1:10080/");
  const other = tunnelInstructions({ grantId: "a", endpoint: "e:443", port: 9000, targetKind: "service", serviceKind: "clickhouse", certificate: "c", relayCa: "r" });
  assert.equal(other.connect, "127.0.0.1:9001");
});

test("the local port sits next to the target's and is never privileged", () => {
  assert.equal(tunnelLocalPort(5432), 5433);
  assert.equal(tunnelLocalPort(443), 10443);
  assert.equal(tunnelLocalPort(65535), 15433);
});

test("a service's Developer access opens Grant access for it, with its own port (lake#741)", () => {
  assert.equal(
    serviceAccessHref("6bb777ba-498a", "5103dc69-b44c", "prod-svc-5103dc69b44c-rw.prod-ws-6bb777ba498a.svc.cluster.local:5432"),
    "/w/6bb777ba-498a/network/access?grant=5103dc69-b44c&port=5432",
  );
  assert.equal(serviceAccessHref("w", "s", "http://prod-ws-x-zetta.prod-ws-x.svc.cluster.local:4800"), "/w/w/network/access?grant=s&port=4800");
  assert.equal(serviceAccessHref("w", "s", ""), "/w/w/network/access?grant=s");
});

test("a Network page reads again while a row waits for the cluster, and stops once all are settled", () => {
  const waiting = { tunnel_grants: [{ id: "g", desired: "present", generation: 1, status: null }] };
  const ready = { tunnel_grants: [{ id: "g", desired: "present", generation: 1, status: { phase: "Ready", observed_generation: 1 } }] };
  const updating = { connections: [{ id: "c", desired: "present", generation: 2, status: { phase: "Ready", observed_generation: 1 } }] };
  assert.equal(pendingPoll(waiting), PENDING_POLL_MS);
  assert.equal(pendingPoll(updating), PENDING_POLL_MS);
  assert.equal(pendingPoll(ready), false);
  assert.equal(pendingPoll({ tunnel_grants: [] }), false);
  assert.equal(pendingPoll(undefined), false);
});

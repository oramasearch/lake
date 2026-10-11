/** What a delete or revoke takes, said from its `dry_run: true` answer (lake#645,
 * docs/ui/deletion.md). Each capability answers its own plan; these turn one into the lines the
 * confirmation dialog lists (a figure and what it counts) and the text the person types to confirm.
 * Pure, so `npm test` checks it (tests/deletion.test.ts). */

/** One line of a deletion dialog: a figure (null: no count, the line is a statement) and words. */
export type DeletionLine = { count: number | null; text: string };

type Plan = Record<string, unknown>;

const plural = (n: number, one: string, many: string) => `${n === 1 ? one : many}`;

function num(value: unknown): number {
  return typeof value === "number" && Number.isFinite(value) ? value : 0;
}

function obj(value: unknown): Plan {
  return value && typeof value === "object" && !Array.isArray(value) ? (value as Plan) : {};
}

function list(value: unknown): unknown[] {
  return Array.isArray(value) ? value : [];
}

function line(count: number, one: string, many: string, rest = ""): DeletionLine {
  return { count, text: `${plural(count, one, many)}${rest}` };
}

/** A size in bytes, short. */
function size(bytes: number): string {
  const units = ["bytes", "KB", "MB", "GB", "TB"];
  let value = bytes;
  let unit = 0;
  while (value >= 1000 && unit < units.length - 1) {
    value /= 1000;
    unit += 1;
  }
  return unit === 0 ? `${value} ${units[0]}` : `${value.toFixed(value < 10 ? 1 : 0)} ${units[unit]}`;
}

/** The workspaces an organization still owns: its deletion is refused until they are gone. */
export type BlockingWorkspace = { id: string; name: string; environment?: string };
export function organizationBlockers(plan: Plan): BlockingWorkspace[] {
  return list(plan.workspaces)
    .map((value) => obj(value))
    .filter((value) => typeof value.id === "string")
    .map((value) => ({ id: String(value.id), name: String(value.name ?? value.id), environment: typeof value.environment === "string" ? value.environment : undefined }));
}

export function organizationLines(plan: Plan): DeletionLine[] {
  const counts = obj(plan.counts);
  return [
    { count: null, text: "The organization itself. Its handle is retired and cannot be used again." },
    line(num(counts.memberships), "member", "members", ", who lose access"),
    line(num(counts.teams), "team", "teams"),
    line(num(counts.invitations), "invitation", "invitations"),
    line(num(counts.grants), "access grant", "access grants"),
    line(num(counts.sso_providers), "SSO provider", "SSO providers"),
    line(num(counts.federation_issuers), "federation issuer", "federation issuers"),
  ].filter((entry) => entry.count === null || entry.count > 0 || entry.text.startsWith("member"));
}

/** The text that confirms deleting an organization: its handle, or its id without one. */
export function organizationTyped(plan: Plan): string {
  return typeof plan.handle === "string" && plan.handle ? plan.handle : String(plan.organization_id ?? "");
}

export function secretMountLines(plan: Plan): DeletionLine[] {
  const counts = obj(plan.counts);
  return [
    line(num(counts.secrets), "secret", "secrets", ` with ${num(counts.versions)} ${num(counts.versions) === 1 ? "version" : "versions"}`),
    line(num(counts.encryption_keys), "encryption key", "encryption keys", ". Anything they encrypted can no longer be decrypted"),
    line(num(counts.data_keys), "data key", "data keys"),
    line(num(counts.leases), "dynamic secret lease", "dynamic secret leases"),
    { count: null, text: "Every access grant on the mount and its secrets." },
  ].filter((entry, index) => index === 0 || entry.count === null || entry.count > 0);
}

export function datasetLines(plan: Plan): DeletionLine[] {
  const removed = obj(plan.removed);
  const lines: DeletionLine[] = [line(num(removed.rows), "row", "rows", removed.size_bytes ? ` (${size(num(removed.size_bytes))})` : "")];
  const indexes = list(removed.indexes).length;
  if (indexes > 0) lines.push(line(indexes, "search index", "search indexes", " built on it"));
  if (num(removed.partitions) > 1) lines.push(line(num(removed.partitions), "partition", "partitions"));
  return lines;
}

export function modelLines(plan: Plan): DeletionLine[] {
  const removed = obj(plan.removed);
  const lines: DeletionLine[] = [{ count: 1, text: "trained model" }];
  if (removed.was_exposed) lines.push({ count: null, text: "It is published: its endpoint stops answering." });
  if (obj(plan.kept).run) lines.push({ count: null, text: "Kept: the run that trained it." });
  return lines;
}

export function runLines(plan: Plan): DeletionLine[] {
  const kept = list(obj(plan.kept).models).length;
  const lines: DeletionLine[] = [{ count: 1, text: "run and its report" }];
  if (kept > 0) lines.push({ count: null, text: `Kept: the ${kept === 1 ? "model" : `${kept} models`} it trained.` });
  return lines;
}

export function indexLines(plan: Plan): DeletionLine[] {
  const removed = obj(plan.removed);
  const lines: DeletionLine[] = [{ count: 1, text: "search index" }];
  if (removed.was_exposed) lines.push({ count: null, text: "It is published: its endpoint stops answering." });
  lines.push({ count: null, text: "Kept: the dataset it indexes." });
  return lines;
}

export function conversationLines(plan: Plan): DeletionLine[] {
  const messages = num(plan.messages);
  const first = typeof plan.first_message === "string" ? plan.first_message.trim() : "";
  const lines: DeletionLine[] = [line(messages, "message", "messages")];
  if (first) lines.push({ count: null, text: `It starts: “${first.length > 120 ? `${first.slice(0, 119)}…` : first}”` });
  lines.push({ count: null, text: "Kept: the agent tasks it started, in Agent Tasks." });
  return lines;
}

export function invitationLines(plan: Plan): DeletionLine[] {
  return [{ count: null, text: `${String(plan.invitee ?? "The invitee")} can no longer accept the invitation to join as ${String(plan.role ?? "a member")}. It stays listed as revoked.` }];
}

export function grantLines(plan: Plan): DeletionLine[] {
  return [{ count: null, text: `The ${String(plan.role ?? "")} role of this principal on this object. Access it has from elsewhere stays.` }];
}

/** The fixed word a revoke or a conversation deletion is confirmed with. */
export const REVOKE_WORD = "revoke";
export const DELETE_WORD = "delete";

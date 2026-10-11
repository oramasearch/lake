/** The issue pages' pure logic (lake#598): which issues a view shows, the labels a person typed, the
 * colour a label carries, who a TAAP identity is, and the smallest `update_issue` an edit needs.
 * `npm test` covers it (tests/issues.test.ts). */

/** An issue as `list_issues` answers it: its fields are in `value`. */
export type IssueRecord = {
  id: string;
  created_at: string;
  updated_at: string;
  value: { title?: string; body?: string; state?: string; labels?: string[]; assignees?: string[]; author?: string; [key: string]: unknown };
};

/** One of the repository's people, as `issue_assignees` answers them. */
export type Person = { taap_id: string; kind: "user" | "organization"; display_name: string; email: string; handle: string };

/** The view the Issues list shows. No `state` in the address is the open issues; `all` is both. */
export type IssueView = "open" | "closed" | "all";

export function issueView(state: string | null | undefined): IssueView {
  return state === "closed" || state === "all" ? state : "open";
}

export function issueState(issue: IssueRecord): "open" | "closed" {
  return issue.value.state === "closed" ? "closed" : "open";
}

/** Whether an issue is in the view, carries the label and the assignee (when chosen) and contains
 * the search text in its title or id. */
export function issueMatches(issue: IssueRecord, filter: { view: IssueView; label?: string; assignee?: string; search?: string }): boolean {
  const labels = issue.value.labels ?? [];
  const assignees = issue.value.assignees ?? [];
  const needle = (filter.search ?? "").trim().toLowerCase();
  return (
    (filter.view === "all" || issueState(issue) === filter.view) &&
    (!filter.label || labels.includes(filter.label)) &&
    (!filter.assignee || assignees.includes(filter.assignee)) &&
    (!needle || `${issue.value.title ?? ""} ${issue.id}`.toLowerCase().includes(needle))
  );
}

/** Labels as typed ("bug, needs triage,  bug"): trimmed, empty ones dropped, each once, in order. */
export function parseLabels(text: string): string[] {
  const out: string[] = [];
  for (const part of text.split(",")) {
    const label = part.trim();
    if (label && !out.includes(label)) out.push(label);
  }
  return out;
}

/** Adds the label when absent, removes it when present. */
export function toggle(list: string[], value: string): string[] {
  return list.includes(value) ? list.filter((item) => item !== value) : [...list, value];
}

/** Every label used across the issues, each once, sorted. */
export function knownLabels(issues: IssueRecord[]): string[] {
  const all = new Set<string>();
  for (const issue of issues) for (const label of issue.value.labels ?? []) all.add(label);
  return [...all].sort((a, b) => a.localeCompare(b));
}

/** The colour a label carries: lake keeps no label colours, so the same name always gets the same
 * one of the kit's chip tones. A word that means trouble reads red. */
export type LabelTone = "ok" | "warn" | "ai" | "neutral" | "bad";
/** Not the action blue: a chosen filter chip is drawn in it. */
const TONES: LabelTone[] = ["ok", "warn", "ai", "neutral"];
export function labelTone(label: string): LabelTone {
  const name = label.trim().toLowerCase();
  if (/^(bug|defect|regression|security|incident|blocker|critical)$/.test(name)) return "bad";
  let hash = 0;
  for (const c of name) hash = (hash * 31 + c.codePointAt(0)!) >>> 0;
  return TONES[hash % TONES.length];
}

/** How a TAAP identity reads: the person's name (or handle, or email) when they are one of the
 * repository's people, "you" for the viewer, else the identity itself, abbreviated as lake
 * abbreviates identifiers (a GitHub import keeps a GitHub login). */
export function personName(id: string | undefined, people: Person[], viewer?: string): string {
  if (!id) return "";
  const person = people.find((p) => p.taap_id === id);
  const name = person ? person.display_name || (person.handle ? `@${person.handle}` : "") || person.email : "";
  if (id === viewer) return name ? `${name} (you)` : "you";
  return name || (id.length <= 20 ? id : `${id.slice(0, 12)}…${id.slice(-7)}`);
}

/** What an edit changed, as the fields `update_issue` takes, so two people editing different
 * fields both keep their change. Empty when nothing changed. */
export function issueChanges(
  before: { title?: string; body?: string; labels?: string[]; assignees?: string[] },
  after: { title?: string; body?: string; labels?: string[]; assignees?: string[] },
): Record<string, string | string[]> {
  const out: Record<string, string | string[]> = {};
  if (after.title !== undefined && after.title.trim() !== (before.title ?? "")) out.title = after.title.trim();
  if (after.body !== undefined && after.body !== (before.body ?? "")) out.body = after.body;
  const same = (a: string[] = [], b: string[] = []) => a.length === b.length && a.every((item, index) => item === b[index]);
  if (after.labels !== undefined && !same(before.labels, after.labels)) out.labels = after.labels;
  if (after.assignees !== undefined && !same(before.assignees, after.assignees)) out.assignees = after.assignees;
  return out;
}

/** An issue's address. */
export function issueHref(repositoryId: string, issueId: string): string {
  return `/issue?repository_id=${encodeURIComponent(repositoryId)}&id=${encodeURIComponent(issueId)}`;
}

/** Why a person cannot change issues here, or null when they can. */
export function writeReason(signedIn: boolean, canWrite: boolean, loading: boolean): string | null {
  if (!signedIn) return "Sign in with write access on this repository to open, comment on or change issues.";
  if (loading) return "Checking your access…";
  if (!canWrite) return "You can read these issues. Opening, commenting and changing them need write access on the repository.";
  return null;
}

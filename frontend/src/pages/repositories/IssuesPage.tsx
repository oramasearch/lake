// SPDX-License-Identifier: MIT

/** /issues?repository_id=&state=&label=&assignee= (web_ui_issues): the issue tracker. `list_issues`
 * takes no filters, so the page filters: Open, Closed and All tabs (`state`; no state is Open), label
 * and assignee chips (`label`, `assignee`), all in the address, and a search by title or id. A person
 * with `write` on the repository opens a new issue from the header (lake#598). */
import { useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { CircleCheck, CircleDot, CircleSlash, Plus, Tags } from "lucide-react";
import { DataTable, Empty, FilterChips, SearchInput, Stat, StatRow, StateTabs, Toolbar, Button } from "@/lib/ui";
import { date, relative } from "@/lib/format";
import { useToast } from "@/lib/toast";
import {
  ForgePage,
  ForgeStatus,
  Identifier,
  itemsOf,
  pending,
  ToRepositories,
  useForgeRead,
  useRepositoryTrail,
  type ForgeRecord,
  type Items,
  type Repository,
} from "./forge";
import { LabelChip, useAssignable, useIssueAccess } from "./IssueParts";
import { NewIssue } from "./NewIssue";
import { issueHref, issueMatches, issueState, issueView, knownLabels, personName, type IssueRecord, type IssueView } from "./issues";

export default function IssuesPage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  if (!id) return <ToRepositories />;
  return <Issues id={id} />;
}

/** Each value with how many issues carry it, the selected one kept even when no issue does. */
function options(values: string[][], selected: string, name: (value: string) => string = (value) => value) {
  const counts = new Map<string, number>();
  for (const list of values) for (const value of list) counts.set(value, (counts.get(value) ?? 0) + 1);
  if (selected && !counts.has(selected)) counts.set(selected, 0);
  return [...counts.entries()].sort(([a], [b]) => a.localeCompare(b)).map(([value, count]) => ({ id: value, label: name(value), count }));
}

function Issues({ id }: { id: string }) {
  const [params, setParams] = useSearchParams();
  const navigate = useNavigate();
  const toast = useToast();
  const view = issueView(params.get("state"));
  const label = params.get("label") ?? "";
  const assignee = params.get("assignee") ?? "";
  const [search, setSearch] = useState("");
  const [creating, setCreating] = useState(false);
  const repository = useForgeRead<Repository>("get_repository", { id });
  const issues = useForgeRead<Items<ForgeRecord>>("list_issues", { repository_id: id, limit: 1000 });
  const access = useIssueAccess(id);
  const people = useAssignable(id, access.canWrite);
  useRepositoryTrail(repository.data, "Issues");
  const wait = pending(repository, issues);
  if (wait) return wait;
  const r = repository.data!;
  const all = itemsOf(issues.data) as IssueRecord[];
  const setFilter = (key: "state" | "label" | "assignee", value: string | null) => {
    const next = new URLSearchParams({ repository_id: id });
    const current = { state: view === "open" ? "" : view, label, assignee, [key]: value ?? "" };
    if (current.state && current.state !== "open") next.set("state", current.state);
    if (current.label) next.set("label", current.label);
    if (current.assignee) next.set("assignee", current.assignee);
    setParams(next);
  };
  const visible = all.filter((issue) => issueMatches(issue, { view, label, assignee, search }));
  const open = all.filter((issue) => issueState(issue) === "open").length;
  const closed = all.length - open;
  const labelOptions = options(all.map((issue) => issue.value.labels ?? []), label);
  const assigneeOptions = options(all.map((issue) => issue.value.assignees ?? []), assignee, (value) => personName(value, people, access.viewer));
  const newIssue = access.canWrite ? (
    <Button variant="primary" onClick={() => setCreating(true)}>
      <Plus size={14} aria-hidden /> New issue
    </Button>
  ) : (
    <Button variant="primary" disabled title={access.reason ?? undefined}>
      <Plus size={14} aria-hidden /> New issue
    </Button>
  );
  return (
    <ForgePage title="Issues" repository={r} tab="issues" sub="Work and questions tracked against this repository." actions={newIssue}>
      <StatRow>
        <Stat label="Open" value={open} icon={CircleDot} sub="need attention" />
        <Stat label="Closed" value={closed} icon={CircleCheck} sub="resolved" />
        <Stat label="Labels" value={labelOptions.filter((o) => o.count > 0).length} icon={Tags} sub="in use" />
        <Stat label="Unassigned" value={all.filter((issue) => !(issue.value.assignees ?? []).length).length} icon={CircleSlash} sub="no one on them" />
      </StatRow>
      {!access.canWrite && access.reason && <p className="muted mb-3 text-[12.5px]">{access.reason}</p>}
      <StateTabs<IssueView>
        label="Issue state"
        value={view}
        onChange={(value) => setFilter("state", value === "open" ? null : value)}
        tabs={[
          { value: "open", label: `Open ${open}` },
          { value: "closed", label: `Closed ${closed}` },
          { value: "all", label: `All ${all.length}` },
        ]}
      />
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search issues…" label="Search issues" />
      </Toolbar>
      {(labelOptions.length > 0 || assigneeOptions.length > 0) && (
        <Toolbar>
          {labelOptions.length > 0 && (
            <>
              <span className="label m-0">Label</span>
              <FilterChips options={labelOptions} value={label || null} onChange={(value) => setFilter("label", value)} allLabel="Any label" />
            </>
          )}
          {assigneeOptions.length > 0 && (
            <>
              <span className="label m-0 sm:ml-3">Assignee</span>
              <FilterChips options={assigneeOptions} value={assignee || null} onChange={(value) => setFilter("assignee", value)} allLabel="Anyone" />
            </>
          )}
        </Toolbar>
      )}
      <DataTable<IssueRecord>
        what="issues"
        caption="Issues"
        rows={visible}
        rowKey={(issue) => issue.id}
        empty={
          all.length === 0 ? (
            <Empty
              icon={CircleDot}
              title="No issues yet"
              action={
                access.canWrite ? (
                  <Button variant="primary" onClick={() => setCreating(true)}>
                    <Plus size={14} aria-hidden /> New issue
                  </Button>
                ) : undefined
              }
            >
              Open an issue to track a bug, a task or a question.
            </Empty>
          ) : (
            <Empty compact title={view === "closed" && !label && !assignee && !search ? "No closed issues" : view === "open" && !label && !assignee && !search ? "No open issues" : "No issues match these filters"}>
              {label || assignee || search ? "Clear a filter to see more." : "Switch the tab to see the others."}
            </Empty>
          )
        }
        columns={[
          {
            key: "issue",
            label: "Issue",
            sort: (issue) => issue.value.title ?? "",
            render: (issue) => (
              <div className="min-w-0">
                <Link className="font-semibold" to={issueHref(id, issue.id)}>
                  {issue.value.title ?? "Untitled issue"}
                </Link>
                <div className="text-[12px] text-ink-3">
                  <Identifier value={issue.id} /> · opened {date(issue.created_at)}
                  {issue.value.author ? ` by ${personName(issue.value.author, people, access.viewer)}` : ""}
                </div>
              </div>
            ),
          },
          { key: "state", label: "State", sort: issueState, render: (issue) => <ForgeStatus value={issueState(issue)} /> },
          {
            key: "labels",
            label: "Labels",
            render: (issue) =>
              (issue.value.labels ?? []).length ? (
                <div className="flex flex-wrap gap-1">
                  {(issue.value.labels as string[]).map((item) => (
                    <LabelChip key={item} label={item} onClick={() => setFilter("label", item)} active={item === label} />
                  ))}
                </div>
              ) : (
                <span className="muted">—</span>
              ),
          },
          {
            key: "assignees",
            label: "Assignees",
            render: (issue) =>
              (issue.value.assignees ?? []).length ? (
                <span className="text-[12.5px] text-ink-2">{(issue.value.assignees as string[]).map((who) => personName(who, people, access.viewer)).join(", ")}</span>
              ) : (
                <span className="muted">Unassigned</span>
              ),
          },
          {
            key: "updated",
            label: "Updated",
            sort: (issue) => issue.updated_at,
            render: (issue) => <span className="font-mono text-[12px] text-ink-2">{relative(issue.updated_at)}</span>,
          },
        ]}
      />
      {creating && (
        <NewIssue
          repositoryId={id}
          known={knownLabels(all)}
          people={people}
          viewer={access.viewer}
          onClose={() => setCreating(false)}
          onCreated={(issue) => {
            setCreating(false);
            toast("Issue opened");
            navigate(issueHref(id, issue.id));
          }}
        />
      )}
    </ForgePage>
  );
}

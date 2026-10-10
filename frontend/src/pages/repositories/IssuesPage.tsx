/** /issues?repository_id=&state=&label=&assignee= (web_ui_issues): the issue tracker, filtered in
 * the page by state, label and assignee as the renderer did (`list_issues` takes no filters). The
 * filters are chips that write the same query parameters; a search narrows by title in the page. */
import { useState } from "react";
import { Link, useSearchParams } from "react-router-dom";
import { CircleCheck, CircleDot, CircleSlash, Tags } from "lucide-react";
import { Chip, DataTable, Empty, FilterChips, SearchInput, Stat, StatRow, Toolbar } from "@/lib/ui";
import { date, relative } from "@/lib/format";
import {
  ForgePage,
  ForgeStatus,
  Identifier,
  IdentifierList,
  itemsOf,
  pending,
  ToRepositories,
  useForgeRead,
  useRepositoryTrail,
  type ForgeRecord,
  type Items,
  type Repository,
} from "./forge";

export default function IssuesPage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  if (!id) return <ToRepositories />;
  return <Issues id={id} />;
}

/** Each value with how many issues carry it, the selected one kept even when no issue does. */
function options(values: string[][], selected: string) {
  const counts = new Map<string, number>();
  for (const list of values) for (const value of list) counts.set(value, (counts.get(value) ?? 0) + 1);
  if (selected && !counts.has(selected)) counts.set(selected, 0);
  return [...counts.entries()].sort(([a], [b]) => a.localeCompare(b)).map(([value, count]) => ({ id: value, label: value, count }));
}

function Issues({ id }: { id: string }) {
  const [params, setParams] = useSearchParams();
  const state = params.get("state") ?? "";
  const label = params.get("label") ?? "";
  const assignee = params.get("assignee") ?? "";
  const [search, setSearch] = useState("");
  const repository = useForgeRead<Repository>("get_repository", { id });
  const issues = useForgeRead<Items<ForgeRecord>>("list_issues", { repository_id: id, limit: 1000 });
  useRepositoryTrail(repository.data, "Issues");
  const wait = pending(repository, issues);
  if (wait) return wait;
  const r = repository.data!;
  const all = itemsOf(issues.data);
  const stateOf = (issue: ForgeRecord): string => issue.value.state ?? "open";
  const setFilter = (key: "state" | "label" | "assignee", value: string | null) => {
    const next = new URLSearchParams({ repository_id: id });
    const current = { state, label, assignee, [key]: value ?? "" };
    next.set("state", current.state);
    next.set("label", current.label);
    next.set("assignee", current.assignee);
    setParams(next);
  };
  const needle = search.trim().toLowerCase();
  const visible = all.filter((issue) => {
    const labels: string[] = issue.value.labels ?? [];
    const assignees: string[] = issue.value.assignees ?? [];
    return (
      (!state || stateOf(issue) === state) &&
      (!label || labels.includes(label)) &&
      (!assignee || assignees.includes(assignee)) &&
      (!needle || `${issue.value.title ?? ""} ${issue.id}`.toLowerCase().includes(needle))
    );
  });
  const open = all.filter((issue) => stateOf(issue) === "open").length;
  const closed = all.filter((issue) => stateOf(issue) === "closed").length;
  const labelOptions = options(all.map((issue) => issue.value.labels ?? []), label);
  const assigneeOptions = options(all.map((issue) => issue.value.assignees ?? []), assignee);
  const encoded = encodeURIComponent(id);
  return (
    <ForgePage title="Issues" repository={r} tab="issues" sub="Work and questions tracked against this repository.">
      <StatRow>
        <Stat label="Open" value={open} icon={CircleDot} sub="need attention" />
        <Stat label="Closed" value={closed} icon={CircleCheck} sub="resolved" />
        <Stat label="Labels" value={labelOptions.filter((o) => o.count > 0).length} icon={Tags} sub="in use" />
        <Stat label="Unassigned" value={all.filter((issue) => !(issue.value.assignees ?? []).length).length} icon={CircleSlash} sub="no one on them" />
      </StatRow>
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search issues…" label="Search issues" />
        <FilterChips
          options={[
            { id: "open", label: "Open", count: open },
            { id: "closed", label: "Closed", count: closed },
          ]}
          value={state || null}
          onChange={(value) => setFilter("state", value)}
          allLabel="Any state"
        />
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
      <DataTable<ForgeRecord>
        what="issues"
        caption="Issues"
        rows={visible}
        rowKey={(issue) => issue.id}
        empty={
          all.length === 0 ? (
            <Empty icon={CircleDot} title="No issues yet">
              No issues have been opened on this repository.
            </Empty>
          ) : (
            <Empty compact title="No issues match these filters">
              Clear a filter to see more.
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
                <Link className="font-semibold" to={`/issue?repository_id=${encoded}&id=${encodeURIComponent(issue.id)}`}>
                  {issue.value.title ?? "Untitled issue"}
                </Link>
                <div className="text-[12px] text-ink-3">
                  <Identifier value={issue.id} /> · opened {date(issue.created_at)}
                </div>
              </div>
            ),
          },
          { key: "state", label: "State", sort: stateOf, render: (issue) => <ForgeStatus value={stateOf(issue)} /> },
          {
            key: "labels",
            label: "Labels",
            render: (issue) =>
              (issue.value.labels ?? []).length ? (
                <div className="flex flex-wrap gap-1">
                  {(issue.value.labels as string[]).map((item) => (
                    <Chip key={item} onClick={() => setFilter("label", item)} active={item === label}>
                      {item}
                    </Chip>
                  ))}
                </div>
              ) : (
                <span className="muted">—</span>
              ),
          },
          { key: "assignees", label: "Assignees", render: (issue) => <IdentifierList values={issue.value.assignees} empty="Unassigned" /> },
          {
            key: "updated",
            label: "Updated",
            sort: (issue) => issue.updated_at,
            render: (issue) => <span className="font-mono text-[12px] text-ink-2">{relative(issue.updated_at)}</span>,
          },
        ]}
      />
    </ForgePage>
  );
}

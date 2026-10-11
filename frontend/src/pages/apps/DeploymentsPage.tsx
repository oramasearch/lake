/** Ship → Deployments (web_ui.cpp web_ui_deployments; serve_deployments_page): every environment
 * across the repositories the viewer can open, read per repository as the server did, with their
 * figures, a search and a phase filter. A repository whose environments cannot be read is
 * skipped. `server_info` says first whether Deployments are set up, so a lake without them is
 * asked once, not once per repository (lake#605, `deployments.ts`). */
import { useState } from "react";
import { Link } from "react-router-dom";
import { useQueries } from "@tanstack/react-query";
import { FolderGit2, Globe, Rocket } from "lucide-react";
import { read, useList, useRead, type Row } from "@/lib/api";
import { isNotSetUp } from "@/lib/errors";
import { relative } from "@/lib/format";
import { type Column, DataTable, Empty, ErrorBlock, FilterChips, Mono, Page, SearchInput, Skeleton, Stat, StatRow, Status, toneOf, Toolbar } from "@/lib/ui";
import { asksRepository, deploymentsSetUp, type ServerInfo } from "./deployments";
import { abbreviated, repositoryName, StaleBanner, text, UrlText, type Repository } from "./shared";

type Environments = { items?: Row[]; stale_since?: string | null };
type Deployment = { repository: Repository; environment: Row };

const SUB = "Every environment across the repositories you can open: what is running, which change it runs, and where it is served.";

export default function DeploymentsPage() {
  const info = useRead<ServerInfo>("server_info", undefined, { workspace: null });
  // A failed `server_info` says nothing: the first repository is then asked alone.
  const setUp = deploymentsSetUp(info.data);
  const off = setUp === false;
  const repositories = useList<Repository>(off || info.isLoading ? null : "list_repositories", { limit: 500 }, { pick: "items", workspace: null });
  const firstId = repositories.data?.[0]?.id;
  // The same query as the first entry below (one key, one request), read here so the others can
  // wait for its answer when `server_info` does not say.
  const first = useRead<Environments>(firstId ? "list_environments" : null, { repository_id: firstId }, { workspace: null });
  const environments = useQueries({
    queries: (repositories.data ?? []).map((repository, index) => ({
      queryKey: ["list_environments", "", { repository_id: repository.id }],
      queryFn: ({ signal }: { signal: AbortSignal }) => read<Environments>("list_environments", { repository_id: repository.id }, { workspace: null, signal }),
      enabled: asksRepository(index, setUp, first),
    })),
  });
  const [search, setSearch] = useState("");
  const [phase, setPhase] = useState<string | null>(null);

  const rows: Deployment[] = [];
  let staleSince = "";
  (repositories.data ?? []).forEach((repository, index) => {
    const value = environments[index]?.data;
    if (!value) return;
    if (!staleSince && value.stale_since) staleSince = value.stale_since;
    for (const environment of value.items ?? []) rows.push({ repository, environment });
  });
  const pending = environments.some((q) => q.isLoading);
  // A lake without the platform mirror answers every repository `not_supported` at once, and
  // nothing asks again (lake#575): the page says so once instead of an empty list.
  const notSetUp = environments.find((q) => isNotSetUp(q.error));
  const phaseCounts = new Map<string, number>();
  for (const row of rows) {
    const p = text(row.environment, "phase") || "unknown";
    phaseCounts.set(p, (phaseCounts.get(p) ?? 0) + 1);
  }
  const phases = [...phaseCounts].map(([id, count]) => ({ id, label: id.replace(/[_-]+/g, " "), count }));

  if (off) {
    return (
      <Page eyebrow="Ship" title="Deployments" sub={SUB}>
        <NotSetUp />
      </Page>
    );
  }
  if (info.isLoading || repositories.isLoading) {
    return (
      <Page eyebrow="Ship" title="Deployments" sub={SUB}>
        <Skeleton />
      </Page>
    );
  }
  if (repositories.isError) {
    return (
      <Page eyebrow="Ship" title="Deployments" sub={SUB}>
        <ErrorBlock error={repositories.error} retry={() => void repositories.refetch()} what="repositories" />
      </Page>
    );
  }

  if (notSetUp) {
    return (
      <Page eyebrow="Ship" title="Deployments" sub={SUB}>
        <NotSetUp />
      </Page>
    );
  }

  const needle = search.trim().toLowerCase();
  const filtered = rows.filter(({ repository, environment }) => {
    const hay = `${repositoryName(repository)} ${text(environment, "name")} ${text(environment, "url")}`.toLowerCase();
    return (!needle || hay.includes(needle)) && (!phase || (text(environment, "phase") || "unknown") === phase);
  });
  const healthy = rows.filter(({ environment }) => toneOf(text(environment, "phase")) === "success").length;
  const failing = rows.filter(({ environment }) => toneOf(text(environment, "phase")) === "danger").length;
  const repositoriesDeployed = new Set(rows.map(({ repository }) => repository.id)).size;

  const columns: Column<Deployment>[] = [
    {
      key: "environment",
      label: "Environment",
      sort: ({ environment }) => text(environment, "name"),
      render: ({ repository, environment }) => {
        const name = text(environment, "name");
        const id = encodeURIComponent(repository.id);
        return (
          <div className="min-w-0">
            <Link to={`/environment?repository_id=${id}&name=${encodeURIComponent(name)}`} className="font-semibold">
              {name}
            </Link>
            <div className="truncate text-[12px] text-ink-3">
              <Link to={`/environments?repository_id=${id}`} className="text-ink-3">
                {repositoryName(repository)}
              </Link>
            </div>
          </div>
        );
      },
    },
    { key: "phase", label: "Phase", sort: ({ environment }) => text(environment, "phase"), render: ({ environment }) => <Status value={text(environment, "phase")} /> },
    { key: "change", label: "Change", render: ({ environment }) => <Mono>{abbreviated(text(environment, "change"))}</Mono> },
    { key: "url", label: "URL", render: ({ environment }) => <UrlText url={text(environment, "url")} /> },
    {
      key: "updated",
      label: "Updated",
      sort: ({ environment }) => text(environment, "updated_at"),
      render: ({ environment }) => <span title={text(environment, "updated_at")}>{relative(text(environment, "updated_at"), Date.now(), text(environment, "updated_at") || "—")}</span>,
    },
  ];

  return (
    <Page eyebrow="Ship" title="Deployments" sub={SUB}>
      <StaleBanner since={staleSince} />
      {rows.length === 0 ? (
        pending ? (
          <Skeleton />
        ) : (
          <Empty icon={Rocket} title="Nothing Deployed Yet">
            Deploy a repository with <code>liquid deploy</code>, and its environments appear here.
          </Empty>
        )
      ) : (
        <>
          <StatRow>
            <Stat label="Environments" value={rows.length} icon={Globe} />
            <Stat label="Repositories" value={repositoriesDeployed} icon={FolderGit2} sub="with an environment" />
            <Stat label="Healthy" value={healthy} tone={healthy ? "ok" : undefined} />
            <Stat label="Failing" value={failing} tone={failing ? "bad" : undefined} />
          </StatRow>
          <Toolbar>
            <SearchInput value={search} onChange={setSearch} placeholder="Search environments…" label="Search environments" />
            <FilterChips options={phases} value={phase} onChange={setPhase} />
            {pending && <span className="text-[12.5px] text-ink-3">Reading more repositories…</span>}
          </Toolbar>
          <DataTable
            rows={filtered}
            q={repositories}
            what="deployments"
            columns={columns}
            rowKey={({ repository, environment }) => `${repository.id}/${text(environment, "name")}`}
            caption="Deployments"
            empty={
              <Empty compact title="No Environment Matches">
                Clear a filter to see more.
              </Empty>
            }
          />
        </>
      )}
    </Page>
  );
}

/** Deployments on a lake started without the cluster operator's platform source: one plain
 * state, and what turns them on (docs/api/platform.md, lake#575). */
function NotSetUp() {
  return (
    <Empty icon={Rocket} title="Deployments Are Not Set Up on This Lake">
      An operator turns them on by connecting this Lake to the cluster operator's platform source. Until then, Lake has no environments to show.
    </Empty>
  );
}

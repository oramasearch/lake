// SPDX-License-Identifier: MIT

/** A repository's environments (web_ui.cpp web_ui_environments; serve_environments_page): their
 * figures, a search and phase filter, and each environment as a rich row. */
import { useState } from "react";
import { Link, Navigate, useSearchParams } from "react-router-dom";
import { Globe } from "lucide-react";
import { useRead, type Row } from "@/lib/api";
import { relative } from "@/lib/format";
import { entityCrumb, useTrail } from "@/lib/trail";
import { type Column, DataTable, Empty, ErrorBlock, FilterChips, Mono, Page, SearchInput, Skeleton, Stat, StatRow, Status, toneOf, Toolbar } from "@/lib/ui";
import { abbreviated, repositoryName, RepositoryTabs, shortImage, StaleBanner, text, UrlText, type Repository } from "./shared";

/** The repository a platform page belongs to, and its trail: Repositories › name › page. */
export function usePlatformRepository(repositoryId: string, page: string) {
  const repository = useRead<Repository>(repositoryId ? "get_repository" : null, { id: repositoryId }, { workspace: null });
  const name = repositoryName(repository.data);
  useTrail(
    repository.data ? [{ label: "Repositories", to: "/repositories" }, entityCrumb(name, `/repository?id=${encodeURIComponent(repositoryId)}`), { label: page }] : null,
    repository.data ? page : undefined,
  );
  return repository;
}

/** A platform time as "3h ago", its exact value on hover. */
export function When({ value }: { value: string }) {
  if (!value) return <span className="muted">—</span>;
  return <span title={value}>{relative(value, Date.now(), value)}</span>;
}

export default function EnvironmentsPage() {
  const [params] = useSearchParams();
  const repositoryId = params.get("repository_id") ?? "";
  const repository = usePlatformRepository(repositoryId, "Environments");
  const environments = useRead<{ items?: Row[]; stale_since?: string | null }>(repositoryId ? "list_environments" : null, { repository_id: repositoryId }, { workspace: null });
  const [search, setSearch] = useState("");
  const [phase, setPhase] = useState<string | null>(null);
  if (!repositoryId) return <Navigate to="/repositories" replace />;
  const loading = repository.isLoading || environments.isLoading;
  const failed = repository.isError ? repository : environments.isError ? environments : null;
  const items = environments.data?.items ?? [];
  const counts = new Map<string, number>();
  for (const item of items) counts.set(text(item, "phase") || "unknown", (counts.get(text(item, "phase") || "unknown") ?? 0) + 1);
  const needle = search.trim().toLowerCase();
  const filtered = items.filter((item) => (!needle || `${text(item, "name")} ${text(item, "url")}`.toLowerCase().includes(needle)) && (!phase || (text(item, "phase") || "unknown") === phase));
  const environmentHref = (name: string) => `/environment?repository_id=${encodeURIComponent(repositoryId)}&name=${encodeURIComponent(name)}`;

  const columns: Column<Row>[] = [
    {
      key: "name",
      label: "Environment",
      sort: (item) => text(item, "name"),
      render: (item) => (
        <div className="min-w-0">
          <Link to={environmentHref(text(item, "name"))} className="font-semibold">
            {text(item, "name")}
          </Link>
          <div className="truncate text-[12px] text-ink-3">
            <UrlText url={text(item, "url")} empty="No URL yet" />
          </div>
        </div>
      ),
    },
    { key: "phase", label: "Phase", sort: (item) => text(item, "phase"), render: (item) => <Status value={text(item, "phase")} /> },
    { key: "change", label: "Change", render: (item) => <Mono>{abbreviated(text(item, "change"))}</Mono> },
    { key: "image", label: "Image", render: (item) => <Mono>{shortImage(text(item, "image"))}</Mono> },
    { key: "domains", label: "Domains", numeric: true, sort: (item) => (Array.isArray(item.domains) ? item.domains.length : 0), render: (item) => <span className="font-mono">{Array.isArray(item.domains) ? item.domains.length : 0}</span> },
    { key: "updated", label: "Updated", sort: (item) => text(item, "updated_at"), render: (item) => <When value={text(item, "updated_at")} /> },
  ];

  return (
    <Page eyebrow="Data" title="Environments" sub={repository.data ? <>Deployments of <code>{repositoryName(repository.data)}</code>, mirrored from the operator control plane.</> : "Deployments mirrored from the operator control plane."}>
      <RepositoryTabs repositoryId={repositoryId} current="platform" />
      {loading ? (
        <Skeleton />
      ) : failed ? (
        <ErrorBlock error={failed.error} retry={() => void failed.refetch()} what="environments" />
      ) : (
        <>
          <StaleBanner since={environments.data?.stale_since} />
          {items.length === 0 ? (
            <Empty icon={Globe} title="No Environments">
              No environments have been deployed for this repository. Deploy it with <code>liquid deploy</code>.
            </Empty>
          ) : (
            <>
              <StatRow cols={3}>
                <Stat label="Environments" value={items.length} icon={Globe} />
                <Stat label="Healthy" value={items.filter((item) => toneOf(text(item, "phase")) === "success").length} tone="ok" />
                <Stat label="Failing" value={items.filter((item) => toneOf(text(item, "phase")) === "danger").length} tone={items.some((item) => toneOf(text(item, "phase")) === "danger") ? "bad" : undefined} />
              </StatRow>
              <Toolbar>
                <SearchInput value={search} onChange={setSearch} placeholder="Search environments…" label="Search environments" />
                <FilterChips options={[...counts].map(([id, count]) => ({ id, label: id.replace(/[_-]+/g, " "), count }))} value={phase} onChange={setPhase} />
              </Toolbar>
              <DataTable
                rows={filtered}
                q={environments}
                what="environments"
                columns={columns}
                rowKey={(item) => text(item, "name")}
                caption="Environments"
                empty={
                  <Empty compact title="No Environment Matches">
                    Clear a filter to see more.
                  </Empty>
                }
              />
            </>
          )}
        </>
      )}
    </Page>
  );
}

/** Ship → Deployments (web_ui.cpp web_ui_deployments; serve_deployments_page): every environment
 * across the repositories the viewer can open, read per repository as the server did. A repository
 * whose environments cannot be read is skipped. */
import { Link } from "react-router-dom";
import { useQueries } from "@tanstack/react-query";
import { read, useList, type Row } from "@/lib/api";
import { ErrorBlock, Page, Skeleton } from "@/lib/ui";
import { abbreviated, repositoryName, StaleBanner, text, UrlText, type Repository } from "./shared";

type Environments = { items?: Row[]; stale_since?: string | null };

export default function DeploymentsPage() {
  const repositories = useList<Repository>("list_repositories", { limit: 500 }, { pick: "items", workspace: null });
  const environments = useQueries({
    queries: (repositories.data ?? []).map((repository) => ({
      queryKey: ["list_environments", "", { repository_id: repository.id }],
      queryFn: ({ signal }: { signal: AbortSignal }) => read<Environments>("list_environments", { repository_id: repository.id }, { workspace: null, signal }),
    })),
  });

  const sub = "Every environment across the repositories you can open: what is running, which change it runs, and where it is served.";
  if (repositories.isLoading) {
    return (
      <Page eyebrow="Ship" title="Deployments" sub={sub}>
        <Skeleton />
      </Page>
    );
  }
  if (repositories.isError) {
    return (
      <Page eyebrow="Ship" title="Deployments" sub={sub}>
        <ErrorBlock error={repositories.error} retry={() => void repositories.refetch()} what="repositories" />
      </Page>
    );
  }
  const rows: { repository: Repository; environment: Row }[] = [];
  let staleSince = "";
  (repositories.data ?? []).forEach((repository, index) => {
    const value = environments[index]?.data;
    if (!value) return;
    if (!staleSince && value.stale_since) staleSince = value.stale_since;
    for (const environment of value.items ?? []) rows.push({ repository, environment });
  });
  const pending = environments.some((q) => q.isLoading);

  return (
    <Page eyebrow="Ship" title="Deployments" sub={sub}>
      <StaleBanner since={staleSince} />
      {rows.length === 0 ? (
        pending ? (
          <Skeleton />
        ) : (
          <div className="panel empty">
            Nothing is deployed yet. Deploy a repository with <code>liquid deploy</code>, and its environments appear here.
          </div>
        )
      ) : (
        <div className="table-wrap">
          <table>
            <thead>
              <tr>
                <th>Repository</th>
                <th>Environment</th>
                <th>Phase</th>
                <th>Change</th>
                <th>URL</th>
                <th>Updated</th>
              </tr>
            </thead>
            <tbody>
              {rows.map(({ repository, environment }) => {
                const name = text(environment, "name");
                const id = encodeURIComponent(repository.id);
                return (
                  <tr key={`${repository.id}/${name}`}>
                    <td>
                      <Link to={`/environments?repository_id=${id}`}>{repositoryName(repository)}</Link>
                    </td>
                    <td>
                      <Link to={`/environment?repository_id=${id}&name=${encodeURIComponent(name)}`}>{name}</Link>
                    </td>
                    <td>
                      <span className="badge">{text(environment, "phase")}</span>
                    </td>
                    <td>
                      <code>{abbreviated(text(environment, "change"))}</code>
                    </td>
                    <td>
                      <UrlText url={text(environment, "url")} />
                    </td>
                    <td>{text(environment, "updated_at")}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}
    </Page>
  );
}

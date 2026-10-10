/** A repository's environments (web_ui.cpp web_ui_environments; serve_environments_page). */
import { Link, Navigate, useSearchParams } from "react-router-dom";
import { useRead, type Row } from "@/lib/api";
import { entityCrumb, useTrail } from "@/lib/trail";
import { ErrorBlock, Page, Skeleton } from "@/lib/ui";
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

export default function EnvironmentsPage() {
  const [params] = useSearchParams();
  const repositoryId = params.get("repository_id") ?? "";
  const repository = usePlatformRepository(repositoryId, "Environments");
  const environments = useRead<{ items?: Row[]; stale_since?: string | null }>(repositoryId ? "list_environments" : null, { repository_id: repositoryId }, { workspace: null });
  if (!repositoryId) return <Navigate to="/repositories" replace />;
  const loading = repository.isLoading || environments.isLoading;
  const failed = repository.isError ? repository : environments.isError ? environments : null;
  const items = environments.data?.items ?? [];
  return (
    <Page eyebrow="platform" title="Environments">
      <RepositoryTabs repositoryId={repositoryId} current="platform" />
      {loading ? (
        <Skeleton />
      ) : failed ? (
        <ErrorBlock error={failed.error} retry={() => void failed.refetch()} what="environments" />
      ) : (
        <>
          <StaleBanner since={environments.data?.stale_since} />
          <p className="sub">Deployments mirrored from the operator control plane.</p>
          {items.length === 0 ? (
            <div className="panel empty">No environments have been deployed for this repository.</div>
          ) : (
            <div className="table-wrap">
              <table>
                <thead>
                  <tr>
                    <th>Name</th>
                    <th>Phase</th>
                    <th>Change</th>
                    <th>Image</th>
                    <th>URL</th>
                    <th>Domains</th>
                    <th>Updated</th>
                  </tr>
                </thead>
                <tbody>
                  {items.map((item) => {
                    const name = text(item, "name");
                    return (
                      <tr key={name}>
                        <td>
                          <Link to={`/environment?repository_id=${encodeURIComponent(repositoryId)}&name=${encodeURIComponent(name)}`}>{name}</Link>
                        </td>
                        <td>
                          <span className="badge">{text(item, "phase")}</span>
                        </td>
                        <td>
                          <code>{abbreviated(text(item, "change"))}</code>
                        </td>
                        <td>
                          <code>{shortImage(text(item, "image"))}</code>
                        </td>
                        <td>
                          <UrlText url={text(item, "url")} />
                        </td>
                        <td>{Array.isArray(item.domains) ? item.domains.length : 0}</td>
                        <td>{text(item, "updated_at")}</td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          )}
        </>
      )}
    </Page>
  );
}

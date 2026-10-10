/** Repository Export (web_ui.cpp web_ui_exports; serve_exports_page). The download stays the
 * server's `/exports/download` route, which answers the archive as an attachment. */
import { Navigate, useSearchParams } from "react-router-dom";
import { useList, type Row } from "@/lib/api";
import { DataTable, ErrorBlock, Page, Skeleton } from "@/lib/ui";
import { usePlatformRepository } from "./EnvironmentsPage";
import { repositoryName } from "./shared";

export default function ExportsPage() {
  const [params] = useSearchParams();
  const repositoryId = params.get("repository_id") ?? "";
  const repository = usePlatformRepository(repositoryId, "Exports");
  const exports = useList<Row>(repositoryId ? "list_exports" : null, { repository_id: repositoryId }, { pick: "items", workspace: null });
  if (!repositoryId) return <Navigate to="/repositories" replace />;
  if (repository.isLoading) {
    return (
      <Page eyebrow="portability" title="Repository Export">
        <Skeleton />
      </Page>
    );
  }
  if (repository.isError) {
    return (
      <Page eyebrow="portability" title="Repository Export">
        <ErrorBlock error={repository.error} retry={() => void repository.refetch()} what="this repository" />
      </Page>
    );
  }
  return (
    <Page
      eyebrow="portability"
      title="Repository Export"
      sub={
        <>
          Download a signed archive for <code>{repositoryName(repository.data)}</code>.
        </>
      }
    >
      <div className="panel">
        <form className="stack" method="get" action="/exports/download">
          <input type="hidden" name="repository_id" value={repositoryId} />
          <label>
            <span>
              <input type="checkbox" name="include_git" value="true" /> Include Git projection
            </span>
          </label>
          <button className="button button-primary" type="submit">
            Request and download export
          </button>
        </form>
      </div>
      <h2>Prior Exports</h2>
      <DataTable
        rows={exports.data}
        q={exports}
        what="exports"
        caption="Prior exports"
        empty="No exports requested."
        columns={[
          { key: "created", label: "Created", render: (row) => String(row.created_at ?? "") },
          { key: "git", label: "Git", render: (row) => (row.value?.include_git ? "requested" : "omitted") },
          { key: "manifest", label: "Manifest", render: (row) => <code>{String(row.value?.manifest?.format ?? "lake_export_manifest/1")}</code> },
        ]}
      />
    </Page>
  );
}

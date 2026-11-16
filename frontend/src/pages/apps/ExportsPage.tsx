// SPDX-License-Identifier: MIT

/** Repository Export (web_ui.cpp web_ui_exports; serve_exports_page). The download stays the
 * server's `/exports/download` route, which answers the archive as an attachment. */
import { Navigate, useSearchParams } from "react-router-dom";
import { Archive, Download } from "lucide-react";
import { useList, type Row } from "@/lib/api";
import { Card, DataTable, Empty, ErrorBlock, Page, Skeleton, Split, Status } from "@/lib/ui";
import { usePlatformRepository, When } from "./EnvironmentsPage";
import { repositoryName } from "./shared";

export default function ExportsPage() {
  const [params] = useSearchParams();
  const repositoryId = params.get("repository_id") ?? "";
  const repository = usePlatformRepository(repositoryId, "Exports");
  const exports = useList<Row>(repositoryId ? "list_exports" : null, { repository_id: repositoryId }, { pick: "items", workspace: null });
  if (!repositoryId) return <Navigate to="/repositories" replace />;
  if (repository.isLoading) {
    return (
      <Page eyebrow="Data · Portability" title="Repository Export">
        <Skeleton />
      </Page>
    );
  }
  if (repository.isError) {
    return (
      <Page eyebrow="Data · Portability" title="Repository Export">
        <ErrorBlock error={repository.error} retry={() => void repository.refetch()} what="this repository" />
      </Page>
    );
  }
  return (
    <Page
      eyebrow="Data · Portability"
      title="Repository Export"
      sub={
        <>
          Download a signed archive for <code>{repositoryName(repository.data)}</code>.
        </>
      }
      actions={
        <button className="button button-primary" type="submit" form="export-form">
          <Download size={14} aria-hidden /> Request and download export
        </button>
      }
    >
      <Split
        side={
          <Card label="New export" title="Request an Export">
            <form id="export-form" className="stack" method="get" action="/exports/download">
              <input type="hidden" name="repository_id" value={repositoryId} />
              <label>
                <span>
                  <input type="checkbox" name="include_git" value="true" /> Include Git projection
                </span>
              </label>
              <p className="muted m-0 text-[13px]">The archive is signed and downloads as soon as it is ready. Use the button above to request it.</p>
            </form>
          </Card>
        }
      >
        <Card title="Prior Exports" padded={false}>
          <DataTable
            rows={exports.data}
            q={exports}
            what="exports"
            caption="Prior exports"
            empty={
              <div className="p-4">
                <Empty compact icon={Archive} title="No Exports Yet">
                  No exports requested.
                </Empty>
              </div>
            }
            columns={[
              { key: "created", label: "Created", sort: (row) => String(row.created_at ?? ""), render: (row) => <When value={String(row.created_at ?? "")} /> },
              { key: "git", label: "Git", render: (row) => <Status value={row.value?.include_git ? "requested" : "omitted"} tone={row.value?.include_git ? "info" : "neutral"} /> },
              { key: "manifest", label: "Manifest", render: (row) => <code>{String(row.value?.manifest?.format ?? "lake_export_manifest/1")}</code> },
            ]}
          />
        </Card>
      </Split>
    </Page>
  );
}

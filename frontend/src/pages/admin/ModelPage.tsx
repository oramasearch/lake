/** Model (`/models/:namespace/:id`, `/models/:id`): a model repository's storage, training
 * provenance, files and revisions (web_ui_model, serve_model_page). The name is resolved with
 * resolve_model, then read with get_model. Public models open signed out. */
import { useState } from "react";
import { useParams } from "react-router-dom";
import { Database, FileStack, GitCommitHorizontal, HardDrive } from "lucide-react";
import { useRead, type Row } from "@/lib/api";
import { bytes } from "@/lib/format";
import { useSession } from "@/lib/session";
import { useTrail } from "@/lib/trail";
import { Card, DataTable, ErrorBlock, KeyValue, Mono, Page, SearchInput, Skeleton, Split, Stat, StatRow, Status, Toolbar } from "@/lib/ui";
import { PanelEmpty, matches } from "@/pages/organizations/shared";

export default function ModelPage() {
  const { namespace, id = "" } = useParams();
  const { viewer } = useSession();
  const resolved = useRead<Row>("resolve_model", { namespace, name: id });
  const repositoryId: string | undefined = resolved.data?.repository_id;
  const model = useRead<Row>(repositoryId ? "get_model" : null, { id: repositoryId, revision: resolved.data?.revision || "main" });
  const modelId: string | undefined = model.data?.modelId;
  useTrail(
    [{ label: viewer.signed_in ? "Repositories" : "Public Repositories", to: "/repositories" }, { label: modelId ?? (namespace ? `${namespace}/${id}` : id) }],
    "Model",
  );

  // Any failure to find the repository reads as not found, as on the server.
  if (resolved.isError) {
    return (
      <Page eyebrow="Model" title="Model Not Found">
        <ErrorBlock error={resolved.error} what="the model" />
      </Page>
    );
  }
  if (model.isError) {
    return (
      <Page eyebrow="Model" title="Model Unavailable">
        <ErrorBlock error={model.error} retry={() => void model.refetch()} what="the model" />
      </Page>
    );
  }
  if (!model.data) {
    return (
      <Page eyebrow="Model" title={id}>
        <Skeleton lines={6} />
      </Page>
    );
  }
  return <Model model={model.data} />;
}

/** A byte count in words, the exact count on hover. */
function Bytes({ value }: { value: unknown }) {
  const n = Number(value ?? 0);
  return <span title={`${n} bytes`}>{bytes(n)}</span>;
}

function Model({ model }: { model: Row }) {
  const storage: Row = model.storage ?? {};
  const provenance: Row | null = model.training_provenance && typeof model.training_provenance === "object" ? model.training_provenance : null;
  const code = (value: unknown) => <code>{String(value ?? "")}</code>;
  const files = (model.siblings ?? []) as Row[];
  const revisions = (storage.revisions ?? []) as Row[];
  const [search, setSearch] = useState("");
  const shownFiles = files.filter((f) => matches(search, f.rfilename));
  return (
    <Page
      eyebrow="Model"
      title={model.modelId}
      sub={
        <span className="inline-flex flex-wrap items-center gap-2">
          Revision <Mono short={16}>{model.sha}</Mono>
          {provenance ? <Status value={provenance.verified ? "verified" : "not verified"} tone={provenance.verified ? "success" : "warning"} /> : null}
        </span>
      }
    >
      <StatRow>
        <Stat label="Logical bytes" value={<Bytes value={storage.logical_bytes} />} icon={HardDrive} />
        <Stat label="Unique bytes" value={<Bytes value={storage.unique_bytes} />} icon={Database} />
        <Stat label="Deduplicated bytes" value={<Bytes value={storage.deduplicated_bytes} />} tone={Number(storage.deduplicated_bytes ?? 0) > 0 ? "ok" : undefined} />
        <Stat label="Deduplication ratio" value={Number(storage.dedup_ratio ?? 0).toFixed(6)} />
      </StatRow>
      <Split
        side={
          <Card label="Training provenance" actions={provenance ? <Status value={provenance.verified ? "verified" : "not verified"} tone={provenance.verified ? "success" : "warning"} /> : undefined}>
            {provenance ? (
              <KeyValue
                rows={[
                  ["Dataset", code(provenance.dataset_repository_id)],
                  ["Dataset revision", code(provenance.dataset_revision)],
                  ["Code revision", code(provenance.code_revision)],
                  ["Run", code(provenance.invocation_id)],
                  ["Runner", code(provenance.runner)],
                  ["Builder", code(provenance.builder)],
                  ["Environment digest", code(provenance.environment_digest)],
                  ["Parameters", code(JSON.stringify(provenance.parameters ?? []))],
                  ["Environment", code(JSON.stringify(provenance.environment ?? {}))],
                  ["Model attestation", code(provenance.model_attestation)],
                  ["Run attestation", code(provenance.run_attestation)],
                  ...(provenance.reason ? ([["Reason", provenance.reason]] as [string, string][]) : []),
                ]}
              />
            ) : (
              <PanelEmpty>No training provenance is attached.</PanelEmpty>
            )}
          </Card>
        }
      >
        <Card label="Files" padded={false} actions={<span className="font-mono text-[11.5px] text-ink-3">{files.length}</span>}>
          {files.length > 6 && (
            <div className="px-4 pt-3">
              <Toolbar>
                <SearchInput value={search} onChange={setSearch} placeholder="Search files…" />
              </Toolbar>
            </div>
          )}
          <DataTable
            rows={shownFiles}
            rowKey={(f) => f.rfilename}
            empty={
              <div className="p-4">
                <PanelEmpty>No files.</PanelEmpty>
              </div>
            }
            columns={[
              {
                key: "path",
                label: "Path",
                sort: (f) => f.rfilename,
                render: (f) => (
                  <span className="inline-flex items-center gap-1.5">
                    <FileStack size={14} className="text-ink-3" aria-hidden />
                    <span className="font-mono text-[12.5px]">{f.rfilename}</span>
                  </span>
                ),
              },
              { key: "size", label: "Size", sort: (f) => Number(f.size ?? 0), render: (f) => <Bytes value={f.size} />, numeric: true },
              { key: "stored", label: "Stored", sort: (f) => Number(f.storedSize ?? 0), render: (f) => <Bytes value={f.storedSize} />, numeric: true },
              { key: "saved", label: "Saved", sort: (f) => Number(f.deduplicatedBytes ?? 0), render: (f) => <Bytes value={f.deduplicatedBytes} />, numeric: true },
            ]}
          />
        </Card>
        <Card label="Revisions" padded={false}>
          <DataTable
            rows={revisions}
            rowKey={(r) => r.revision}
            empty={
              <div className="p-4">
                <PanelEmpty>No revisions.</PanelEmpty>
              </div>
            }
            columns={[
              {
                key: "revision",
                label: "Revision",
                render: (r) => (
                  <span className="inline-flex items-center gap-1.5">
                    <GitCommitHorizontal size={14} className="text-ink-3" aria-hidden />
                    <Mono short={16}>{r.revision}</Mono>
                  </span>
                ),
              },
              { key: "logical", label: "Logical", render: (r) => <Bytes value={r.logical_bytes} />, numeric: true },
              { key: "unique", label: "Unique", render: (r) => <Bytes value={r.unique_bytes} />, numeric: true },
              { key: "saved", label: "Saved", render: (r) => <Bytes value={r.deduplicated_bytes} />, numeric: true },
            ]}
          />
        </Card>
      </Split>
    </Page>
  );
}

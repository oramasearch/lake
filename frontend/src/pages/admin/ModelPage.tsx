/** Model (`/models/:namespace/:id`, `/models/:id`): a model repository's storage, training
 * provenance, files and revisions (web_ui_model, serve_model_page). The name is resolved with
 * resolve_model, then read with get_model. Public models open signed out. */
import { useParams } from "react-router-dom";
import { useRead, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { useTrail } from "@/lib/trail";
import { DataTable, ErrorBlock, Page, Skeleton } from "@/lib/ui";

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
      <Page eyebrow="model" title="Model Not Found">
        <ErrorBlock error={resolved.error} what="the model" />
      </Page>
    );
  }
  if (model.isError) {
    return (
      <Page eyebrow="model" title="Model Unavailable">
        <ErrorBlock error={model.error} retry={() => void model.refetch()} what="the model" />
      </Page>
    );
  }
  if (!model.data) {
    return (
      <Page eyebrow="model" title={id}>
        <Skeleton lines={6} />
      </Page>
    );
  }
  return <Model model={model.data} />;
}

function Model({ model }: { model: Row }) {
  const storage: Row = model.storage ?? {};
  const provenance: Row | null = model.training_provenance && typeof model.training_provenance === "object" ? model.training_provenance : null;
  const code = (value: unknown) => <code>{String(value ?? "")}</code>;
  return (
    <Page
      eyebrow="model"
      title={model.modelId}
      sub={
        <>
          Revision <code>{model.sha}</code>
        </>
      }
    >
      <div className="grid">
        <section className="panel">
          <h2>Storage</h2>
          <dl>
            <dt>Logical bytes</dt>
            <dd>{storage.logical_bytes ?? 0}</dd>
            <dt>Unique bytes</dt>
            <dd>{storage.unique_bytes ?? 0}</dd>
            <dt>Deduplicated bytes</dt>
            <dd>{storage.deduplicated_bytes ?? 0}</dd>
            <dt>Deduplication ratio</dt>
            <dd>{Number(storage.dedup_ratio ?? 0).toFixed(6)}</dd>
          </dl>
        </section>
        <section className="panel">
          <h2>Training Provenance</h2>
          {provenance ? (
            <>
              <p>
                <span className="badge">{provenance.verified ? "verified" : "not verified"}</span>
              </p>
              <dl>
                <dt>Dataset</dt>
                <dd>{code(provenance.dataset_repository_id)}</dd>
                <dt>Dataset revision</dt>
                <dd>{code(provenance.dataset_revision)}</dd>
                <dt>Code revision</dt>
                <dd>{code(provenance.code_revision)}</dd>
                <dt>Run</dt>
                <dd>{code(provenance.invocation_id)}</dd>
                <dt>Runner</dt>
                <dd>{code(provenance.runner)}</dd>
                <dt>Builder</dt>
                <dd>{code(provenance.builder)}</dd>
                <dt>Environment digest</dt>
                <dd>{code(provenance.environment_digest)}</dd>
                <dt>Parameters</dt>
                <dd>{code(JSON.stringify(provenance.parameters ?? []))}</dd>
                <dt>Environment</dt>
                <dd>{code(JSON.stringify(provenance.environment ?? {}))}</dd>
                <dt>Model attestation</dt>
                <dd>{code(provenance.model_attestation)}</dd>
                <dt>Run attestation</dt>
                <dd>{code(provenance.run_attestation)}</dd>
                {provenance.reason ? (
                  <>
                    <dt>Reason</dt>
                    <dd>{provenance.reason}</dd>
                  </>
                ) : null}
              </dl>
            </>
          ) : (
            <p className="empty">No training provenance is attached.</p>
          )}
        </section>
      </div>
      <h2>Files</h2>
      <DataTable
        rows={(model.siblings ?? []) as Row[]}
        rowKey={(f) => f.rfilename}
        empty={<p className="empty">No files.</p>}
        columns={[
          { key: "path", label: "Path", render: (f) => f.rfilename },
          { key: "size", label: "Size", render: (f) => f.size ?? 0, numeric: true },
          { key: "stored", label: "Stored", render: (f) => f.storedSize ?? 0, numeric: true },
          { key: "saved", label: "Saved", render: (f) => f.deduplicatedBytes ?? 0, numeric: true },
        ]}
      />
      <h2>Revisions</h2>
      <DataTable
        rows={(storage.revisions ?? []) as Row[]}
        rowKey={(r) => r.revision}
        empty={<p className="empty">No revisions.</p>}
        columns={[
          { key: "revision", label: "Revision", render: (r) => <code>{r.revision}</code> },
          { key: "logical", label: "Logical", render: (r) => r.logical_bytes ?? 0, numeric: true },
          { key: "unique", label: "Unique", render: (r) => r.unique_bytes ?? 0, numeric: true },
          { key: "saved", label: "Saved", render: (r) => r.deduplicated_bytes ?? 0, numeric: true },
        ]}
      />
    </Page>
  );
}

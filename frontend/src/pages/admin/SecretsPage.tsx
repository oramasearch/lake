/** Secrets (`/secrets?mount_id=&name=`): one static secret's version metadata, never its value
 * (web_ui_secrets, serve_secrets_page). */
import { Link, Navigate } from "react-router-dom";
import { useRead, type Row } from "@/lib/api";
import { DataTable, ErrorBlock, Page, Skeleton } from "@/lib/ui";
import { PanelEmpty, When, useMetaTrail, useParam, useQuerySubmit } from "@/pages/organizations/shared";

export default function SecretsPage() {
  const mountId = useParam("mount_id");
  const name = useParam("name");
  const mount = useRead<Row>(mountId ? "get_secret_mount" : null, { mount_id: mountId });
  const metadata = useRead<Row>(mountId && name ? "secret_metadata" : null, { mount_id: mountId, name });
  const submit = useQuerySubmit("/secrets");
  useMetaTrail(mount.data?.name, "Secrets");
  if (!mountId) return <Navigate to="/secret-mounts" replace />;
  if (mount.isError) {
    return (
      <Page eyebrow="static secrets" title="Secrets">
        <ErrorBlock error={mount.error} retry={() => void mount.refetch()} what="the secret mount" />
      </Page>
    );
  }
  if (!mount.data) {
    return (
      <Page eyebrow="static secrets" title="Secrets">
        <Skeleton />
      </Page>
    );
  }
  const m = metadata.data;
  return (
    <Page eyebrow="static secrets" title={mount.data.name} sub="Inspect version metadata without rendering secret values. Writes, reads, deletion and destruction use the capability API.">
      <form className="filters" onSubmit={submit} key={name}>
        <input type="hidden" name="mount_id" defaultValue={mountId} />
        <label>
          Secret name
          <input name="name" defaultValue={name} required />
        </label>
        <button type="submit">Inspect history</button>
      </form>
      {!name ? (
        <PanelEmpty>Enter a secret name to inspect its metadata.</PanelEmpty>
      ) : metadata.isError ? (
        <ErrorBlock error={metadata.error} retry={() => void metadata.refetch()} what="the secret's metadata" />
      ) : !m ? (
        <Skeleton />
      ) : (
        <>
          <div className="panel">
            <dl>
              <dt>Name</dt>
              <dd>
                <code>{m.name}</code>
              </dd>
              <dt>Current version</dt>
              <dd>{m.current_version}</dd>
              <dt>Custom metadata</dt>
              <dd>
                <code>{JSON.stringify(m.custom_metadata ?? {})}</code>
              </dd>
            </dl>
          </div>
          <h2>Version History</h2>
          <DataTable
            rows={(m.versions ?? []) as Row[]}
            rowKey={(v) => String(v.version)}
            columns={[
              { key: "version", label: "Version", render: (v) => v.version },
              { key: "created", label: "Created", render: (v) => <When value={v.created_at} /> },
              { key: "deleted", label: "Deleted", render: (v) => <When value={v.deleted_at} /> },
              { key: "destroyed", label: "Destroyed", render: (v) => <When value={v.destroyed_at} /> },
            ]}
          />
        </>
      )}
      <p className="sub">
        <Link to={`/encryption-keys?mount_id=${encodeURIComponent(mountId)}`}>Named encryption keys</Link> · API: <code>POST /v1/put_secret</code>, <code>GET /v1/read_secret</code>,{" "}
        <code>GET /v1/secret_metadata</code>.
      </p>
    </Page>
  );
}

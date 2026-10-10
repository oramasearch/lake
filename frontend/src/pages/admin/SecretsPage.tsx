/** Secrets (`/secrets?mount_id=&name=`): one static secret's version metadata, never its value
 * (web_ui_secrets, serve_secrets_page). */
import { Link, Navigate } from "react-router-dom";
import { History, KeyRound, Search } from "lucide-react";
import { useRead, type Row } from "@/lib/api";
import { Button, Card, DataTable, Empty, ErrorBlock, KeyValue, Mono, Page, Skeleton, Split, Stat, StatRow, Status } from "@/lib/ui";
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
      <Page eyebrow="Organization" title="Secrets">
        <ErrorBlock error={mount.error} retry={() => void mount.refetch()} what="the secret mount" />
      </Page>
    );
  }
  if (!mount.data) {
    return (
      <Page eyebrow="Organization" title="Secrets">
        <Skeleton lines={5} />
      </Page>
    );
  }
  const m = metadata.data;
  const versions = (m?.versions ?? []) as Row[];
  const state = (v: Row) => (v.destroyed_at ? "destroyed" : v.deleted_at ? "deleted" : "active");
  return (
    <Page
      eyebrow="Organization"
      title={mount.data.name}
      sub="Inspect a secret's version metadata. Values are never shown here; writes, reads, deletion and destruction use the capability API."
      actions={
        <Button to={`/encryption-keys?mount_id=${encodeURIComponent(mountId)}`}>
          <KeyRound size={14} aria-hidden /> Named encryption keys
        </Button>
      }
    >
      <Split
        side={
          <Card label="Mount">
            <KeyValue
              rows={[
                ["Name", mount.data.name],
                ["Description", mount.data.description],
                ["Organization", mount.data.organization_id ? <Mono short={20}>{mount.data.organization_id}</Mono> : null],
                ["Workspace", mount.data.workspace_id ? <Mono short={20}>{mount.data.workspace_id}</Mono> : null],
                ["API", <><code>POST /v1/put_secret</code>, <code>GET /v1/read_secret</code>, <code>GET /v1/secret_metadata</code></>],
              ]}
            />
            <p className="muted mb-0 text-[12.5px]">
              <Link to="/secret-mounts">All secret mounts</Link>
            </p>
          </Card>
        }
      >
        <form className="flex flex-wrap items-end gap-2" onSubmit={submit} key={name}>
          <input type="hidden" name="mount_id" defaultValue={mountId} />
          <label className="field m-0 min-w-0 flex-1 basis-[14rem]">
            <span className="field-label">Secret name</span>
            <input name="name" defaultValue={name} required placeholder="database/password" />
          </label>
          <Button type="submit" variant="primary">
            <Search size={14} aria-hidden /> Inspect history
          </Button>
        </form>
        {!name ? (
          <Empty icon={History} title="Inspect a secret">
            Enter a secret name to inspect its metadata.
          </Empty>
        ) : metadata.isError ? (
          <ErrorBlock error={metadata.error} retry={() => void metadata.refetch()} what="the secret's metadata" />
        ) : !m ? (
          <Skeleton />
        ) : (
          <>
            <StatRow cols={3}>
              <Stat label="Current version" value={m.current_version ?? "—"} />
              <Stat label="Versions" value={versions.length} />
              <Stat label="Destroyed" value={versions.filter((v) => v.destroyed_at).length} sub="their data is gone" />
            </StatRow>
            <Card label="Secret" title={<code>{m.name}</code>}>
              <KeyValue
                rows={[
                  ["Current version", m.current_version],
                  ["Custom metadata", <code>{JSON.stringify(m.custom_metadata ?? {})}</code>],
                ]}
              />
            </Card>
            <Card label="Version history" padded={false}>
              <DataTable
                rows={versions}
                rowKey={(v) => String(v.version)}
                empty={
                  <div className="p-4">
                    <PanelEmpty>No versions.</PanelEmpty>
                  </div>
                }
                initialSort={{ key: "version", dir: "descending" }}
                columns={[
                  { key: "version", label: "Version", numeric: true, sort: (v) => Number(v.version), render: (v) => <span className="font-mono">{v.version}</span> },
                  { key: "state", label: "State", render: (v) => <Status value={state(v)} tone={state(v) === "active" ? "success" : state(v) === "deleted" ? "warning" : "danger"} /> },
                  { key: "created", label: "Created", render: (v) => <When value={v.created_at} /> },
                  { key: "deleted", label: "Deleted", render: (v) => <When value={v.deleted_at} /> },
                  { key: "destroyed", label: "Destroyed", render: (v) => <When value={v.destroyed_at} /> },
                ]}
              />
            </Card>
          </>
        )}
      </Split>
    </Page>
  );
}

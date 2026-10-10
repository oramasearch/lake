/** Encryption Keys (`/encryption-keys?mount_id=`): a mount's named, versioned keys, never their
 * material (web_ui_encryption_keys, serve_encryption_keys_page). */
import { Link, Navigate } from "react-router-dom";
import { useList, useRead, type Row } from "@/lib/api";
import { DataTable, ErrorBlock, Page, Skeleton } from "@/lib/ui";
import { PanelEmpty, When, useMetaTrail, useParam } from "@/pages/organizations/shared";

export default function EncryptionKeysPage() {
  const mountId = useParam("mount_id");
  const mount = useRead<Row>(mountId ? "get_secret_mount" : null, { mount_id: mountId });
  const keys = useList<Row>(mountId ? "list_encryption_keys" : null, { mount_id: mountId, limit: 500 }, { pick: "items" });
  useMetaTrail(keys.data ? `${keys.data.length} keys` : null, "Encryption Keys");
  if (!mountId) return <Navigate to="/secret-mounts" replace />;
  if (mount.isError) {
    return (
      <Page eyebrow="encryption service" title="Encryption Keys">
        <ErrorBlock error={mount.error} retry={() => void mount.refetch()} what="the secret mount" />
      </Page>
    );
  }
  if (!mount.data) {
    return (
      <Page eyebrow="encryption service" title="Encryption Keys">
        <Skeleton />
      </Page>
    );
  }
  return (
    <Page eyebrow="encryption service" title={`${mount.data.name} Keys`} sub="Named, versioned keys for encryption, signing, HMAC and derivation. Key material is never returned.">
      <DataTable
        q={keys}
        what="encryption keys"
        rows={keys.data}
        rowKey={(k) => k.id}
        empty={<PanelEmpty>No keys in this mount yet.</PanelEmpty>}
        columns={[
          { key: "name", label: "Name", render: (k) => <code>{k.name}</code> },
          { key: "version", label: "Current version", render: (k) => k.current_version },
          { key: "derived", label: "Derived from", render: (k) => <code>{k.derived_from || "—"}</code> },
          { key: "created", label: "Created", render: (k) => <When value={k.created_at} /> },
          { key: "updated", label: "Updated", render: (k) => <When value={k.updated_at} /> },
        ]}
      />
      <p className="sub">
        <Link to={`/secrets?mount_id=${encodeURIComponent(mountId)}`}>Static secrets</Link> · API: <code>POST /v1/encrypt</code>, <code>POST /v1/decrypt</code>, <code>POST /v1/rewrap</code>.
      </p>
    </Page>
  );
}

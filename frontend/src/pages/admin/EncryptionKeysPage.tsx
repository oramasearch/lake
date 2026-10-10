/** Encryption Keys (`/encryption-keys?mount_id=`): a mount's named, versioned keys, never their
 * material (web_ui_encryption_keys, serve_encryption_keys_page). */
import { useState } from "react";
import { Navigate } from "react-router-dom";
import { KeyRound, Lock } from "lucide-react";
import { useList, useRead, type Row } from "@/lib/api";
import { Button, DataTable, Empty, ErrorBlock, Mono, Page, SearchInput, Skeleton, Stat, StatRow, Toolbar } from "@/lib/ui";
import { NameCell, PanelEmpty, When, matches, useMetaTrail, useParam } from "@/pages/organizations/shared";

export default function EncryptionKeysPage() {
  const mountId = useParam("mount_id");
  const mount = useRead<Row>(mountId ? "get_secret_mount" : null, { mount_id: mountId });
  const keys = useList<Row>(mountId ? "list_encryption_keys" : null, { mount_id: mountId, limit: 500 }, { pick: "items" });
  const [search, setSearch] = useState("");
  useMetaTrail(keys.data ? `${keys.data.length} keys` : null, "Encryption Keys");
  if (!mountId) return <Navigate to="/secret-mounts" replace />;
  if (mount.isError) {
    return (
      <Page eyebrow="Organization" title="Encryption Keys">
        <ErrorBlock error={mount.error} retry={() => void mount.refetch()} what="the secret mount" />
      </Page>
    );
  }
  if (!mount.data) {
    return (
      <Page eyebrow="Organization" title="Encryption Keys">
        <Skeleton lines={5} />
      </Page>
    );
  }
  const rows = keys.data?.filter((k) => matches(search, k.name, k.derived_from));
  return (
    <Page
      eyebrow="Organization"
      title={`${mount.data.name} Keys`}
      sub="Named, versioned keys for encryption, signing, HMAC and derivation. Key material is never returned."
      actions={
        <Button to={`/secrets?mount_id=${encodeURIComponent(mountId)}`}>
          <Lock size={14} aria-hidden /> Static secrets
        </Button>
      }
    >
      {keys.data && keys.data.length > 0 && (
        <StatRow cols={2}>
          <Stat label="Keys" value={keys.data.length} icon={KeyRound} />
          <Stat label="Derived" value={keys.data.filter((k) => k.derived_from).length} sub="children of another key" />
        </StatRow>
      )}
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search keys…" />
      </Toolbar>
      <DataTable
        q={keys}
        what="encryption keys"
        rows={rows}
        rowKey={(k) => k.id}
        empty={
          keys.data?.length ? (
            <PanelEmpty title="No key matches">Clear the search to see every key.</PanelEmpty>
          ) : (
            <Empty icon={KeyRound} title="No keys yet">
              No keys in this mount yet. Create one with <code>POST /v1/create_encryption_key</code>.
            </Empty>
          )
        }
        columns={[
          { key: "name", label: "Key", sort: (k) => k.name, render: (k) => <NameCell name={<code>{k.name}</code>} sub={k.derived_from ? <>derived from <Mono short={20}>{k.derived_from}</Mono></> : undefined} /> },
          { key: "version", label: "Current version", numeric: true, sort: (k) => Number(k.current_version), render: (k) => <span className="font-mono">{k.current_version}</span> },
          { key: "created", label: "Created", sort: (k) => k.created_at, render: (k) => <When value={k.created_at} /> },
          { key: "updated", label: "Updated", sort: (k) => k.updated_at, render: (k) => <When value={k.updated_at} /> },
        ]}
      />
      <p className="muted mt-3 text-[12.5px]">
        API: <code>POST /v1/encrypt</code>, <code>POST /v1/decrypt</code>, <code>POST /v1/rewrap</code>.
      </p>
    </Page>
  );
}

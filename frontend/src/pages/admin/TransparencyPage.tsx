/** Transparency Monitor (`/transparency?identity=`): public. Verifies the current checkpoint and
 * scans its tiles for attestations naming one TAAP identity (web_ui_transparency,
 * serve_transparency_page). */
import { useRead, type Row } from "@/lib/api";
import { DataTable, ErrorBlock, Page, Skeleton } from "@/lib/ui";
import { PanelEmpty, useMetaTrail, useParam, useQuerySubmit } from "@/pages/organizations/shared";

export default function TransparencyPage() {
  const identity = useParam("identity");
  const scan = useRead<Row>(identity ? "transparency_identity_scan" : null, { identity });
  const submit = useQuerySubmit("/transparency");
  useMetaTrail("public monitor", "Transparency");
  const s = scan.data;
  const matches: Row[] = s?.matches ?? [];
  return (
    <Page eyebrow="public evidence" title="Transparency Monitor" sub="Scan checkpoint-bound bulk tiles for every TAAP attestation that names an identity.">
      <form className="search" onSubmit={submit} key={identity}>
        <input name="identity" aria-label="TAAP identity" placeholder="taap:1:…" defaultValue={identity} />
        <button type="submit">Scan</button>
      </form>
      {!identity ? (
        <PanelEmpty>Enter a TAAP identity to verify the current checkpoint and scan its immutable tiles.</PanelEmpty>
      ) : scan.isError ? (
        <ErrorBlock error={scan.error} retry={() => void scan.refetch()} what="the scan" />
      ) : !s ? (
        <Skeleton />
      ) : (
        <>
          <div className="panel">
            <dl>
              <dt>Checkpoint</dt>
              <dd>{s.tree_size ?? 0} entries</dd>
              <dt>Root</dt>
              <dd>
                <code>{s.root_hash}</code>
              </dd>
              <dt>Tiles read</dt>
              <dd>{s.tiles_scanned ?? 0}</dd>
              <dt>Scan</dt>
              <dd>{Number(s.duration_ms ?? 0).toFixed(3)} ms; verified</dd>
              <dt>Matches</dt>
              <dd>{matches.length}</dd>
            </dl>
          </div>
          <DataTable
            rows={matches}
            rowKey={(m) => String(m.index)}
            empty={<PanelEmpty>No attestations name this identity.</PanelEmpty>}
            columns={[
              { key: "index", label: "Index", render: (m) => m.index ?? 0 },
              { key: "profile", label: "Profile", render: (m) => <code>{m.profile}</code> },
              { key: "issuer", label: "Issuer", render: (m) => <code>{m.issuer}</code> },
              { key: "subject", label: "Subject", render: (m) => <code>{m.subject}</code> },
            ]}
          />
        </>
      )}
      <p className="sub">
        API: <code>GET /v1/transparency_identity_scan</code>, <code>GET /v1/transparency_tile</code>, and <code>GET /v1/transparency_checkpoint</code>.
      </p>
    </Page>
  );
}

// SPDX-License-Identifier: MIT

/** Transparency Monitor (`/transparency?identity=&start=`): public. Verifies the current
 * checkpoint and scans its tiles for attestations naming one TAAP identity, 100 a page from entry
 * `start` (web_ui_transparency, serve_transparency_page). */
import { Fingerprint, Search, ShieldCheck } from "lucide-react";
import { useRead, type Row } from "@/lib/api";
import { Button, Card, CopyBlock, DataTable, Empty, ErrorBlock, Mono, Page, Skeleton, Stat, StatRow, Status } from "@/lib/ui";
import { PanelEmpty, useMetaTrail, useParam, useQuerySubmit } from "@/pages/organizations/shared";

export default function TransparencyPage() {
  const identity = useParam("identity");
  const start = useParam("start");
  const scan = useRead<Row>(identity ? "transparency_identity_scan" : null, start ? { identity, start_index: Number(start) } : { identity });
  const submit = useQuerySubmit("/transparency");
  useMetaTrail("public monitor", "Transparency");
  const s = scan.data;
  const matches: Row[] = s?.matches ?? [];
  return (
    <Page eyebrow="Public evidence" title="Transparency Monitor" sub="Scan checkpoint-bound bulk tiles for every TAAP attestation that names an identity.">
      <form className="mb-4 flex flex-wrap items-center gap-2" onSubmit={submit} key={identity}>
        <div className="relative min-w-0 flex-1 basis-[18rem]">
          <Fingerprint size={15} className="pointer-events-none absolute top-1/2 left-3 -translate-y-1/2 text-ink-3" aria-hidden />
          <input className="search-input w-full !pl-9" name="identity" aria-label="TAAP identity" placeholder="taap:1:…" defaultValue={identity} />
        </div>
        <Button type="submit" variant="primary">
          <Search size={14} aria-hidden /> Scan
        </Button>
      </form>
      {!identity ? (
        <Empty icon={ShieldCheck} title="Scan for an identity">
          Enter a TAAP identity to verify the current checkpoint and scan its immutable tiles.
        </Empty>
      ) : scan.isError ? (
        <ErrorBlock error={scan.error} retry={() => void scan.refetch()} what="the scan" />
      ) : !s ? (
        <Skeleton lines={5} />
      ) : (
        <>
          <StatRow>
            <Stat label="Checkpoint" value={s.tree_size ?? 0} sub="entries" icon={ShieldCheck} />
            <Stat label="Tiles read" value={s.tiles_scanned ?? 0} />
            <Stat label="Scan" value={`${Number(s.duration_ms ?? 0).toFixed(3)} ms`} sub={<Status value="verified" tone="success" />} />
            <Stat label="Matches" value={matches.length} icon={Fingerprint} sub={s.next_index != null || start ? "on this page" : undefined} />
          </StatRow>
          <Card label="Checkpoint root" className="mb-4">
            <CopyBlock value={String(s.root_hash ?? "")} />
          </Card>
          <Card label="Attestations naming this identity" padded={false}>
            <DataTable
              rows={matches}
              rowKey={(m) => String(m.index)}
              empty={
                <div className="p-4">
                  <PanelEmpty>No attestations name this identity.</PanelEmpty>
                </div>
              }
              columns={[
                { key: "index", label: "Index", numeric: true, sort: (m) => Number(m.index ?? 0), render: (m) => <span className="font-mono">{m.index ?? 0}</span> },
                { key: "profile", label: "Profile", render: (m) => <code>{m.profile}</code> },
                { key: "issuer", label: "Issuer", render: (m) => <Mono short={32}>{m.issuer}</Mono> },
                { key: "subject", label: "Subject", render: (m) => <Mono short={32}>{m.subject}</Mono> },
              ]}
            />
          </Card>
          {s.next_index != null && (
            <nav className="pagination mt-3 flex gap-2" aria-label="Scan pages">
              <Button to={`/transparency?${new URLSearchParams({ identity, start: String(s.next_index) })}`}>Next</Button>
            </nav>
          )}
        </>
      )}
      <p className="muted mt-3 text-[12.5px]">
        API: <code>GET /v1/transparency_identity_scan</code>, <code>GET /v1/transparency_tile</code>, and <code>GET /v1/transparency_checkpoint</code>.
      </p>
    </Page>
  );
}

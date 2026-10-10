/** Sessions and devices (`/sessions`): every session and token of the account, and every device,
 * each revocable. The session this browser is using leads the page; revoking it signs this browser
 * out. */
import { useState } from "react";
import { Laptop, MonitorSmartphone } from "lucide-react";
import { act, useList } from "@/lib/api";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { useTrail } from "@/lib/trail";
import { ConfirmButton } from "@/lib/overlay";
import { Button, Card, Chip, DataTable, Empty, ErrorBlock, FilterChips, KeyValue, Notice, Page, SearchInput, Section, Skeleton, Toolbar } from "@/lib/ui";
import { dateTime } from "@/lib/format";
import { PanelEmpty, When, countBy, matches } from "@/pages/organizations/shared";
import { ActiveBadge, type DeviceRow, type SessionRow, sessionKind } from "./labels";
import { leaveFor, messageOf, NO_WORKSPACE } from "./signIn";

export default function SessionsPage() {
  useTrail([{ label: "Account", to: "/settings" }, { label: "Sessions" }]);
  const { viewer } = useSession();
  const toast = useToast();
  const sessions = useList<SessionRow>("list_sessions", undefined, { ...NO_WORKSPACE, pick: "items" });
  const devices = useList<DeviceRow>("list_devices", undefined, { ...NO_WORKSPACE, pick: "items" });
  const [error, setError] = useState<string | null>(null);
  const [pending, setPending] = useState<string | null>(null);
  const [search, setSearch] = useState("");
  const [kind, setKind] = useState<string | null>(null);
  const [state, setState] = useState<"active" | "revoked" | null>(null);

  const revokeSession = async (row: SessionRow) => {
    setPending(row.id);
    setError(null);
    try {
      await act("revoke_session", { id: row.id }, NO_WORKSPACE);
      if (row.id === viewer.session_id) {
        leaveFor("/sign-in");
        return;
      }
      toast("Session revoked");
    } catch (e) {
      setError(messageOf(e));
    } finally {
      setPending(null);
    }
  };
  const revokeDevice = async (row: DeviceRow) => {
    setPending(row.id);
    setError(null);
    try {
      await act("revoke_device", { id: row.id }, NO_WORKSPACE);
      toast("Device revoked");
    } catch (e) {
      setError(messageOf(e));
    } finally {
      setPending(null);
    }
  };

  const current = sessions.data?.find((row) => row.id === viewer.session_id);
  const others = sessions.data?.filter(
    (row) =>
      row.id !== viewer.session_id &&
      matches(search, row.name, row.kind, ...(row.scopes ?? [])) &&
      (!kind || row.kind === kind) &&
      (!state || (state === "revoked") === !!row.revoked_at),
  );
  const activeCount = (sessions.data ?? []).filter((r) => !r.revoked_at).length;

  return (
    <Page eyebrow="Account" title="Sessions and Devices" sub="Revocation takes effect on the next request.">
      {error && <Notice tone="danger">{error}</Notice>}
      {current && (
        <Card
          tone="active"
          label="This session"
          title={current.name || sessionKind(current.kind)}
          actions={<ActiveBadge revokedAt={current.revoked_at} />}
          className="mb-4"
          footer={
            <ConfirmButton
              title="Revoke this session?"
              confirm="This browser is using it. You will be signed out on your next request."
              confirmLabel="Revoke"
              variant="danger"
              disabled={!!current.revoked_at}
              onConfirm={() => revokeSession(current)}
            >
              Revoke and sign out
            </ConfirmButton>
          }
        >
          <KeyValue
            rows={[
              ["Kind", sessionKind(current.kind)],
              ["Started", current.created_at ? <When value={current.created_at} /> : null],
              ["Expires", dateTime(current.expires_at)],
            ]}
          />
        </Card>
      )}
      <Section title="Sessions" sub={sessions.data ? `${activeCount} active of ${sessions.data.length}.` : undefined}>
        <Toolbar>
          <SearchInput value={search} onChange={setSearch} placeholder="Search sessions…" />
          <FilterChips options={countBy(sessions.data, (r) => r.kind, sessionKind)} value={kind} onChange={setKind} allLabel="Any kind" />
          <FilterChips
            options={[
              { id: "active" as const, label: "Active" },
              { id: "revoked" as const, label: "Revoked" },
            ]}
            value={state}
            onChange={setState}
            allLabel="Any state"
          />
        </Toolbar>
        <DataTable<SessionRow>
          q={sessions}
          what="sessions"
          rows={others}
          rowKey={(row) => row.id}
          empty={<PanelEmpty title="No other sessions">{sessions.data && sessions.data.length > 1 ? "Clear the search or the filters to see more." : "This browser's session is the only one."}</PanelEmpty>}
          columns={[
            {
              key: "name",
              label: "Session",
              sort: (row) => row.name,
              render: (row) => (
                <div className="min-w-0">
                  <div className="font-semibold text-ink">{row.name || "—"}</div>
                  <div className="text-[12px] text-ink-3">{sessionKind(row.kind)}</div>
                </div>
              ),
            },
            {
              key: "scopes",
              label: "Scopes",
              render: (row) =>
                (row.scopes ?? []).length ? (
                  <span className="flex max-w-[22rem] flex-wrap gap-1">
                    {(row.scopes ?? []).slice(0, 4).map((s) => (
                      <Chip key={s}>{s}</Chip>
                    ))}
                    {(row.scopes ?? []).length > 4 && <Chip>+{(row.scopes ?? []).length - 4}</Chip>}
                  </span>
                ) : (
                  <span className="muted">—</span>
                ),
            },
            { key: "expires", label: "Expires", sort: (row) => row.expires_at ?? "", render: (row) => (row.expires_at ? <span title={dateTime(row.expires_at)}><When value={row.expires_at} /></span> : dateTime(row.expires_at)) },
            { key: "status", label: "Status", render: (row) => <ActiveBadge revokedAt={row.revoked_at} /> },
            {
              key: "revoke",
              label: <span className="sr-only">Actions</span>,
              render: (row) => (
                <Button onClick={() => void revokeSession(row)} loading={pending === row.id} disabled={!!row.revoked_at}>
                  Revoke
                </Button>
              ),
            },
          ]}
        />
      </Section>
      <Section title="Devices">
        {devices.isLoading ? (
          <Skeleton lines={2} />
        ) : devices.isError ? (
          <ErrorBlock error={devices.error} retry={() => void devices.refetch()} what="devices" />
        ) : !devices.data?.length ? (
          <Empty icon={MonitorSmartphone} title="No devices">
            A device appears here when you sign in with MFA or register a passkey.
          </Empty>
        ) : (
          <div className="grid grid-cols-1 gap-3 md:grid-cols-2 xl:grid-cols-3">
            {devices.data.map((row) => (
              <Card
                key={row.id}
                tone={row.revoked_at ? "quiet" : "default"}
                label="Device"
                title={
                  <span className="inline-flex items-center gap-1.5">
                    <Laptop size={15} className="text-ink-3" aria-hidden />
                    {row.name || "—"}
                  </span>
                }
                actions={<ActiveBadge revokedAt={row.revoked_at} />}
                footer={
                  <Button onClick={() => void revokeDevice(row)} loading={pending === row.id} disabled={!!row.revoked_at}>
                    Revoke
                  </Button>
                }
              >
                <span className="text-[12.5px] text-ink-2" title={dateTime(row.created_at)}>
                  Added <When value={row.created_at} />
                </span>
              </Card>
            ))}
          </div>
        )}
      </Section>
    </Page>
  );
}

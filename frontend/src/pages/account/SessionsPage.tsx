/** Sessions and devices (`/sessions`): every session and token of the account, and every device,
 * each revocable. The session this browser is using is marked; revoking it signs this browser out. */
import { useState } from "react";
import { act, useList } from "@/lib/api";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { useTrail } from "@/lib/trail";
import { ConfirmButton } from "@/lib/overlay";
import { Badge, Button, DataTable, Notice, Page, Section } from "@/lib/ui";
import { dateTime } from "@/lib/format";
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

  return (
    <Page eyebrow="identity" title="Sessions and Devices" sub="Revocation takes effect on the next request.">
      {error && <Notice tone="danger">{error}</Notice>}
      <Section title="Sessions">
        <DataTable<SessionRow>
          q={sessions}
          what="sessions"
          rows={sessions.data}
          rowKey={(row) => row.id}
          empty="No sessions."
          columns={[
            { key: "kind", label: "Kind", render: (row) => sessionKind(row.kind) },
            {
              key: "name",
              label: "Name",
              render: (row) => (
                <>
                  {row.name || "—"} {row.id === viewer.session_id && <Badge tone="info">This session</Badge>}
                </>
              ),
            },
            { key: "expires", label: "Expires", render: (row) => dateTime(row.expires_at) },
            { key: "status", label: "Status", render: (row) => <ActiveBadge revokedAt={row.revoked_at} /> },
            {
              key: "revoke",
              label: <span className="sr-only">Actions</span>,
              render: (row) =>
                row.id === viewer.session_id ? (
                  <ConfirmButton
                    title="Revoke this session?"
                    confirm="This browser is using it. You will be signed out on your next request."
                    confirmLabel="Revoke"
                    variant="danger"
                    disabled={!!row.revoked_at}
                    onConfirm={() => revokeSession(row)}
                  >
                    Revoke
                  </ConfirmButton>
                ) : (
                  <Button onClick={() => void revokeSession(row)} loading={pending === row.id} disabled={!!row.revoked_at}>
                    Revoke
                  </Button>
                ),
            },
          ]}
        />
      </Section>
      <Section title="Devices">
        <DataTable<DeviceRow>
          q={devices}
          what="devices"
          rows={devices.data}
          rowKey={(row) => row.id}
          empty="No devices."
          columns={[
            { key: "name", label: "Name", render: (row) => row.name || "—" },
            { key: "created", label: "Created", render: (row) => dateTime(row.created_at) },
            { key: "status", label: "Status", render: (row) => <ActiveBadge revokedAt={row.revoked_at} /> },
            {
              key: "revoke",
              label: <span className="sr-only">Actions</span>,
              render: (row) => (
                <Button onClick={() => void revokeDevice(row)} loading={pending === row.id} disabled={!!row.revoked_at}>
                  Revoke
                </Button>
              ),
            },
          ]}
        />
      </Section>
    </Page>
  );
}

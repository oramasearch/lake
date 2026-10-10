/** Personal settings (`/settings?tab=`): Profile, Sessions and devices, Passkeys, Tokens and
 * Notifications, each a tab of cards. An unknown tab shows Notifications, as the server page did. */
import { useSearchParams } from "react-router-dom";
import { Bell, Fingerprint, KeyRound, MonitorSmartphone, ShieldCheck } from "lucide-react";
import { useList } from "@/lib/api";
import { useSession } from "@/lib/session";
import { useTrail } from "@/lib/trail";
import { Button, Card, Empty, ErrorBlock, KeyValue, Page, Skeleton, Stat, StatRow, Status } from "@/lib/ui";
import { QueryTabs } from "@/pages/organizations/shared";
import { NO_WORKSPACE } from "./signIn";
import type { DeviceRow, SessionRow } from "./labels";

const TABS = [
  { key: "profile", label: "Profile" },
  { key: "sessions", label: "Sessions and devices" },
  { key: "passkeys", label: "Passkeys" },
  { key: "tokens", label: "Tokens" },
  { key: "notifications", label: "Notifications" },
] as const;

export default function SettingsPage() {
  const [params] = useSearchParams();
  const selected = params.get("tab") || "profile";
  const known = TABS.some((tab) => tab.key === selected);
  const label = TABS.find((tab) => tab.key === selected)?.label ?? "Notifications";
  useTrail([{ label: "Account", to: "/settings" }, { label }], "Settings");
  return (
    <Page eyebrow="Account" title="Settings" sub="Your profile, how you sign in, and the credentials you hold.">
      <QueryTabs
        label="Personal settings"
        tabs={TABS.map((tab) => ({ to: `/settings?tab=${tab.key}`, label: tab.label, active: known ? selected === tab.key : tab.key === "notifications" }))}
      />
      <Panel selected={selected} />
    </Page>
  );
}

function Panel({ selected }: { selected: string }) {
  const { viewer } = useSession();
  switch (selected) {
    case "profile":
      return (
        <div className="grid grid-cols-1 gap-3 lg:grid-cols-2">
          <Card label="Profile" title={viewer.display_name || viewer.handle || "Your account"}>
            <KeyValue
              rows={[
                ["Identity", viewer.taap_id ? <code>{viewer.taap_id}</code> : "—"],
                ["Handle", viewer.handle ? <code>{viewer.handle}</code> : null],
                ["Email", viewer.email],
              ]}
            />
          </Card>
          <Card label="Multi-factor authentication" actions={<Status value={viewer.mfa_enrolled ? "enabled" : "not enrolled"} tone={viewer.mfa_enrolled ? "success" : "warning"} />}>
            <p className="mt-0 text-[13px] text-ink-2">A TOTP authenticator for password sign-in.</p>
            <Button to="/mfa">
              <ShieldCheck size={14} aria-hidden /> Manage MFA
            </Button>
          </Card>
        </div>
      );
    case "sessions":
      return <SessionsSummary />;
    case "passkeys":
      return (
        <Card label="Passkeys" title="Sign in with a passkey">
          <p className="mt-0 text-[13px] text-ink-2">Registered passkeys appear as devices on Sessions.</p>
          <Button variant="primary" to="/passkeys">
            <Fingerprint size={14} aria-hidden /> Manage passkeys
          </Button>
        </Card>
      );
    case "tokens":
      return (
        <Card label="Tokens" title="API tokens">
          <p className="mt-0 text-[13px] text-ink-2">Named, scoped tokens for the API, MCP and Liquid push and pull.</p>
          <Button variant="primary" to="/tokens">
            <KeyRound size={14} aria-hidden /> Create or revoke tokens
          </Button>
        </Card>
      );
    default:
      return (
        <Empty icon={Bell} title="Notifications">
          No notification delivery preferences are configured.
        </Empty>
      );
  }
}

function SessionsSummary() {
  const sessions = useList<SessionRow>("list_sessions", undefined, { ...NO_WORKSPACE, pick: "items" });
  const devices = useList<DeviceRow>("list_devices", undefined, { ...NO_WORKSPACE, pick: "items" });
  const failed = sessions.isError ? sessions : devices.isError ? devices : null;
  if (sessions.isLoading || devices.isLoading) return <Skeleton lines={2} />;
  if (failed) return <ErrorBlock error={failed.error} retry={() => void failed.refetch()} what="sessions and devices" />;
  const live = (rows: { revoked_at?: string | null }[] | undefined) => (rows ?? []).filter((r) => !r.revoked_at).length;
  return (
    <>
      <StatRow cols={2}>
        <Stat label="Sessions" value={sessions.data?.length ?? 0} icon={KeyRound} sub={`${live(sessions.data)} active`} to="/sessions" />
        <Stat label="Devices" value={devices.data?.length ?? 0} icon={MonitorSmartphone} sub={`${live(devices.data)} active`} to="/sessions" />
      </StatRow>
      <Button variant="primary" to="/sessions">
        Manage sessions and devices
      </Button>
    </>
  );
}

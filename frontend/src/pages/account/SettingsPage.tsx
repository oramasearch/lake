// SPDX-License-Identifier: MIT

/** Personal settings (`/settings?tab=`): Profile, Identity, Sessions and devices, Passkeys, Tokens
 * and Notifications, each a tab of cards. Identity is which key the account signs with (lake#762). An unknown tab shows Notifications, as the server page did.
 * Profile ends with Delete account (lake#810, docs/ui/settings.md#deleting-your-account). */
import { Link, useSearchParams } from "react-router-dom";
import { Fingerprint, KeyRound, MonitorSmartphone, ShieldCheck, Trash2 } from "lucide-react";
import { signOut } from "@/components/Shell";
import { type Row, useList } from "@/lib/api";
import { accountBlockers, accountLines, accountTyped } from "@/lib/deletion";
import { DeletionButton } from "@/lib/deletionDialog";
import { useSession } from "@/lib/session";
import { useTrail } from "@/lib/trail";
import { Button, Card, ErrorBlock, KeyValue, Page, Skeleton, Stat, StatRow, Status } from "@/lib/ui";
import { QueryTabs } from "@/pages/organizations/shared";
import { IdentityPanel } from "./IdentityPanel";
import { NO_WORKSPACE } from "./signIn";
import { NotificationsList } from "@/pages/apps/repair";
import type { DeviceRow, SessionRow } from "./labels";

const TABS = [
  { key: "profile", label: "Profile" },
  { key: "identity", label: "Identity" },
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
                ["Identity", viewer.taap_id ? <Link to="/settings?tab=identity"><code>{viewer.taap_id}</code></Link> : "—"],
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
          <Card label="Delete account" title="Delete your account">
            <p className="mt-0 text-[13px] text-ink-2">
              Deletes your workspaces and the organizations you alone own, signs you out everywhere and erases your email and name. It cannot be undone. Sign in again first if you
              signed in more than 15 minutes ago.
            </p>
            <DeletionButton
              variant="danger"
              capability="delete_account"
              input={{}}
              workspace={null}
              title="Delete your account"
              confirmLabel="Delete account"
              intro="Deleting your account removes everything below. It cannot be undone."
              typed={accountTyped}
              sendConfirm
              lines={accountLines}
              blocked={(plan) => (accountBlockers(plan).length > 0 ? <OrganizationsInTheWay plan={plan} /> : null)}
              done="Your account is deleted"
              onDone={() => signOut()}
            >
              <Trash2 size={14} aria-hidden /> Delete account
            </DeletionButton>
          </Card>
        </div>
      );
    case "identity":
      return <IdentityPanel />;
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
      return <WorkspaceNotifications />;
  }
}

/** What Lake has told you about the current workspace's resources (lake#1207): failed apps,
 * failed builds and auto-fix that stopped, kept in the workspace. */
function WorkspaceNotifications() {
  const { space, workspaceId } = useSession();
  return (
    <Card label="Notifications" title="This workspace">
      <NotificationsList customerId={space?.id ?? ""} workspaceId={workspaceId} />
    </Card>
  );
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

/** The organizations other people belong to that you alone own: they need another owner first. */
function OrganizationsInTheWay({ plan }: { plan: Row }) {
  const organizations = accountBlockers(plan);
  if (organizations.length === 0) return null;
  return (
    <>
      <strong className="block">Give {organizations.length === 1 ? "this organization" : "these organizations"} another owner first</strong>
      You are the only owner, and other people belong to {organizations.length === 1 ? "it" : "them"}. Make another member an owner, or delete the organization from its page.
      <ul className="mt-2 mb-0 space-y-1 pl-4">
        {organizations.map((organization) => (
          <li key={organization.id}>
            <Link to={`/organization?id=${encodeURIComponent(organization.id)}`}>{organization.name}</Link>
            <span className="text-ink-2">
              {" "}
              ({organization.members} {organization.members === 1 ? "other member" : "other members"})
            </span>
          </li>
        ))}
      </ul>
    </>
  );
}

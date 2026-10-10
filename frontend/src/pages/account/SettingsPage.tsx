/** Personal settings (`/settings?tab=`): Profile, Sessions and devices, Passkeys, Tokens and
 * Notifications. An unknown tab shows Notifications, as the server page did. */
import { Link, useSearchParams } from "react-router-dom";
import { useList } from "@/lib/api";
import { useSession } from "@/lib/session";
import { useTrail } from "@/lib/trail";
import { Page, Skeleton, ErrorBlock } from "@/lib/ui";
import { plural } from "@/lib/format";
import { NO_WORKSPACE } from "./signIn";

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
  const label = TABS.find((tab) => tab.key === selected)?.label ?? "Notifications";
  useTrail([{ label: "Account", to: "/settings" }, { label }], "Settings");
  return (
    <Page eyebrow="personal settings" title="Settings">
      <nav className="tabs" aria-label="Personal settings">
        {TABS.map((tab) => (
          <Link key={tab.key} to={`/settings?tab=${tab.key}`} aria-current={selected === tab.key ? "page" : undefined}>
            {tab.label}
          </Link>
        ))}
      </nav>
      <section className="panel">
        <Panel selected={selected} />
      </section>
    </Page>
  );
}

function Panel({ selected }: { selected: string }) {
  const { viewer } = useSession();
  switch (selected) {
    case "profile":
      return (
        <>
          <h2>Profile</h2>
          <p>
            <code>{viewer.taap_id || "—"}</code>
          </p>
        </>
      );
    case "sessions":
      return <SessionsSummary />;
    case "passkeys":
      return (
        <>
          <h2>Passkeys</h2>
          <p>
            <Link to="/passkeys">Manage passkeys</Link>
          </p>
        </>
      );
    case "tokens":
      return (
        <>
          <h2>Tokens</h2>
          <p>
            <Link to="/tokens">Create or revoke tokens</Link>
          </p>
        </>
      );
    default:
      return (
        <>
          <h2>Notifications</h2>
          <p className="empty">No notification delivery preferences are configured.</p>
        </>
      );
  }
}

function SessionsSummary() {
  const sessions = useList("list_sessions", undefined, { ...NO_WORKSPACE, pick: "items" });
  const devices = useList("list_devices", undefined, { ...NO_WORKSPACE, pick: "items" });
  const failed = sessions.isError ? sessions : devices.isError ? devices : null;
  return (
    <>
      <h2>Sessions and Devices</h2>
      {sessions.isLoading || devices.isLoading ? (
        <Skeleton lines={1} />
      ) : failed ? (
        <ErrorBlock error={failed.error} retry={() => void failed.refetch()} what="sessions and devices" />
      ) : (
        <p>
          {plural(sessions.data?.length ?? 0, "session")} and {plural(devices.data?.length ?? 0, "device")}.
        </p>
      )}
      <p>
        <Link to="/sessions">Manage sessions and devices</Link>
      </p>
    </>
  );
}

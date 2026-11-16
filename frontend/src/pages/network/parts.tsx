// SPDX-License-Identifier: MIT

/** What the Network pages share (lake#729, docs/ui/network.md): the page frame with the section's
 * tabs, the account and workspace every capability takes, the names that replace ids, and the state
 * of a desired row in words. The rows live in Lake's control plane, so the pages do not wait for the
 * workspace to be prepared. */
import { type ReactNode, useMemo } from "react";
import { Link } from "react-router-dom";
import { useRead, useList, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { useTrail } from "@/lib/trail";
import { Notice, Page, Status, Tabs } from "@/lib/ui";
import { useWorkspace, useWsPath } from "@/pages/train/workspace";
import { pendingPoll, NETWORK_PAGES, OVERVIEW_PATH, nameOf, recordState, type Names } from "./network";

export function useScope() {
  const { customerId, workspaceId } = useWorkspace();
  return { customerId, workspaceId, input: { customer_id: customerId, workspace_id: workspaceId } };
}

/** A network read for this workspace (`list_network_*`, `network_rules`, `network_activity`). */
export function useNetworkRead<T = Row>(name: string, extra: Record<string, string | number> = {}, options: { refetchInterval?: number } = {}) {
  const scope = useScope();
  return useRead<T>(name, { ...scope.input, ...extra }, {
    enabled: !!scope.customerId && !!scope.workspaceId,
    // Read again while a row waits for the cluster, so Waiting turns to Ready without a reload.
    refetchInterval: (query) => pendingPoll(query.state.data),
    ...options,
  });
}

/** A Network page: eyebrow, title, the section's tabs and the content. */
export function NetworkPage({ title, sub, actions, children }: { title: string; sub: ReactNode; actions?: ReactNode; children: ReactNode }) {
  const ws = useWsPath();
  useTrail([{ label: "Network", to: ws(OVERVIEW_PATH) }, ...(title === "Overview" ? [] : [{ label: title }])], title === "Overview" ? "Network" : title);
  return (
    <Page eyebrow="Network" title={title === "Overview" ? "Network" : title} sub={sub} actions={actions}>
      <Tabs label="Network" tabs={NETWORK_PAGES.map((p) => ({ to: ws(p.path), label: p.label, end: true }))} />
      {children}
    </Page>
  );
}

/** The ids the pages name: this workspace's apps and services, and the account's people. */
export function useNames(): { names: Names; apps: Row[]; services: Row[]; people: { id: string; label: string }[] } {
  const scope = useScope();
  const { viewer } = useSession();
  const apps = useList<Row>(scope.customerId && scope.workspaceId ? "list_apps" : null, scope.input, { pick: "apps", staleTime: 60_000 });
  const services = useList<Row>(scope.customerId ? "list_services" : null, { customer_id: scope.customerId }, { pick: "services", staleTime: 60_000 });
  const customer = useRead<Row>(scope.customerId ? "get_customer" : null, { customer_id: scope.customerId }, { staleTime: 300_000, retry: false });
  const organizationId = (customer.data?.organization_id as string | null | undefined) ?? null;
  const members = useList<Row>(organizationId ? "list_memberships" : null, { organization_id: organizationId ?? "", limit: 500 }, { pick: "items", staleTime: 300_000, retry: false });
  return useMemo(() => {
    const appRows = (apps.data ?? []).filter((a) => a.desired !== "deleted" && a.desired !== "absent");
    // A service of another workspace of the account is not this workspace's to connect.
    const serviceRows = (services.data ?? []).filter((s) => s.desired !== "absent" && (!s.workspace_id || s.workspace_id === scope.workspaceId));
    const people = new Map<string, string>();
    if (viewer.user_id) people.set(viewer.user_id, "You");
    for (const m of members.data ?? []) {
      if (m.principal_type !== "user" || !m.principal_id) continue;
      if (!people.has(String(m.principal_id))) people.set(String(m.principal_id), String(m.principal_taap_id || m.principal_id));
    }
    const names: Names = {
      app: new Map(appRows.map((a) => [String(a.id), String(a.name || a.id)])),
      service: new Map(serviceRows.map((s) => [String(s.id), String(s.name || s.id)])),
      user: people,
    };
    return { names, apps: appRows, services: serviceRows, people: [...people].map(([id, label]) => ({ id, label })) };
  }, [apps.data, services.data, members.data, viewer.user_id, scope.workspaceId]);
}

/** A desired row's state, with what the cluster said as its title. */
export function RecordState({ record }: { record: Row }) {
  const state = recordState(record);
  return (
    <span title={state.detail}>
      <Status value={state.label} tone={state.tone} />
    </span>
  );
}

/** One end of a path: its name, linked to its page where it has one. */
export function Endpoint({ names, kind, id }: { names: Names; kind: string; id: string }) {
  const ws = useWsPath();
  const label = nameOf(names, kind, id);
  const to = kind === "app" && names.app.has(id) ? `/ship/app?app_id=${encodeURIComponent(id)}` : kind === "service" && names.service.has(id) ? `/services?q=${encodeURIComponent(label)}` : null;
  return (
    <span className="inline-flex min-w-0 items-center gap-1.5">
      <span className="font-mono text-[10.5px] uppercase text-ink-3">{kind}</span>
      {to ? (
        <Link to={kind === "app" ? ws(to) : to} className="font-semibold" title={id}>
          {label}
        </Link>
      ) : (
        <span className="font-semibold text-ink" title={id}>
          {label}
        </span>
      )}
    </span>
  );
}

/** The workspace records Lake does not keep yet (sessions, activity): said, never shown as empty. */
export function NotRecorded({ what, page }: { what: "Tunnel Sessions" | "Network Activity"; page?: Row }) {
  if (page?.available === true) return null;
  return (
    <Notice title={what === "Tunnel Sessions" ? "Tunnel Sessions Are Not Recorded Yet" : "Network Activity Is Not Recorded Yet"}>
      Lake will keep {what.toLowerCase()} in this workspace once the cluster reports them. Until then nothing is listed here, and the rules on the other pages still apply.
    </Notice>
  );
}

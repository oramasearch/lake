// SPDX-License-Identifier: MIT

/** What the application's pages share: the application read first (its name and workspace), the
 * header with the application's name, and the route tabs between its overview, users,
 * organizations and webhook events. */
import type { ReactNode } from "react";
import { useLocation } from "react-router-dom";
import type { UseQueryResult } from "@tanstack/react-query";
import { useRead, type Row } from "@/lib/api";
import { ErrorBlock, Page, Skeleton } from "@/lib/ui";
import { QueryTabs } from "@/pages/organizations/shared";

/** The application, whose `workspace_id` every application list and action needs. */
export function useApplication(id: string) {
  return useRead<Row>("get_application", { id });
}

/** Overview, Users, Organizations and Webhook events, for one application. */
export function ApplicationTabs({ id }: { id: string }) {
  const { pathname } = useLocation();
  const app = encodeURIComponent(id);
  const path = pathname.replace(/^\/w\/[^/]+/, "");
  return (
    <QueryTabs
      label="Application"
      tabs={[
        { to: `/application?id=${app}`, label: "Overview", active: path === "/application" },
        { to: `/application/users?id=${app}`, label: "Users", active: path === "/application/users" },
        { to: `/application/organizations?id=${app}`, label: "Organizations", active: path === "/application/organizations" },
        { to: `/application/events?id=${app}`, label: "Webhook events", active: path === "/application/events" },
      ]}
    />
  );
}

/** The page frame while the application loads or fails; `children` once it is read. */
export function ApplicationFrame({
  application,
  title,
  sub,
  actions,
  children,
}: {
  application: UseQueryResult<Row>;
  /** Kept for the callers' wording; the frame's eyebrow is the group's. */
  eyebrow?: string;
  title: (name: string) => string;
  sub?: ReactNode;
  actions?: (app: Row) => ReactNode;
  children: (app: Row) => ReactNode;
}) {
  if (application.isError) {
    return (
      <Page eyebrow="Administration" title={title("Application")}>
        <ErrorBlock error={application.error} retry={() => void application.refetch()} what="the application" />
      </Page>
    );
  }
  if (!application.data) {
    return (
      <Page eyebrow="Administration" title={title("Application")}>
        <Skeleton lines={5} />
      </Page>
    );
  }
  return (
    <Page eyebrow="Administration" title={title(application.data.name || application.data.id)} sub={sub} actions={actions?.(application.data)}>
      <ApplicationTabs id={application.data.id} />
      {children(application.data)}
    </Page>
  );
}

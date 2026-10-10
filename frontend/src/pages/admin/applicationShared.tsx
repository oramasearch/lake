/** What the application's users, organizations and events pages share: the application read
 * first (its name and workspace), then a list read in that workspace. */
import type { ReactNode } from "react";
import type { UseQueryResult } from "@tanstack/react-query";
import { useRead, type Row } from "@/lib/api";
import { ErrorBlock, Page, Skeleton } from "@/lib/ui";

/** The application, whose `workspace_id` every application list and action needs. */
export function useApplication(id: string) {
  return useRead<Row>("get_application", { id });
}

/** The page frame while the application loads or fails; `children` once it is read. */
export function ApplicationFrame({ application, eyebrow, title, children }: { application: UseQueryResult<Row>; eyebrow: string; title: (name: string) => string; children: (app: Row) => ReactNode }) {
  if (application.isError) {
    return (
      <Page eyebrow={eyebrow} title={title("Application")}>
        <ErrorBlock error={application.error} retry={() => void application.refetch()} what="the application" />
      </Page>
    );
  }
  if (!application.data) {
    return (
      <Page eyebrow={eyebrow} title={title("Application")}>
        <Skeleton />
      </Page>
    );
  }
  return (
    <Page eyebrow={eyebrow} title={title(application.data.name || application.data.id)}>
      {children(application.data)}
    </Page>
  );
}

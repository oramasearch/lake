/** Application users (`/application/users?id=`): the users of one application's pool, each with
 * Suspend (web_ui_application_users, serve_application_users_page,
 * serve_application_user_suspend). */
import { useState } from "react";
import { act, useList, type Row } from "@/lib/api";
import { ConfirmButton } from "@/lib/overlay";
import { DataTable, FilterChips, SearchInput, Status, Toolbar } from "@/lib/ui";
import { NameCell, PanelEmpty, countBy, matches, useMetaTrail, useParam } from "@/pages/organizations/shared";
import { ApplicationFrame, useApplication } from "./applicationShared";

export default function ApplicationUsersPage() {
  const id = useParam("id");
  const application = useApplication(id);
  const workspaceId: string | undefined = application.data?.workspace_id ?? undefined;
  const users = useList<Row>(workspaceId ? "list_application_users" : null, { application_id: id, workspace_id: workspaceId, limit: 500 }, { pick: "items", workspace: null });
  useMetaTrail(users.data ? `${users.data.length} users` : null, "Application Users");
  return (
    <ApplicationFrame application={application} title={(name) => `${name} Users`} sub="The people in this application's user pool.">
      {(app) => <Users applicationId={app.id} workspaceId={app.workspace_id ?? ""} users={users} />}
    </ApplicationFrame>
  );
}

function Users({ applicationId, workspaceId, users }: { applicationId: string; workspaceId: string; users: ReturnType<typeof useList<Row>> }) {
  const [search, setSearch] = useState("");
  const [status, setStatus] = useState<string | null>(null);
  const rows = users.data?.filter((u) => matches(search, u.email, u.display_name, u.user_id) && (!status || u.status === status));
  return (
    <>
      <Toolbar>
        <SearchInput value={search} onChange={setSearch} placeholder="Search users…" />
        <FilterChips options={countBy(users.data, (u) => u.status)} value={status} onChange={setStatus} allLabel="Any status" />
      </Toolbar>
      <DataTable
        q={users}
        what="application users"
        rows={rows}
        rowKey={(u) => u.user_id}
        empty={<PanelEmpty title="No users">{users.data?.length ? "Clear the search or the status filter to see more." : "Users appear here when they sign up through the application's hosted sign-in."}</PanelEmpty>}
        columns={[
          { key: "email", label: "User", sort: (u) => u.email, render: (u) => <NameCell name={u.email} sub={u.display_name} /> },
          { key: "status", label: "Status", render: (u) => <Status value={u.status} tone={u.status === "suspended" ? "danger" : undefined} /> },
          {
            key: "suspend",
            label: <span className="sr-only">Actions</span>,
            // There is no unsuspend: a suspended user has nothing to press.
            render: (u) =>
              u.status === "suspended" ? null : (
                <ConfirmButton
                  title="Suspend the user?"
                  confirm={`${u.email || u.user_id} can no longer sign in to the application. There is no unsuspend.`}
                  confirmLabel="Suspend"
                  variant="danger"
                  onConfirm={() => act("suspend_application_user", { application_id: applicationId, workspace_id: workspaceId, user_id: u.user_id })}
                >
                  Suspend
                </ConfirmButton>
              ),
          },
        ]}
      />
    </>
  );
}

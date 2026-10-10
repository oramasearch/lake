/** Application users (`/application/users?id=`): the users of one application's pool, each with
 * Suspend (web_ui_application_users, serve_application_users_page,
 * serve_application_user_suspend). */
import { useState } from "react";
import { act, useList, type Row } from "@/lib/api";
import { useToast } from "@/lib/toast";
import { Button, DataTable } from "@/lib/ui";
import { useMetaTrail, useParam } from "@/pages/organizations/shared";
import { ApplicationFrame, useApplication } from "./applicationShared";

export default function ApplicationUsersPage() {
  const id = useParam("id");
  const application = useApplication(id);
  const workspaceId: string | undefined = application.data?.workspace_id ?? undefined;
  const users = useList<Row>(workspaceId ? "list_application_users" : null, { application_id: id, workspace_id: workspaceId, limit: 500 }, { pick: "items", workspace: null });
  useMetaTrail(users.data ? `${users.data.length} users` : null, "Application Users");
  return (
    <ApplicationFrame application={application} eyebrow="application users" title={(name) => `${name} Users`}>
      {(app) => <Users applicationId={app.id} workspaceId={app.workspace_id ?? ""} users={users} />}
    </ApplicationFrame>
  );
}

function Users({ applicationId, workspaceId, users }: { applicationId: string; workspaceId: string; users: ReturnType<typeof useList<Row>> }) {
  const toast = useToast();
  const [busy, setBusy] = useState<string | null>(null);
  const suspend = async (user: Row) => {
    setBusy(user.user_id);
    try {
      await act("suspend_application_user", { application_id: applicationId, workspace_id: workspaceId, user_id: user.user_id });
      toast("User suspended");
    } catch (e) {
      toast((e as Error).message, "bad");
    } finally {
      setBusy(null);
    }
  };
  return (
    <DataTable
      q={users}
      what="application users"
      rows={users.data}
      rowKey={(u) => u.user_id}
      columns={[
        { key: "email", label: "Email", render: (u) => u.email },
        { key: "name", label: "Name", render: (u) => u.display_name },
        { key: "status", label: "Status", render: (u) => u.status },
        {
          key: "suspend",
          label: "",
          // There is no unsuspend: a suspended user has nothing to press.
          render: (u) =>
            u.status === "suspended" ? null : (
              <Button loading={busy === u.user_id} onClick={() => void suspend(u)}>
                Suspend
              </Button>
            ),
        },
      ]}
    />
  );
}

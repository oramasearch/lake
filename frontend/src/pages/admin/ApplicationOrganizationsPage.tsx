/** Application organizations (`/application/organizations?id=`): one application's
 * organizations, with Delete and the create form (web_ui_application_organizations and its two
 * POST handlers). */
import { useState } from "react";
import { act, useList, type Row } from "@/lib/api";
import { ConfirmButton } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { Button, DataTable, Field, Form, fieldError } from "@/lib/ui";
import { When, useMetaTrail, useParam } from "@/pages/organizations/shared";
import { ApplicationFrame, useApplication } from "./applicationShared";

export default function ApplicationOrganizationsPage() {
  const id = useParam("id");
  const application = useApplication(id);
  const workspaceId: string | undefined = application.data?.workspace_id ?? undefined;
  const organizations = useList<Row>(workspaceId ? "list_application_organizations" : null, { application_id: id, workspace_id: workspaceId, limit: 500 }, { pick: "items", workspace: null });
  useMetaTrail(organizations.data ? `${organizations.data.length} organizations` : null, "Application Organizations");
  return (
    <ApplicationFrame application={application} eyebrow="application organizations" title={(name) => `${name} Organizations`}>
      {(app) => (
        <>
          <DataTable
            q={organizations}
            what="application organizations"
            rows={organizations.data}
            rowKey={(o) => o.id}
            columns={[
              { key: "name", label: "Name", render: (o) => o.name },
              { key: "taap", label: "TAAP identity", render: (o) => <code>{o.taap_id}</code> },
              { key: "updated", label: "Updated", render: (o) => <When value={o.updated_at} /> },
              {
                key: "delete",
                label: "",
                render: (o) => (
                  <ConfirmButton
                    title="Delete the organization?"
                    confirm={`${o.name} and its memberships are deleted from the application.`}
                    confirmLabel="Delete"
                    variant="danger"
                    onConfirm={() => act("delete_application_organization", { application_id: app.id, workspace_id: app.workspace_id, organization_id: o.id })}
                  >
                    Delete
                  </ConfirmButton>
                ),
              },
            ]}
          />
          <CreateOrganization applicationId={app.id} workspaceId={app.workspace_id ?? ""} />
        </>
      )}
    </ApplicationFrame>
  );
}

function CreateOrganization({ applicationId, workspaceId }: { applicationId: string; workspaceId: string }) {
  const toast = useToast();
  const [name, setName] = useState("");
  const [taapId, setTaapId] = useState("");
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      await act("create_application_organization", { application_id: applicationId, workspace_id: workspaceId, name, taap_id: taapId });
      setName("");
      setTaapId("");
      toast("Organization created");
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const nameError = fieldError(error, "name");
  const taapError = fieldError(error, "taap_id");
  return (
    <div className="panel">
      <h2>Create Organization</h2>
      <Form onSubmit={submit} error={nameError || taapError ? null : error}>
        <Field label="Name" required error={nameError}>
          <input name="name" value={name} onChange={(e) => setName(e.target.value)} />
        </Field>
        <Field label="TAAP identity" required error={taapError}>
          <input name="taap_id" value={taapId} onChange={(e) => setTaapId(e.target.value)} />
        </Field>
        <div>
          <Button variant="primary" type="submit" loading={busy}>
            Create
          </Button>
        </div>
      </Form>
    </div>
  );
}

/** Application organizations (`/application/organizations?id=`): one application's
 * organizations, with Delete and the create dialog (web_ui_application_organizations and its two
 * POST handlers). */
import { useState } from "react";
import { Plus } from "lucide-react";
import { act, useList, type Row } from "@/lib/api";
import { ConfirmButton, Modal } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { Button, DataTable, Field, Form, Mono, SearchInput, Toolbar, fieldError } from "@/lib/ui";
import { NameCell, PanelEmpty, When, matches, useMetaTrail, useParam } from "@/pages/organizations/shared";
import { ApplicationFrame, useApplication } from "./applicationShared";

export default function ApplicationOrganizationsPage() {
  const id = useParam("id");
  const application = useApplication(id);
  const workspaceId: string | undefined = application.data?.workspace_id ?? undefined;
  const organizations = useList<Row>(workspaceId ? "list_application_organizations" : null, { application_id: id, workspace_id: workspaceId, limit: 500 }, { pick: "items", workspace: null });
  const [creating, setCreating] = useState(false);
  const [search, setSearch] = useState("");
  useMetaTrail(organizations.data ? `${organizations.data.length} organizations` : null, "Application Organizations");
  const rows = organizations.data?.filter((o) => matches(search, o.name, o.taap_id));
  return (
    <ApplicationFrame
      application={application}
      title={(name) => `${name} Organizations`}
      sub="The organizations of this application's customers."
      actions={() => (
        <Button variant="primary" onClick={() => setCreating(true)}>
          <Plus size={14} aria-hidden /> New organization
        </Button>
      )}
    >
      {(app) => (
        <>
          <Toolbar>
            <SearchInput value={search} onChange={setSearch} placeholder="Search organizations…" />
          </Toolbar>
          <DataTable
            q={organizations}
            what="application organizations"
            rows={rows}
            rowKey={(o) => o.id}
            empty={
              <PanelEmpty
                title="No organizations"
                action={
                  <Button variant="primary" onClick={() => setCreating(true)}>
                    New organization
                  </Button>
                }
              >
                {organizations.data?.length ? "Clear the search to see every organization." : "Create the application's first organization."}
              </PanelEmpty>
            }
            columns={[
              { key: "name", label: "Organization", sort: (o) => o.name, render: (o) => <NameCell name={o.name} sub={<Mono short={36}>{o.taap_id}</Mono>} /> },
              { key: "updated", label: "Updated", sort: (o) => o.updated_at, render: (o) => <When value={o.updated_at} /> },
              {
                key: "delete",
                label: <span className="sr-only">Actions</span>,
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
          {creating && <CreateOrganization applicationId={app.id} workspaceId={app.workspace_id ?? ""} onClose={() => setCreating(false)} />}
        </>
      )}
    </ApplicationFrame>
  );
}

function CreateOrganization({ applicationId, workspaceId, onClose }: { applicationId: string; workspaceId: string; onClose: () => void }) {
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
      toast("Organization created");
      onClose();
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const nameError = fieldError(error, "name");
  const taapError = fieldError(error, "taap_id");
  return (
    <Modal
      title="New Organization"
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="create-application-organization" loading={busy}>
            Create
          </Button>
        </>
      }
    >
      <Form id="create-application-organization" onSubmit={submit} error={nameError || taapError ? null : error}>
        <Field label="Name" required error={nameError}>
          <input name="name" value={name} onChange={(e) => setName(e.target.value)} autoFocus />
        </Field>
        <Field label="TAAP identity" required error={taapError}>
          <input name="taap_id" value={taapId} onChange={(e) => setTaapId(e.target.value)} />
        </Field>
      </Form>
    </Modal>
  );
}

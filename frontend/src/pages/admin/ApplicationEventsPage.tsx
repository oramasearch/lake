/** Application event log (`/application/events?id=`): one application's lifecycle events and the
 * form to create a webhook, whose secret is shown once (web_ui_application_events,
 * serve_application_events_page, serve_application_webhook_create). */
import { useState } from "react";
import { act, useList, type Row } from "@/lib/api";
import { Button, DataTable, Field, Form, fieldError } from "@/lib/ui";
import { When, useMetaTrail, useParam } from "@/pages/organizations/shared";
import { ApplicationFrame, useApplication } from "./applicationShared";

const DEFAULT_EVENTS = "user.created,user.updated,user.deleted,organization.created,organization.updated,organization.deleted,session.started,mfa.changed";

/** "a, b,,c" → ["a", "b", "c"], as the handler's comma_separated did. */
export function commaSeparated(value: string): string[] {
  return value
    .split(",")
    .map((item) => item.trim())
    .filter((item) => item !== "");
}

export default function ApplicationEventsPage() {
  const id = useParam("id");
  const application = useApplication(id);
  const workspaceId: string | undefined = application.data?.workspace_id ?? undefined;
  const events = useList<Row>(workspaceId ? "list_application_webhook_events" : null, { application_id: id, workspace_id: workspaceId, limit: 500 }, { pick: "items", workspace: null });
  // The webhook secret from the create answer: held in this page only, never read again.
  const [secret, setSecret] = useState<string | null>(null);
  useMetaTrail(events.data ? `${events.data.length} events` : null, "Application Events");
  return (
    <ApplicationFrame application={application} eyebrow="application webhooks" title={(name) => `${name} Event Log`}>
      {(app) => (
        <>
          {secret && (
            <p className="notice">
              Save this webhook secret now: <code>{secret}</code>
            </p>
          )}
          <DataTable
            q={events}
            what="events"
            rows={events.data}
            rowKey={(e) => e.id}
            columns={[
              { key: "event", label: "Event", render: (e) => e.event },
              { key: "id", label: "ID", render: (e) => <code>{e.id}</code> },
              { key: "created", label: "Created", render: (e) => <When value={e.created_at} /> },
            ]}
          />
          <CreateWebhook applicationId={app.id} workspaceId={app.workspace_id ?? ""} onSecret={setSecret} />
        </>
      )}
    </ApplicationFrame>
  );
}

function CreateWebhook({ applicationId, workspaceId, onSecret }: { applicationId: string; workspaceId: string; onSecret: (secret: string) => void }) {
  const [url, setUrl] = useState("");
  const [events, setEvents] = useState(DEFAULT_EVENTS);
  const [error, setError] = useState<unknown>(null);
  const [busy, setBusy] = useState(false);
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      const webhook = await act<Row>("create_application_webhook", { application_id: applicationId, workspace_id: workspaceId, url, events: commaSeparated(events) });
      onSecret(webhook.secret);
      setUrl("");
      window.scrollTo({ top: 0 });
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const urlError = fieldError(error, "url");
  const eventsError = fieldError(error, "events");
  return (
    <div className="panel">
      <h2>Create Webhook</h2>
      <Form onSubmit={submit} error={urlError || eventsError ? null : error}>
        <Field label="HTTPS URL" required error={urlError}>
          <input type="url" name="url" value={url} onChange={(e) => setUrl(e.target.value)} />
        </Field>
        <Field label="Events (comma-separated)" required error={eventsError}>
          <input name="events" value={events} onChange={(e) => setEvents(e.target.value)} />
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

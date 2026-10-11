/** Application event log (`/application/events?id=`): one application's lifecycle events and the
 * drawer to create a webhook, whose secret is shown once (web_ui_application_events,
 * serve_application_events_page, serve_application_webhook_create). */
import { useState } from "react";
import { Plus } from "lucide-react";
import { act, useList, type Row } from "@/lib/api";
import { Drawer } from "@/lib/overlay";
import { Banner, Button, Chip, CopyBlock, DataTable, Field, FilterChips, Form, Mono, SearchInput, Toolbar, fieldError } from "@/lib/ui";
import { PanelEmpty, When, countBy, matches, useMetaTrail, useParam } from "@/pages/organizations/shared";
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
  const [creating, setCreating] = useState(false);
  const [search, setSearch] = useState("");
  const [kind, setKind] = useState<string | null>(null);
  useMetaTrail(events.data ? `${events.data.length} events` : null, "Application Events");
  const rows = events.data?.filter((e) => matches(search, e.event, e.id) && (!kind || String(e.event ?? "").split(".")[0] === kind));
  return (
    <ApplicationFrame
      application={application}
      title={(name) => `${name} Event Log`}
      sub="Signed lifecycle events, delivered to the application's webhooks."
      actions={() => (
        <Button variant="primary" onClick={() => setCreating(true)}>
          <Plus size={14} aria-hidden /> New webhook
        </Button>
      )}
    >
      {(app) => (
        <>
          {secret && (
            <Banner tone="ok" title="Save this webhook secret now. It is not shown again." action={<Button onClick={() => setSecret(null)}>Done</Button>}>
              <div className="mt-2">
                <CopyBlock value={secret} label="Webhook secret" />
              </div>
            </Banner>
          )}
          <Toolbar>
            <SearchInput value={search} onChange={setSearch} placeholder="Search events…" />
            <FilterChips options={countBy(events.data, (e) => String(e.event ?? "").split(".")[0] || null)} value={kind} onChange={setKind} allLabel="All events" />
          </Toolbar>
          <DataTable
            q={events}
            what="events"
            rows={rows}
            rowKey={(e) => e.id}
            empty={
              <PanelEmpty
                title="No events"
                action={
                  <Button variant="primary" onClick={() => setCreating(true)}>
                    New webhook
                  </Button>
                }
              >
                {events.data?.length ? "Clear the search or the filter to see more." : "Events appear as users and organizations change. Add a webhook to receive them."}
              </PanelEmpty>
            }
            columns={[
              { key: "event", label: "Event", sort: (e) => e.event, render: (e) => <Chip tone={/deleted$/.test(e.event ?? "") ? "bad" : /created$/.test(e.event ?? "") ? "ok" : "neutral"}>{e.event}</Chip> },
              { key: "id", label: "ID", render: (e) => <Mono short={14}>{e.id}</Mono> },
              { key: "created", label: "Created", sort: (e) => e.created_at, render: (e) => <When value={e.created_at} /> },
            ]}
            initialSort={{ key: "created", dir: "descending" }}
          />
          {creating && (
            <CreateWebhook
              applicationId={app.id}
              workspaceId={app.workspace_id ?? ""}
              onSecret={(value) => {
                setSecret(value);
                setCreating(false);
              }}
              onClose={() => setCreating(false)}
            />
          )}
        </>
      )}
    </ApplicationFrame>
  );
}

function CreateWebhook({ applicationId, workspaceId, onSecret, onClose }: { applicationId: string; workspaceId: string; onSecret: (secret: string) => void; onClose: () => void }) {
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
    <Drawer
      title="New Webhook"
      subtitle="Its signing secret is shown once, after it is created."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="create-webhook" loading={busy}>
            Create
          </Button>
        </>
      }
    >
      <Form id="create-webhook" onSubmit={submit} error={urlError || eventsError ? null : error}>
        <Field label="HTTPS URL" required error={urlError}>
          <input type="url" name="url" value={url} onChange={(e) => setUrl(e.target.value)} placeholder="https://example.com/webhooks/lake" autoFocus />
        </Field>
        <Field label="Events (comma-separated)" required error={eventsError}>
          <input name="events" value={events} onChange={(e) => setEvents(e.target.value)} />
        </Field>
      </Form>
    </Drawer>
  );
}

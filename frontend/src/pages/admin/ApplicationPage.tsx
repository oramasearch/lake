// SPDX-License-Identifier: MIT

/** Application (`/application?id=`): one identity application's settings and the links to its
 * users, organizations, events and hosted sign-in (web_ui_application, serve_application_page). */
import { Building2, ExternalLink, Users, Webhook } from "lucide-react";
import { type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { Button, Card, Chip, CopyBlock, KeyValue, Split } from "@/lib/ui";
import { useMetaTrail, useParam } from "@/pages/organizations/shared";
import { ApplicationFrame, useApplication } from "./applicationShared";

export default function ApplicationPage() {
  const id = useParam("id");
  const application = useApplication(id);
  const a = application.data;
  useMetaTrail(a?.pool_id, a ? `Application: ${a.name}` : "Application");
  return (
    <ApplicationFrame
      application={application}
      title={(name) => name}
      sub="Hosted sign-in, a user pool and signed lifecycle webhooks for one product."
      actions={(app) => (
        // The hosted pages stay server-rendered: a plain link leaves the app.
        <Button href={`/apps/${encodeURIComponent(app.id)}/sign-in`}>
          <ExternalLink size={14} aria-hidden /> Hosted sign-in
        </Button>
      )}
    >
      {(app) => <Overview a={app} />}
    </ApplicationFrame>
  );
}

function Overview({ a }: { a: Row }) {
  const { workspaces } = useSession();
  const app = encodeURIComponent(a.id);
  const origins = (a.allowed_origins ?? []) as string[];
  const redirects = (a.redirect_uris ?? []) as string[];
  const workspace = workspaces.find((w) => w.id === a.workspace_id);
  const list = (values: string[]) =>
    values.length ? (
      <span className="flex flex-wrap gap-1">
        {values.map((v) => (
          <Chip key={v}>
            <span className="font-mono text-[11.5px]">{v}</span>
          </Chip>
        ))}
      </span>
    ) : null;
  return (
    <Split
      side={
        <>
          <Card label="Application">
            <KeyValue
              rows={[
                ["ID", <code>{a.id}</code>],
                ["Pool", <code>{a.pool_id}</code>],
                ["Workspace", a.workspace_id ? (workspace ? <span title={a.workspace_id}>{workspace.name}</span> : <code>{a.workspace_id}</code>) : <code>not assigned</code>],
                ["Custom domain", a.custom_sign_in_domain || "—"],
              ]}
            />
          </Card>
          <Card label="Hosted sign-in">
            <CopyBlock value={`${window.location.origin}/apps/${app}/sign-in`} />
          </Card>
        </>
      }
    >
      <Card label="Sign-in settings">
        <KeyValue
          rows={[
            ["Origins", list(origins)],
            ["Redirect URIs", list(redirects)],
          ]}
        />
      </Card>
      <div className="grid grid-cols-1 gap-3 md:grid-cols-3">
        <Card to={`/application/users?id=${app}`} title="Users">
          <span className="inline-flex items-center gap-1.5 text-[12.5px] text-ink-2">
            <Users size={14} aria-hidden /> The application's user pool
          </span>
        </Card>
        <Card to={`/application/organizations?id=${app}`} title="Organizations">
          <span className="inline-flex items-center gap-1.5 text-[12.5px] text-ink-2">
            <Building2 size={14} aria-hidden /> Its customers' organizations
          </span>
        </Card>
        <Card to={`/application/events?id=${app}`} title="Webhook events">
          <span className="inline-flex items-center gap-1.5 text-[12.5px] text-ink-2">
            <Webhook size={14} aria-hidden /> Lifecycle events and webhooks
          </span>
        </Card>
      </div>
    </Split>
  );
}

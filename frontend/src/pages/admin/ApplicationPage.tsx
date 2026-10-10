/** Application (`/application?id=`): one identity application's settings and the links to its
 * users, organizations, events and hosted sign-in (web_ui_application, serve_application_page). */
import { Link } from "react-router-dom";
import { useRead, type Row } from "@/lib/api";
import { ErrorBlock, Page, Skeleton } from "@/lib/ui";
import { Dotted, useMetaTrail, useParam } from "@/pages/organizations/shared";

export default function ApplicationPage() {
  const id = useParam("id");
  const application = useRead<Row>("get_application", { id });
  const a = application.data;
  useMetaTrail(a?.pool_id, a ? `Application: ${a.name}` : "Application");
  if (application.isError) {
    return (
      <Page eyebrow="application" title="Application">
        <ErrorBlock error={application.error} retry={() => void application.refetch()} what="the application" />
      </Page>
    );
  }
  if (!a) {
    return (
      <Page eyebrow="application" title="Application">
        <Skeleton />
      </Page>
    );
  }
  const app = encodeURIComponent(a.id);
  return (
    <Page eyebrow="application" title={a.name}>
      <div className="panel">
        <dl>
          <dt>ID</dt>
          <dd>
            <code>{a.id}</code>
          </dd>
          <dt>Pool</dt>
          <dd>
            <code>{a.pool_id}</code>
          </dd>
          <dt>Origins</dt>
          <dd>{((a.allowed_origins ?? []) as string[]).join(", ")}</dd>
          <dt>Redirect URIs</dt>
          <dd>{((a.redirect_uris ?? []) as string[]).join(", ")}</dd>
          <dt>Custom domain</dt>
          <dd>{a.custom_sign_in_domain || "—"}</dd>
          <dt>Workspace</dt>
          <dd>
            <code>{a.workspace_id || "not assigned"}</code>
          </dd>
        </dl>
      </div>
      <p>
        <Dotted>
          {[
            <Link key="users" to={`/application/users?id=${app}`}>
              Users
            </Link>,
            <Link key="organizations" to={`/application/organizations?id=${app}`}>
              Organizations
            </Link>,
            <Link key="events" to={`/application/events?id=${app}`}>
              Webhook events
            </Link>,
            // The hosted pages stay server-rendered: a plain link leaves the app.
            <a key="hosted" href={`/apps/${app}/sign-in`}>
              Hosted sign-in
            </a>,
          ]}
        </Dotted>
      </p>
    </Page>
  );
}

/** Design → Projects (lake's workspace_content, projects branch): the workspace's projects, a way
 * to the Opportunities, and New Project. */
import { useState } from "react";
import { Link, useNavigate } from "react-router-dom";
import { Button, Field, Form, Page, Query, fieldError } from "@/lib/ui";
import { WorkspaceGate } from "@/components/WorkspaceGate";
import { qs, useScope, useWq, useWsPath, wa } from "./api";
import { FormError, Panel, useDesignTrail } from "./parts";

export default function ProjectsPage() {
  useDesignTrail({ label: "Projects", path: "/design/projects" }, [], "Projects");
  return (
    <Page
      eyebrow="Design"
      title="Projects"
      sub="A project starts from the business question: the decision it should improve and how you will measure it. Its pipelines are built from that intent."
    >
      <WorkspaceGate>
        <Projects />
      </WorkspaceGate>
    </Page>
  );
}

function Projects() {
  const ws = useWsPath();
  const listed = useWq<{ projects?: unknown[] }>("list_projects");
  return (
    <>
      <Query q={listed} what="projects">
        {(value) => {
          const projects = Array.isArray(value.projects) ? value.projects : [];
          if (!projects.length) return <div className="panel empty">No projects yet.</div>;
          return (
            <div className="table-wrap">
              <table>
                <thead>
                  <tr>
                    <th>Project</th>
                  </tr>
                </thead>
                <tbody>
                  {projects.map((entry, index) => {
                    const name = typeof entry === "string" ? entry : JSON.stringify(entry);
                    return (
                      <tr key={index}>
                        <td>
                          <Link to={ws(`/design/project?${qs({ name })}`)}>{name}</Link>
                        </td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          );
        }}
      </Query>
      <p>
        <Button to={ws("/design/opportunities")}>See your Opportunities</Button> Not sure where to start? Auto-Suggest recommends what your data could be used for.
      </p>
      <NewProject />
    </>
  );
}

function NewProject() {
  const scope = useScope();
  const ws = useWsPath();
  const navigate = useNavigate();
  const [name, setName] = useState("");
  const [description, setDescription] = useState("");
  const [intent, setIntent] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      // An empty description or intent is left out, as the form handler did.
      await wa(scope, "create_project", { name, ...(description ? { description } : {}), ...(intent ? { intent } : {}) });
      navigate(ws(`/design/project?${qs({ name })}`));
    } catch (e) {
      setError(e);
      setBusy(false);
    }
  };
  const inField = ["name", "description", "intent"].some((f) => fieldError(error, f) || fieldError(error, `input/${f}`));
  return (
    <Panel title="New Project">
      <Form onSubmit={submit}>
        <FormError error={inField ? null : error} />
        <Field label="Name" required error={fieldError(error, "name") ?? fieldError(error, "input/name")}>
          <input name="name" pattern="[a-z0-9][a-z0-9_\-]{0,62}" placeholder="retention-review" value={name} onChange={(e) => setName(e.target.value)} />
        </Field>
        <Field label="Description" error={fieldError(error, "description") ?? fieldError(error, "input/description")}>
          <input name="description" placeholder="Which enterprise accounts will not renew?" value={description} onChange={(e) => setDescription(e.target.value)} />
        </Field>
        <Field label="The decision it should improve" error={fieldError(error, "intent") ?? fieldError(error, "input/intent")}>
          <textarea
            name="intent"
            rows={3}
            placeholder="Which accounts the team calls first each week, measured by accounts saved per quarter"
            value={intent}
            onChange={(e) => setIntent(e.target.value)}
          />
        </Field>
        <div>
          <Button type="submit" variant="primary" loading={busy}>
            Create project
          </Button>
        </div>
      </Form>
    </Panel>
  );
}

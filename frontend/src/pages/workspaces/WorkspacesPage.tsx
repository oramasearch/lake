/** Ship → Workspaces (lake's workspaces_content and workspace_deletion_confirmation): a space's
 * workspaces, a new one, and the typed-name deletion that first lists what goes with counts from
 * `delete_workspace {dry_run: true}`. Not a workspace page: it is never under `/w/<id>/`, so every
 * call here goes unprefixed with the space named. */
import { useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { useQuery } from "@tanstack/react-query";
import { act, useRead, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { useTrail } from "@/lib/trail";
import { Button, ErrorBlock, Field, Form, Page, Skeleton, fieldError } from "@/lib/ui";
import { dateTime } from "@/lib/format";
import { postRead } from "@/pages/design/api";
import { FormError } from "@/pages/design/parts";

type Workspace = { id: string; name: string; environment?: string; phase?: string; namespace?: string; created_at?: string; desired?: string };
type Plan = {
  workspace_id: string;
  workspace_name: string;
  counts?: Record<string, number>;
  repositories?: { name?: string; state?: string }[];
  apps?: { name?: string }[];
  images?: { name?: string; tags?: unknown[] }[];
};

const plural = (n: number, one: string, many: string) => `${n} ${n === 1 ? one : many}`;

export default function WorkspacesPage() {
  const { viewer, space, refresh } = useSession();
  const [params, setParams] = useSearchParams();
  // The space: an accessible `?customer_id=`, else the current workspace's, else the first.
  const asked = params.get("customer_id");
  const customerId = viewer.spaces.find((s) => s.id === asked)?.id ?? space?.id ?? viewer.customer_id ?? viewer.spaces[0]?.id ?? "";
  const doomed = params.get("delete") ?? "";
  const back = `/workspaces?customer_id=${encodeURIComponent(customerId)}`;

  useTrail([{ label: "Ship", to: "/ship/models" }, { label: "Workspaces", to: back }], "Workspaces");

  const listed = useRead<{ workspaces?: Workspace[] }>("list_workspaces", { customer_id: customerId }, { workspace: null, enabled: !!customerId });

  return (
    <Page eyebrow="Ship" title="Workspaces" sub="Isolated environments for your space.">
      {!customerId ? (
        <div className="panel empty">You have no space to work in yet.</div>
      ) : listed.isLoading ? (
        <Skeleton />
      ) : listed.isError ? (
        <div className="panel empty">{(listed.error as Error).message}</div>
      ) : (
        <>
          {doomed && <DeleteConfirmation key={doomed} customerId={customerId} workspaceId={doomed} back={back} onDone={refresh} />}
          <NewWorkspace customerId={customerId} onDone={refresh} />
          <div className="panel">
            <div className="table-wrap">
              <table>
                <thead>
                  <tr>
                    <th>Name</th>
                    <th>Environment</th>
                    <th>Phase</th>
                    <th>Namespace</th>
                    <th>Created</th>
                    <th>Actions</th>
                  </tr>
                </thead>
                <tbody>
                  {(listed.data?.workspaces ?? []).map((row) => (
                    <tr key={row.id}>
                      <td>
                        <Link to={`/w/${encodeURIComponent(row.id)}/design/projects`}>{row.name}</Link>
                      </td>
                      <td>{row.environment}</td>
                      <td>{row.phase}</td>
                      <td>{row.namespace}</td>
                      <td>{row.created_at ? dateTime(row.created_at) : ""}</td>
                      <td>
                        {row.desired !== "deleted" ? (
                          // The confirmation lists what the deletion destroys before it asks for the name.
                          <a
                            href={`${back}&delete=${encodeURIComponent(row.id)}#delete-workspace`}
                            onClick={(e) => {
                              e.preventDefault();
                              const next = new URLSearchParams(params);
                              next.set("customer_id", customerId);
                              next.set("delete", row.id);
                              setParams(next);
                              window.scrollTo({ top: 0 });
                            }}
                          >
                            Delete
                          </a>
                        ) : (
                          "Deletion requested"
                        )}
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          </div>
        </>
      )}
    </Page>
  );
}

function NewWorkspace({ customerId, onDone }: { customerId: string; onDone: () => void }) {
  const toast = useToast();
  const [name, setName] = useState("");
  const [environment, setEnvironment] = useState("dev");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      await act("create_workspace", { customer_id: customerId, name, environment }, { workspace: null });
      setName("");
      setEnvironment("dev");
      onDone();
      toast("Workspace created");
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };
  const generic = error && !fieldError(error, "name") && !fieldError(error, "environment") ? error : null;
  return (
    <section className="panel">
      <h2>New Workspace</h2>
      <Form onSubmit={submit}>
        <FormError error={generic} />
        <Field label="Name" required error={fieldError(error, "name")}>
          <input name="name" maxLength={128} value={name} onChange={(e) => setName(e.target.value)} />
        </Field>
        <Field label="Environment" required help="Use dev, staging, prod or a lowercase DNS label." error={fieldError(error, "environment")}>
          <input name="environment" maxLength={47} pattern="[a-z0-9]([a-z0-9-]*[a-z0-9])?" value={environment} onChange={(e) => setEnvironment(e.target.value)} />
        </Field>
        <div>
          <Button type="submit" loading={busy}>
            New workspace
          </Button>
        </div>
      </Form>
    </section>
  );
}

function DeleteConfirmation({ customerId, workspaceId, back, onDone }: { customerId: string; workspaceId: string; back: string; onDone: () => void }) {
  const navigate = useNavigate();
  const toast = useToast();
  const [confirm, setConfirm] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  // The dry run deletes nothing: a read over POST, so it refreshes nothing on screen. It is not
  // asked again while the deletion itself is under way.
  const plan = useQuery<Plan>({
    queryKey: ["delete_workspace", "dry_run", customerId, workspaceId],
    queryFn: ({ signal }) => postRead<Plan>("delete_workspace", { customer_id: customerId, workspace_id: workspaceId, dry_run: true }, { signal, workspace: null }),
    retry: false,
    enabled: !busy,
  });

  if (plan.isPending && !plan.data) return plan.fetchStatus === "idle" ? null : <Skeleton />;
  if (plan.isError) return <ErrorBlock error={plan.error} what="what this deletion destroys" />;
  const value = plan.data;
  if (!value || !value.workspace_id) return null;
  const counts = value.counts ?? {};
  const count = (key: string) => (typeof counts[key] === "number" ? counts[key] : 0);
  const name = value.workspace_name ?? "";

  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      await act("delete_workspace", { customer_id: customerId, workspace_id: value.workspace_id, confirm } as Row, { workspace: null });
      onDone();
      toast(`Deleting ${name}`);
      navigate(back);
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };

  return (
    <section className="panel" aria-labelledby="delete-workspace">
      <h2 id="delete-workspace">Delete {name}</h2>
      <p>Deleting this workspace destroys everything below. It cannot be undone: retained volume snapshots are the only copy left.</p>
      <ul>
        <li>Its Zetta, with every record, dataset, topic and store in it, and its own audit history.</li>
        <li>
          {plural(count("repositories"), "repository", "repositories")} ({plural(count("live_repositories"), "live", "live")},{" "}
          {plural(count("tombstoned_repositories"), "deleted and still retained", "deleted and still retained")}). Their retention ends now and their names become free.
        </li>
        <li>{plural(count("apps"), "app", "apps")}.</li>
        <li>{plural(count("builds"), "unfinished build", "unfinished builds")}, cancelled.</li>
        <li>{plural(count("images"), "image repository", "image repositories")}, no longer pullable.</li>
      </ul>
      {!!value.repositories?.length && (
        <ul>
          {value.repositories.map((item, index) => (
            <li key={index}>
              Repository <code>{item.name ?? ""}</code>
              {item.state === "tombstoned" ? " (deleted, retained)" : ""}
            </li>
          ))}
        </ul>
      )}
      {!!value.apps?.length && (
        <ul>
          {value.apps.map((item, index) => (
            <li key={index}>
              App <code>{item.name ?? ""}</code>
            </li>
          ))}
        </ul>
      )}
      {!!value.images?.length && (
        <ul>
          {value.images.map((item, index) => {
            const tags = Array.isArray(item.tags) ? item.tags.length : 0;
            return (
              <li key={index}>
                Image <code>{item.name ?? ""}</code> ({tags} {tags === 1 ? "tag" : "tags"})
              </li>
            );
          })}
        </ul>
      )}
      <Form onSubmit={submit}>
        <FormError error={error && !fieldError(error, "confirm") ? error : null} />
        <Field
          label={
            <>
              Type <code>{name}</code> to confirm
            </>
          }
          required
          error={fieldError(error, "confirm")}
        >
          <input name="confirm" autoComplete="off" value={confirm} onChange={(e) => setConfirm(e.target.value)} />
        </Field>
        <div className="flex flex-wrap items-center gap-3">
          <Button type="submit" variant="danger" loading={busy}>
            Delete workspace and everything in it
          </Button>
          <Link to={back}>Cancel</Link>
        </div>
      </Form>
    </section>
  );
}

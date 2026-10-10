/** Ship → Packages (web_ui.cpp web_ui_packages): the instance registry's catalog, the space's
 * image repositories, and its image builds with their live log. */
import { type ReactNode, useEffect, useState } from "react";
import { Link } from "react-router-dom";
import { read, useRead, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { ErrorBlock, Page, Skeleton } from "@/lib/ui";
import { pause, streamHref, useWorkspaceLink } from "./shared";

type Catalog = { available?: boolean; origin?: string; npm?: Row[]; python?: Row[]; oci?: Row[] };
type Package = { name: string; versions: string[] };

const KINDS: { key: "oci" | "npm" | "python"; title: string; install: string }[] = [
  { key: "oci", title: "Container images", install: "docker pull {host}/{name}:{version}" },
  { key: "npm", title: "npm packages", install: "npm install {name} --registry {origin}" },
  { key: "python", title: "Python distributions", install: "pip install {name} --index-url {origin}/simple/" },
];

function packagesOf(items: Row[] | undefined): Package[] {
  return (items ?? []).map((item) => ({
    name: typeof item.name === "string" ? item.name : "",
    versions: Array.isArray(item.versions) ? item.versions.map((v: unknown) => (typeof v === "string" ? v : "")) : [],
  }));
}

function installCommand(template: string, origin: string, item: Package): string {
  if (!origin) return "";
  const host = origin.startsWith("https://") ? origin.slice(8) : origin;
  const latest = item.versions[item.versions.length - 1] || "latest";
  return template.replaceAll("{host}", host).replaceAll("{origin}", origin).replaceAll("{name}", item.name).replaceAll("{version}", latest);
}

export default function PackagesPage() {
  const { space, workspaceId, viewer } = useSession();
  const customerId = space?.id ?? "";
  const catalog = useRead<Catalog>("list_packages", customerId ? { customer_id: customerId } : undefined);
  const images = useRead<{ items?: Row[] }>(customerId ? "list_image_repositories" : null, { customer_id: customerId, workspace_id: workspaceId ?? undefined });
  const builds = useRead<{ builds?: Row[] }>(customerId && workspaceId ? "list_builds" : null, { customer_id: customerId, workspace_id: workspaceId ?? undefined });

  const shell = (children: ReactNode) => (
    <Page eyebrow="Ship" title="Packages" sub="Container images, npm packages and Python distributions published to this lake. Private images are visible only to their space.">
      {children}
    </Page>
  );
  if (catalog.isLoading) return shell(<Skeleton />);
  if (catalog.isError) return shell(<ErrorBlock error={catalog.error} retry={() => void catalog.refetch()} what="the registry catalog" />);
  if (!catalog.data?.available) return shell(<div className="panel empty">This lake has no registry storage configured.</div>);
  const origin = catalog.data.origin || viewer.external_origin || (viewer.public_host ? `https://${viewer.public_host}` : "");
  // The space's pushed images join the instance's one-component images; cache namespaces are not
  // packages.
  const spaceImages: Package[] = (images.data?.items ?? [])
    .filter((item) => !String(item.name ?? "").startsWith("cache/"))
    .map((item) => ({
      name: String(item.repository ?? ""),
      versions: (Array.isArray(item.tags) ? item.tags : []).map((tag: Row) => String(tag.tag ?? "")),
    }));
  const lists: Record<string, Package[]> = {
    oci: [...packagesOf(catalog.data.oci), ...spaceImages],
    npm: packagesOf(catalog.data.npm),
    python: packagesOf(catalog.data.python),
  };

  return shell(
    <>
      {KINDS.map((kind) => (
        <section key={kind.key} className="panel">
          <h2>{kind.title}</h2>
          {kind.key === "oci" && images.isError && <ErrorBlock error={images.error} retry={() => void images.refetch()} what="this space's images" />}
          {lists[kind.key].length === 0 ? (
            kind.key === "oci" && images.isLoading ? (
              <Skeleton lines={2} />
            ) : (
              <p className="muted">None published.</p>
            )
          ) : (
            <div className="table-wrap">
              <table>
                <thead>
                  <tr>
                    <th>Name</th>
                    <th>Versions</th>
                    <th>Install</th>
                  </tr>
                </thead>
                <tbody>
                  {lists[kind.key].map((item, index) => {
                    const install = installCommand(kind.install, origin, item);
                    return (
                      <tr key={`${item.name}-${index}`}>
                        <td>{item.name}</td>
                        <td>{item.versions.join(", ")}</td>
                        <td>{install ? <code>{install}</code> : <span className="muted">—</span>}</td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          )}
        </section>
      ))}
      <section className="panel">
        <h2>Builds</h2>
        {!workspaceId ? (
          <p className="muted">No image builds in this space.</p>
        ) : builds.isLoading ? (
          <Skeleton lines={2} />
        ) : builds.isError ? (
          <ErrorBlock error={builds.error} retry={() => void builds.refetch()} what="builds" />
        ) : !builds.data?.builds?.length ? (
          <p className="muted">No image builds in this space.</p>
        ) : (
          <div className="table-wrap">
            <table>
              <thead>
                <tr>
                  <th>Image</th>
                  <th>Phase</th>
                  <th>Digest</th>
                  <th>Log</th>
                </tr>
              </thead>
              <tbody>
                {builds.data.builds.map((build) => (
                  <BuildRow key={String(build.id)} build={build} customerId={customerId} workspaceId={workspaceId} />
                ))}
              </tbody>
            </table>
          </div>
        )}
      </section>
    </>,
  );
}

/** One build, its phase and log following `receive_build_events` by long poll from the first
 * retained event, as the page's script did. */
function BuildRow({ build, customerId, workspaceId }: { build: Row; customerId: string; workspaceId: string }) {
  const ws = useWorkspaceLink();
  const id = String(build.id ?? "");
  const [phase, setPhase] = useState<string>(typeof build.phase === "string" ? build.phase : "");
  const [log, setLog] = useState("");
  useEffect(() => {
    const controller = new AbortController();
    const signal = controller.signal;
    let next = 1;
    void (async () => {
      while (!signal.aborted) {
        try {
          const value = await read<{ events?: Row[]; next_sequence?: number }>(
            "receive_build_events",
            { customer_id: customerId, workspace_id: workspaceId, build_id: id, from_sequence: next, wait_ms: 25000 },
            { signal },
          );
          for (const event of value.events ?? []) {
            next = Math.max(next, Number(event.sequence) + 1);
            if (event.phase) setPhase(String(event.phase));
            const lines = Array.isArray(event.lines) ? event.lines : [];
            if (lines.length) setLog((current) => current + lines.map((line: unknown) => `${String(line)}\n`).join(""));
          }
          next = Math.max(next, Number(value.next_sequence) || next);
          await pause(250, signal);
        } catch {
          // A later poll retries a transient failure, after a pause.
          await pause(2000, signal);
        }
      }
    })();
    return () => {
      controller.abort();
    };
  }, [customerId, workspaceId, id]);
  return (
    <tr>
      <td>
        <code>{String(build.target ?? "")}</code>
      </td>
      <td>{phase}</td>
      <td>
        <code>{build.digest ? String(build.digest) : ""}</code>
      </td>
      <td>
        <details>
          <summary>Live log</summary>
          <pre className="build-log">{log}</pre>
        </details>
        <Link to={streamHref(ws, customerId, `build-events.${id}`)}>Browse messages</Link>
      </td>
    </tr>
  );
}

// SPDX-License-Identifier: MIT

/** Ship → Packages (web_ui.cpp web_ui_packages): the instance registry's catalog, the space's
 * image repositories, and its image builds with their live log — a tab per registry kind, each
 * package with its versions and its install command to copy, and a Builds tab. */
import { type ReactNode, useEffect, useState } from "react";
import { Link } from "react-router-dom";
import { Box, Hammer, Package as PackageIcon } from "lucide-react";
import { read, useRead, type Row } from "@/lib/api";
import { useSession } from "@/lib/session";
import { Card, Chip, CopyBlock, Empty, ErrorBlock, Page, SearchInput, Skeleton, Stat, StatRow, Status, Toolbar } from "@/lib/ui";
import { pause, QueryTabs, streamHref, Terminal, useQueryTab, useWorkspaceLink } from "./shared";
import { stopsPolling } from "@/lib/errors";

type Catalog = { available?: boolean; origin?: string; npm?: Row[]; python?: Row[]; oci?: Row[] };
type Package = { name: string; versions: string[] };

const KINDS: { key: "oci" | "npm" | "python"; title: string; tab: string; install: string }[] = [
  { key: "oci", title: "Container images", tab: "Container Images", install: "docker pull {host}/{name}:{version}" },
  { key: "npm", title: "npm packages", tab: "npm", install: "npm install {name} --registry {origin}" },
  { key: "python", title: "Python distributions", tab: "Python", install: "pip install {name} --index-url {origin}/simple/" },
];

const TABS = ["oci", "npm", "python", "builds"] as const;
type Tab = (typeof TABS)[number];

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

const SUB = "Container images, npm packages and Python distributions published to this Lake. Private images are visible only to their workspace.";

export default function PackagesPage() {
  const { space, workspaceId, viewer } = useSession();
  const customerId = space?.id ?? "";
  const tab = useQueryTab<Tab>(TABS);
  const [search, setSearch] = useState("");
  const catalog = useRead<Catalog>("list_packages", customerId ? { customer_id: customerId } : undefined);
  const images = useRead<{ items?: Row[] }>(customerId ? "list_image_repositories" : null, { customer_id: customerId, workspace_id: workspaceId ?? undefined });
  const builds = useRead<{ builds?: Row[] }>(customerId && workspaceId ? "list_builds" : null, { customer_id: customerId, workspace_id: workspaceId ?? undefined });

  const shell = (children: ReactNode) => (
    <Page eyebrow="Ship" title="Packages" sub={SUB}>
      {children}
    </Page>
  );
  if (catalog.isLoading) return shell(<Skeleton />);
  if (catalog.isError) return shell(<ErrorBlock error={catalog.error} retry={() => void catalog.refetch()} what="the registry catalog" />);
  if (!catalog.data?.available) {
    return shell(
      <Empty icon={PackageIcon} title="No Registry Storage">
        This Lake has no registry storage configured.
      </Empty>,
    );
  }
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
  const buildList = builds.data?.builds ?? [];
  const needle = search.trim().toLowerCase();

  return shell(
    <>
      <StatRow>
        <Stat label="Container images" value={lists.oci.length} icon={Box} />
        <Stat label="npm packages" value={lists.npm.length} icon={PackageIcon} />
        <Stat label="Python distributions" value={lists.python.length} icon={PackageIcon} />
        <Stat label="Image builds" value={workspaceId && builds.data ? buildList.length : "—"} icon={Hammer} />
      </StatRow>
      <QueryTabs<Tab>
        value={tab}
        label="Registries"
        tabs={[...KINDS.map((kind) => ({ id: kind.key as Tab, label: kind.tab, count: lists[kind.key].length })), { id: "builds" as Tab, label: "Builds", count: workspaceId && builds.data ? buildList.length : undefined }]}
      />
      {KINDS.filter((kind) => kind.key === tab).map((kind) => {
        const items = lists[kind.key].filter((item) => !needle || item.name.toLowerCase().includes(needle));
        return (
          <section key={kind.key} aria-label={kind.title}>
            {kind.key === "oci" && images.isError && <ErrorBlock error={images.error} retry={() => void images.refetch()} what="your workspace's images" />}
            {lists[kind.key].length === 0 ? (
              kind.key === "oci" && images.isLoading ? (
                <Skeleton lines={2} />
              ) : (
                <Empty icon={PackageIcon} title="Nothing Published">
                  None published.
                </Empty>
              )
            ) : (
              <>
                <Toolbar>
                  <SearchInput value={search} onChange={setSearch} placeholder={`Search ${kind.title.toLowerCase()}…`} label={`Search ${kind.title}`} />
                  <span className="text-[12.5px] text-ink-3">
                    {items.length} of {lists[kind.key].length}
                  </span>
                </Toolbar>
                {items.length === 0 ? (
                  <Empty compact title="No package matches">
                    Clear the search to see more.
                  </Empty>
                ) : (
                  <div className="grid grid-cols-1 gap-3 lg:grid-cols-2">
                    {items.map((item, index) => {
                      const install = installCommand(kind.install, origin, item);
                      return (
                        <Card key={`${item.name}-${index}`} label={kind.title} title={<span className="break-all">{item.name}</span>} actions={<span className="font-mono text-[12px] text-ink-3">{item.versions.length}</span>}>
                          <div className="flex flex-wrap gap-1">
                            {item.versions.length ? item.versions.map((version, i) => <Chip key={`${version}-${i}`}>{version || "—"}</Chip>) : <span className="muted text-[13px]">No versions</span>}
                          </div>
                          {install ? <CopyBlock label="Install" value={install} /> : <p className="muted mb-0 mt-2 text-[13px]">—</p>}
                        </Card>
                      );
                    })}
                  </div>
                )}
              </>
            )}
          </section>
        );
      })}
      {tab === "builds" && (
        <section aria-label="Builds">
          {!workspaceId ? (
            <Empty icon={Hammer} title="No Image Builds">
              No image builds in this workspace.
            </Empty>
          ) : builds.isLoading ? (
            <Skeleton lines={2} />
          ) : builds.isError ? (
            <ErrorBlock error={builds.error} retry={() => void builds.refetch()} what="builds" />
          ) : !buildList.length ? (
            <Empty icon={Hammer} title="No Image Builds">
              No image builds in this workspace.
            </Empty>
          ) : (
            <div className="space-y-3">
              {buildList.map((build) => (
                <BuildRow key={String(build.id)} build={build} customerId={customerId} workspaceId={workspaceId} />
              ))}
            </div>
          )}
        </section>
      )}
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
        } catch (error) {
          // A later poll retries a transient failure, after a pause; an answer asking again
          // cannot change ends the follow (lib/errors.ts, lake#575).
          if (signal.aborted || stopsPolling(error)) break;
          await pause(2000, signal);
        }
      }
    })();
    return () => {
      controller.abort();
    };
  }, [customerId, workspaceId, id]);
  return (
    <Card
      label="Image build"
      title={<code className="break-all text-[13px]">{String(build.target ?? "")}</code>}
      actions={<Status value={phase} />}
      footer={<Link to={streamHref(ws, customerId, `build-events.${id}`)}>Browse messages</Link>}
    >
      {build.digest ? (
        <div className="mb-2 text-[12.5px] text-ink-2">
          Digest <code className="break-all">{String(build.digest)}</code>
        </div>
      ) : null}
      <details>
        <summary className="cursor-pointer text-[13px] text-ink-2">Live log</summary>
        <Terminal className="mt-2" title="Build log" empty="No log lines yet.">
          {log}
        </Terminal>
      </details>
    </Card>
  );
}

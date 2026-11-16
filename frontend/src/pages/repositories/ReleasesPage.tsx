// SPDX-License-Identifier: MIT

/** /releases?repository_id= (web_ui_releases): published releases, newest first, each a card with
 * its tag, change, notes and attested artifacts, under the figures they add up to. */
import { useSearchParams } from "react-router-dom";
import { Clock, Package, ShieldCheck, Tag } from "lucide-react";
import { Card, Empty, Stat, StatRow } from "@/lib/ui";
import { dateTime, relative } from "@/lib/format";
import { Markdown } from "./Markdown";
import { ForgePage, Identifier, itemsOf, pending, ToRepositories, useForgeRead, useRepositoryTrail, type ForgeRecord, type Items, type Repository } from "./forge";

type ReleaseArtifact = { id?: string; action?: string; outputs?: number; attestations?: unknown[] };

export default function ReleasesPage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  if (!id) return <ToRepositories />;
  return <Releases id={id} />;
}

function Releases({ id }: { id: string }) {
  const repository = useForgeRead<Repository>("get_repository", { id });
  const releases = useForgeRead<Items<ForgeRecord>>("list_releases", { repository_id: id, limit: 1000 });
  useRepositoryTrail(repository.data, "Releases");
  const wait = pending(repository, releases);
  if (wait) return wait;
  const r = repository.data!;
  const rows = itemsOf(releases.data);
  const artifactsOf = (release: ForgeRecord): ReleaseArtifact[] => release.value.artifacts ?? [];
  const artifactCount = rows.reduce((sum, release) => sum + artifactsOf(release).length, 0);
  const attestationCount = rows.reduce((sum, release) => sum + artifactsOf(release).reduce((n, a) => n + (a.attestations ?? []).length, 0), 0);
  return (
    <ForgePage title="Releases" repository={r} tab="releases" sub="Published releases with their notes and attested artifacts.">
      {rows.length === 0 ? (
        <Empty icon={Package} title="No releases yet">
          No releases have been published.
        </Empty>
      ) : (
        <>
          <StatRow>
            <Stat label="Releases" value={rows.length} icon={Tag} sub={rows[0]?.value.tag ? `latest ${rows[0].value.tag}` : "published"} />
            <Stat label="Latest" value={relative(rows[0]?.created_at)} icon={Clock} sub={dateTime(rows[0]?.created_at)} />
            <Stat label="Artifacts" value={artifactCount} icon={Package} sub="across every release" />
            <Stat label="Attestations" value={attestationCount} icon={ShieldCheck} sub="on those artifacts" />
          </StatRow>
          <div className="space-y-4">
            {rows.map((release, index) => {
              const artifacts = artifactsOf(release);
              return (
                <Card
                  key={release.id}
                  tone={index === 0 ? "active" : "default"}
                  label={<Identifier value={release.id} />}
                  title={
                    <span className="inline-flex items-center gap-2 text-[17px]">
                      <Tag size={15} aria-hidden className="text-ink-3" />
                      {release.value.tag ?? "Untitled release"}
                    </span>
                  }
                  actions={
                    <span className="flex items-center gap-1.5 text-[12px] text-ink-3">
                      change <Identifier value={release.value.change_id ?? ""} />
                    </span>
                  }
                  footer={
                    artifacts.length === 0 ? (
                      <span className="muted">No release artifacts.</span>
                    ) : (
                      <div>
                        <div className="label mb-1">Artifacts and attestations</div>
                        <ul className="m-0 list-none space-y-1 p-0">
                          {artifacts.map((artifact, n) => (
                            <li key={artifact.id ?? n} className="flex flex-wrap items-center justify-between gap-2">
                              <span>
                                <strong>{artifact.action ?? "artifact"}</strong> · {artifact.outputs ?? 0} outputs
                              </span>
                              <span>
                                <Identifier value={artifact.id ?? ""} /> · {(artifact.attestations ?? []).length} attestations
                              </span>
                            </li>
                          ))}
                        </ul>
                      </div>
                    )
                  }
                >
                  <p className="sub mt-0">
                    Published {dateTime(release.created_at)} at {dateTime(release.updated_at)}
                  </p>
                  <Markdown>{String(release.value.notes ?? "")}</Markdown>
                </Card>
              );
            })}
          </div>
        </>
      )}
    </ForgePage>
  );
}

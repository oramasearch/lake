/** /releases?repository_id= (web_ui_releases): published releases with notes and attested
 * artifacts. */
import { useSearchParams } from "react-router-dom";
import { dateTime } from "@/lib/format";
import { Markdown } from "./Markdown";
import { ForgePage, ForgeTabs, Identifier, itemsOf, pending, repositoryName, ToRepositories, useForgeRead, useRepositoryTrail, type ForgeRecord, type Items, type Repository } from "./forge";

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
  return (
    <ForgePage eyebrow="repository releases" title="Releases" sub={repositoryName(r)}>
      <ForgeTabs repository={r} current="releases" />
      {rows.length === 0 && <div className="panel empty">No releases have been published.</div>}
      {rows.map((release) => {
        const artifacts: ReleaseArtifact[] = release.value.artifacts ?? [];
        return (
          <article key={release.id} className="panel release-card">
            <header>
              <div>
                <p className="eyebrow">
                  <Identifier value={release.id} />
                </p>
                <h2>{release.value.tag ?? "Untitled release"}</h2>
              </div>
              <span>
                <Identifier value={release.value.change_id ?? ""} />
              </span>
            </header>
            <p className="sub">
              Published {dateTime(release.created_at)} at {dateTime(release.updated_at)}
            </p>
            <Markdown>{String(release.value.notes ?? "")}</Markdown>
            {artifacts.length === 0 ? (
              <div className="empty compact-empty">No release artifacts.</div>
            ) : (
              <>
                <h3>Artifacts and Attestations</h3>
                <ul className="release-artifacts">
                  {artifacts.map((artifact, index) => (
                    <li key={artifact.id ?? index}>
                      <span>
                        <strong>{artifact.action ?? "artifact"}</strong> · {artifact.outputs ?? 0} outputs
                      </span>
                      <span>
                        <Identifier value={artifact.id ?? ""} /> · {(artifact.attestations ?? []).length} attestations
                      </span>
                    </li>
                  ))}
                </ul>
              </>
            )}
          </article>
        );
      })}
    </ForgePage>
  );
}

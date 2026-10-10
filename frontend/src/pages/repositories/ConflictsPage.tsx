/** /conflicts?repository_id= (web_ui_repository_conflicts): unresolved conflict values, each
 * explained, with its preserved values and the choice of which to keep (on `main`, as the page
 * always resolved). Reached from Drive's Conflicts link; no repository tab is current. */
import { Link, useSearchParams } from "react-router-dom";
import { useToast } from "@/lib/toast";
import {
  ConflictReasons,
  ConflictRegion,
  ConflictResolution,
  ConflictValues,
  conflictKindLabel,
  driveHref,
  ForgeBadge,
  ForgePage,
  ForgeTabs,
  itemsOf,
  pending,
  repositoryName,
  ToRepositories,
  useForgeRead,
  useRepositoryTrail,
  type Conflict,
  type Items,
  type Repository,
} from "./forge";

export default function ConflictsPage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  if (!id) return <ToRepositories />;
  return <Conflicts id={id} />;
}

function Conflicts({ id }: { id: string }) {
  const toast = useToast();
  const repository = useForgeRead<Repository>("get_repository", { id });
  const conflicts = useForgeRead<Items<Conflict>>("list_conflicts", { id });
  useRepositoryTrail(repository.data, "Conflicts");
  const wait = pending(repository, conflicts);
  if (wait) return wait;
  const r = repository.data!;
  const rows = itemsOf(conflicts.data);
  return (
    <ForgePage eyebrow="repository conflicts" title="Conflicts" sub={repositoryName(r)}>
      <ForgeTabs repository={r} current="conflicts" />
      {rows.length === 0 ? (
        <div className="panel empty">No unresolved conflict values.</div>
      ) : (
        rows.map((conflict) => (
          <article key={conflict.id} className="panel conflict-value">
            <header>
              <h2>
                <code>{conflict.region.path}</code>
              </h2>
              <ForgeBadge value={conflict.kind} />
            </header>
            <p>
              <strong>{conflictKindLabel(conflict.kind)}</strong>
            </p>
            <p>{conflict.explanation}</p>
            <p className="sub">
              Affected region: <ConflictRegion region={conflict.region} />
            </p>
            <ConflictReasons conflict={conflict} />
            <ConflictValues conflict={conflict} />
            <Link to={driveHref("/drive/conflict", id, "main", conflict.region.path)}>Compare preserved values</Link>
            <ConflictResolution repositoryId={id} refName="main" conflict={conflict} done={() => toast("Conflict resolved")} />
          </article>
        ))
      )}
    </ForgePage>
  );
}

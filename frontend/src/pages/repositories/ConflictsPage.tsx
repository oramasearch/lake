// SPDX-License-Identifier: MIT

/** /conflicts?repository_id= (web_ui_repository_conflicts): unresolved conflict values, each
 * explained, with its preserved values and the choice of which to keep (on `main`, as the page
 * always resolved). Reached from Drive's Conflicts link; no repository tab is current. */
import { useSearchParams } from "react-router-dom";
import { CircleCheck, GitCompareArrows, TriangleAlert } from "lucide-react";
import { useToast } from "@/lib/toast";
import { Banner, Button, Card, Empty, Split } from "@/lib/ui";
import {
  ConflictReasons,
  ConflictRegion,
  ConflictResolution,
  ConflictValues,
  conflictKindLabel,
  driveHref,
  ForgePage,
  ForgeStatus,
  itemsOf,
  pending,
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
    <ForgePage title="Conflicts" repository={r} tab="conflicts" sub="Concurrent values Lake preserved instead of choosing one. Compare them, then keep one.">
      {rows.length === 0 ? (
        <Empty icon={CircleCheck} title="No conflicts">
          No unresolved conflict values.
        </Empty>
      ) : (
        <>
          <Banner tone="info" title={`${rows.length} unresolved ${rows.length === 1 ? "conflict" : "conflicts"}`}>
            Each is resolved on <code>main</code>.
          </Banner>
          <div className="space-y-4">
            {rows.map((conflict) => (
              <Card
                key={conflict.id}
                tone="exception"
                label={conflictKindLabel(conflict.kind)}
                title={<code>{conflict.region.path}</code>}
                actions={
                  <>
                    <ForgeStatus value={conflict.kind} />
                    <Button to={driveHref("/drive/conflict", id, "main", conflict.region.path)}>
                      <GitCompareArrows size={14} aria-hidden /> Compare preserved values
                    </Button>
                  </>
                }
              >
                <Split
                  side={
                    <div className="rounded-[6px] border border-hairline bg-raised/40 px-3 py-2.5">
                      <div className="label mb-1.5">Preserved values</div>
                      <ConflictValues conflict={conflict} />
                    </div>
                  }
                >
                  <div className="flex items-start gap-2">
                    <TriangleAlert size={15} aria-hidden className="mt-0.5 shrink-0 text-warn" />
                    <p className="m-0">{conflict.explanation}</p>
                  </div>
                  <p className="sub m-0">
                    Affected region: <ConflictRegion region={conflict.region} />
                  </p>
                  <ConflictReasons conflict={conflict} />
                  <ConflictResolution repositoryId={id} refName="main" conflict={conflict} done={() => toast("Conflict resolved")} />
                </Split>
              </Card>
            ))}
          </div>
        </>
      )}
    </ForgePage>
  );
}

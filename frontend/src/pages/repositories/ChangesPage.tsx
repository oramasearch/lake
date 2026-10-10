/** /changes?repository_id= (web_ui_repository_log): the repository's changes, newest first. */
import { Link, useSearchParams } from "react-router-dom";
import { DataTable } from "@/lib/ui";
import { dateTime } from "@/lib/format";
import { ForgePage, itemsOf, pending, repositoryName, ToRepositories, useForgeRead, useRepositoryTrail, type Change, type Items, type Repository } from "./forge";

export default function ChangesPage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  if (!id) return <ToRepositories />;
  return <ChangeLog id={id} />;
}

function ChangeLog({ id }: { id: string }) {
  const repository = useForgeRead<Repository>("get_repository", { id });
  const changes = useForgeRead<Items<Change>>("list_changes", { id });
  useRepositoryTrail(repository.data, "Changes");
  const wait = pending(repository, changes);
  if (wait) return wait;
  const encoded = encodeURIComponent(id);
  return (
    <ForgePage eyebrow="repository history" title="Change Log" sub={repositoryName(repository.data!)}>
      <DataTable<Change>
        q={changes}
        what="changes"
        caption="Changes"
        rows={itemsOf(changes.data)}
        rowKey={(change) => change.id}
        empty={<div className="panel empty">No changes have been cut.</div>}
        columns={[
          { key: "change", label: "Change", render: (change) => <Link to={`/change?repository_id=${encoded}&id=${encodeURIComponent(change.id)}`}>{change.id}</Link> },
          { key: "message", label: "Message", render: (change) => change.message },
          { key: "author", label: "Author", render: (change) => <code>{change.author}</code> },
          { key: "when", label: "When", render: (change) => dateTime(change.occurred_at) },
        ]}
      />
    </ForgePage>
  );
}

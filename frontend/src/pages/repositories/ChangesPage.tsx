// SPDX-License-Identifier: MIT

/** /changes?repository_id= (web_ui_repository_log): the repository's changes, newest first, with
 * their figures (changes, authors, the latest) and a search over message, author and id. */
import { useState } from "react";
import { Link, useSearchParams } from "react-router-dom";
import { Clock, GitCommitHorizontal, History, Users } from "lucide-react";
import { DataTable, Empty, SearchInput, Stat, StatRow, Toolbar } from "@/lib/ui";
import { dateTime, relative } from "@/lib/format";
import { useSession } from "@/lib/session";
import { ForgePage, Identifier, itemsOf, pending, SignerName, ToRepositories, useForgeRead, useRepositoryTrail, type Change, type Items, type Repository, type WithPeople } from "./forge";
import { signerText } from "./signers";

export default function ChangesPage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  if (!id) return <ToRepositories />;
  return <ChangeLog id={id} />;
}

function ChangeLog({ id }: { id: string }) {
  const repository = useForgeRead<Repository>("get_repository", { id });
  const changes = useForgeRead<Items<Change> & WithPeople>("list_changes", { id });
  const { viewer } = useSession();
  const [search, setSearch] = useState("");
  useRepositoryTrail(repository.data, "Changes");
  const wait = pending(repository, changes);
  if (wait) return wait;
  const encoded = encodeURIComponent(id);
  const all = itemsOf(changes.data);
  const needle = search.trim().toLowerCase();
  const people = changes.data?.people;
  // An author is found by name, handle or key (lake#1326).
  const rows = needle ? all.filter((change) => `${change.message} ${signerText(change.author, people)} ${change.author} ${change.id}`.toLowerCase().includes(needle)) : all;
  const authors = new Set(all.map((change) => change.author));
  return (
    <ForgePage title="Change Log" repository={repository.data!} tab="changes" sub="Every change cut on this repository, newest first.">
      {all.length > 0 && (
        <StatRow cols={3}>
          <Stat label="Changes" value={all.length} icon={GitCommitHorizontal} sub="in the log" />
          <Stat label="Authors" value={authors.size} icon={Users} sub="distinct signers" />
          <Stat label="Latest" value={relative(all[0]?.occurred_at)} icon={Clock} sub={dateTime(all[0]?.occurred_at)} />
        </StatRow>
      )}
      {all.length > 0 && (
        <Toolbar>
          <SearchInput value={search} onChange={setSearch} placeholder="Search changes…" label="Search changes" />
          <span className="ml-auto font-mono text-[12px] text-ink-3">
            {rows.length} of {all.length}
          </span>
        </Toolbar>
      )}
      <DataTable<Change>
        q={changes}
        what="changes"
        caption="Changes"
        rows={rows}
        rowKey={(change) => change.id}
        empty={
          all.length === 0 ? (
            <Empty icon={History} title="No changes yet">
              No changes have been cut.
            </Empty>
          ) : (
            <Empty compact title="No change matches">
              Clear the search to see every change.
            </Empty>
          )
        }
        columns={[
          {
            key: "change",
            label: "Change",
            render: (change) => (
              <div className="min-w-0">
                <Link className="font-semibold" to={`/change?repository_id=${encoded}&id=${encodeURIComponent(change.id)}`}>
                  {change.message.split("\n")[0].replace(/^# /, "") || "Untitled change"}
                </Link>
                <div className="text-[12px] text-ink-3">
                  <Identifier value={change.id} />
                </div>
              </div>
            ),
          },
          {
            key: "author",
            label: "Author",
            sort: (change) => signerText(change.author, people),
            render: (change) => <SignerName id={change.author} people={people} viewer={viewer.taap_id} />,
          },
          {
            key: "when",
            label: "When",
            sort: (change) => change.occurred_at,
            render: (change) => (
              <span className="font-mono text-[12px] text-ink-2" title={dateTime(change.occurred_at)}>
                {relative(change.occurred_at)}
              </span>
            ),
          },
        ]}
      />
    </ForgePage>
  );
}

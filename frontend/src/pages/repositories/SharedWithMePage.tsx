/** /shared-with-me (web_ui_shared_with_me): live folders shared directly with the viewer's
 * identity. */
import { Link } from "react-router-dom";
import { useTrail } from "@/lib/trail";
import { DataTable } from "@/lib/ui";
import { driveHref, itemsOf, useForgeRead, type Repository } from "./forge";

type SharedFolder = { repository: Repository; ref: string; node_id: string; path: string; role: string; shared_by: string; available: boolean; actions: string[] };

export default function SharedWithMePage() {
  const folders = useForgeRead<{ items: SharedFolder[] }>("drive_shared_with_me", {});
  useTrail([{ label: "Shared with Me", to: "/shared-with-me" }, { label: "drive" }], "Shared with Me");
  return (
    <div className="page-container">
      <header className="page-header">
        <div className="min-w-0">
          <p className="eyebrow">drive</p>
          <h1>Shared with Me</h1>
          <p className="sub">Live folders shared directly with your identity. A move keeps the same folder identity; a downloaded copy remains independent.</p>
        </div>
      </header>
      <DataTable<SharedFolder>
        q={folders}
        what="shared folders"
        caption="Folders shared with me"
        rows={itemsOf(folders.data)}
        rowKey={(folder) => `${folder.repository.id}:${folder.node_id}`}
        empty={<div className="panel empty">No folders are shared directly with you.</div>}
        columns={[
          {
            key: "folder",
            label: "Folder",
            render: (folder) => {
              const label = `${folder.repository.name || folder.repository.id}/${folder.path}`;
              return folder.available ? <Link to={driveHref("/drive", folder.repository.id, folder.ref || "main", folder.path)}>{label}</Link> : label;
            },
          },
          { key: "shared_by", label: "Shared by", render: (folder) => <code>{folder.shared_by}</code> },
          { key: "actions", label: "What you can do", render: (folder) => folder.actions.join(", ") },
          { key: "state", label: "State", render: (folder) => (folder.available ? "available" : "removed") },
        ]}
      />
    </div>
  );
}

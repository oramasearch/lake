/** The connection forms shared by Data → Connections and Data → Datasets (lake#651): Add connection
 * (a drawer) and the pull of a table into a dataset (`create_dataset_from_query`), which Datasets'
 * Load data and a connection's "Load into a dataset" both use. */
import { useEffect, useState } from "react";
import { Link } from "react-router-dom";
import { Drawer } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import type { Row } from "@/lib/api";
import { Button, Form } from "@/lib/ui";
import { text, useAsync, useWorkspace, wa } from "@/pages/train/workspace";
import { connectionInput } from "./connections";
import { SafeModeCancelled, useSafeModeAction } from "./SafeMode";

export const NAME_PATTERN = "[A-Za-z_][A-Za-z0-9_]{0,62}";

/** Pull a table from one of the workspace's connections into a dataset. */
export function Pull({
  connections,
  initial,
  onDone,
  onBusy,
  formId = "load-pull",
}: {
  connections: Row[];
  initial?: { connection?: string; query?: string; tableName?: string };
  onDone: () => void;
  onBusy: (busy: boolean) => void;
  formId?: string;
}) {
  const scope = useWorkspace();
  const toast = useToast();
  const pull = useAsync();
  const safe = useSafeModeAction();
  useEffect(() => {
    onBusy(pull.busy);
  }, [pull.busy, onBusy]);
  const [connection, setConnection] = useState(initial?.connection || text(connections[0], "name"));
  const [query, setQuery] = useState(initial?.query ?? "");
  const [tableName, setTableName] = useState(initial?.tableName ?? "");
  const [maxRows, setMaxRows] = useState("");
  return (
    <Form
      id={formId}
      error={pull.error}
      onSubmit={() =>
        pull.run(async () => {
          const input: Row = { connection, query, table_name: tableName };
          if (maxRows) {
            if (!/^\d+$/.test(maxRows) || Number(maxRows) < 1) throw new Error("The row limit must be a whole number.");
            input.max_rows = Number(maxRows);
          }
          try {
            // The connection's safe mode may ask first (lake#906): the dialog shows the query, and
            // Run sends it again with the confirmation.
            await safe.run((confirmation) => wa(scope, "create_dataset_from_query", confirmation ? { ...input, safe_mode_confirmation: confirmation } : input));
          } catch (e) {
            if (e instanceof SafeModeCancelled) return;
            // A workspace started with --governance asks for an approval lake cannot give.
            if ((e as { code?: string }).code === "consent_required")
              throw new Error("This workspace requires an approval before it reaches an outside database, and Lake cannot approve it.");
            throw e;
          }
          toast(`Pulled ${tableName}`);
          setQuery("");
          setTableName("");
          setMaxRows("");
          onDone();
        })
      }
    >
      <label>
        From
        <select name="connection" value={connection} onChange={(e) => setConnection(e.target.value)}>
          {connections.map((c, i) => (
            <option key={i}>{text(c, "name")}</option>
          ))}
        </select>
      </label>
      <label>
        Query
        <textarea
          name="query"
          rows={4}
          required
          className="font-mono"
          placeholder="SELECT account_id, plan, seats, renewed FROM accounts"
          value={query}
          onChange={(e) => setQuery(e.target.value)}
        />
      </label>
      <label>
        Dataset name
        <input name="table_name" required pattern={NAME_PATTERN} placeholder="accounts" value={tableName} onChange={(e) => setTableName(e.target.value)} />
      </label>
      <label>
        At most this many rows
        <input name="max_rows" inputMode="numeric" placeholder="100000" value={maxRows} onChange={(e) => setMaxRows(e.target.value)} />
      </label>
      {safe.dialog}
    </Form>
  );
}

/** "Load into a dataset" from a connection's page: the pull in a drawer, its query filled in. */
export function LoadFromConnection({
  connections,
  initial,
  onClose,
}: {
  connections: Row[];
  initial: { connection: string; query?: string; tableName?: string };
  onClose: () => void;
}) {
  const [busy, setBusy] = useState(false);
  return (
    <Drawer
      title="Load into a Dataset"
      subtitle={`One read-only SELECT against ${initial.connection}; the rows become a dataset you can train on.`}
      onClose={onClose}
      width={560}
      footer={
        <>
          <Link to="/docs/load-data" className="mr-auto self-center text-[12.5px]">
            How loading data works
          </Link>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="connection-pull" loading={busy}>
            Pull dataset
          </Button>
        </>
      }
    >
      <Pull connections={connections} initial={initial} onDone={onClose} onBusy={setBusy} formId="connection-pull" />
    </Drawer>
  );
}

/** Add a connection: the credential goes to the workspace, which never returns it; the field is
 * never filled back in. */
export function AddConnection({ onClose, onSaved }: { onClose: () => void; onSaved?: (name: string) => void }) {
  const scope = useWorkspace();
  const toast = useToast();
  const save = useAsync();
  const blank = { name: "", type: "postgres", host: "", port: "", user: "", sslmode: "require", account: "", warehouse: "", role: "", database: "", schema: "", token: "" };
  const [form, setForm] = useState<Record<string, string>>(blank);
  const set = (key: string) => (e: React.ChangeEvent<HTMLInputElement | HTMLSelectElement>) => setForm({ ...form, [key]: e.target.value });
  const postgres = form.type === "postgres";
  return (
    <Drawer
      title="Add a Connection"
      subtitle="Use a read-only account: only a single SELECT ever runs against it."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="add-connection" loading={save.busy}>
            Save connection
          </Button>
        </>
      }
    >
      <Form
        id="add-connection"
        error={save.error}
        onSubmit={() =>
          save.run(async () => {
            const input = connectionInput(form);
            await wa(scope, "create_connection", input);
            setForm(blank);
            toast(`Saved ${input.name}`);
            onSaved?.(input.name);
            onClose();
          })
        }
      >
        <label>
          Name
          <input name="name" required pattern="[A-Za-z0-9_\-]{1,64}" placeholder="warehouse" value={form.name} onChange={set("name")} />
        </label>
        <label>
          Type
          <select name="type" value={form.type} onChange={set("type")}>
            <option value="postgres">Postgres</option>
            <option value="snowflake">Snowflake</option>
          </select>
        </label>
        {postgres ? (
          <>
            <label>
              Host
              <input name="host" placeholder="db.example.com" value={form.host} onChange={set("host")} />
            </label>
            <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
              <label>
                Port
                <input name="port" inputMode="numeric" placeholder="5432" value={form.port} onChange={set("port")} />
              </label>
              <label>
                SSL mode
                <select name="sslmode" value={form.sslmode} onChange={set("sslmode")}>
                  <option value="require">require</option>
                  <option value="verify-full">verify-full</option>
                  <option value="disable">disable</option>
                </select>
              </label>
            </div>
            <label>
              User
              <input name="user" value={form.user} onChange={set("user")} />
            </label>
          </>
        ) : (
          <>
            <label>
              Account
              <input name="account" placeholder="ab12345.us-east-1" value={form.account} onChange={set("account")} />
            </label>
            <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
              <label>
                Warehouse
                <input name="warehouse" value={form.warehouse} onChange={set("warehouse")} />
              </label>
              <label>
                Role
                <input name="role" value={form.role} onChange={set("role")} />
              </label>
            </div>
          </>
        )}
        <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
          <label>
            Database
            <input name="database" value={form.database} onChange={set("database")} />
          </label>
          <label>
            Schema
            <input name="schema" value={form.schema} onChange={set("schema")} />
          </label>
        </div>
        <label>
          Password or access token
          <input type="password" name="token" autoComplete="new-password" value={form.token} onChange={set("token")} />
          <small className="muted">Sent to the workspace, which never returns it.</small>
        </label>
      </Form>
    </Drawer>
  );
}

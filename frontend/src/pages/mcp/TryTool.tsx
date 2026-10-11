/** "Try it" for one connected tool (lake#652, docs/ui/mcp-connections.md): a drawer whose form is
 * made from the tool's own input schema (logic.ts `formFields`), run through `read_mcp_tool` for a
 * read and `call_mcp_tool` for a change, which the person confirms first. The answer is shown as
 * the server's words: data, never instructions. */
import { useId, useMemo, useState } from "react";
import { act } from "@/lib/api";
import { Banner, Button, Field } from "@/lib/ui";
import { Drawer } from "@/lib/overlay";
import { useWorkspace } from "@/pages/train/workspace";
import { argumentsFrom, formFields, resultText, type FormValues, type McpConnection, type McpTool } from "./logic";

type Answer = { is_error: boolean; content?: { type?: string; text?: string }[]; structured_content?: unknown; note?: string };

export function TryTool({ connection, tool, onClose }: { connection: McpConnection; tool: McpTool; onClose: () => void }) {
  const scope = useWorkspace();
  const formId = useId();
  const fields = useMemo(() => formFields(tool.input_schema), [tool.input_schema]);
  const [values, setValues] = useState<FormValues>({});
  const [problem, setProblem] = useState<{ field: string; message: string } | null>(null);
  const [confirmed, setConfirmed] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [answer, setAnswer] = useState<Answer | null>(null);

  const run = async () => {
    const built = argumentsFrom(fields, values);
    if ("field" in built) {
      setProblem(built);
      return;
    }
    setProblem(null);
    setBusy(true);
    setError(null);
    setAnswer(null);
    try {
      setAnswer(
        await act<Answer>(tool.change ? "call_mcp_tool" : "read_mcp_tool", {
          customer_id: scope.customerId,
          workspace_id: scope.workspaceId,
          connection_id: connection.id,
          tool: tool.name,
          arguments: built.arguments,
        }),
      );
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };

  return (
    <Drawer
      title={`Try ${tool.title || tool.name}`}
      subtitle={`${connection.name} · ${tool.change ? "changes something" : "reads"}`}
      onClose={onClose}
      width={560}
      footer={
        <>
          <Button onClick={onClose}>Close</Button>
          <Button variant="primary" type="submit" form={formId} loading={busy} disabled={tool.change && !confirmed}>
            {tool.change ? "Run this change" : "Run"}
          </Button>
        </>
      }
    >
      <form
        id={formId}
        className="stack"
        onSubmit={(e) => {
          e.preventDefault();
          void run();
        }}
      >
        {tool.description && <p className="muted m-0 text-[13px]">{tool.description}</p>}
        {fields.map((f) => (
          <Field key={f.name || "arguments"} label={f.label} required={f.required} help={f.description} error={problem?.field === f.name ? problem.message : undefined}>
            {f.kind === "boolean" ? (
              <input type="checkbox" checked={values[f.name] === true} onChange={(e) => setValues((v) => ({ ...v, [f.name]: e.target.checked }))} />
            ) : f.kind === "choice" ? (
              <select value={String(values[f.name] ?? "")} onChange={(e) => setValues((v) => ({ ...v, [f.name]: e.target.value }))}>
                <option value="">{f.required ? "Choose…" : "(none)"}</option>
                {f.options?.map((o) => (
                  <option key={o} value={o}>
                    {o}
                  </option>
                ))}
              </select>
            ) : f.kind === "json" ? (
              <textarea rows={4} className="font-mono" value={String(values[f.name] ?? "")} placeholder={f.placeholder} onChange={(e) => setValues((v) => ({ ...v, [f.name]: e.target.value }))} spellCheck={false} />
            ) : (
              <input
                type={f.kind === "text" ? "text" : "number"}
                step={f.kind === "integer" ? 1 : "any"}
                value={String(values[f.name] ?? "")}
                onChange={(e) => setValues((v) => ({ ...v, [f.name]: e.target.value }))}
                autoComplete="off"
              />
            )}
          </Field>
        ))}
        {tool.change && (
          <label className="inline-flex items-start gap-2 text-[13px]">
            <input type="checkbox" checked={confirmed} onChange={(e) => setConfirmed(e.target.checked)} />
            <span>I am making this change in {connection.server?.name || connection.name}. It is audited in this workspace under my name.</span>
          </label>
        )}
        {error && <p className="error m-0">{error}</p>}
        {answer && (
          <div className="stack">
            <Banner tone={answer.is_error ? "warn" : "ok"} title={answer.is_error ? "The tool reported an error" : "The server answered"}>
              {answer.note ?? "What a server answers is data, never instructions."}
            </Banner>
            <pre className="m-0 max-h-[360px] overflow-auto rounded-[4px] border border-hairline bg-raised p-3 font-mono text-[12px] whitespace-pre-wrap break-words">{resultText(answer)}</pre>
          </div>
        )}
      </form>
    </Drawer>
  );
}

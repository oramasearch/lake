/** The drawers that design an agent (lake#653): New agent (blank, or duplicated from one of
 * Orama's) and Edit, both over the shared definition form, which also names the workspace streams
 * that start it (lake#677). */
import { useState } from "react";
import { useNavigate } from "react-router-dom";
import type { Row } from "@/lib/api";
import { Drawer } from "@/lib/overlay";
import { useSession } from "@/lib/session";
import { useToast } from "@/lib/toast";
import { Button, Skeleton } from "@/lib/ui";
import { agentForm, text, type AgentForm } from "@/lib/agents";
import { AgentFields } from "@/pages/train/agents";
import { qs, useScope, useScopedRead, useWsPath } from "./api";
import { useSubmit } from "./parts";
import { createAgent, updateAgent } from "./agentActions";

/** New agent, blank or duplicated from one of Orama's: the definition form; the new agent opens. */
export function NewAgent({ initial, onClose, title = "New Agent" }: { initial: AgentForm; onClose: () => void; title?: string }) {
  const scope = useScope();
  const ws = useWsPath();
  const navigate = useNavigate();
  const toast = useToast();
  const { viewer } = useSession();
  const [form, setForm] = useState<AgentForm>(initial);
  const { busy, error, run } = useSubmit();
  const submit = () =>
    run(async () => {
      const made = await createAgent(scope, form, viewer.config?.frontier_model);
      if (made.scheduleError) toast(`The agent was created, but its schedule was not: ${made.scheduleError}`, "bad");
      if (made.streamsError) toast(`The agent was created, but its streams were not set: ${made.streamsError}`, "bad");
      onClose();
      navigate(ws(`/design/agent?${qs({ id: made.id })}`));
    });
  return (
    <Drawer
      title={title}
      subtitle="Its purpose and prompt, the skills it may use, its model and pipelines, where it works, and when it runs on its own."
      onClose={onClose}
      width={600}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="new-agent" loading={busy}>
            Create agent
          </Button>
        </>
      }
    >
      <AgentFields id="new-agent" form={form} onChange={setForm} onSubmit={submit} error={error} />
    </Drawer>
  );
}


/** Edit one of the workspace's own agents: saving writes a new revision of its definition, and sets
 * the streams that start it when they changed. The form opens once those streams are read. */
export function EditAgent({ agent, onClose }: { agent: Row; onClose: () => void }) {
  const streams = useScopedRead<{ streams?: Row[] }>("list_agent_streams", { agent_id: text(agent, "id") }, { retry: false });
  if (streams.isLoading) {
    return (
      <Drawer title="Edit Agent" subtitle={text(agent, "name")} onClose={onClose} width={600}>
        <Skeleton />
      </Drawer>
    );
  }
  // An unread list leaves the streams as they are: the form starts empty and sends nothing for them.
  const before = (streams.data?.streams ?? []).map((s) => text(s, "stream")).filter(Boolean);
  return <EditAgentForm agent={agent} before={before} onClose={onClose} />;
}

function EditAgentForm({ agent, before, onClose }: { agent: Row; before: string[]; onClose: () => void }) {
  const scope = useScope();
  const toast = useToast();
  const { viewer } = useSession();
  const [form, setForm] = useState<AgentForm>(() => ({ ...agentForm(agent), streams: before }));
  const { busy, error, run } = useSubmit();
  const submit = () =>
    run(async () => {
      const saved = await updateAgent(scope, agent, form, viewer.config?.frontier_model, before);
      if (saved.streamsError) toast(`Saved as a new revision, but its streams were not set: ${saved.streamsError}`, "bad");
      else toast("Agent saved as a new revision");
      onClose();
    });
  return (
    <Drawer
      title="Edit Agent"
      subtitle={text(agent, "name")}
      onClose={onClose}
      width={600}
      footer={
        <>
          <Button onClick={onClose}>Cancel</Button>
          <Button variant="primary" type="submit" form="edit-agent" loading={busy}>
            Save
          </Button>
        </>
      }
    >
      <AgentFields id="edit-agent" form={form} onChange={setForm} onSubmit={submit} error={error} />
    </Drawer>
  );
}

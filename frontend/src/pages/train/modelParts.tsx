// SPDX-License-Identifier: MIT

/** What the Train and Ship model pages share on screen (lake#599): the workspace's models grouped
 * into versions, a held-out score as a person reads it, and the links between the two pages for
 * the same model. The logic is in `models.ts`. */
import { useMemo } from "react";
import type { Row } from "@/lib/api";
import { Chip } from "@/lib/ui";
import { useWq, useWsPath, qs, text, useWorkspace } from "./workspace";
import { useExperimentRefs } from "./RunsPage";
import { groupModels, heldOutScore, inSampleAuc, type ModelGroup, type Score } from "./models";

/** `list_models` as models with their versions, and the experiment refs that named them. */
export function useModelGroups() {
  const q = useWq<Row>("list_models");
  const experiments = useExperimentRefs();
  const groups = useMemo(() => {
    const records = ((q.data?.models ?? []) as Row[]).filter((model) => model && typeof model === "object");
    return groupModels(records, (model) => experiments.byModel.get(text(model, "id")) ?? experiments.byRun.get(text(model, "run")));
  }, [q.data, experiments]);
  return { q, groups, experiments };
}

/** A score as "AUC 0.742" in mono, "held out" beside it. */
export function ScoreText({ score, note = true }: { score: Score | null; note?: boolean }) {
  if (!score) return <span className="muted">not measured</span>;
  return (
    <span className="whitespace-nowrap">
      <span className="font-mono tabular-nums">
        {score.label} {score.value.toFixed(3)}
      </span>
      {note && <small className="muted"> held out</small>}
    </span>
  );
}

/** A version's score, or why it has none: in-sample only, or nothing measured. */
export function VersionScore({ model }: { model: Row }) {
  const score = heldOutScore(model);
  if (score) return <ScoreText score={score} />;
  const inSample = inSampleAuc(model);
  if (inSample !== null)
    return (
      <span>
        <span className="font-mono tabular-nums">AUC {inSample.toFixed(3)}</span>
        <br />
        <small className="text-warn">in-sample only, not held out</small>
      </span>
    );
  return <span className="muted">not measured</span>;
}

/** "v3" as a chip. */
export function VersionChip({ n, tone }: { n: number; tone?: "ok" }) {
  return <Chip tone={tone}>v{n}</Chip>;
}

/** The two pages of one model: Train → Model (training) and Ship → Model (serving). */
export function useModelLinks() {
  const path = useWsPath();
  const { customerId } = useWorkspace();
  return {
    train: (id: string) => path(`/train/model?${qs({ customer_id: customerId, id })}`),
    ship: (id: string) => path(`/ship/model?${qs({ customer_id: customerId, id })}`),
    trainList: path(`/train/models?${qs({ customer_id: customerId })}`),
    shipList: path(`/ship/models?${qs({ customer_id: customerId })}`),
  };
}

/** Where a model came from, in words: its pipeline in its project, its experiment, made on its
 * own (trained or brought), or none of these. A standalone model's project is never named. */
export function SourceText({ group, experiment }: { group: ModelGroup; experiment?: { name: string } }) {
  if (group.standalone) return <>{group.brought ? "brought from a file" : "trained on its own"}</>;
  if (group.pipeline) {
    return (
      <>
        {group.pipeline}
        {group.projectName && <span className="muted"> in {group.projectName}</span>}
      </>
    );
  }
  if (experiment) return <>experiment {experiment.name}</>;
  return <span className="muted">no pipeline</span>;
}

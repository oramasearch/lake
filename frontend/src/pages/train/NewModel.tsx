/** New model on Train → Models (lake#654, docs/ui/train.md#a-standalone-model): a model made on its
 * own, not as a project pipeline's step. Two ways in:
 *
 * - Train one: a dataset, the column to predict (none for a methodology that learns without one),
 *   a methodology from `list_methodologies` and a held-out share; `train_model` starts the run and
 *   the model appears with its versions when it finishes.
 * - Bring one: a model file uploaded from this computer, from a drive (`load_drive_model`) or from
 *   a model repository (`resolve_model_file`, then `register_model`), kept so it is scored, served
 *   and compared like a trained one.
 *
 * The same forms add a version: `RetrainDialog` trains a standalone model again on new settings
 * (`retrain_model`), and `BringDialog` with `versionOf` adds a brought model's next file. An engine
 * older than these capabilities answers `not_supported`, which the form says as it is. */
import { type ReactNode, useMemo, useState } from "react";
import { useNavigate } from "react-router-dom";
import { Brain, FileUp, FolderGit2, HardDrive, Upload } from "lucide-react";
import { act, read, useRead, type Row } from "@/lib/api";
import { Modal } from "@/lib/overlay";
import { useToast } from "@/lib/toast";
import { Banner, Button, Field, FormError, fieldError } from "@/lib/ui";
import { datasetChoices, datasetColumns } from "../design/logic";
import { qs, text, useWorkspace, useWq, useWsPath } from "./workspace";
import { holdoutOf, nameProblem, needsTarget, type ModelGroup, type Training } from "./models";

type Mode = "choose" | "train" | "bring";

export function NewModelDialog({ groups, onClose }: { groups: ModelGroup[]; onClose: () => void }) {
  const [mode, setMode] = useState<Mode>("choose");
  if (mode === "train") return <TrainForm groups={groups} onClose={onClose} onBack={() => setMode("choose")} />;
  if (mode === "bring") return <BringDialog groups={groups} onClose={onClose} onBack={() => setMode("choose")} />;
  return (
    <Modal sheet width={600} title="New Model" subtitle="A model of its own, with no project or pipeline to look after. Its versions, scores and serving work as for any model." onClose={onClose}>
      <div className="grid grid-cols-1 gap-2 sm:grid-cols-2">
        <Choice icon={Brain} title="Train one" sub="From a dataset, a column to predict and a methodology." onClick={() => setMode("train")} />
        <Choice icon={FileUp} title="Bring one" sub="A model file from this computer, a drive or a model repository." onClick={() => setMode("bring")} />
      </div>
    </Modal>
  );
}

function Choice({ icon: IconC, title, sub, onClick }: { icon: typeof Brain; title: ReactNode; sub: ReactNode; onClick: () => void }) {
  return (
    <button
      type="button"
      onClick={onClick}
      className="plain flex min-h-[64px] w-full items-center justify-start gap-3 rounded-[6px] border border-hairline bg-surface px-3 py-2.5 text-left transition-colors hover:border-action"
    >
      <span className="flex h-9 w-9 shrink-0 items-center justify-center rounded-full bg-action-soft text-action">
        <IconC size={17} aria-hidden />
      </span>
      <span className="min-w-0">
        <span className="block text-[14px] font-semibold text-ink">{title}</span>
        <span className="block text-[12.5px] text-ink-2">{sub}</span>
      </span>
    </button>
  );
}

/** What the workspace can train with: its datasets, and its runnable methodologies with whether
 * each needs a target. */
function useTrainingChoices() {
  const datasets = useWq<Row>("list_datasets");
  const methodologies = useWq<Row>("list_methodologies", {}, { retry: false });
  const listed: Row[] = Array.isArray(methodologies.data?.methodologies) ? (methodologies.data!.methodologies as Row[]) : [];
  return {
    datasets,
    methodologies,
    datasetPaths: datasetChoices(datasets.data?.datasets).map((d) => d.path),
    runnable: listed.filter((m) => m.runnable === true),
  };
}

type Settings = { dataset: string; methodology: string; target: string; holdout: string };

/** Dataset, methodology, target and held-out share: the fields Train one and New version share. */
function TrainingFields({ value, onChange, error, busy, fixedMethodology }: { value: Settings; onChange: (next: Settings) => void; error: unknown; busy: boolean; fixedMethodology?: boolean }) {
  const { datasets, methodologies, datasetPaths, runnable } = useTrainingChoices();
  const methodology = runnable.find((m) => text(m, "id") === value.methodology);
  const columns = datasetColumns(datasets.data?.datasets, value.dataset);
  const targetRequired = methodology ? needsTarget(methodology) : true;
  const set = (key: keyof Settings) => (e: { target: { value: string } }) => onChange({ ...value, [key]: e.target.value });
  return (
    <>
      <Field label="Dataset" required error={fieldError(error, "dataset_path")} help={datasets.isError ? "The datasets could not be read." : datasetPaths.length === 0 && !datasets.isLoading ? "This workspace has no dataset yet. Load one in Data first." : undefined}>
        <select value={value.dataset} onChange={(e) => onChange({ ...value, dataset: e.target.value, target: "" })} disabled={busy}>
          <option value="">Choose a dataset</option>
          {datasetPaths.map((path) => (
            <option key={path} value={path}>
              {path}
            </option>
          ))}
        </select>
      </Field>
      <Field label="Methodology" required error={fieldError(error, "methodology")} help={methodologies.isError ? "This workspace's engine does not list its methodologies." : methodology ? text(methodology, "family") || undefined : "How it learns."}>
        <select value={value.methodology} onChange={set("methodology")} disabled={busy || fixedMethodology}>
          <option value="">Choose a methodology</option>
          {runnable.map((m) => (
            <option key={text(m, "id")} value={text(m, "id")}>
              {text(m, "id")} · {text(m, "name")}
            </option>
          ))}
        </select>
      </Field>
      <Field label="Column to predict" required={targetRequired} error={fieldError(error, "target")} help={targetRequired ? "The outcome it learns." : "This methodology learns without one; leave it empty."}>
        {columns.length > 0 ? (
          <select value={value.target} onChange={set("target")} disabled={busy}>
            <option value="">{targetRequired ? "Choose a column" : "None"}</option>
            {columns.map((column) => (
              <option key={column} value={column}>
                {column}
              </option>
            ))}
          </select>
        ) : (
          <input value={value.target} onChange={set("target")} placeholder={value.dataset ? "column name" : "choose a dataset first"} disabled={busy} />
        )}
      </Field>
      <Field label="Held-out share" error={fieldError(error, "holdout_fraction")} help="Rows kept out of training to score it honestly, such as 0.2. Empty: in-sample figures only, which flatter.">
        <input value={value.holdout} onChange={set("holdout")} inputMode="decimal" placeholder="0.2" disabled={busy} />
      </Field>
    </>
  );
}

function settingsProblem(settings: Settings, methodologies: Row[]): string | null {
  if (!settings.dataset) return "Choose a dataset.";
  if (!settings.methodology) return "Choose a methodology.";
  const methodology = methodologies.find((m) => text(m, "id") === settings.methodology);
  if ((!methodology || needsTarget(methodology)) && !settings.target.trim()) return "Choose the column to predict.";
  const holdout = holdoutOf(settings.holdout);
  return "error" in holdout ? holdout.error : null;
}

function TrainForm({ groups, onClose, onBack }: { groups: ModelGroup[]; onClose: () => void; onBack: () => void }) {
  const scope = useWorkspace();
  const path = useWsPath();
  const navigate = useNavigate();
  const toast = useToast();
  const { runnable } = useTrainingChoices();
  const [name, setName] = useState("");
  const [settings, setSettings] = useState<Settings>({ dataset: "", methodology: "M-1", target: "", holdout: "0.2" });
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const [touched, setTouched] = useState(false);
  const problem = nameProblem(name, groups) ?? settingsProblem(settings, runnable);

  const submit = async () => {
    setTouched(true);
    if (problem) return;
    setBusy(true);
    setError(null);
    try {
      const holdout = holdoutOf(settings.holdout);
      const answer = await act<Row>("train_model", {
        customer_id: scope.customerId,
        workspace_id: scope.workspaceId,
        name: name.trim(),
        methodology: settings.methodology,
        dataset_path: settings.dataset,
        ...(settings.target.trim() ? { target: settings.target.trim() } : {}),
        ...("value" in holdout && holdout.value !== null ? { holdout_fraction: holdout.value } : {}),
      });
      toast(`Training ${name.trim()}`);
      onClose();
      navigate(path(`/train/run?${qs({ customer_id: scope.customerId, id: text(answer, "run_id") })}`));
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };

  return (
    <Modal
      sheet
      width={600}
      title="Train a Model"
      subtitle="It trains in this workspace and appears in Models with its versions and held-out scores when the run finishes."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onBack} disabled={busy}>
            Back
          </Button>
          <Button variant="primary" loading={busy} onClick={() => void submit()}>
            Train
          </Button>
        </>
      }
    >
      <form
        className="grid gap-3"
        onSubmit={(e) => {
          e.preventDefault();
          void submit();
        }}
      >
        <Field label="Name" required error={fieldError(error, "name") ?? (touched && name.trim() === "" ? "Name the model." : undefined)}>
          <input value={name} onChange={(e) => setName(e.target.value)} placeholder="churn" maxLength={120} disabled={busy} autoFocus />
        </Field>
        <TrainingFields value={settings} onChange={setSettings} error={error} busy={busy} />
        {touched && problem && !error && <p className="error m-0">{problem}</p>}
        <FormError error={error} />
      </form>
    </Modal>
  );
}

/** New version of a model the engine trained: the same settings, or new ones it keeps for its next
 * retrain (`retrain_model`). */
export function RetrainDialog({ group, training, onClose }: { group: ModelGroup; training: Training | null; onClose: () => void }) {
  const scope = useWorkspace();
  const path = useWsPath();
  const navigate = useNavigate();
  const toast = useToast();
  const { runnable } = useTrainingChoices();
  const before: Settings = {
    dataset: training?.dataset ?? "",
    methodology: training?.methodology ?? "",
    target: training?.target ?? "",
    holdout: training?.holdout != null ? String(training.holdout) : "",
  };
  const [settings, setSettings] = useState<Settings>(before);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const problem = settingsProblem(settings, runnable);

  const submit = async () => {
    if (problem) return;
    setBusy(true);
    setError(null);
    try {
      const input: Row = { customer_id: scope.customerId, workspace_id: scope.workspaceId, model_id: text(group.latest, "id") };
      if (settings.dataset !== before.dataset) input.dataset_path = settings.dataset;
      if (settings.target.trim() && settings.target.trim() !== before.target) input.target = settings.target.trim();
      const holdout = holdoutOf(settings.holdout);
      if ("value" in holdout && holdout.value !== null && settings.holdout !== before.holdout) input.holdout_fraction = holdout.value;
      const answer = await act<Row>("retrain_model", input);
      toast(`Training a new version of ${group.name}`);
      onClose();
      navigate(path(`/train/run?${qs({ customer_id: scope.customerId, id: text(answer, "run_id") })}`));
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };

  return (
    <Modal
      sheet
      width={600}
      title={`New Version of ${group.name}`}
      subtitle="Train it again on new data, another column or another held-out share. The model keeps what you choose for its next retrain."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onClose} disabled={busy}>
            Cancel
          </Button>
          <Button variant="primary" loading={busy} disabled={!!problem} onClick={() => void submit()}>
            Train new version
          </Button>
        </>
      }
    >
      <form
        className="grid gap-3"
        onSubmit={(e) => {
          e.preventDefault();
          void submit();
        }}
      >
        <TrainingFields value={settings} onChange={setSettings} error={error} busy={busy} fixedMethodology />
        {problem && <p className="muted m-0 text-[12.5px]">{problem}</p>}
        <FormError error={error} />
      </form>
    </Modal>
  );
}

type From = "upload" | "drive" | "repository";

/** Bring a model file: a new standalone model, or with `versionOf` the next version of a brought
 * one. */
export function BringDialog({ groups, versionOf, onClose, onBack }: { groups: ModelGroup[]; versionOf?: ModelGroup; onClose: () => void; onBack?: () => void }) {
  const scope = useWorkspace();
  const path = useWsPath();
  const navigate = useNavigate();
  const toast = useToast();
  const [from, setFrom] = useState<From>("upload");
  const [name, setName] = useState("");
  const [file, setFile] = useState<File | null>(null);
  const [driveFile, setDriveFile] = useState("");
  const [repository, setRepository] = useState("");
  const [repositoryPath, setRepositoryPath] = useState("model.json");
  const [revision, setRevision] = useState("main");
  const [auc, setAuc] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<unknown>(null);
  const recent = useRead<{ items?: Row[] }>(from === "drive" && scope.customerId && scope.workspaceId ? "recent_drive_entries" : null, { customer_id: scope.customerId, workspace_id: scope.workspaceId, limit: 100 });
  const repositories = useRead<{ items?: Row[] }>(from === "repository" && scope.customerId ? "list_repositories" : null, { customer_id: scope.customerId, limit: 500 }, { workspace: null });
  const files = useMemo(() => (recent.data?.items ?? []).filter((item) => item?.entry && text(item.entry as Row, "kind") === "file"), [recent.data]);
  const here = (repositories.data?.items ?? []).filter((r) => text(r, "workspace_id") === scope.workspaceId && !r.deleted_at);

  const named = versionOf ? null : nameProblem(name, groups);
  const score = auc.trim() ? Number(auc) : null;
  const scoreProblem = score !== null && (!Number.isFinite(score) || score < 0 || score > 1) ? "An AUC is between 0 and 1." : null;
  const sourceProblem = from === "upload" ? (!file ? "Choose the file." : null) : from === "drive" ? (!driveFile ? "Choose the file." : null) : !repository || !repositoryPath.trim() ? "Choose the repository and the file's path." : null;
  const problem = named ?? sourceProblem ?? scoreProblem;

  const submit = async () => {
    if (problem) return;
    setBusy(true);
    setError(null);
    try {
      const common: Row = {
        customer_id: scope.customerId,
        workspace_id: scope.workspaceId,
        ...(versionOf ? { version_of: text(versionOf.latest, "id") } : { name: name.trim() }),
        ...(score !== null ? { metrics: { held_out_auc: score } } : {}),
      };
      let answer: Row;
      if (from === "drive") {
        const [driveId, id] = driveFile.split("\u0000");
        answer = await act<Row>("load_drive_model", { ...common, drive_id: driveId, id });
      } else if (from === "upload") {
        answer = await act<Row>("register_model", { ...common, weights: await file!.text(), source: `uploaded ${file!.name}` });
      } else {
        const resolved = await read<Row>("resolve_model_file", { id: repository, revision: revision.trim() || "main", path: repositoryPath.trim() }, { workspace: null });
        const repositoryName = text(here.find((r) => text(r, "id") === repository), "name") || repository;
        answer = await act<Row>("register_model", { ...common, weights: text(resolved, "contents"), source: `model repository ${repositoryName}@${text(resolved, "revision") || revision}: ${repositoryPath.trim()}` });
      }
      toast(versionOf ? `Added a version of ${versionOf.name}` : `Brought ${name.trim()}`);
      onClose();
      navigate(path(`/train/model?${qs({ customer_id: scope.customerId, id: text(answer, "model_id") })}`));
    } catch (e) {
      setError(e);
    } finally {
      setBusy(false);
    }
  };

  const tab = (id: From, label: string, Icon: typeof Upload) => (
    <Button variant={from === id ? "primary" : "secondary"} onClick={() => setFrom(id)} disabled={busy}>
      <Icon size={14} aria-hidden /> {label}
    </Button>
  );

  return (
    <Modal
      sheet
      width={600}
      title={versionOf ? `New Version of ${versionOf.name}` : "Bring a Model"}
      subtitle="A model trained elsewhere, kept here so it is scored, served and compared like a trained one."
      onClose={onClose}
      footer={
        <>
          <Button onClick={onBack ?? onClose} disabled={busy}>
            {onBack ? "Back" : "Cancel"}
          </Button>
          <Button variant="primary" loading={busy} disabled={!!problem} onClick={() => void submit()}>
            {versionOf ? "Add version" : "Bring model"}
          </Button>
        </>
      }
    >
      <form
        className="grid gap-3"
        onSubmit={(e) => {
          e.preventDefault();
          void submit();
        }}
      >
        <div className="flex flex-wrap gap-2" role="group" aria-label="Where the file is">
          {tab("upload", "This computer", Upload)}
          {tab("drive", "A drive", HardDrive)}
          {tab("repository", "A model repository", FolderGit2)}
        </div>
        <Banner tone="quiet" title="What can be brought">
          A gradient-boosted tree model in the engine&apos;s own JSON weights format, the format every model trained in Lake is kept in. Another format is refused and nothing is kept.
        </Banner>
        {!versionOf && (
          <Field label="Name" required error={fieldError(error, "name") ?? (name && named ? named : undefined)}>
            <input value={name} onChange={(e) => setName(e.target.value)} placeholder="partner churn" maxLength={120} disabled={busy} />
          </Field>
        )}
        {from === "upload" && (
          <Field label="Model file" required error={fieldError(error, "weights")}>
            <input type="file" accept=".json,application/json" onChange={(e) => setFile(e.target.files?.[0] ?? null)} disabled={busy} />
          </Field>
        )}
        {from === "drive" && (
          <Field label="File" required error={fieldError(error, "id")} help={recent.isError ? "The drives could not be read." : files.length === 0 && !recent.isLoading ? "No file in this workspace's drives yet." : "The files of this workspace's drives, newest first."}>
            <select value={driveFile} onChange={(e) => setDriveFile(e.target.value)} disabled={busy}>
              <option value="">Choose a file</option>
              {files.map((item) => {
                const entry = item.entry as Row;
                const drive = item.drive as Row;
                return (
                  <option key={`${text(drive, "id")}/${text(entry, "id")}`} value={`${text(drive, "id")}\u0000${text(entry, "id")}`}>
                    {text(drive, "name")}: {text(entry, "path") || text(entry, "name")}
                  </option>
                );
              })}
            </select>
          </Field>
        )}
        {from === "repository" && (
          <>
            <Field label="Repository" required help={repositories.isError ? "The repositories could not be read." : here.length === 0 && !repositories.isLoading ? "This workspace has no repository yet." : undefined}>
              <select value={repository} onChange={(e) => setRepository(e.target.value)} disabled={busy}>
                <option value="">Choose a repository</option>
                {here.map((r) => (
                  <option key={text(r, "id")} value={text(r, "id")}>
                    {text(r, "name")}
                  </option>
                ))}
              </select>
            </Field>
            <div className="grid grid-cols-1 gap-3 sm:grid-cols-[2fr_1fr]">
              <Field label="Path" required>
                <input value={repositoryPath} onChange={(e) => setRepositoryPath(e.target.value)} placeholder="model.json" disabled={busy} />
              </Field>
              <Field label="Revision">
                <input value={revision} onChange={(e) => setRevision(e.target.value)} placeholder="main" disabled={busy} />
              </Field>
            </div>
          </>
        )}
        <Field label="Held-out AUC" error={scoreProblem ?? fieldError(error, "metrics")} help="Optional: the score it got on rows it did not train on, where it was trained, so it compares with this workspace's models.">
          <input value={auc} onChange={(e) => setAuc(e.target.value)} inputMode="decimal" placeholder="0.81" disabled={busy} />
        </Field>
        {problem && !named && !scoreProblem && <p className="muted m-0 text-[12.5px]">{problem}</p>}
        <FormError error={error} />
      </form>
    </Modal>
  );
}

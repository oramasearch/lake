// SPDX-License-Identifier: MIT

/** The fields of one of your own solutions (lake#597), shared by New Solution and Edit Solution:
 * the catalogue's shape as far as a person writes it. A refusal names its field: lake's `path`
 * (such as /measurement/metric) puts the message under the field it points at. */
import { Field, Form, fieldError } from "@/lib/ui";
import { useWq } from "./api";
import { text, type J, type SolutionForm } from "./logic";
import { FormError } from "./parts";

const FIELDS = ["name", "problem", "decision", "measurement", "methodologies", "pipeline", "notes"];

export function SolutionFields({
  id,
  form,
  onChange,
  onSubmit,
  error,
}: {
  id: string;
  form: SolutionForm;
  onChange: (form: SolutionForm) => void;
  onSubmit: () => void | Promise<unknown>;
  error: unknown;
}) {
  // The methodologies this workspace's engine knows, named in the field's help; absent on an
  // older engine, and then the help says only how to write one.
  const methodologies = useWq<J>("list_methodologies", {}, { retry: false });
  const known: J[] = !methodologies.isError && Array.isArray(methodologies.data?.methodologies) ? methodologies.data!.methodologies : [];
  const set = (key: keyof SolutionForm) => (e: { target: { value: string } }) => onChange({ ...form, [key]: e.target.value });
  const inField = FIELDS.some((f) => fieldError(error, f));
  return (
    <Form id={id} onSubmit={onSubmit}>
      <FormError error={inField ? null : error} />
      <Field label="Name" required error={fieldError(error, "name")}>
        <input name="name" maxLength={200} placeholder="Late payments" value={form.name} onChange={set("name")} />
      </Field>
      <Field label="The business problem" required error={fieldError(error, "problem")}>
        <textarea name="problem" rows={3} placeholder="Invoices are paid late and cash runs short at month end." value={form.problem} onChange={set("problem")} />
      </Field>
      <Field label="The decision it drives" required error={fieldError(error, "decision")}>
        <textarea name="decision" rows={3} placeholder="Which customers collections calls first each morning." value={form.decision} onChange={set("decision")} />
      </Field>
      <div className="label mt-2">How Its Return Is Measured</div>
      <Field label="Metric" required error={fieldError(error, "measurement/metric") ?? fieldError(error, "measurement")}>
        <input name="metric" placeholder="Days sales outstanding" value={form.metric} onChange={set("metric")} />
      </Field>
      <div className="grid grid-cols-1 gap-x-3 sm:grid-cols-2">
        <Field label="Baseline" help="Where it stands today." error={fieldError(error, "measurement/baseline")}>
          <input name="baseline" placeholder="52 days" value={form.baseline} onChange={set("baseline")} />
        </Field>
        <Field label="Target" help="Where it should go." error={fieldError(error, "measurement/target")}>
          <input name="target" placeholder="40 days" value={form.target} onChange={set("target")} />
        </Field>
      </div>
      <div className="label mt-2">What It Composes</div>
      <Field
        label="Methodologies"
        help={
          <>
            Ids separated by commas, with the level each starts at after a colon when above 1: <code>M-1, M-3:2</code>.
            {known.length > 0 && <> This workspace's engine knows {known.map((m) => text(m, "id")).filter(Boolean).join(", ")}.</>}
          </>
        }
        error={fieldError(error, "methodologies")}
      >
        <input name="methodologies" placeholder="M-1, M-3:2" value={form.methodologies} onChange={set("methodologies")} />
      </Field>
      <Field label="Pipeline template or reference" help="Optional: a pipeline as project/pipeline, or a description of its steps." error={fieldError(error, "pipeline")}>
        <input name="pipeline" placeholder="collections/late-payers" value={form.pipeline} onChange={set("pipeline")} />
      </Field>
      <Field label="Notes" error={fieldError(error, "notes")}>
        <textarea name="notes" rows={4} value={form.notes} onChange={set("notes")} />
      </Field>
    </Form>
  );
}

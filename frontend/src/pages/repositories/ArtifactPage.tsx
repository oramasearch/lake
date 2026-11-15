// SPDX-License-Identifier: MIT

/** /artifact?repository_id=&id= (web_ui_artifact): one build result. Outputs, indexed members and
 * attestations on the left; the result's facts, timing, trace and a search inside it on the
 * right. */
import { useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { Search } from "lucide-react";
import { Actions, Button, Card, DataTable, Empty, Field, KeyValue, Meter, Split, Status } from "@/lib/ui";
import { dateTime } from "@/lib/format";
import { ForgePage, ForgeStatus, octal, pending, ToRepositories, useForgeRead, useRepositoryTrail, type Artifact, type ArtifactMember, type ArtifactOutput, type Attestation, type Repository } from "./forge";

export default function ArtifactPage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  const resultId = params.get("id") ?? "";
  if (!id || !resultId) return <ToRepositories />;
  return <BuildResult id={id} resultId={resultId} />;
}

function BuildResult({ id, resultId }: { id: string; resultId: string }) {
  const navigate = useNavigate();
  const [search, setSearch] = useState("");
  const repository = useForgeRead<Repository>("get_repository", { id });
  const artifact = useForgeRead<Artifact>("get_artifact", { id, result_id: resultId });
  useRepositoryTrail(repository.data, "Artifact");
  const wait = pending(repository, artifact);
  if (wait) return wait;
  const a = artifact.data!;
  const encoded = encodeURIComponent(id);
  const phases: [string, number][] = [
    ["Queued", a.timing.queued_ms],
    ["Sandbox", a.timing.sandbox_ms],
    ["Execute", a.timing.exec_ms],
    ["Hash", a.timing.hash_ms],
    ["Store", a.timing.store_ms],
  ];
  const longest = Math.max(1, ...phases.map(([, ms]) => ms));
  return (
    <ForgePage
      title={<span className="font-mono text-[22px]">{a.id}</span>}
      repository={repository.data!}
      tab="artifacts"
      sub={
        <span className="inline-flex flex-wrap items-center gap-2">
          <code>{a.action}</code>
          <ForgeStatus value={a.exit_code === 0 ? "passed" : "failed"} />
          <ForgeStatus value={a.divergent ? "divergent" : a.cache_status} />
        </span>
      }
      actions={
        <Button to={`/artifacts?repository_id=${encoded}`} variant="ghost">
          All artifacts
        </Button>
      }
    >
      <Split
        side={
          <>
            <Card label="Result">
              <KeyValue
                rows={[
                  ["Action", <code>{a.action}</code>],
                  ["Exit", <span className="font-mono">{a.exit_code}</span>],
                  ["Cache", <ForgeStatus value={a.cache_status} />],
                  ["Divergent", a.divergent ? "yes" : "no"],
                  ["Verification", a.verification],
                  ["Worker", a.worker],
                  ["Observed", dateTime(a.observed_at)],
                  ["Started", a.started_at ? dateTime(a.started_at) : "—"],
                  ["Finished", a.finished_at ? dateTime(a.finished_at) : "—"],
                ]}
              />
            </Card>
            <Card label="Timing">
              <ul className="m-0 list-none space-y-2 p-0">
                {phases.map(([phase, ms]) => (
                  <li key={phase}>
                    <div className="mb-0.5 text-[12.5px] text-ink">{phase}</div>
                    <Meter value={ms} max={longest} label={`${ms} ms`} />
                  </li>
                ))}
              </ul>
            </Card>
            <Card label="Trace">
              {!a.trace ? (
                <p className="muted m-0">No retained trace contains this result.</p>
              ) : (
                <KeyValue
                  rows={[
                    ["Trace", <code>{a.trace.id}</code>],
                    ["Wall time", `${a.trace.wall_ms} ms`],
                    ["Actions", a.trace.actions],
                    ["Dependencies", a.trace.dependencies],
                    ["Cache", `${a.trace.cache_hits} hits, ${a.trace.cache_misses} misses`],
                  ]}
                />
              )}
            </Card>
            <Card label="Search inside">
              <p className="muted mt-0 text-[12.5px]">
                Search an output with <code>in:artifact:&lt;output&gt;@&lt;result&gt;</code>. Members nested more than one container deep are recorded but not indexed.
              </p>
              <form
                className="stack"
                onSubmit={(e) => {
                  e.preventDefault();
                  navigate(`/search?repository=${encoded}&query=${encodeURIComponent(search)}`);
                }}
              >
                <Field label="Symbol or text">
                  <input value={search} onChange={(e) => setSearch(e.target.value)} placeholder="symbol in:artifact:output.deb@result" required />
                </Field>
                <div>
                  <Button type="submit">
                    <Search size={14} aria-hidden /> Search
                  </Button>
                </div>
              </form>
            </Card>
          </>
        }
      >
        <section>
          <div className="section-heading">
            <h2>Outputs</h2>
          </div>
          <DataTable<ArtifactOutput>
            what="outputs"
            caption="Outputs"
            rows={a.outputs}
            rowKey={(output) => output.path}
            empty={<Empty compact>No outputs.</Empty>}
            columns={[
              { key: "path", label: "Path", render: (output) => <code>{output.path}</code> },
              { key: "mode", label: "Mode", render: (output) => <code>{octal(output.mode)}</code> },
              { key: "blob", label: "Blob", render: (output) => <code className="break-all">{output.blob}</code> },
            ]}
          />
        </section>
        <section>
          <div className="section-heading">
            <h2>Indexed Members</h2>
          </div>
          <DataTable<ArtifactMember>
            what="indexed members"
            caption="Indexed members"
            rows={a.members}
            rowKey={(_, index) => String(index)}
            empty={<Empty compact>This artifact has not been indexed yet.</Empty>}
            columns={[
              { key: "artifact", label: "Artifact", render: (member) => <code>{member.artifact}</code> },
              { key: "member", label: "Member", render: (member) => <code>{member.path}</code> },
              { key: "digest", label: "Digest", render: (member) => <code className="break-all">{member.digest}</code> },
              { key: "indexed", label: "Indexed", render: (member) => (member.indexed ? <Status value="yes" tone="success" /> : <span className="text-[12px] text-ink-2">{member.reason}</span>) },
              {
                key: "search",
                label: "Search",
                render: (member) =>
                  member.indexed ? (
                    <Link to={`/search?repository=${encoded}&query=${encodeURIComponent(`${member.path} in:artifact:${member.artifact}@${member.version}`)}`}>Search</Link>
                  ) : (
                    "—"
                  ),
              },
            ]}
          />
        </section>
        <section>
          <div className="section-heading">
            <h2>Attestations</h2>
          </div>
          <DataTable<Attestation>
            what="attestations"
            caption="Attestations"
            rows={a.attestations}
            rowKey={(attestation) => attestation.id}
            empty={<Empty compact>No attestations.</Empty>}
            columns={[
              {
                key: "profile",
                label: "Profile",
                render: (attestation) => (
                  <div>
                    <span className="badge status-ai">{attestation.profile}</span>
                    <div className="mt-1">
                      <code>{attestation.id}</code>
                    </div>
                  </div>
                ),
              },
              { key: "subject", label: "Subject", render: (attestation) => <code>{attestation.subject}</code> },
              { key: "issuer", label: "Issuer", render: (attestation) => <code>{attestation.issuer}</code> },
              {
                key: "verification",
                label: "Verification",
                render: (attestation) =>
                  attestation.verified ? (
                    <ForgeStatus value="verified" />
                  ) : (
                    <span className="text-[12.5px]">{`unverified${attestation.reason ? `: ${attestation.reason}` : ""}`}</span>
                  ),
              },
            ]}
          />
        </section>
        <Actions>
          <Button variant="ghost" to={`/repository?id=${encoded}`}>
            Back to repository
          </Button>
        </Actions>
      </Split>
    </ForgePage>
  );
}

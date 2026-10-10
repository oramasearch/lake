/** /artifact?repository_id=&id= (web_ui_artifact): one build result: outputs, search inside,
 * indexed members, timing, attestations and trace. */
import { useState } from "react";
import { Link, useNavigate, useSearchParams } from "react-router-dom";
import { dateTime } from "@/lib/format";
import { ForgeBadge, ForgePage, octal, pending, repositoryName, ToRepositories, useForgeRead, useRepositoryTrail, type Artifact, type Repository } from "./forge";

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
  return (
    <ForgePage eyebrow="artifact result" title={a.id} sub={repositoryName(repository.data!)}>
      <div className="panel">
        <dl>
          <dt>action</dt>
          <dd>
            <code>{a.action}</code>
          </dd>
          <dt>exit</dt>
          <dd>{a.exit_code}</dd>
          <dt>cache</dt>
          <dd>
            <ForgeBadge value={a.cache_status} />
          </dd>
          <dt>divergent</dt>
          <dd>{a.divergent ? "yes" : "no"}</dd>
          <dt>worker</dt>
          <dd>{a.worker}</dd>
          <dt>observed</dt>
          <dd>{dateTime(a.observed_at)}</dd>
          <dt>started</dt>
          <dd>{a.started_at ? dateTime(a.started_at) : "—"}</dd>
          <dt>finished</dt>
          <dd>{a.finished_at ? dateTime(a.finished_at) : "—"}</dd>
        </dl>
      </div>
      <section className="panel diff-entry">
        <h2>Outputs</h2>
        {a.outputs.length === 0 ? (
          <p className="muted">No outputs.</p>
        ) : (
          <div className="table-wrap">
            <table>
              <thead>
                <tr>
                  <th>Path</th>
                  <th>Mode</th>
                  <th>Blob</th>
                </tr>
              </thead>
              <tbody>
                {a.outputs.map((output) => (
                  <tr key={output.path}>
                    <td>
                      <code>{output.path}</code>
                    </td>
                    <td>
                      <code>{octal(output.mode)}</code>
                    </td>
                    <td>
                      <code>{output.blob}</code>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </section>
      <section className="panel diff-entry">
        <h2>Search Inside</h2>
        <p className="muted">
          Search an output with <code>in:artifact:&lt;output&gt;@&lt;result&gt;</code>. Members nested more than one container deep are recorded but not indexed.
        </p>
        <form
          className="filters"
          onSubmit={(e) => {
            e.preventDefault();
            navigate(`/search?repository=${encoded}&query=${encodeURIComponent(search)}`);
          }}
        >
          <label>
            Symbol or text
            <input value={search} onChange={(e) => setSearch(e.target.value)} placeholder="symbol in:artifact:output.deb@result" required />
          </label>
          <button type="submit">Search</button>
        </form>
      </section>
      <section className="panel diff-entry">
        <h2>Indexed Members</h2>
        {a.members.length === 0 ? (
          <p className="muted">This artifact has not been indexed yet.</p>
        ) : (
          <div className="table-wrap">
            <table>
              <thead>
                <tr>
                  <th>Artifact</th>
                  <th>Member</th>
                  <th>Digest</th>
                  <th>Indexed</th>
                  <th>Search</th>
                </tr>
              </thead>
              <tbody>
                {a.members.map((member, index) => (
                  <tr key={index}>
                    <td>
                      <code>{member.artifact}</code>
                    </td>
                    <td>
                      <code>{member.path}</code>
                    </td>
                    <td>
                      <code>{member.digest}</code>
                    </td>
                    <td>{member.indexed ? "yes" : member.reason}</td>
                    <td>
                      {member.indexed ? (
                        <Link to={`/search?repository=${encoded}&query=${encodeURIComponent(`${member.path} in:artifact:${member.artifact}@${member.version}`)}`}>Search</Link>
                      ) : (
                        "—"
                      )}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </section>
      <section className="panel diff-entry">
        <h2>Timing</h2>
        <dl>
          <dt>queued</dt>
          <dd>{a.timing.queued_ms} ms</dd>
          <dt>sandbox</dt>
          <dd>{a.timing.sandbox_ms} ms</dd>
          <dt>execute</dt>
          <dd>{a.timing.exec_ms} ms</dd>
          <dt>hash</dt>
          <dd>{a.timing.hash_ms} ms</dd>
          <dt>store</dt>
          <dd>{a.timing.store_ms} ms</dd>
        </dl>
      </section>
      <section className="panel diff-entry">
        <h2>Attestations</h2>
        {a.attestations.length === 0 ? (
          <p className="muted">No attestations.</p>
        ) : (
          <div className="table-wrap">
            <table>
              <thead>
                <tr>
                  <th>Profile</th>
                  <th>Subject</th>
                  <th>Issuer</th>
                  <th>Verification</th>
                </tr>
              </thead>
              <tbody>
                {a.attestations.map((attestation) => (
                  <tr key={attestation.id}>
                    <td>
                      <span className="badge status-ai">{attestation.profile}</span>
                      <br />
                      <code>{attestation.id}</code>
                    </td>
                    <td>
                      <code>{attestation.subject}</code>
                    </td>
                    <td>
                      <code>{attestation.issuer}</code>
                    </td>
                    <td>{attestation.verified ? "verified" : `unverified${attestation.reason ? `: ${attestation.reason}` : ""}`}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </section>
      <section className="panel diff-entry">
        <h2>Trace</h2>
        {!a.trace ? (
          <p className="muted">No retained trace contains this result.</p>
        ) : (
          <dl>
            <dt>trace</dt>
            <dd>
              <code>{a.trace.id}</code>
            </dd>
            <dt>wall time</dt>
            <dd>{a.trace.wall_ms} ms</dd>
            <dt>actions</dt>
            <dd>{a.trace.actions}</dd>
            <dt>dependencies</dt>
            <dd>{a.trace.dependencies}</dd>
            <dt>cache</dt>
            <dd>
              {a.trace.cache_hits} hits, {a.trace.cache_misses} misses
            </dd>
          </dl>
        )}
      </section>
      <p>
        <Link to={`/artifacts?repository_id=${encoded}`}>All artifacts</Link> · <Link to={`/repository?id=${encoded}`}>Back to repository</Link>
      </p>
    </ForgePage>
  );
}

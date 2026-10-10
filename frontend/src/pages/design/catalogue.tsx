/** What Solutions, Solution, Methodologies and Methodology share: methodology links and the
 * "engine does not list solutions yet" note. */
import { Fragment } from "react";
import { Link } from "react-router-dom";
import { qs, useWsPath } from "./api";
import { solutionMethodologies, type J } from "./logic";

/** A solution's methodologies as links, each with its minimum level when above 1, or "—". */
export function MethodologyLinks({ solution }: { solution: J }) {
  const ws = useWsPath();
  const list = solutionMethodologies(solution);
  if (!list.length) return <>—</>;
  return (
    <>
      {list.map((m, index) => (
        <Fragment key={`${m.id}-${index}`}>
          {index > 0 && ", "}
          <Link to={ws(`/design/methodology?${qs({ id: m.id })}`)}>{m.id}</Link>
          {m.level > 1 && <span className="sub"> (level {m.level}+)</span>}
        </Fragment>
      ))}
    </>
  );
}

export function NoCatalogue() {
  const ws = useWsPath();
  return (
    <div className="panel empty">
      This workspace's engine does not list solutions yet. The solutions catalogue arrives with the next engine update; meanwhile see{" "}
      <Link to={ws("/design/methodologies")}>Methodologies</Link>.
    </div>
  );
}

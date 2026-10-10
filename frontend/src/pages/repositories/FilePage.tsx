/** /file?repository_id=&ref=&path= (web_ui_repository_file): one file with line anchors, blame,
 * C and C++ highlighting and syntax-unit shading. `#L5` selects a line, `#L5-L9` a range;
 * shift-click on a line number extends the selection from the line clicked before. Raw and
 * Download are lake's byte routes. */
import { type ReactNode, useEffect, useMemo, useRef } from "react";
import { useLocation, useNavigate, useSearchParams } from "react-router-dom";
import { useTrail } from "@/lib/trail";
import { dateTime } from "@/lib/format";
import {
  byteCount,
  fileLanguage,
  ForgeTabs,
  Identifier,
  itemsOf,
  pending,
  PathBreadcrumbs,
  repositoryName,
  ToRepositories,
  useChangeDiffs,
  useForgeRead,
  type Change,
  type DiffEntry,
  type Items,
  type Repository,
} from "./forge";

type RepositoryFile = {
  path: string;
  blob: string;
  contents: string;
  content_base64: string;
  size: number;
  /** Parsed syntax units (`repository_file {units: true}`), absent when the format has none. */
  units?: { start_line: number; end_line: number; kind: string }[];
};
type Blame = { change: string; author: string; occurred_at: string };

/** `syntax_language`: only C and C++ are highlighted. */
function syntaxLanguage(path: string): boolean {
  const dot = path.lastIndexOf(".");
  return dot >= 0 && [".c", ".h", ".cc", ".cpp", ".cxx", ".hh", ".hpp", ".hxx"].includes(path.slice(dot));
}

const KEYWORDS = new Set(
  "alignas alignof auto bool break case catch char class const constexpr continue default delete do double else enum explicit extern false float for friend if inline int long namespace new noexcept nullptr operator private protected public return short signed sizeof static struct switch template this throw true try typedef typename union unsigned using virtual void volatile while".split(
    " ",
  ),
);

/** `highlight_source_line`: `//` comments, quoted strings and C++ keywords. */
export function highlight(line: string): ReactNode[] {
  const out: ReactNode[] = [];
  let plain = "";
  let offset = 0;
  const flush = () => {
    if (plain) out.push(plain);
    plain = "";
  };
  while (offset < line.length) {
    if (line[offset] === "/" && line[offset + 1] === "/") {
      flush();
      out.push(
        <span key={offset} className="syntax-comment">
          {line.slice(offset)}
        </span>,
      );
      return out;
    }
    if (line[offset] === '"' || line[offset] === "'") {
      const quote = line[offset];
      let end = offset + 1;
      let escaped = false;
      while (end < line.length) {
        if (!escaped && line[end] === quote) {
          end += 1;
          break;
        }
        escaped = !escaped && line[end] === "\\";
        if (line[end] !== "\\") escaped = false;
        end += 1;
      }
      flush();
      out.push(
        <span key={offset} className="syntax-string">
          {line.slice(offset, end)}
        </span>,
      );
      offset = end;
      continue;
    }
    if (/[A-Za-z_]/.test(line[offset])) {
      let end = offset + 1;
      while (end < line.length && /[A-Za-z0-9_]/.test(line[end])) end += 1;
      const token = line.slice(offset, end);
      if (KEYWORDS.has(token)) {
        flush();
        out.push(
          <span key={offset} className="syntax-keyword">
            {token}
          </span>,
        );
      } else {
        plain += token;
      }
      offset = end;
      continue;
    }
    plain += line[offset];
    offset += 1;
  }
  flush();
  return out;
}

/** The file's lines as `std::getline` reads them: a trailing newline adds no empty line. */
export function sourceLines(contents: string): string[] {
  if (!contents) return [];
  const lines = contents.split("\n");
  if (contents.endsWith("\n")) lines.pop();
  return lines;
}

/** Blame as the server computed it: newest change first, the first change whose diff has hunks
 * for this path claims lines `new_line … new_line + max(new_count, 1) - 1`; a diff for this path
 * with no hunks claims every line still unclaimed. */
export function blameLines(path: string, count: number, changes: Change[], diffs: DiffEntry[][]): (Blame | null)[] {
  const blame: (Blame | null)[] = new Array(count).fill(null);
  changes.forEach((change, index) => {
    for (const difference of diffs[index] ?? []) {
      if (difference.path !== path) continue;
      const claim = (line: number) => {
        if (line === 0 || line > count || blame[line - 1]) return;
        blame[line - 1] = { change: change.id, author: change.author, occurred_at: change.occurred_at };
      };
      if (difference.line_hunks && difference.line_hunks.length) {
        for (const hunk of difference.line_hunks) {
          const span = Math.max(hunk.new_count, 1);
          for (let line = hunk.new_line; line < hunk.new_line + span; line++) claim(line);
        }
      } else {
        for (let line = 1; line <= count; line++) claim(line);
      }
    }
  });
  return blame;
}

/** `#L5` or `#L5-L9` → [first, last]. */
function selection(hash: string): [number, number] | null {
  const match = /^#L(\d+)(?:-L(\d+))?$/.exec(hash);
  if (!match) return null;
  return [Number(match[1]), Number(match[2] ?? match[1])];
}

export default function FilePage() {
  const [params] = useSearchParams();
  const id = params.get("repository_id") ?? "";
  const ref = params.get("ref") ?? "";
  const path = params.get("path") ?? "";
  if (!id || !ref || !path) return <ToRepositories />;
  return <SourceFile id={id} refName={ref} path={path} />;
}

function SourceFile({ id, refName, path }: { id: string; refName: string; path: string }) {
  const repository = useForgeRead<Repository>("get_repository", { id });
  const file = useForgeRead<RepositoryFile>("repository_file", { id, ref: refName, path, units: true });
  const changes = useForgeRead<Items<Change>>("list_changes", { id });
  const diffs = useChangeDiffs(id, changes.data ? itemsOf(changes.data) : undefined);
  const location = useLocation();
  const navigate = useNavigate();
  const anchor = useRef<number | null>(null);
  useTrail(repository.data ? [{ label: "Repositories", to: "/repositories" }, { label: path }] : null);
  const contents = file.data?.contents ?? "";
  const lines = useMemo(() => sourceLines(contents), [contents]);
  const ready = !!file.data && !!diffs.data;
  const selected = selection(location.hash);
  useEffect(() => {
    if (!ready || !selected) return;
    document.getElementById(`L${selected[0]}`)?.scrollIntoView({ block: "center" });
    // Scroll once when the file arrives or the hash changes, not on every render.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ready, location.hash]);

  const wait = pending(repository, file, changes, diffs);
  if (wait) return wait;
  const r = repository.data!;
  const f = file.data!;
  const highlighted = syntaxLanguage(f.path);
  const units = new Set<number>();
  for (const unit of f.units ?? []) for (let line = unit.start_line; line <= unit.end_line; line++) units.add(line);
  const blame = blameLines(path, lines.length, itemsOf(changes.data), diffs.data!);
  const q = `repository_id=${encodeURIComponent(id)}&ref=${encodeURIComponent(refName)}&path=${encodeURIComponent(f.path)}`;

  const onLine = (event: React.MouseEvent<HTMLAnchorElement>, line: number) => {
    event.preventDefault();
    if (event.shiftKey && anchor.current !== null) {
      const first = Math.min(anchor.current, line);
      const last = Math.max(anchor.current, line);
      navigate({ search: location.search, hash: `L${first}-L${last}` }, { replace: true });
      return;
    }
    anchor.current = line;
    navigate({ search: location.search, hash: `L${line}` }, { replace: true });
  };

  return (
    <div className="page-container">
      <PathBreadcrumbs repository={r} refName={refName} path={f.path} />
      <p className="eyebrow">file</p>
      <div className="file-heading">
        <div>
          <h1>{f.path}</h1>
          <p className="sub">
            {repositoryName(r)} at <code>{refName}</code>
          </p>
        </div>
        <div className="actions">
          <a href={`/file/raw?${q}`}>Raw</a>
          <a href={`/file/download?${q}`}>Download</a>
        </div>
      </div>
      <ForgeTabs repository={r} current="code" />
      <div className="file-meta">
        <span>
          <Identifier value={f.blob} />
        </span>
        <span>{byteCount(f.size)}</span>
        <span>{highlighted ? fileLanguage(f.path) : "Plain text"}</span>
      </div>
      <div className={`source-view ${highlighted ? "syntax-highlighted" : "plain-text"}`} data-language={highlighted ? fileLanguage(f.path) : "Plain text"}>
        <table>
          <thead className="sr-only">
            <tr>
              <th>Blame</th>
              <th>Line</th>
              <th>Source</th>
            </tr>
          </thead>
          <tbody>
            {lines.map((text, index) => {
              const number = index + 1;
              const who = blame[index];
              const isSelected = !!selected && number >= selected[0] && number <= selected[1];
              const classes = [units.has(number) && "syntax-unit", isSelected && "selected"].filter(Boolean).join(" ");
              return (
                <tr key={number} id={`L${number}`} data-line={number} className={classes || undefined}>
                  <td className="blame-cell">
                    {who && (
                      <>
                        <Identifier value={who.change} />
                        <span>{who.author}</span>
                        <time dateTime={who.occurred_at}>{dateTime(who.occurred_at)}</time>
                      </>
                    )}
                  </td>
                  <td className="line-number">
                    <a href={`#L${number}`} data-line-range={`L${number}-L${number}`} aria-label={`Line ${number}`} onClick={(e) => onLine(e, number)}>
                      {number}
                    </a>
                  </td>
                  <td className="source-line">
                    <code>{highlighted ? highlight(text) : text}</code>
                  </td>
                </tr>
              );
            })}
            {lines.length === 0 && (
              <tr>
                <td colSpan={3} className="empty">
                  This file is empty.
                </td>
              </tr>
            )}
          </tbody>
        </table>
      </div>
    </div>
  );
}


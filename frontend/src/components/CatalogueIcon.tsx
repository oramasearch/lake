// SPDX-License-Identifier: MIT

/** A solution's or methodology's icon (lake#639): the glyph `lib/catalogueIcons.ts` picks for the
 * entry, drawn in lake's tokens. At 28 px and up it sits on a soft tile; below, it is the bare
 * glyph, for lists and search hits. The accent is blue for a methodology and amber for a solution
 * (`styles/catalogue-icons.css`); both follow the theme. The icon is an image with a label that says
 * what it shows. */
import type { ReactElement } from "react";
import { methodologyIcon, solutionIcon, type IconChoice, type Mark } from "@/lib/catalogueIcons";

export type CatalogueKind = "solution" | "methodology";

function MarkShape({ mark }: { mark: Mark }): ReactElement {
  const cls = `cat-${mark.tone}${mark.dash ? " cat-dash" : ""}`;
  if (mark.kind === "circle") return <circle className={cls} cx={mark.cx} cy={mark.cy} r={mark.r} />;
  if (mark.kind === "rect") return <rect className={cls} x={mark.x} y={mark.y} width={mark.w} height={mark.h} rx={mark.rx} />;
  return <path className={cls} d={mark.d} />;
}

/** Draws a chosen icon. */
export function CatalogueGlyph({ choice, kind, size = 32, tile, className }: { choice: IconChoice; kind: CatalogueKind; size?: number; tile?: boolean; className?: string }) {
  const tiled = tile ?? size >= 28;
  return (
    <span
      className={["cat-icon", tiled && "cat-icon-tile", className].filter(Boolean).join(" ")}
      data-kind={kind}
      data-icon={choice.key}
      style={{ width: size, height: size }}
      role="img"
      aria-label={choice.label}
      title={choice.label}
    >
      <svg viewBox="0 0 24 24" aria-hidden="true" focusable="false">
        {"glyph" in choice ? (
          choice.glyph.marks.map((mark, index) => <MarkShape key={index} mark={mark} />)
        ) : (
          <text className="cat-monogram" x="12" y="12" textAnchor="middle" dominantBaseline="central">
            {choice.monogram}
          </text>
        )}
      </svg>
    </span>
  );
}

/** A solution's icon. `families` maps a methodology id to its family, from the engine's list, for a
 * solution of your own built on a methodology the set has not drawn yet. */
export function SolutionIcon({ entry, size, tile, families, className }: { entry: Record<string, unknown> | null | undefined; size?: number; tile?: boolean; families?: Record<string, string>; className?: string }) {
  return <CatalogueGlyph choice={solutionIcon(entry, families)} kind="solution" size={size} tile={tile} className={className} />;
}

/** A methodology's icon. */
export function MethodologyIcon({ entry, size, tile, className }: { entry: Record<string, unknown> | null | undefined; size?: number; tile?: boolean; className?: string }) {
  return <CatalogueGlyph choice={methodologyIcon(entry)} kind="methodology" size={size} tile={tile} className={className} />;
}

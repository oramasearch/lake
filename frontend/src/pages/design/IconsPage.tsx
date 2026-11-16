// SPDX-License-Identifier: MIT

/** Design → Catalogue Icons (lake#639, `/design/icons`): every solution and methodology icon at
 * 20, 32 and 64 px, the family glyphs and the monogram a new entry falls back to, for reviewing the
 * set. It reads nothing; the glyphs are `lib/catalogueIcons.ts`. Not in the rail; linked from
 * `docs/ui/catalogue-icons.md`. */
import { CatalogueGlyph, type CatalogueKind } from "@/components/CatalogueIcon";
import { CATALOGUE_NAMES, ENGINE_FAMILIES, FAMILY_GLYPHS, METHODOLOGY_GLYPHS, SOLUTION_GLYPHS, type IconChoice } from "@/lib/catalogueIcons";
import { Grid, Page, Section } from "@/lib/ui";
import { useDesignTrail } from "./parts";

const SIZES = [20, 32, 64];

function Row({ id, name, choice, kind }: { id: string; name: string; choice: IconChoice; kind: CatalogueKind }) {
  return (
    <div className="flex items-center gap-3 rounded-[6px] border border-hairline bg-surface px-3 py-2.5">
      <div className="flex shrink-0 items-center gap-2.5">
        {SIZES.map((size) => (
          <CatalogueGlyph key={size} choice={choice} kind={kind} size={size} />
        ))}
      </div>
      <div className="min-w-0">
        <div className="label m-0">{id}</div>
        <div className="text-[13.5px] font-semibold text-ink">{name}</div>
        <div className="text-[12px] text-ink-3">{choice.label}</div>
      </div>
    </div>
  );
}

const glyphChoice = (key: string, glyph: (typeof METHODOLOGY_GLYPHS)[string]): IconChoice => ({ glyph, from: "catalogue", key, label: glyph.label });

export default function IconsPage() {
  useDesignTrail({ label: "Catalogue Icons", path: "/design/icons" }, [], "Catalogue Icons");
  const grid = "grid-cols-1 gap-2 lg:grid-cols-2";
  return (
    <Page
      eyebrow="Design"
      title="Catalogue Icons"
      sub="One icon for every solution and methodology in Orama's catalogue, shown at 20, 32 and 64 px. Methodologies take a blue accent, solutions amber; a solution borrows the motif of the methodology it is built on."
    >
      <Section title="Methodologies">
        <Grid className={grid}>
          {Object.entries(METHODOLOGY_GLYPHS).map(([id, glyph]) => (
            <Row key={id} id={id} name={CATALOGUE_NAMES[id] ?? id} choice={glyphChoice(id, glyph)} kind="methodology" />
          ))}
        </Grid>
      </Section>
      <Section title="Solutions">
        <Grid className={grid}>
          {Object.entries(SOLUTION_GLYPHS).map(([id, glyph]) => (
            <Row key={id} id={id} name={CATALOGUE_NAMES[id] ?? id} choice={glyphChoice(id, glyph)} kind="solution" />
          ))}
        </Grid>
      </Section>
      <Section title="Fallbacks" sub="A methodology the set has not drawn takes its family's glyph; a solution of your own takes its original's or its methodology's; with neither, the first letter of its name.">
        <Grid className={grid}>
          {Object.entries(ENGINE_FAMILIES).map(([family, key]) => (
            <Row key={key} id={`Family: ${key}`} name={family} choice={glyphChoice(`family:${key}`, FAMILY_GLYPHS[key])} kind="methodology" />
          ))}
          <Row id="Monogram" name="A solution of your own with no methodology" choice={{ monogram: "L", from: "monogram", key: "monogram:L", label: "The letter L" }} kind="solution" />
        </Grid>
      </Section>
    </Page>
  );
}

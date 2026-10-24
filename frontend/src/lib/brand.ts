/** The instance brand (lake#1135, docs/serving/brand.md): what this Lake calls itself. Lake reads
 * it from the operator's LAKE_BRAND_* settings at start and answers it in `viewer.brand`, which
 * index.html carries embedded, so the console needs no build per brand and paints the brand
 * first. Lake's own brand everywhere it is not configured. */

export type Brand = {
  /** False for Lake's own brand. */
  custom: boolean;
  /** The product's name in prose and titles ("Lake"). */
  name: string;
  /** The name in identifiers ("lake"). */
  short_name: string;
  /** Who ships the catalogues and library agents ("Orama"). */
  company: string;
  /** The assistant beside every page ("Ask Orama"). */
  assistant_name: string;
  /** The console's document title ("Lake Console"). */
  console_title: string;
  /** "Lake by Orama", or the name alone. */
  eyebrow: string;
  /** Whether the brand shows "by Orama" (the wordmark's kicker). */
  by_orama?: boolean;
  logo?: string | null;
  logo_dark?: string | null;
  favicon?: string | null;
  accent?: string | null;
  accent_dark?: string | null;
  support_email?: string | null;
  legal_name?: string | null;
  terms_url?: string | null;
  privacy_url?: string | null;
};

export const LAKE_BRAND: Brand = {
  custom: false,
  name: "Lake",
  short_name: "lake",
  company: "Orama",
  assistant_name: "Ask Orama",
  console_title: "Lake Console",
  eyebrow: "Lake by Orama",
  by_orama: true,
};

/** `viewer.brand` with Lake's defaults under it, so an older server's answer still reads. */
export function brandOf(viewer: { brand?: Partial<Brand> | null } | null | undefined): Brand {
  const given = viewer?.brand ?? {};
  const brand = { ...LAKE_BRAND, ...Object.fromEntries(Object.entries(given).filter(([, v]) => v !== undefined)) } as Brand;
  if (brand.by_orama === undefined) brand.by_orama = !brand.custom || brand.eyebrow !== brand.name;
  return brand;
}

let embedded: Brand | undefined;
/** The brand lake put in this page (index.html's `#lake-viewer`), once; Lake's without one. It is
 * instance configuration, fixed while lake runs, so one read serves every page. */
export function instanceBrand(): Brand {
  if (embedded) return embedded;
  let viewer: { brand?: Partial<Brand> } | null = null;
  try {
    const node = typeof document === "undefined" ? null : document.getElementById("lake-viewer");
    const envelope = node ? JSON.parse(node.textContent ?? "") : null;
    viewer = envelope && envelope.ok ? envelope.value : null;
  } catch {
    viewer = null;
  }
  embedded = brandOf(viewer);
  return embedded;
}

function escapeRegExp(text: string): string {
  return text.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

function replaceWords(text: string, from: string, to: string): string {
  return text.replace(new RegExp(`(?<![A-Za-z0-9_])${escapeRegExp(from)}(?![A-Za-z0-9_])`, "g"), () => to);
}

/** Copy written for Lake in this brand's names: the same rewriting as lake's Brand::apply, so the
 * console and the server say the same thing. Unchanged for Lake's own brand. */
export function brandText(brand: Brand, text: string): string {
  if (!brand.custom) return text;
  let out = text;
  out = replaceWords(out, "lake.live is prepaid", `${brand.name} is prepaid when billing is on`);
  out = replaceWords(out, "Lake by Orama", brand.eyebrow);
  out = replaceWords(out, "Lake Console", brand.console_title);
  out = replaceWords(out, "Ask Orama", brand.assistant_name);
  out = replaceWords(out, "The Orama operator", "The operator");
  out = replaceWords(out, "the Orama operator", "the operator");
  out = replaceWords(out, "Orama operator", "operator");
  out = replaceWords(out, "The orama-operator", "The operator");
  out = replaceWords(out, "the orama-operator", "the operator");
  out = replaceWords(out, "Orama", brand.company);
  out = replaceWords(out, "Lake", brand.name);
  return out;
}

/** The document title: "<name> — <page>", or the console title on a page with none (lake#642). */
export function documentTitle(brand: Brand, title?: string | null): string {
  return title ? `${brand.name} — ${title}` : brand.console_title;
}

/** The wordmark's small line above the name: "Orama" where the brand shows it, else nothing. */
export function brandKicker(brand: Brand): string {
  return brand.by_orama ? "Orama" : "";
}

/** The brand's mark: a white-labelled Lake's logo (its dark version in the dark theme), or Lake's
 * own mark (lake#1135). The logo carries the brand's name as its text. */
import type { Brand } from "@/lib/brand";
import { LakeMark } from "./LakeMark";

export function BrandMark({ brand, size = 28 }: { brand: Brand; size?: number }) {
  if (!brand.logo) return <LakeMark size={size} />;
  return (
    <span className="brand-logo" style={{ height: size }}>
      <img className={brand.logo_dark ? "brand-logo-light" : undefined} src={brand.logo} alt={brand.name} height={size} />
      {brand.logo_dark && <img className="brand-logo-dark" src={brand.logo_dark} alt={brand.name} height={size} />}
    </span>
  );
}

// SPDX-License-Identifier: MIT

/** Each rail section's icon (Shell's rail and Ask Orama's home, lake#786), so the two always match. */
import { Building2, Code2, Compass, Database, FlaskConical, Home, type LucideIcon, Network, Rocket, ShieldCheck } from "lucide-react";

export const SECTION_ICONS: Partial<Record<string, LucideIcon>> = {
  home: Home,
  data: Database,
  design: Compass,
  train: FlaskConical,
  ship: Rocket,
  network: Network,
  organization: Building2,
  administration: ShieldCheck,
  developer: Code2,
};

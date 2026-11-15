// SPDX-License-Identifier: MIT

/** Load data → Upload's dataset name (lake#814): choosing a file names the dataset after it, made a
 * valid SQL name (`customers.csv` → `customers`, `My Data 2024.csv` → `my_data_2024`), and the name
 * stays editable. Pure, tested by `tests/upload.test.ts`. */
import { datasetName } from "../drives/paths.ts";

/** The Dataset name after a file is chosen. A name the person typed is kept; an empty name, or one
 * Lake filled in from the previous file, follows the new file. With no file the name is left as
 * it is. `filled` is the name Lake last filled in ("" when none). */
export function nameForFile(current: string, filled: string, file: string | null): string {
  if (!file) return current;
  if (current.trim() !== "" && current !== filled) return current;
  return datasetName(file);
}

/** The example the rename option names: the next free name after the chosen one. */
export function nextFreeExample(name: string): string {
  return `${name.trim() || "accounts"}_2`;
}

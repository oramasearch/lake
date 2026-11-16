// SPDX-License-Identifier: MIT

/** The exact call an Ask Orama proposal's Confirm runs (lake#942), laid out so every value can be
 * read before confirming (lake#1192). Each argument is three rows: its name, its value, then the
 * schema's description. The value has the card's full width and wraps at any character, so a UUID
 * or an intent sentence never shrinks to one character per line beside a long description, as it
 * did in the two-column `dl` grid. A value longer than a few lines (JSON, a long intent) is a
 * wrapped block shown in part, with a button to show all of it; the whole value is always in the
 * page, never cut. Written with createElement, not JSX, so `npm test` renders it
 * (tests/home.test.ts). */
import { createElement as h, useState } from "react";
import { shownValue, visibleText, type ProposalCall, type ProposalCallField } from "./home.ts";

/** Lines a long value shows before "Show all". */
export const VALUE_PREVIEW_LINES = 6;
/** Characters past which a one-line value is laid out as a wrapped block. */
const BLOCK_CHARACTERS = 80;

/** How one argument's value is shown: its text (shownValue), whether it is a block (several lines,
 * or long), and how many lines it has. */
export function fieldValueLayout(value: unknown): { text: string; block: boolean; lines: number; collapsible: boolean } {
  const text = shownValue(value);
  const lines = text.split("\n").length;
  const block = lines > 1 || text.length > BLOCK_CHARACTERS;
  return { text, block, lines, collapsible: lines > VALUE_PREVIEW_LINES };
}

function FieldValue({ field }: { field: ProposalCallField }) {
  const [open, setOpen] = useState(false);
  const layout = fieldValueLayout(field.value);
  if (!layout.block) return h("code", { className: "ask-proposal-value" }, layout.text);
  const collapsed = layout.collapsible && !open;
  return h(
    "div",
    { className: "ask-proposal-value-block" },
    h("pre", { className: collapsed ? "ask-proposal-value ask-proposal-value-collapsed" : "ask-proposal-value" }, h("code", null, layout.text)),
    layout.collapsible
      ? h(
          "button",
          { type: "button", className: "ask-proposal-more cursor-pointer border-0 bg-transparent p-0 text-[12px] text-action", "aria-expanded": open, onClick: () => setOpen((v) => !v) },
          open ? "Show less" : `Show all ${layout.lines} lines`,
        )
      : null,
  );
}

/** One argument: name, value, description, each on its own row. */
function Field({ field }: { field: ProposalCallField }) {
  return h(
    "li",
    { className: "ask-proposal-field" },
    h("code", { className: "ask-proposal-field-name" }, visibleText(field.name)),
    h(FieldValue, { field }),
    field.description ? h("p", { className: "ask-proposal-field-about" }, field.description) : null,
  );
}

/** The exact call Confirm runs, as Lake states it: the capability, then every input field with
 * its value and schema description (shownValue and visibleText, so nothing can hide). The person
 * reviews these, not the model's explanation, which the proposal shows apart from them. */
export function ProposalCallView({ call }: { call: ProposalCall }) {
  return h(
    "div",
    { className: "stack gap-1" },
    h(
      "p",
      { className: "m-0" },
      h("strong", null, "Confirm runs"),
      " ",
      h("code", null, call.capability),
      `${call.title ? ` (${call.title}${call.kind === "action" ? ", a change" : ""})` : ""} with exactly:`,
    ),
    call.fields.length
      ? h("ul", { className: "ask-proposal-fields" }, ...call.fields.map((field) => h(Field, { key: field.name, field })))
      : h("p", { className: "muted m-0" }, "no input"),
  );
}

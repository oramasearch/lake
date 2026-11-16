// SPDX-License-Identifier: MIT

/**
 * Formatting tests — run under a negative-offset time zone (Pacific), where a
 * date-only value read as UTC would show the day before and a typed
 * time read in the server's zone would move. `npm test`; the build runs it.
 */
process.env.TZ = "America/Vancouver";

import { test } from "node:test";
import assert from "node:assert/strict";
import { age, date, dateTime, dueFromDate, fromLocalInput, longDate, parseDate, periodLabel, relative, todayISO, toLocalInput } from "../src/lib/format.ts";

test("the time zone is Pacific", () => {
  assert.equal(new Date(2026, 9, 3).getTimezoneOffset(), 420);
});

test("a date-only value shows its own calendar date", () => {
  assert.equal(date("2026-10-03"), "Oct 3, 2026");
  assert.equal(dateTime("2026-10-03"), "Oct 3, 2026");
  assert.equal(longDate("2026-10-03"), "3 October 2026");
  assert.equal(parseDate("2026-10-03")!.getDate(), 3);
});

test("an instant shows in local time", () => {
  assert.equal(date("2026-10-04T02:00:00Z"), "Oct 3, 2026");
  assert.equal(dateTime("2026-10-04T00:00:00Z"), "Oct 3, 5:00 PM");
});

test("a typed time round-trips in the user's zone", () => {
  const sent = fromLocalInput("2026-10-03T17:00");
  assert.equal(sent, "2026-10-03T17:00:00-07:00");
  // The server stores the instant and returns it in its own zone (Eastern).
  const fromServer = "2026-10-03T20:00:00-04:00";
  assert.equal(new Date(sent!).getTime(), new Date(fromServer).getTime());
  assert.equal(dateTime(fromServer), "Oct 3, 5:00 PM");
  assert.equal(toLocalInput(fromServer), "2026-10-03T17:00");
  assert.equal(fromLocalInput(""), null);
});

test("a due date picked without a time is the end of that working day", () => {
  assert.equal(dueFromDate("2026-10-10"), "2026-10-10T17:00:00-07:00");
  assert.equal(date(dueFromDate("2026-10-10")), "Oct 10, 2026");
  assert.equal(dueFromDate(""), null);
});

test("today is the local calendar date, not UTC's", () => {
  // 21:00 Pacific on Oct 3 is already Oct 4 in UTC.
  assert.equal(todayISO(new Date(2026, 9, 3, 21, 0)), "2026-10-03");
});

test("a period key reads as words", () => {
  assert.equal(periodLabel("last_30d"), "last 30 days");
  assert.equal(periodLabel("last_1w"), "last 1 week");
  assert.equal(periodLabel("2026-10"), "Oct 2026");
  assert.equal(periodLabel(null), "—");
});

test("relative times never read '— ago' or 'in —'", () => {
  const now = new Date("2026-10-03T12:00:00-07:00").getTime();
  assert.equal(relative(null, now), "—");
  assert.equal(relative(undefined, now, "never"), "never");
  assert.equal(relative("2026-10-03T10:00:00-07:00", now), "2h ago");
  assert.equal(relative("2026-10-03T18:00:00-07:00", now), "in 6h");
  assert.equal(relative("2026-10-06T12:00:00-07:00", now), "in 3d");
  assert.equal(relative("2026-10-29T12:00:00-07:00", now), "Oct 29");
  assert.equal(age(null, now), "—");
  assert.equal(age("2026-10-01T12:00:00-07:00", now), "2d");
});

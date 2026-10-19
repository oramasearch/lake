/** Regions in the web app (lake#757): read from `server_info`, nothing when the Lake has no region.
 * `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { accountRegionSentence, currentRegion, orderSheetLinks, otherRegions, regionUrl, workspaceRegionLabel } from "../src/lib/regions.ts";

const production = {
  region: { name: "swedencentral", label: "Europe" },
  regions: [
    { name: "swedencentral", label: "Europe", host: "eu.lake.live" },
    { name: "eastus", label: "United States", host: "us.lake.live" },
  ],
};
const nonprod = { region: null, regions: [] };

test("the current region is server_info's region, and nothing on a Lake without one", () => {
  assert.deepEqual(currentRegion(production), { name: "swedencentral", label: "Europe" });
  assert.equal(currentRegion(nonprod), null);
  assert.equal(currentRegion({}), null);
  assert.equal(currentRegion(undefined), null);
  assert.deepEqual(currentRegion({ region: { name: "westus2", label: "" } }), { name: "westus2", label: "westus2" });
});

test("the other regions are every region but this one, in order", () => {
  assert.deepEqual(otherRegions(production), [{ name: "eastus", label: "United States", host: "us.lake.live" }]);
  assert.deepEqual(otherRegions({ ...production, region: { name: "eastus", label: "United States" } }), [
    { name: "swedencentral", label: "Europe", host: "eu.lake.live" },
  ]);
  assert.deepEqual(otherRegions(nonprod), []);
  // A list without a region of our own offers nothing: there is no "here" to leave.
  assert.deepEqual(otherRegions({ regions: production.regions }), []);
  assert.deepEqual(otherRegions({ region: production.region, regions: [{ name: "x", label: "X", host: "" }] }), []);
});

test("another region is its own host's front page", () => {
  assert.equal(regionUrl({ name: "eastus", label: "United States", host: "us.lake.live" }), "https://us.lake.live/");
});

test("sign-in and sign-up say which region the account is in, and nothing without a region", () => {
  assert.equal(
    accountRegionSentence(production, "sign-up"),
    "Your account is created in Europe. Each region is a separate Lake with its own accounts, and your workspaces stay in Europe.",
  );
  assert.equal(accountRegionSentence(production, "sign-in"), "You are signing in to Lake in Europe. An account belongs to the region it was created in.");
  assert.equal(accountRegionSentence(nonprod, "sign-up"), null);
  assert.equal(accountRegionSentence(undefined, "sign-in"), null);
});

test("a workspace shows the region of the Lake it was created on", () => {
  assert.equal(workspaceRegionLabel(production), "Europe");
  assert.equal(workspaceRegionLabel(nonprod), null);
});

test("the region selector leads back to the order sheet where there is one (lake#831)", () => {
  assert.deepEqual(orderSheetLinks({ ...production, order_sheet_url: "https://lake.live/" }), {
    newSetup: "https://lake.live/",
    onPrem: "https://lake.live/order/on-prem",
  });
  // A configured URL with a path still leads to the sheet at its origin.
  assert.deepEqual(orderSheetLinks({ order_sheet_url: "https://lake.live/pricing" }), {
    newSetup: "https://lake.live/",
    onPrem: "https://lake.live/order/on-prem",
  });
  assert.equal(orderSheetLinks({ ...production, order_sheet_url: null }), null);
  assert.equal(orderSheetLinks(production), null);
  assert.equal(orderSheetLinks({ order_sheet_url: "http://lake.live/" }), null);
  assert.equal(orderSheetLinks({ order_sheet_url: "javascript:alert(1)" }), null);
  assert.equal(orderSheetLinks(undefined), null);
});

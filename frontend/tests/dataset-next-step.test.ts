/** A dataset's page leads to a model (lake#818): Train a model on this opens New project for the
 * dataset, and creating the project opens Build pipeline with the dataset chosen under From data.
 * `npm test`; the build runs it. */
import { test } from "node:test";
import assert from "node:assert/strict";
import { buildFromDatasetPath, datasetChoices, datasetSource, projectNameFor, trainFromDatasetPath } from "../src/pages/design/logic.ts";

const params = (path: string) => new URLSearchParams(path.slice(path.indexOf("?") + 1));

test("Train a model on this opens Projects with New project open for the dataset", () => {
  const path = trainFromDatasetPath("datasets", "customers");
  assert.ok(path.startsWith("/design/projects?"));
  assert.equal(params(path).get("new"), "1");
  assert.equal(params(path).get("dataset"), "datasets.customers");
  assert.equal(params(trainFromDatasetPath("", "customers")).get("dataset"), "datasets.customers", "the default database");
});

test("the dataset is named the way From data names its choices", () => {
  const choices = datasetChoices([{ database: "datasets", name: "customers" }, { database: "sales", name: "orders" }]);
  assert.ok(choices.some((c) => c.path === datasetSource("datasets", "customers")));
  assert.ok(choices.some((c) => c.path === datasetSource("sales", "orders")));
});

test("creating the project opens it with Build pipeline open on the dataset", () => {
  const path = buildFromDatasetPath("customers-model", "datasets.customers");
  assert.ok(path.startsWith("/design/project?"));
  assert.equal(params(path).get("name"), "customers-model");
  assert.equal(params(path).get("build"), "1");
  assert.equal(params(path).get("dataset"), "datasets.customers");
  // Without a dataset New project opens the project as before.
  assert.equal(buildFromDatasetPath("retention-review", ""), "/design/project?name=retention-review");
});

test("the suggested project name follows a project's name rule", () => {
  const rule = /^[a-z0-9][a-z0-9_\-]{0,62}$/;
  assert.equal(projectNameFor("datasets.customers"), "customers-model");
  assert.equal(projectNameFor("datasets.My_Data_2024"), "my_data_2024-model");
  assert.equal(projectNameFor("datasets._tmp"), "tmp-model");
  assert.equal(projectNameFor(""), "");
  for (const name of ["datasets.customers", "x." + "a".repeat(80), "datasets.__x__"]) assert.match(projectNameFor(name), rule);
});

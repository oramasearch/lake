// SPDX-License-Identifier: MIT

/**
 * Design → Solutions with a workspace's own solutions beside Orama's (lake#597,
 * src/pages/design/logic.ts): which kind an entry is, the toolbar's filter, the methodology field a
 * person types, the input the form sends, and what Start as a Project records.
 */
import assert from "node:assert/strict";
import {test} from "node:test";

import {
    filterSolutions,
    formatMethodologies,
    parseMethodologies,
    pipelineReference,
    solutionForm,
    solutionInput,
    solutionSource,
    sourceLabel,
    startDecision,
    startIntent,
} from "../src/pages/design/logic.ts";

const churn = {
    id : "S-1",
    name : "Churn & Retention",
    source : "orama",
    editable : false,
    promise : "Find who may leave.",
    runnable : true,
    status : "catalogue"
};
const sketch = {
    id : "S-20",
    name : "Pricing",
    source : "orama",
    promise : "Set prices.",
    runnable : false,
    status : "sketch"
};
const late = {
    id : "0b6c3c5e-1f0a-4c55-9d1e-2b7f0d1e9a10",
    source : "workspace",
    editable : true,
    name : "Late payments",
    problem : "Invoices are paid late.",
    decision : "Who collections calls first.",
    measurement : {metric : "days sales outstanding", baseline : "52", target : "40"},
    methodologies : [
        {id : "M-1", minimum_level : 1},
        {id : "M-3", minimum_level : 2},
    ],
    pipeline : "collections/late-payers",
    notes : null,
};

test("an entry is Orama's unless lake marks it the workspace's own", () => {
    assert.equal(solutionSource(churn), "orama");
    assert.equal(solutionSource({id : "S-2"}), "orama", "an engine read carries no source");
    assert.equal(solutionSource(late), "workspace");
    assert.equal(sourceLabel(churn), "Orama");
    assert.equal(sourceLabel(late), "Yours");
});

test("the toolbar filters by source, Orama's run state and a search", () => {
    const all = [ churn, sketch, late ];
    assert.deepEqual(filterSolutions(all, {source : null, state : null, search : ""}), all);
    assert.deepEqual(filterSolutions(all, {source : "workspace", state : null, search : ""}),
                     [ late ]);
    assert.deepEqual(filterSolutions(all, {source : "orama", state : null, search : ""}),
                     [ churn, sketch ]);
    assert.deepEqual(filterSolutions(all, {source : null, state : "Runs today", search : ""}),
                     [ churn ],
                     "a run state keeps only Orama's");
    assert.deepEqual(filterSolutions(all, {source : null, state : null, search : "COLLECTIONS"}),
                     [ late ],
                     "the decision is searched");
    assert.deepEqual(filterSolutions(all, {source : "orama", state : null, search : "late"}), []);
});

test("methodologies are typed as ids with an optional level", () => {
    assert.equal(formatMethodologies(late.methodologies), "M-1, M-3:2");
    assert.equal(formatMethodologies(undefined), "");
    assert.deepEqual(parseMethodologies(" M-1, M-3:2,\nM-5 : 4 ,"), [
        {id : "M-1", minimum_level : 1},
        {id : "M-3", minimum_level : 2},
        {id : "M-5", minimum_level : 4},
    ]);
    assert.deepEqual(parseMethodologies(""), []);
    assert.throws(() => parseMethodologies("M-1:5"), /minimum level must be 1 to 4/);
    assert.throws(() => parseMethodologies("M-1, M-1:2"), /listed twice/);
    assert.throws(() => parseMethodologies("churn model"), /not a methodology id/);
    assert.deepEqual(parseMethodologies(formatMethodologies(late.methodologies)),
                     late.methodologies,
                     "round trip");
});

test("the form starts from a solution and sends every field, emptied ones cleared", () => {
    const form = solutionForm(late);
    assert.deepEqual(form, {
        name : "Late payments",
        problem : "Invoices are paid late.",
        decision : "Who collections calls first.",
        metric : "days sales outstanding",
        baseline : "52",
        target : "40",
        methodologies : "M-1, M-3:2",
        pipeline : "collections/late-payers",
        notes : "",
    });
    assert.deepEqual(solutionInput({...form, name : " Late payers ", target : "", notes : " "}), {
        name : "Late payers",
        problem : "Invoices are paid late.",
        decision : "Who collections calls first.",
        measurement : {metric : "days sales outstanding", baseline : "52", target : ""},
        methodologies : late.methodologies,
        pipeline : "collections/late-payers",
        notes : "",
    });
    assert.deepEqual(solutionForm(null).name, "");
});

test("a pipeline reference links to its pipeline; a template stays text", () => {
    assert.deepEqual(pipelineReference("collections/late-payers"),
                     {project : "collections", graph : "late-payers"});
    assert.deepEqual(pipelineReference(" collections/Late payers v2 "),
                     {project : "collections", graph : "Late payers v2"});
    assert.equal(pipelineReference("Methodologies in order: M-2, M-1"), null);
    assert.equal(pipelineReference("a/b/c"), null);
    assert.equal(pipelineReference(""), null);
});

test("Start as a Project records the decision with its measure, and builds from it", () => {
    assert.equal(startDecision(late),
                 "Who collections calls first. Measured by days sales outstanding, from 52 to 40.");
    assert.equal(startDecision({...late, measurement : {metric : "DSO", target : "40"}}),
                 "Who collections calls first. Measured by DSO, to 40.");
    assert.equal(startDecision({decision : "Call first."}), "Call first.");
    assert.equal(startIntent(late),
                 "Who collections calls first. The problem: Invoices are paid late.");
});

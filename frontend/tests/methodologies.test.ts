// SPDX-License-Identifier: MIT

/**
 * Design → Methodologies with a workspace's own methodologies from Python code beside Orama's
 * (lake#655, src/pages/design/methodologies.ts): who an entry is from, the toolbar's filter, a
 * workspace methodology's validation state, Validate's findings, a run's tone, and the parameters
 * Train sends; and who wrote a New Methodology draft (lake#688).
 */
import assert from "node:assert/strict";
import {test} from "node:test";

import {
    declaredParameters,
    draftAuthor,
    failedFindings,
    filterMethodologies,
    heldOutMetrics,
    methodologyBadge,
    methodologySource,
    needsTarget,
    orderedFiles,
    parseParams,
    runIsLive,
    runPhaseTone,
    runQueuedReason,
    sortedFindings,
    validationState,
} from "../src/pages/design/methodologies.ts";

const m1 = {
    id : "M-1",
    name : "Tabular supervised learning",
    family : "The tabular decision core",
    source : "orama",
    editable : false,
    runnable : true,
    outputs : [ "model" ]
};
const m3 = {
    id : "M-3",
    name : "Segmentation",
    family : "Identity & structure",
    runnable : true
};
const late = {
    id : "W-0a1b2c3d",
    name : "Late payers",
    description : "Who pays late",
    source : "workspace",
    editable : true,
    code : {repository_id : "r", revision : "c2", path : ""},
    validation : {status : "passed", revision : "c2"},
    interface : {
        input_kinds : [ "tabular" ],
        target : {required : true, kind : "binary"},
        parameters : [
            {name : "epochs", kind : "int", default : 200, minimum : 1},
            {name : "rate", kind : "float", default : 0.1},
            {name : "loss", kind : "categorical", default : "log", choices : [ "log", "hinge" ]},
            {name : "balanced", kind : "bool", default : false},
        ],
        metrics : [ "auc" ],
    },
};

test("an entry is Orama's or the workspace's own, by its marker or its id", () => {
    assert.equal(methodologySource(m1), "orama");
    assert.equal(methodologySource(m3), "orama", "an engine read without source is Orama's");
    assert.equal(methodologySource(late), "workspace");
    assert.equal(methodologySource({id : "W-0a1b2c3d"}), "workspace");
    assert.equal(methodologyBadge(m1), "By Orama");
    assert.equal(methodologyBadge(late), "Yours");
});

test("the toolbar filters by source, family and a search", () => {
    const all = [ m1, m3, late ];
    assert.deepEqual(filterMethodologies(all, {source : "workspace", family : null, search : ""}),
                     [ late ]);
    assert.deepEqual(filterMethodologies(all, {source : "orama", family : null, search : ""}),
                     [ m1, m3 ]);
    assert.deepEqual(
        filterMethodologies(all, {source : null, family : "Identity & structure", search : ""}),
        [ m3 ]);
    assert.deepEqual(filterMethodologies(all, {source : null, family : null, search : "pays late"}),
                     [ late ]);
    assert.deepEqual(filterMethodologies(all, {source : null, family : null, search : "m-1"}),
                     [ m1 ]);
});

test("a workspace methodology is usable only once Validate passed for its current code", () => {
    assert.equal(validationState(late).label, "Usable");
    assert.equal(validationState({...late, code : {revision : "c3"}}).label, "Validate again");
    assert.equal(
        validationState({...late, validation : {status : "running", revision : "c2"}}).label,
        "Validating");
    assert.equal(validationState({...late, validation : {status : "failed", revision : "c2"}}).tone,
                 "bad");
    assert.equal(validationState({...late, validation : undefined}).label, "Not validated");
});

test("Validate's findings list the failed ones first", () => {
    const findings = [
        {check : "manifest", ok : true},
        {check : "entrypoint.score", ok : false, message : "no score"},
    ];
    assert.equal(sortedFindings(findings)[0].check, "entrypoint.score");
    assert.equal(failedFindings(findings), 1);
    assert.deepEqual(sortedFindings(null), []);
});

test("a run says whether it is under way and how it ended", () => {
    assert.equal(runIsLive({phase : "Running"}), true);
    assert.equal(runIsLive({phase : "Succeeded"}), false);
    assert.equal(runIsLive({phase : "Queued"}), true, "a queued run is still under way");
    assert.equal(runPhaseTone({phase : "Queued"}), "neutral");
    assert.equal(
        runQueuedReason({phase : "Queued", message : "Queued: 2 of this workspace's runs are running."}),
        "Queued: 2 of this workspace's runs are running.");
    assert.equal(runQueuedReason({phase : "Running", message : "Running your code."}), "");
    assert.equal(runPhaseTone({phase : "Succeeded", outcome : "passed"}), "ok");
    assert.equal(runPhaseTone({phase : "Succeeded", outcome : "failed"}),
                 "bad",
                 "the code's outcome decides");
    assert.equal(runPhaseTone({phase : "Stopped"}), "warn");
    assert.deepEqual(
        heldOutMetrics({result : {metrics : {in_sample_auc : 0.9, held_out_auc : 0.8}}}),
        [ [ "held_out_auc", 0.8 ] ]);
});

test("Train sends each parameter as its kind, blanks left to the default", () => {
    const parameters = declaredParameters(late);
    assert.equal(parameters.length, 4);
    assert.equal(needsTarget(late), true);
    assert.deepEqual(
        parseParams(parameters, {epochs : "50", rate : "", loss : "hinge", balanced : "true"}),
        {epochs : 50, loss : "hinge", balanced : true});
    assert.throws(() => parseParams(parameters, {epochs : "1.5"}), /epochs takes a whole number/);
    assert.throws(() => parseParams(parameters, {loss : "squared"}),
                  /loss takes one of log, hinge/);
});

test("files are read manifest, entrypoint, lock, then the rest", () => {
    const files = [
        {path : "notes.md"},
        {path : "requirements.lock"},
        {path : "methodology.py"},
        {path : "methodology.json"}
    ];
    assert.deepEqual(
        orderedFiles(files).map((f) => f.path),
        [ "methodology.json", "methodology.py", "requirements.lock", "notes.md" ],
    );
});

test("a draft says whether Ask Orama wrote it, or why Lake's template is offered", () => {
    assert.deepEqual(draftAuthor({drafted_by : "orama"}, true),
                     {badge : "Written by Ask Orama", tone : "ai", note : "", failing : []});
    const refused = draftAuthor({
        drafted_by : "template",
        orama : {
            offered : false,
            reason : "Ask Orama's draft did not pass the checks Validate runs first.",
            findings : [
                {check : "packages", ok : false, message : "numpy==2.1.3 needs --hash"},
                {check : "manifest", ok : true, message : "reads"}
            ]
        }
    },
                                true);
    assert.equal(refused.badge, "Lake's template");
    assert.equal(refused.tone, "warning");
    assert.match(refused.note, /did not pass/);
    assert.deepEqual(refused.failing.map((f) => f.check), [ "packages" ]);
    assert.match(draftAuthor({drafted_by : "template"}, false).note, /not running on this Lake/);
    assert.equal(draftAuthor({drafted_by : "template"}, true).note, "");
});

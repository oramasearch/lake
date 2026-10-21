/**
 * Design → Agents and Train → Agents (lake#653, src/lib/agents.ts): Orama's agents are read-only
 * and the workspace's own editable, the "By Orama" / "Yours" filter, Duplicate (of one of Orama's
 * and of one's own), New agent's input, the skills read from Lake's OpenAPI rather than written
 * down, the revisions' changes, and Train → Agents' filter to the agents with something to train,
 * with promote and roll back of a trained version, and the workspace streams that start an agent
 * (lake#677).
 */
import assert from "node:assert/strict";
import {readFileSync} from "node:fs";
import {test} from "node:test";

import {
    AGENT_SKILLS,
    BLANK_AGENT,
    OFFERED_SKILLS,
    agentForm,
    agentInput,
    agentSource,
    bestVersion,
    copyName,
    currentVersion,
    definitionOf,
    duplicateInput,
    duplicateLibraryForm,
    editable,
    experimentAsk,
    filterAgents,
    formProblem,
    libraryState,
    talkToAgents,
    ownAgents,
    revisionChanges,
    streamsToSet,
    triggerStreams,
    runFigures,
    scoreSeries,
    skillChoices,
    skillLabel,
    sourceLabel,
    switchInput,
    switchKind,
    trainable,
    trainableBecause,
    trainedVersions,
    trainingInput,
} from "../src/lib/agents.ts";

const library = {
    key : "agent-project",
    name : "Project Agent",
    purpose : "Builds a pipeline from what you want to predict.",
    used_in : "Design → a project",
    runs_as : "service",
    chat : true,
    status : "Available",
};
const seeded = {id : "a-0", name : "Ask Orama", kind : "orama", runtime : "service"};
const native = {
    id : "a-1",
    name : "Renewal watcher",
    kind : "custom",
    runtime : "native",
    model : "/data/models/base-agent",
    model_name : "Orama base model",
    purpose : "Flag renewals at risk",
    manifesto : "Answer from company data only.",
    goal : "Every at-risk renewal is flagged.",
    skills : [ "search", "summarize" ],
    work_area : {kind : "project", detail : "churn"},
    pipelines : [ {project : "churn", graph : "training", version : "2026-10-01T00:00:00Z"} ],
    trigger : "",
    status : "idle",
    created_at : "2026-10-01T00:00:00Z",
    updated_at : "2026-10-02T00:00:00Z",
    versions : [],
};
const frontier = {
    id : "a-2",
    name : "Digest writer",
    kind : "custom",
    runtime : "python",
    model_name : "gpt-4o",
    purpose : "Summarize each finished run",
    definition : {source : "zetta", instructions : "# Digest writer", files : [ {path : "agent.py", contents : "x"} ], entrypoint : "agent:main"},
    trigger : "every monday at 09:00",
};

test("Orama's agents are read-only and the workspace's own are editable", () => {
    assert.equal(agentSource(library), "orama", "a library entry has a key and no engine id");
    assert.equal(agentSource(seeded), "orama", "an engine record of kind orama is Orama's");
    assert.equal(agentSource(native), "workspace");
    assert.equal(editable(library), false);
    assert.equal(editable(seeded), false);
    assert.equal(editable(native), true);
    assert.equal(sourceLabel(library), "By Orama");
    assert.equal(sourceLabel(native), "Yours");
    assert.deepEqual(ownAgents([ seeded, native, frontier, null, "x" ]).map((a) => a.id), [ "a-1", "a-2" ]);
});

test("the filter keeps By Orama or Yours, and searches the name, purpose and where it is used", () => {
    const all = [ native, frontier, library ];
    assert.deepEqual(filterAgents(all, {source : "orama", search : ""}).map((a) => a.name), [ "Project Agent" ]);
    assert.deepEqual(filterAgents(all, {source : "workspace", search : ""}).map((a) => a.name), [ "Renewal watcher", "Digest writer" ]);
    assert.deepEqual(filterAgents(all, {source : null, search : "RENEWALS"}).map((a) => a.name), [ "Renewal watcher" ]);
    assert.deepEqual(filterAgents(all, {source : null, search : "a project"}).map((a) => a.name), [ "Project Agent" ]);
    assert.deepEqual(filterAgents(all, {source : "workspace", search : "pipeline"}), []);
});

test("Duplicate of one of Orama's prefills New agent from the library, never its key", () => {
    const form = duplicateLibraryForm(library, [ "Project Agent (copy)" ]);
    assert.equal(form.name, "Project Agent (copy 2)");
    assert.equal(form.purpose, library.purpose);
    assert.match(form.instructions, /Builds a pipeline/);
    const input = agentInput(form);
    assert.equal(input.kind, "custom", "a duplicate is the workspace's own");
    assert.equal(input.runtime, "native");
    assert.equal(input.model, "", "an empty model: Lake gives it the base model");
    assert.equal("key" in input, false);
});

test("Duplicate of one's own copies its definition under a new name, without its id or state", () => {
    const input = duplicateInput({...native, is_default : true}, [ "Renewal watcher (copy)" ]);
    assert.equal(input.name, "Renewal watcher (copy 2)");
    assert.equal(input.kind, "custom");
    for (const field of [ "id", "status", "versions", "created_at", "updated_at", "is_default" ]) assert.equal(field in input, false, field);
    assert.deepEqual(input.pipelines, [ {project : "churn", graph : "training"} ], "the engine pins the approval again");
    assert.deepEqual(input.skills, [ "search", "summarize" ]);
    assert.equal(copyName("Digest writer (copy)"), "Digest writer (copy)", "a copy of a copy is named from the original");
});

test("New agent sends the Studio shape; the model decides where it runs", () => {
    const blank = agentForm(null);
    assert.deepEqual(blank, BLANK_AGENT);
    assert.notEqual(blank.skills, BLANK_AGENT.skills, "each form has its own lists");
    assert.deepEqual(formProblem(blank), {field : "name", message : "An agent needs a name."});
    const form = {...blank, name : " Watcher ", purpose : "Flag renewals", instructions : "Be brief.", skills : [ "search" ], pipelines : [ {project : "churn", graph : "training"} ], trigger : "every day"};
    assert.equal(formProblem(form), null);
    const base = agentInput(form);
    assert.equal(base.name, "Watcher");
    assert.equal(base.runtime, "native");
    assert.equal(base.system_prompt, "Be brief.");
    assert.equal(base.manifesto, "Be brief.");
    assert.deepEqual(base.work_area, {kind : "workspace", detail : ""});
    assert.deepEqual(base.pipelines, [ {project : "churn", graph : "training"} ]);
    const python = agentInput({...form, model : "frontier"}, {frontierModel : "gpt-5"});
    assert.equal(python.runtime, "python");
    assert.equal(python.model_name, "gpt-5");
    assert.equal(python.definition.source, "zetta");
    assert.match(python.definition.instructions, /^# Watcher\n\nFlag renewals\n\n## How you work\n\nBe brief\./);
    const trained = agentInput({...form, model : "workspace", workspaceModel : "model:m-9"});
    assert.equal(trained.model, "model:m-9");
    assert.deepEqual(formProblem({...form, model : "workspace", workspaceModel : ""}), {field : "model", message : "Choose one of this workspace's models."});
});

test("Edit keeps what the form does not show: a trained model, a Python agent's files", () => {
    const form = agentForm(native);
    assert.equal(form.model, "base");
    assert.equal(form.workArea, "project");
    assert.equal(form.workAreaDetail, "churn");
    assert.equal(agentInput(form, {existing : native}).model, native.model, "the checkpoint it runs is kept");
    const edited = agentInput({...agentForm(frontier), purpose : "Summarize runs"}, {existing : frontier});
    assert.deepEqual(edited.definition.files, frontier.definition.files);
    assert.equal(edited.definition.entrypoint, "agent:main");
    assert.equal(edited.model_name, "gpt-4o");
});

test("the skills are the runtime's, vendored from agent-runtime's skills.json, never written here", () => {
    const vendored = JSON.parse(readFileSync(new URL("../src/pages/train/agent-skills.json", import.meta.url), "utf8"));
    assert.deepEqual(OFFERED_SKILLS, vendored.map(([key]: [string, string]) => key));
    assert.ok(OFFERED_SKILLS.length >= 12, "agent-runtime#7's twelve");
    for (const skill of [ "search", "drives", "transcripts", "pipelines", "usage" ]) assert.ok(OFFERED_SKILLS.includes(skill), skill);
    assert.deepEqual(AGENT_SKILLS, vendored);
    // The engine accepts every one: create_agent's schema in Lake's vendored engine OpenAPI.
    const engine = JSON.parse(readFileSync(new URL("../../src/api/engine_openapi.json", import.meta.url), "utf8"));
    // A skill is one of the named ones or an MCP connection's, mcp:<prefix> (zetta#1267, lake#652).
    const item = engine.capabilities.create_agent.input_schema.properties.skills.items;
    const named = item.anyOf.find((choice: {enum?: string[]}) => Array.isArray(choice.enum));
    const mcp = item.anyOf.find((choice: {pattern?: string}) => typeof choice.pattern === "string");
    for (const skill of OFFERED_SKILLS) assert.ok(named.enum.includes(skill), `${skill} is accepted by create_agent`);
    assert.ok(new RegExp(mcp.pattern).test("mcp:ticket_desk"), "an MCP connection's skill is accepted");
    assert.ok(!new RegExp(mcp.pattern).test("mcp:Ticket-Desk"), "only a prefix Lake makes");
    assert.equal(skillLabel("search"), vendored.find(([key]: [string, string]) => key === "search")[1]);
    assert.equal(skillLabel("read-drives"), "Read drives", "a skill the runtime no longer lists is shown by its name");
    assert.equal(skillLabel("mcp:github"), "MCP: github");
    assert.deepEqual(skillChoices([ "search" ], [ "train", "search" ]), [ "search", "train" ], "a skill the agent has is kept");
});

test("a revision says what changed since the one before it", () => {
    const first = definitionOf(native);
    assert.deepEqual(revisionChanges(null, first), [ "Created" ]);
    assert.deepEqual(revisionChanges(first, first), []);
    const next = {...first, manifesto : "Be brief.", system_prompt : "Be brief.", skills : [ "search" ], trigger : "daily"};
    assert.deepEqual(revisionChanges(first, next), [ "instructions", "skills", "schedule" ]);
    for (const field of [ "id", "status", "versions", "created_at", "updated_at", "training" ]) assert.equal(field in first, false, field);
});

test("Train → Agents lists only agents with something to train", () => {
    assert.equal(trainable(native), true, "a native agent runs a model the engine trains");
    assert.equal(trainable(frontier), false, "a frontier model with no pipeline has nothing to train");
    assert.equal(trainable({...frontier, pipelines : [ {project : "p", graph : "g"} ]}), true, "a pipeline it calls is trained");
    assert.equal(trainable({...frontier, training : {cadence : "weekly"}}), true, "a training schedule");
    assert.equal(trainable({...frontier, training : {cadence : "manual"}}), false, "manual is no schedule");
    assert.equal(trainable({...native, kind : "orama"}), false, "Orama's are not trained here");
    assert.equal(trainable(library), false);
    assert.deepEqual(trainableBecause({...native, training : {cadence : "on_change"}}), [ "model", "1 pipeline", "retrains on change" ]);
});

test("trained versions: the one it runs, the best, promote and roll back", () => {
    const status = {
        versions : [
            {checkpoint : "/m/v2", created_at : "2026-10-03T00:00:00Z", corpus : "renewals", eval_score : 0.81},
            {checkpoint : "/m/v1", created_at : "2026-10-01T00:00:00Z", corpus : "", eval_score : -1},
            {checkpoint : "/m/v3", created_at : "2026-10-05T00:00:00Z", corpus : "renewals", eval_score : 0.78},
        ]
    };
    const versions = trainedVersions(status);
    assert.deepEqual(versions.map((v) => v.checkpoint), [ "/m/v1", "/m/v2", "/m/v3" ], "oldest first");
    assert.equal(versions[0].eval_score, null, "below 0 is not evaluated");
    const agent = {...native, model : "/m/v2"};
    const current = currentVersion(agent, versions);
    assert.equal(current?.checkpoint, "/m/v2");
    assert.equal(bestVersion(versions)?.checkpoint, "/m/v2");
    assert.equal(switchKind(versions[2], current), "promote");
    assert.equal(switchKind(versions[0], current), "rollback");
    assert.equal(switchKind(versions[1], current), null);
    assert.equal(switchKind(versions[0], null), "promote", "an agent on no trained version promotes any");
    assert.deepEqual(switchInput("a-1", versions[0]), {id : "a-1", model : "/m/v1", model_version : "2026-10-01T00:00:00Z"});
    const [xs, ys] = scoreSeries(versions);
    assert.deepEqual(ys, [ 0.81, 0.78 ]);
    assert.equal(xs.length, 2);
    assert.deepEqual(trainedVersions(undefined), []);
});

test("run figures: success rate and median latency from finished runs", () => {
    const figures = runFigures([
        {phase : "Succeeded", created_at : "2026-10-01T00:00:00Z", finished_at : "2026-10-01T00:00:10Z"},
        {phase : "Failed", created_at : "2026-10-01T00:00:00Z", finished_at : "2026-10-01T00:00:30Z"},
        {phase : "Succeeded", created_at : "2026-10-01T00:00:00Z"},
        {phase : "Running", created_at : "2026-10-01T00:00:00Z"},
    ]);
    assert.equal(figures.finished, 3);
    assert.equal(figures.succeeded, 2);
    assert.equal(Math.round((figures.successRate ?? 0) * 100), 67);
    assert.equal(figures.medianSeconds, 10);
    assert.deepEqual(runFigures([]), {finished : 0, succeeded : 0, successRate : null, medianSeconds : null});
});

test("the training schedule and the experiment ask", () => {
    assert.deepEqual(trainingInput({cadence : "weekly", corpus : " renewals ", base_checkpoint : "", steps : "200"}), {cadence : "weekly", corpus : "renewals", steps : 200});
    assert.deepEqual(trainingInput({cadence : "", corpus : "", base_checkpoint : "", steps : "-1"}), {cadence : "manual"});
    const versions = trainedVersions({versions : [ {checkpoint : "/m/v1", created_at : "2026-10-01T00:00:00Z", eval_score : 0.7} ]});
    const ask = experimentAsk(native, "versions", "", versions);
    assert.match(ask, /"Renewal watcher" \(id a-1\)/);
    assert.match(ask, /compares its trained versions \(\/m\/v1\)/);
    assert.match(ask, /Its goal: Every at-risk renewal is flagged\./);
    assert.match(experimentAsk(native, "prompts", "fewer false alarms", []), /variations of its instructions\. What it should improve: fewer false alarms\./);
    assert.doesNotMatch(ask, /\bstud(y|ies)\b/i, "an experiment, never a study");
});

test("the streams an agent may start from: the workspace's own, never a system stream", () => {
    const groups = [
        {zetta: "workspace", topics: [
            {topic: "topic:orders", system: false},
            {topic: "agent-events", system: true},
            {topic: "app-events.shop", system: false},
            {topic: "invoices", system: false},
            {topic: "orders", system: false},
        ]},
        {zetta: "bus", topics: [{topic: "agent-tasks.agent-project", system: true}, {topic: "bus-only"}]},
    ];
    assert.deepEqual(triggerStreams(groups), ["invoices", "orders"]);
    assert.deepEqual(triggerStreams(undefined), []);
});

test("saving sends the streams only when they change, and none for an agent off a frontier model", () => {
    const form = {...BLANK_AGENT, name: "Watcher", purpose: "Watch orders", model: "frontier" as const, streams: ["orders"]};
    assert.deepEqual(streamsToSet(form), ["orders"], "a new agent sends what it chose");
    assert.equal(streamsToSet({...form, streams: []}), null, "nothing chosen, nothing before: nothing sent");
    assert.equal(streamsToSet(form, ["orders"]), null, "unchanged");
    assert.deepEqual(streamsToSet({...form, streams: ["orders", "invoices"]}, ["orders"]), ["orders", "invoices"]);
    assert.deepEqual(streamsToSet({...form, streams: []}, ["orders"]), [], "all removed");
    assert.deepEqual(streamsToSet({...form, model: "base"}, ["orders"]), [], "a native agent loses its streams");
    assert.equal(streamsToSet({...form, model: "base"}), null);
    assert.deepEqual(agentForm({name: "x", runtime: "python"}).streams, [], "streams are not on the engine's record");
});

test("Talk to lists every library agent by its library name and marks the ones not running (lake#763, lake#788)", () => {
    const rows = [
        {key: "agent-ask-orama", name: "Ask Orama", purpose: "Answers questions.", used_in: "Everywhere", chat: true, deployed: true},
        {key: "agent-project", name: "Project Agent", purpose: "Builds pipelines.", used_in: "Design", chat: true, deployed: true},
        {key: "agent-creator", name: "Agent Creator Agent", purpose: "Designs agents.", used_in: "Design → Agents", chat: true, deployed: false},
        // An older Lake still flags it `internal`: it is listed all the same (lake#788).
        {key: "agent-eval-generator", name: "Eval Generator Agent", purpose: "Creates an eval for you.", used_in: "Ask Orama", chat: true, deployed: true, internal: true},
    ];
    const own = [{id: "a-1", name: "Renewal watcher", kind: "custom", purpose: "Watches renewals."}, {id: "a-0", kind: "orama", name: "Seeded"}];
    const agents = talkToAgents(rows, own, "");
    assert.deepEqual(agents.map((a) => a.id), ["agent-ask-orama", "agent-project", "agent-creator", "agent-eval-generator", "a-1"]);
    assert.deepEqual(agents[2], {id: "agent-creator", name: "Agent Creator Agent", purpose: "Designs agents.", usedIn: "Design → Agents", running: false});
    assert.deepEqual(agents[3], {id: "agent-eval-generator", name: "Eval Generator Agent", purpose: "Creates an eval for you.", usedIn: "Ask Orama", running: true});
    assert.equal(agents[1].purpose, "Builds pipelines.");
    assert.equal(agents[4].purpose, "Watches renewals.", "your own agent shows its own description");
    assert.equal(talkToAgents(undefined, undefined, "")[0].name, "Loading…", "no name is written down while the library loads");
    assert.deepEqual(talkToAgents(rows, undefined, "a-9").map((a) => a.id).at(-1), "a-9", "the agent in the URL stays listed");
});

test("every catalogue agent is in Talk to, and can be picked, when it runs (lake#788)", () => {
    const keys = ["agent-ask-orama", "agent-project", "agent-application", "agent-experiments", "agent-autosuggest", "agent-creator",
        "agent-eval-generator", "agent-autosuggest-corpus", "agent-project-corpus"];
    const rows = keys.map((key) => ({key, name: key, purpose: `${key} does one job.`, chat: true, deployed: true, status: "Available"}));
    const agents = talkToAgents(rows, [], "");
    assert.deepEqual(agents.map((a) => a.id), keys);
    assert.ok(agents.every((a) => a.running), "each one that runs can be picked");
    for (const row of rows) assert.equal(libraryState(row).talkable, true, `${row.key} is offered to talk to`);
});

test("Design → Agents shows whether this Lake runs each of Orama's agents, and none is a testing agent (lake#763, lake#788)", () => {
    assert.deepEqual(libraryState({status: "Available", chat: true}), {label: "Available", running: true, talkable: true});
    assert.deepEqual(libraryState({status: "Not running on this Lake", chat: true}), {label: "Not running on this Lake", running: false, talkable: false});
    assert.deepEqual(libraryState({status: "Available", chat: true, internal: true}), {label: "Available", running: true, talkable: true}, "an older Lake's flag changes nothing");
});

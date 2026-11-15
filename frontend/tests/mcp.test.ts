/**
 * Data → MCP Connections (lake#652, src/pages/mcp/logic.ts): the "Try it" form a tool's own JSON
 * Schema makes and the arguments its values make, the prefix shown while a name is typed, the
 * sign-in callback's state, the words statuses and calls read in, and what the re-export switch says.
 */
import assert from "node:assert/strict";
import {test} from "node:test";

import {
    argsOf,
    argumentsFrom,
    envNameProblem,
    exportWarning,
    formFields,
    hostOf,
    prefixOf,
    resultText,
    signInState,
    statusLabel,
    statusTone,
    stdioCommandLine,
    viaLabel,
} from "../src/pages/mcp/logic.ts";

const ticket = {
    type : "object",
    properties : {
        title : {type : "string", description : "What is wrong"},
        priority : {type : "string", enum : [ "low", "high" ]},
        points : {type : "integer"},
        weight : {type : [ "number", "null" ]},
        urgent : {type : "boolean"},
        labels : {type : "array", items : {type : "string"}},
    },
    required : [ "title", "urgent" ],
};

test("a tool's schema becomes one field per property, each with its own control", () => {
    const fields = formFields(ticket);
    assert.deepEqual(fields.map((f) => [f.name, f.kind, f.required]), [
        [ "title", "text", true ],
        [ "priority", "choice", false ],
        [ "points", "integer", false ],
        [ "weight", "number", false ],
        [ "urgent", "boolean", true ],
        [ "labels", "json", false ],
    ]);
    assert.deepEqual(fields[1].options, [ "low", "high" ]);
    assert.equal(fields[0].description, "What is wrong");
    assert.equal(fields[5].placeholder, "[]");
});

test("a schema without properties is one JSON field for the whole arguments", () => {
    const fields = formFields({type : "object"});
    assert.equal(fields.length, 1);
    assert.equal(fields[0].kind, "json");
    assert.deepEqual(argumentsFrom(fields, {"" : ""}), {arguments : {}});
    assert.deepEqual(argumentsFrom(fields, {"" : "{\"q\": 1}"}), {arguments : {q : 1}});
    assert.equal((argumentsFrom(fields, {"" : "[1]"}) as {message: string}).message, "The arguments are a JSON object.");
    assert.equal((argumentsFrom(fields, {"" : "{"}) as {message: string}).message, "The arguments are not valid JSON.");
    assert.equal(formFields(null).length, 1);
});

test("the form's values make the arguments, typed, with empty optional fields left out", () => {
    const fields = formFields(ticket);
    assert.deepEqual(argumentsFrom(fields, {title : "Printer jam", points : "3", weight : "0.5", urgent : true, labels : "[\"office\"]", priority : ""}),
                     {arguments : {title : "Printer jam", points : 3, weight : 0.5, urgent : true, labels : [ "office" ]}});
    assert.deepEqual(argumentsFrom(fields, {title : "x"}), {arguments : {title : "x", urgent : false}},
                     "a required checkbox left alone is false");
    assert.deepEqual(argumentsFrom(fields, {title : " "}), {field : "title", message : "title is required."});
    assert.deepEqual(argumentsFrom(fields, {title : "x", points : "2.5"}), {field : "points", message : "points is a whole number."});
    assert.deepEqual(argumentsFrom(fields, {title : "x", weight : "heavy"}), {field : "weight", message : "weight is a number."});
    assert.deepEqual(argumentsFrom(fields, {title : "x", labels : "[office"}), {field : "labels", message : "labels is not valid JSON."});
});

test("the prefix shown while a name is typed is the one Lake gives", () => {
    assert.equal(prefixOf("GitHub Issues"), "github_issues");
    assert.equal(prefixOf("  Ticket -- Desk!  "), "ticket_desk");
    assert.equal(prefixOf("42 Things"), "things", "a prefix starts with a letter");
    assert.equal(prefixOf("ÉÉ"), "");
    assert.equal(prefixOf("a".repeat(40)).length, 24);
});

test("the sign-in callback finds its workspace in the state Lake made", () => {
    const customer = "8f1c2d3e-1111-4222-8333-444455556666";
    const workspace = "4b2e9a10-aaaa-4bbb-8ccc-dddd00001111";
    assert.deepEqual(signInState(`${customer}.${workspace}.0f0e0d0c-1234-4abc-8def-001122334455`),
                     {customerId : customer, workspaceId : workspace});
    assert.equal(signInState(null), null);
    assert.equal(signInState("not.a-state"), null);
    assert.equal(signInState(`${customer}.${workspace}.<script>`), null);
});

test("statuses, calls and answers read in words", () => {
    assert.equal(statusLabel("needs_sign_in"), "Needs sign-in");
    assert.equal(statusTone("failing"), "bad");
    assert.equal(statusTone("connected"), "ok");
    assert.equal(viaLabel("reexport"), "Lake's MCP");
    assert.equal(viaLabel("agent", "Triage"), "Agent Triage");
    assert.equal(hostOf("https://mcp.example.com/v1/mcp"), "mcp.example.com");
    assert.equal(hostOf("not a url"), "not a url");
    assert.equal(resultText({content : [ {type : "text", text : "a"}, {type : "image"}, {type : "text", text : "b"} ]}), "a\n\nb");
    assert.equal(resultText({content : [], structured_content : {id : "T-1"}}), "{\n  \"id\": \"T-1\"\n}");
});

test("the re-export switch says who can then call the tools", () => {
    const words = exportWarning("Ticket Desk");
    assert.match(words, /Anyone who may call Lake's tools in this workspace/);
    assert.match(words, /Ticket Desk/);
    assert.match(words, /only gate/);
});

// lake#659: a server Lake runs here, as its page shows it and as the new-connection sheet reads it.
test("a stdio server's command line, arguments and variable names", () => {
    assert.equal(
        stdioCommandLine({runtime: "node", package: "@modelcontextprotocol/server-everything@2026.8.31", args: ["stdio"]}),
        "npx @modelcontextprotocol/server-everything@2026.8.31 stdio",
    );
    assert.equal(stdioCommandLine({runtime: "node", package: "@acme/tools@1.2.0", command: "acme-mcp"}), "npx --package @acme/tools@1.2.0 acme-mcp");
    assert.equal(stdioCommandLine({runtime: "python", package: "mcp-server-fetch==2025.4.7"}), "uvx mcp-server-fetch==2025.4.7");
    assert.equal(stdioCommandLine({runtime: "python", package: "acme==1.0", command: "acme-mcp", args: ["--root", "/tmp"]}), "uvx --from acme==1.0 acme-mcp --root /tmp");
    assert.deepEqual(argsOf(" --root \n\n /data \n"), ["--root", "/data"]);
    assert.equal(envNameProblem("GITHUB_TOKEN"), null);
    assert.equal(envNameProblem("_private"), null);
    assert.match(envNameProblem("1BAD") ?? "", /digit/);
    assert.match(envNameProblem("has space") ?? "", /Letters/);
    assert.match(envNameProblem("MCP_RUNNER_TOKEN") ?? "", /Lake's/);
    assert.match(envNameProblem("app_port") ?? "", /Lake's/);
    assert.equal(statusLabel("starting"), "Starting");
});

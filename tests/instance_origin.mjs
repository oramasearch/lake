// SPDX-License-Identifier: MIT

import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { randomBytes } from "node:crypto";
import fs from "node:fs/promises";
import http from "node:http";
import net from "node:net";
import path from "node:path";
import { setTimeout as delay } from "node:timers/promises";
import { pathToFileURL } from "node:url";

const database = process.env.LAKE_ORIGIN_DB_URI;
const liquid = process.env.LAKE_TEST_LIQUID;
const root = process.env.LAKE_ORIGIN_ROOT;
assert(database && liquid && root, "Run InstanceOrigin.BrowserToTerminal through CTest");
const { default: puppeteer } = await import(process.env.LAKE_TEST_PUPPETEER
    ? pathToFileURL(process.env.LAKE_TEST_PUPPETEER).href
    : "puppeteer");
await fs.mkdir(root, { recursive: true });
const evidence = process.env.LAKE_ORIGIN_EVIDENCE || root;
await fs.mkdir(evidence, { recursive: true });
const token = randomBytes(32).toString("hex");
const env = {
    ...process.env,
    LIQUID_HOME: path.join(root, "identity"),
    LIQUID_TOKEN: token,
    LAKE_ADMIN_TOKEN: token,
    LAKE_INSTANCE_KEK: randomBytes(32).toString("hex"),
    LAKE_LOG_MODE: "air-gapped",
    LAKE_STORE_ROOT: path.join(root, "store"),
};
// This instance has no configured outbound services, even on a developer's configured shell.
for (const key of Object.keys(env)) {
    if (/^LAKE_(OPERATOR_|LOGS_|LOG_WIT|LOG_AGG|TSA_|IDENTITY_SEED|REPLICATION_TOKEN)/.test(key)) {
        delete env[key];
    }
}
const transcript = [];
function record(text) {
    transcript.push(text);
    console.log(text);
}

function command(program, args, cwd = root, overrides = {}) {
    const result = spawnSync(program, args, {
        cwd,
        env: { ...env, ...overrides },
        encoding: "utf8",
        timeout: 120000,
        maxBuffer: 16 * 1024 * 1024,
    });
    assert.ifError(result.error);
    assert.equal(result.status, 0, `${program}: ${result.stdout}\n${result.stderr}`);
    return result.stdout;
}

async function unusedPort() {
    const probe = net.createServer();
    await new Promise((resolve, reject) => {
        probe.once("error", reject);
        probe.listen(0, "127.0.0.1", resolve);
    });
    const port = probe.address().port;
    await new Promise((resolve) => probe.close(resolve));
    assert.notEqual(port, 4900);
    return port;
}

const origin = `http://127.0.0.1:${await unusedPort()}`;
async function request(route, body) {
    const url = new URL(route, origin);
    assert.equal(url.origin, origin);
    const response = await fetch(url, {
        method: body ? "POST" : "GET",
        headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
        body: body ? JSON.stringify(body) : undefined,
        redirect: "error",
    });
    assert.equal(response.status, 200, await response.clone().text());
    const result = await response.json();
    assert.equal(result.ok, true, JSON.stringify(result));
    return result.value;
}

async function ready(server) {
    for (let attempt = 0; attempt < 300; ++attempt) {
        assert.equal(server.exitCode, null, "Lake exited before readiness");
        try {
            const response = await fetch(`${origin}/readyz`, { redirect: "error" });
            if (response.ok) {
                return;
            }
        } catch {
            // The child has not bound its listener yet.
        }
        await delay(100);
    }
    assert.fail("Lake never became ready");
}

// The origin the web app renders its clone and push commands from (`viewer.external_origin`),
// asked for with forged Host and proxy headers (lake#561: the page is the app's).
async function viewerWithForgedHeaders() {
    return new Promise((resolve, reject) => {
        const pending = http.get(`${origin}/v1/viewer`, {
            headers: {
                Authorization: `Bearer ${token}`,
                Host: "injected.example.invalid:9443",
                Forwarded: "host=injected.example.invalid:9443;proto=https",
                "X-Forwarded-Host": "injected.example.invalid:9443",
                "X-Forwarded-Proto": "https",
            },
        }, (response) => {
            let body = "";
            response.setEncoding("utf8");
            response.on("data", (chunk) => {
                body += chunk;
            });
            response.on("end", () => {
                if (response.statusCode !== 200) {
                    reject(new Error(`Forged-header request failed: ${response.statusCode}`));
                    return;
                }
                resolve(body);
            });
        });
        pending.on("error", reject);
    });
}

function copiedCommands(commands, repository, cwd) {
    const url = `${origin}/v1/repositories/${repository.id}`;
    const allowed = new Set([
        `liquid clone '${url}' './${repository.name}'`,
        `cd './${repository.name}'`,
        `liquid push '${url}'`,
    ]);
    // Refuse before starting a terminal process if the rendered URL or shell text changes.
    for (const line of commands.split("\n")) {
        assert(allowed.has(line), `Refusing unrecognized copied command: ${line}`);
        for (const match of line.matchAll(/https?:\/\/[^']+/g)) {
            assert.equal(new URL(match[0]).origin, origin);
        }
    }
    record(`$ ${commands}`);
    record(command("/bin/sh", ["-eu", "-c", commands], cwd, {
        PATH: `${path.dirname(liquid)}${path.delimiter}${process.env.PATH}`,
    }).trim());
}

let server;
let browser;
let serverOutput = "";
try {
    const postgresVersion = command("psql", [database, "-Atc", "SHOW server_version"]).trim();
    env.LAKE_CONTROL_DB_URI = database;
    // Prove the explicit flag wins over a different environment value without contacting it.
    env.LAKE_EXTERNAL_ORIGIN = "https://unused.example.invalid:7443";
    server = spawn(process.argv[2], ["--listen", origin.slice(7), "--external-origin", `${origin}/`], {
        cwd: root,
        env,
        stdio: ["ignore", "pipe", "pipe"],
    });
    server.stdout.on("data", (chunk) => {
        serverOutput += chunk;
    });
    server.stderr.on("data", (chunk) => {
        serverOutput += chunk;
    });
    await ready(server);
    const user = await request("/v1/create_user", { display_name: "Origin proof" });
    const customer = await request("/v1/create_customer", {
        customer_type: "individual", user_id: user.id,
    });
    // A repository belongs to one workspace of its space (lake#315).
    const workspace = await request("/v1/create_workspace", {
        customer_id: customer.id, name: "origin", environment: "dev",
    });
    const repository = await request("/v1/create_repository", {
        customer_id: customer.id, workspace_id: workspace.id, name: "origin-proof",
        visibility: "private",
    });
    record(`Instance: ${origin}; store: PostgreSQL ${postgresVersion}; repository: ${repository.id}`);
    const expected = Buffer.from(Array.from({ length: 4096 }, (_, index) => index % 256));
    const seed = path.join(root, "seed");
    await fs.mkdir(seed);
    await fs.writeFile(path.join(seed, "bytes.bin"), expected);
    await fs.writeFile(path.join(seed, "empty.txt"), "");
    await fs.writeFile(path.join(seed, "README.md"), "# Private instance proof\n");
    command(liquid, ["init", "."], seed);
    command(liquid, ["cut", "Seed the private repository"], seed);
    command(liquid, ["push", `${origin}/v1/repositories/${repository.id}`], seed);
    browser = await puppeteer.launch({
        headless: true,
        executablePath: process.env.LAKE_TEST_BROWSER,
        userDataDir: path.join(root, "browser"),
        args: ["--disable-background-networking", "--disable-component-update"],
    });
    const page = await browser.newPage();
    const requests = [];
    const refused = [];
    await page.setRequestInterception(true);
    page.on("request", (pending) => {
        const url = new URL(pending.url());
        if (url.origin !== origin) {
            refused.push(pending.url());
            void pending.abort();
            return;
        }
        requests.push(`${pending.method()} ${pending.url()}`);
        void pending.continue();
    });
    await browser.setCookie({ name: "lake_session", value: token, url: origin,
        httpOnly: true, sameSite: "Strict" });
    await page.goto(`${origin}/repository?id=${repository.id}`, { waitUntil: "networkidle0" });
    // The web app renders the page after its reads answer.
    await page.waitForFunction(() => [...document.querySelectorAll("h2")]
        .some((element) => element.textContent === "Clone and Push" &&
            element.parentElement.querySelectorAll("pre code").length === 2), { timeout: 30000 });
    const panel = await page.evaluate(() => {
        const heading = [...document.querySelectorAll("h2")]
            .find((element) => element.textContent === "Clone and Push");
        if (!heading) {
            throw new Error("Clone and Push panel missing");
        }
        return {
            text: heading.parentElement.innerText,
            commands: [...heading.parentElement.querySelectorAll("pre code")]
                .map((element) => element.innerText),
        };
    });
    const html = await page.content();
    await fs.writeFile(path.join(evidence, "repository.html"), html);
    await fs.writeFile(path.join(evidence, "panel.txt"), `${panel.text}\n`);
    await fs.writeFile(path.join(evidence, "browser-requests.txt"), requests.join("\n"));
    record(panel.text);
    assert.deepEqual(refused, [], "Browser attempted an off-origin request");
    assert(!html.includes("lake.live"), "Rendered page contains an unrelated hosted origin");
    assert.equal(panel.commands.length, 2);
    assert(panel.commands[0].startsWith("liquid clone "));
    assert(panel.commands[1].startsWith("liquid push "));
    const forged = await viewerWithForgedHeaders();
    assert.equal(JSON.parse(forged).value.external_origin, origin);
    assert(!forged.includes("injected.example.invalid"));
    record("Host and proxy-header substitution: refused; configured origin preserved");
    copiedCommands(panel.commands[0], repository, root);
    const checkout = path.join(root, repository.name);
    assert.deepEqual(await fs.readFile(path.join(checkout, "bytes.bin")), expected);
    assert.equal((await fs.readFile(path.join(checkout, "empty.txt"))).length, 0);
    record("Clone byte comparison: PASS (4096 binary bytes and empty file)");
    const updated = Buffer.concat([expected, Buffer.from("\nPushed from the copied checkout.\n")]);
    const update = path.join(root, "updated.bin");
    await fs.writeFile(update, updated);
    const references = JSON.parse(command(liquid, ["refs", "--json"], checkout));
    const main = references.find((reference) => reference.name === "main");
    assert(main?.id, "The cloned repository must have the published main reference");
    command(liquid, ["write", "bytes.bin", "--from", update, "--base", main.id], checkout);
    command(liquid, ["cut", "Verify the instance origin"], checkout);
    record("Recorded a binary edit with liquid write and liquid cut.");
    copiedCommands(panel.commands[1], repository, checkout);
    const verify = path.join(root, "verify");
    await fs.mkdir(verify);
    copiedCommands(panel.commands[0], repository, verify);
    assert.deepEqual(await fs.readFile(path.join(verify, repository.name, "bytes.bin")), updated);
    record(`Push byte comparison: PASS (${updated.length} bytes in a fresh clone)`);
    record("Rendered-surface grep: no matches; authenticated origin count: 1");
} finally {
    await browser?.close();
    if (server && server.exitCode === null) {
        server.kill("SIGTERM");
        await new Promise((resolve) => server.once("exit", resolve));
    }
    await fs.writeFile(path.join(evidence, "server.log"), serverOutput);
    await fs.writeFile(path.join(evidence, "terminal.txt"), `${transcript.join("\n")}\n`);
    await fs.rm(root, { recursive: true, force: true });
}

// End-to-end browser walkthrough of lake (docs/deployment/browser-testing.md): Clerk sign-in with a
// one-time ticket, then every Design, Train and Ship page, with a screenshot per step and a
// log.json. It creates one project in the signed-in customer's workspace.
//
//   node tools/e2e/walkthrough.mjs <base-url> <ticket-file> <output-dir>
//
// Playwright comes from the sibling orama-website-v2 checkout, as the UI tools do; set PLAYWRIGHT
// to point at another playwright package.
const candidates = [
    process.env.PLAYWRIGHT,
    "playwright",
    new URL("../../../../../orama-website-v2/node_modules/playwright/index.mjs", import.meta.url)
        .href,
].filter(Boolean);
let chromium;
for (const spec of candidates) {
    try {
        ({chromium} = await import(spec));
        break;
    } catch {
    }
}
if (!chromium) {
    console.error(`Playwright not found. Tried:\n  ${candidates.join("\n  ")}`);
    process.exit(1);
}
import fs from "node:fs";

const [base, ticketFile, out] = process.argv.slice(2);
const ticket = fs.readFileSync(ticketFile, "utf8").trim();
fs.mkdirSync(out, {recursive : true});
const log = [];
const note = (step, detail) => {
    log.push({step, ...detail});
    console.log(step, JSON.stringify(detail));
};

const browser = await chromium.launch();
const context = await browser.newContext({viewport : {width : 1440, height : 900}});
const page = await context.newPage();
let n = 0;
const shot = async (name) =>
    page.screenshot({path : `${out}/${String(++n).padStart(2, "0")}-${name}.png`});
const text = async () =>
    (await page.locator("main").innerText()).replace(/\s+/g, " ").slice(0, 400);
const errorText = async () => (await page.locator("main .error").allInnerTexts()).join(" | ");

try {
    // 1. Sign in through Clerk's own form, which consumes the ticket, then lake's exchange.
    await page.goto(`${base}/sign-in?__clerk_ticket=${encodeURIComponent(ticket)}`);
    await page.waitForURL(`${base}/repositories**`, {timeout : 60000});
    await shot("repositories");
    note("sign-in", {
        url : page.url(),
        you : (await page.content()).includes("(you)") ||
                  (await page.content()).includes("your personal space")
    });

    // 2. Design: the workspace gate, then wait for it to be ready.
    await page.goto(`${base}/design/projects`);
    await shot("projects-gate");
    if (await page.getByRole("button", {name : "Start your workspace"}).count()) {
        await page.getByRole("button", {name : "Start your workspace"}).click();
        await page.waitForLoadState("load");
        note("start-workspace", {text : await text()});
    }
    const deadline = Date.now() + 8 * 60 * 1000;
    while (Date.now() < deadline) {
        await page.goto(`${base}/design/projects`);
        if (await page.locator("form[action='/design/projects/create']").count())
            break;
        if ((await text()).includes("could not be prepared"))
            break;
        await page.waitForTimeout(10000);
    }
    await shot("projects");
    note("workspace", {
        ready : await page.locator("form[action='/design/projects/create']").count() > 0,
        text : await text(),
        error : await errorText()
    });

    // 3. Create a project and build a pipeline from its intent.
    const project = `e2e-${Date.now().toString(36)}`;
    if (await page.locator("form[action='/design/projects/create']").count()) {
        await page.fill("input[name=name]", project);
        await page.fill("input[name=description]", "Which enterprise accounts will not renew?");
        await page.fill("textarea[name=intent]", "Which accounts the team calls first each week");
        await page.getByRole("button", {name : "Create project"}).click();
        await page.waitForLoadState("load");
        await shot("project");
        note("create-project", {url : page.url(), error : await errorText()});

        if (await page.locator("form[action='/design/project/compile']").count()) {
            await page.fill(
                "form[action='/design/project/compile'] textarea[name=intent]",
                "Predict which accounts will churn in the next 90 days, with the reasons");
            await page.getByRole("button", {name : "Build pipeline"}).click();
            await page.waitForLoadState("load");
            await shot("pipeline");
            note("build-pipeline", {
                error : await errorText(),
                pipelines : await page.locator("form[action='/design/project/run']").count()
            });
            if (await page.locator("form[action='/design/project/run']").count()) {
                await page.getByRole("button", {name : "Check and run"}).first().click();
                await page.waitForLoadState("load");
                await shot("run");
                note("check-and-run", {url : page.url(), error : await errorText()});
            }
        }
    }

    // 4. Every remaining page.
    for (const [path, name] of [[ "/train/data", "data" ],
                                [ "/train/runs", "runs" ],
                                [ "/ship/models", "models" ],
                                [ "/ship/indexes", "indexes" ],
                                [ "/deployments", "deployments" ],
                                [ "/", "home" ]]) {
        const response = await page.goto(`${base}${path}`);
        await shot(name);
        note(`page ${path}`, {
            status : response.status(),
            error : await errorText(),
            text : (await text()).slice(0, 160)
        });
    }
    const runLink = await page.goto(`${base}/train/runs`)
                        .then(() => page.locator("a[href^='/train/run?']").first());
    if (await runLink.count()) {
        await runLink.click();
        await page.waitForLoadState("load");
        await shot("run-report");
        note("run-report", {url : page.url(), error : await errorText()});
    }

    // 5. Account menu and sign out (signs out of Clerk too).
    await page.locator("details.account-menu summary").click();
    await shot("account-menu");
    await page.getByRole("button", {name : "Sign out"}).click();
    await page.waitForURL(`${base}/sign-in**`, {timeout : 30000});
    await page.waitForTimeout(4000);
    await shot("signed-out");
    const after = await page.goto(`${base}/design/projects`);
    note("signed-out", {url : page.url(), status : after.status()});
} catch (error) {
    await shot("failure");
    note("failure", {message : String(error), url : page.url()});
    process.exitCode = 1;
} finally {
    fs.writeFileSync(`${out}/log.json`, JSON.stringify(log, null, 2));
    await browser.close();
}

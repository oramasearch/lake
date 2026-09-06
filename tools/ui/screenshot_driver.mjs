import fs from "node:fs/promises";
import process from "node:process";

import puppeteer from "/home/pptruser/node_modules/puppeteer/lib/esm/puppeteer/puppeteer.js";

async function capture(browser, source, output, networkLog) {
  const events = [];
  let failure = "";
  const page = await browser.newPage();
  try {
    await page.setViewport({ width: 1440, height: 900, deviceScaleFactor: 1 });
    page.on("requestfailed", (request) => {
      events.push({ type: "requestfailed", url: request.url(), error: request.failure()?.errorText ?? "unknown" });
    });
    page.on("response", (response) => {
      events.push({ type: "response", status: response.status(), url: response.url() });
    });
    try {
      const deadline = Date.now() + 30000;
      const remaining = () => {
        const milliseconds = deadline - Date.now();

        if (milliseconds <= 0) {
          throw new Error("page capture timed out after 30000ms");
        }

        return milliseconds;
      };

      await page.goto(`file:///work/${source}`, {
        waitUntil: "domcontentloaded",
        timeout: remaining(),
      });
      await page.waitForNetworkIdle({
        concurrentRequests: 0,
        idleTime: 500,
        timeout: remaining(),
      });
    } catch (error) {
      failure = error instanceof Error ? error.message : String(error);
    }
    await page.screenshot({ path: `/work/${output}` });
  } finally {
    await fs.writeFile(`/work/${networkLog}`, `${JSON.stringify({ failure, events }, null, 2)}\n`);
    await page.close();
  }

  return failure;
}

function withinTimeout(promise, milliseconds, message) {
  let timer;
  const timeout = new Promise((resolve, reject) => {
    timer = setTimeout(() => reject(new Error(message)), milliseconds);
  });

  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

const args = process.argv.slice(2);
const browser = await withinTimeout(
  puppeteer.launch({
    headless: true,
    args: ["--no-sandbox", "--disable-gpu", "--hide-scrollbars"],
  }),
  30000,
  "screenshot browser launch timed out after 30000ms",
);

try {
  if (args[0] === "--jobs") {
    const [, jobsPath, failuresPath] = args;

    if (!jobsPath || !failuresPath) {
      throw new Error("jobs and failures paths are required");
    }

    const jobs = (await fs.readFile(`/work/${jobsPath}`, "utf8"))
      .trim()
      .split("\n")
      .filter(Boolean);
    const failures = [];

    for (const job of jobs) {
      const [pageName, source, output, networkLog] = job.split("\t");
      const failure = await capture(browser, source, output, networkLog);

      if (failure) {
        failures.push(`${pageName}\t${networkLog}`);
        console.error(`${pageName}: ${failure}`);
      }
    }

    await fs.writeFile(`/work/${failuresPath}`, `${failures.join("\n")}${failures.length ? "\n" : ""}`);
    process.exitCode = failures.length ? 1 : 0;
  } else {
    const [source, output, networkLog] = args;

    if (!source || !output || !networkLog) {
      throw new Error("source, output, and network log paths are required");
    }

    const failure = await capture(browser, source, output, networkLog);

    if (failure) {
      console.error(failure);
      process.exitCode = 1;
    }
  }
} finally {
  await browser.close();
}

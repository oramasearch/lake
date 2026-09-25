#!/usr/bin/env python3
"""Build the lake cloud navigation mock (docs/design/cloud.md) as static HTML.

The shell markup copies `web_ui_shell` in src/api/web_ui.cpp, and the stylesheet and icons are
extracted from the same source files the binary compiles (`web_ui_tokens.inc`,
`web_ui_styles.inc`, `web_ui_icon`). The mock therefore looks like the served product, and
rerunning this script picks up any change to the real shell. Only the navigation, the page
content and the sample data are the mock's own.

    python3 docs/design/ui/mocks/cloud/build.py      # writes style.css and the *.html pages here
"""
import html
import pathlib
import re

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[4]
SRC = ROOT / "src" / "api"
FONTS = "../../../../../assets/fonts/"


def stylesheet():
    parts = []
    for name, tag in (("web_ui_tokens.inc", "lake_tokens"), ("web_ui_styles.inc", "lake_styles")):
        text = (SRC / name).read_text()
        m = re.search(rf'R"{tag}\((.*)\){tag}"', text, re.S)
        assert m, f"{name}: raw string not found"
        parts.append(m.group(1))
    css = "".join(parts).replace("url('/fonts/", f"url('{FONTS}")
    return "/* Extracted from src/api/web_ui_tokens.inc and web_ui_styles.inc by build.py. */\n" + css


def icons():
    text = (SRC / "web_ui.cpp").read_text()
    found = dict(re.findall(r'name == "([a-z]+)"\) \{\s*body =\s*R"\((.*?)\)";', text, re.S))
    assert "home" in found and "repository" in found, "icons not found in web_ui.cpp"
    return found


ICONS = icons()


def icon(name):
    return f'<svg class="icon" viewBox="0 0 16 16" aria-hidden="true" focusable="false">{ICONS[name]}</svg>'


# The account viewer's sidebar, per docs/design/cloud.md "Navigation per viewer".
NAV = [
    (None, [("Home", "home.html", "home")]),
    (None, [
        ("Repositories", "repositories.html", "repository"),
        ("Shared with me", "#", "repository"),
        ("Search", "#", "search"),
    ]),
    ("Design", [("Projects", "project.html", "audit")]),
    ("Train", [("Datasets", "#", "metering"), ("Models", "models.html", "metering")]),
    ("Ship", [
        ("Deployments", "deployments.html", "api"),
        ("Packages", "#", "repository"),
        ("Applications", "#", "api"),
    ]),
    ("Organization", [
        ("Members and teams", "#", "organization"),
        ("Secrets", "#", "secrets"),
        ("Usage", "#", "metering"),
        ("Audit", "#", "audit"),
    ]),
    ("Developer", [("API and MCP", "#", "api")]),
]

SIGNED_OUT_NAV = [(None, [("Home", "home-signed-out.html", "home"), ("Public repositories", "#", "repository")])]


def nav(current, groups):
    out = ['<nav class="primary-nav" aria-label="Primary">']
    for label, items in groups:
        if label:
            out.append(f'<p class="nav-label">{label}</p>')
        for text, href, ic in items:
            cur = ' aria-current="page"' if href == current else ""
            out.append(f'<a class="nav-link" href="{href}"{cur}>{icon(ic)}<span>{text}</span></a>')
    out.append("</nav>")
    return "".join(out)


def page(filename, title, meta, content, signed_in=True):
    session = (
        '<span class="session"><a href="#">Account</a></span>'
        if signed_in
        else '<span class="session"><a href="#">Sign in</a><a href="#">Create account</a></span>'
    )
    groups = NAV if signed_in else SIGNED_OUT_NAV
    doc = (
        '<!doctype html><html lang="en"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1">'
        f'<meta name="color-scheme" content="light dark"><title>{html.escape(title)}</title>'
        '<link rel="stylesheet" href="style.css"></head><body>'
        '<div class="app-shell"><aside class="sidebar"><a class="brand" href="home.html">'
        f'<span class="brand-mark">{icon("lake")}</span><span>lake</span></a>{nav(filename, groups)}</aside>'
        f'<div class="app-frame"><header class="topbar"><div class="topbar-context">{html.escape(meta)}</div>'
        f'<div class="topbar-actions">{session}</div></header>'
        f'<main id="main-content"><div class="page-container">{content}</div></main></div></div></body></html>'
    )
    (HERE / filename).write_text(doc)


def not_built(what):
    return f'<p class="notice">Mock. {what}</p>'


def table(head, rows):
    th = "".join(f"<th>{h}</th>" for h in head)
    tr = "".join("<tr>" + "".join(f"<td>{c}</td>" for c in r) + "</tr>" for r in rows)
    return f'<div class="table-wrap"><table><thead><tr>{th}</tr></thead><tbody>{tr}</tbody></table></div>'


def badge(t):
    return f'<span class="badge">{t}</span>'


def cards(items):
    body = "".join(f'<section class="card"><h3>{t}</h3><p>{p}</p>{a}</section>' for t, p, a in items)
    return f'<div class="grid">{body}</div>'


page(
    "home-signed-out.html",
    "lake — design, train, and ship AI",
    "Welcome to lake",
    '<p class="eyebrow">lake by Orama</p><h1>Design, train, and ship AI on your own data.</h1>'
    '<p class="sub">Keep your files, datasets and models in shared repositories, train models on them, '
    "and put them into production, in Orama's cloud or your own.</p>"
    '<p class="actions"><a class="button button-primary" href="#">Create account</a> <a class="button" href="#">Sign in</a></p>'
    + cards([
        ("Design", "Start a project from the business question: the decisions it drives and the return it is measured against.", ""),
        ("Train", "Put your datasets in repositories and train models on them. Every model records the data and code it was trained from.", ""),
        ("Ship", "Deploy to an environment, serve it on your domain, and watch its logs. Push from the <code>liquid</code> command line or from here.", ""),
    ]),
    signed_in=False,
)

page(
    "home.html",
    "lake — home",
    "northwind",
    '<p class="eyebrow">northwind</p><h1>Home</h1>'
    '<p class="sub">Your repositories, and what they are designing, training and shipping.</p>'
    '<h2>Recent repositories</h2>'
    + table(
        ["Repository", "Kind", "Last change"],
        [
            ['<a href="#">northwind/churn-signals</a>', badge("dataset"), "2 hours ago"],
            ['<a href="#">northwind/churn-model</a>', badge("model"), "yesterday"],
            ['<a href="#">northwind/retention-review</a>', badge("project"), "3 days ago"],
            ['<a href="#">northwind/account-exports</a>', badge("files"), "last week"],
        ],
    )
    + cards([
        ("Design", "2 projects", '<p><a href="project.html">Open projects</a></p>'),
        ("Train", "3 datasets · 2 models", '<p><a href="models.html">Open models</a></p>'),
        ("Ship", "1 deployment running", '<p><a href="deployments.html">Open deployments</a></p>'),
    ]),
)

page(
    "repositories.html",
    "lake — repositories",
    "6 repositories",
    '<p class="eyebrow">Your work</p><h1>Repositories</h1>'
    '<p class="sub">Every repository you can open: files, datasets, models, code and projects. '
    "Each one is shared, versioned and editable by everyone you invite.</p>"
    + not_built("The Kind column is new; the list itself exists today.")
    + table(
        ["Repository", "Kind", "Visibility", "Last change"],
        [
            ['<a href="#">northwind/churn-signals</a>', badge("dataset"), badge("private"), "2 hours ago"],
            ['<a href="#">northwind/churn-model</a>', badge("model"), badge("private"), "yesterday"],
            ['<a href="#">northwind/retention-review</a>', badge("project"), badge("organization"), "3 days ago"],
            ['<a href="#">northwind/account-exports</a>', badge("files"), badge("private"), "last week"],
            ['<a href="#">northwind/scoring-service</a>', badge("code"), badge("organization"), "last week"],
            ['<a href="#">northwind/brand-assets</a>', badge("files"), badge("public"), "last month"],
        ],
    ),
)

page(
    "project.html",
    "lake — project",
    "northwind/retention-review",
    '<p class="eyebrow">Project</p><h1>Retention review</h1>'
    + not_built("Projects are not built. docs/design/cloud.md proposes a project as a repository holding a project document.")
    + '<p class="sub">Which enterprise accounts will not renew, and what should the account team do about each one this quarter?</p>'
    '<section class="panel"><h2>Decisions</h2>'
    + table(
        ["Decision", "Owner", "Measured by"],
        [
            ["Which accounts get a retention call this week", "Head of customer success", "Accounts saved per quarter"],
            ["Which accounts get a renewal discount", "Revenue operations", "Net revenue retained"],
        ],
    )
    + '</section><section class="panel"><h2>Serves this project</h2>'
    + table(
        ["Repository", "Kind"],
        [
            ['<a href="#">northwind/churn-signals</a>', badge("dataset")],
            ['<a href="models.html">northwind/churn-model</a>', badge("model")],
            ['<a href="deployments.html">northwind/scoring-service</a>', badge("code")],
        ],
    )
    + "</section>",
)

page(
    "models.html",
    "lake — models",
    "2 models",
    '<p class="eyebrow">Train</p><h1>Models</h1>'
    '<p class="sub">Model repositories you can open. Each links to its revisions, files, storage savings, '
    "and the verified record of the data and code it was trained from.</p>"
    + not_built("This list page is new; each model's own page exists today.")
    + table(
        ["Model", "Latest revision", "Trained from", "Stored"],
        [
            ['<a href="#">northwind/churn-model</a>', "<code>r7</code>", '<a href="#">churn-signals</a> · <a href="#">scoring-service</a>', "1.2 GiB (61% saved)"],
            ['<a href="#">northwind/support-triage</a>', "<code>r2</code>", '<a href="#">ticket-history</a>', "480 MiB"],
        ],
    ),
)

page(
    "deployments.html",
    "lake — deployments",
    "1 running",
    '<p class="eyebrow">Ship</p><h1>Deployments</h1>'
    '<p class="sub">Every environment across your repositories: what is running, which change it runs, '
    "and where it is served.</p>"
    + not_built("This cross-repository list is new; each repository's environments page exists today.")
    + table(
        ["Repository", "Environment", "Phase", "Change", "Domain"],
        [
            ['<a href="#">northwind/scoring-service</a>', "production", '<span class="status-success">Running</span>', "<code>7f3c2a1</code>", "score.northwind.example"],
            ['<a href="#">northwind/scoring-service</a>', "staging", '<span class="status-warning">Deploying</span>', "<code>9b04e6d</code>", "—"],
        ],
    ),
)

(HERE / "style.css").write_text(stylesheet())
print("wrote", len(list(HERE.glob("*.html"))), "pages and style.css to", HERE)

# Cloud navigation mock

Static pages for the design in [`../../../cloud.md`](../../../cloud.md): lake as Orama's cloud,
navigated by Repositories, Design, Train and Ship. They are reference material only and change
nothing that is served.

## What makes them faithful

`build.py` extracts the stylesheet from `src/api/web_ui_tokens.inc` and `src/api/web_ui_styles.inc`,
and the icons from `web_ui_icon` in `src/api/web_ui.cpp`. The shell markup copies `web_ui_shell`.
So the pages use the product's own classes, tokens and fonts, and only the navigation, the page
content and the sample data belong to the mock. The sample customer `northwind` and every
repository in it are invented.

Pages whose surface does not exist yet say so in a notice at the top.

## Open or rebuild

```sh
open docs/design/ui/mocks/cloud/home.html
python3 docs/design/ui/mocks/cloud/build.py        # regenerate style.css and the pages
```

| Page | Viewer | Shows |
|---|---|---|
| [`home-signed-out.html`](home-signed-out.html) | Signed out | The new landing: design, train, and ship AI on your own data |
| [`home.html`](home.html) | Account | Recent repositories with their kind, and one card per section |
| [`repositories.html`](repositories.html) | Account | Every repository with a kind column |
| [`project.html`](project.html) | Account | Design: a project as a repository, with its decisions and the repositories that serve it |
| [`models.html`](models.html) | Account | Train: a model list over model repositories |
| [`deployments.html`](deployments.html) | Account | Ship: deployments across repositories |

Screenshots at 1440×900 are in [`screenshots/`](screenshots/).

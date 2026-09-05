# IBM Plex web fonts

Lake self-hosts the Latin WOFF2 subsets used by the Orama design system: IBM Plex Sans 400, 500,
and 600 plus IBM Plex Mono 400 and 500. CMake embeds these source assets into the `lake` binary;
the server exposes them below `/fonts/` with immutable one-year caching, so deployments need no
separate asset directory or third-party font origin.

The files come from the versions pinned by `oramasearch/orama-design`: Fontsource
`@fontsource/ibm-plex-sans` 5.2.8 and `@fontsource/ibm-plex-mono` 5.3.0. The corresponding
OFL-1.1 notices are beside the font files.

To replace a face, update the WOFF2 source, its notice when necessary, the fixed asset list in
`cmake/embed_fonts.cmake`, the `@font-face` declaration in `web_ui_stylesheet()`, and the route list
in `src/api/http_server.cpp`. Rebuild and verify the response begins with the WOFF2 signature and
has `Content-Type: font/woff2` plus the immutable cache policy.

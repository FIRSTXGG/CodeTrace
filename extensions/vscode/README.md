# CodeTrace for Visual Studio Code (planned for v0.3)

This directory will hold the TypeScript extension. It will not re-implement any
analysis: it starts `codetrace serve` and talks to it over JSON-RPC
(see [docs/protocol.md](../../docs/protocol.md)), exactly like the desktop app
uses the same core in-process.

Planned scope for v0.3:

- Command **CodeTrace: Open Call Graph** — webview with the neighbourhood graph of
  the function under the cursor (`graph/calls` with `symbolId`).
- **Callers / callees** tree views (`graph/calls`, depth 1).
- Diagnostics from `project/diagnostics` published as VS Code problems.
- `project/update` on file save for incremental re-analysis.

Server discovery order: setting `codetrace.serverPath`, then `codetrace` on `PATH`.

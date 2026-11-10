# Architecture

```text
            ┌──────────────┐   ┌──────────────┐   ┌───────────────────────┐
            │ desktop-qt   │   │ codetrace    │   │ IDE extensions (v0.3) │
            │ (in-process) │   │ CLI / serve  │   │ VS / VS Code          │
            └──────┬───────┘   └──────┬───────┘   └──────────┬────────────┘
                   │                  │   JSON-RPC (stdio)   │
                   ▼                  ▼◄─────────────────────┘
        ┌─────────────────────────────────────────────────────────────┐
        │ core                                                         │
        │  analysis/  ProjectAnalyzer: scan → per-file analysis →      │
        │             incremental cache → ProjectSnapshot              │
        │  graph/     CallGraph (resolution), DependencyGraph, SCC,    │
        │             layered layout                                   │
        │  protocol/  JSON, serializers, RpcService                    │
        │  diagnostics/                                                │
        └───────────────────────────▲─────────────────────────────────┘
                                    │ ILanguageAnalyzer
        ┌───────────────┬───────────┴────────┬────────────────────────┐
        │ cpp           │ python             │ javascript             │
        │ libclang      │ CPython ast        │ Tree-sitter            │
        │ + heuristic   │ + heuristic        │ + heuristic            │
        └───────────────┴────────────────────┴────────────────────────┘
```

The core knows nothing about Qt or IDEs. Analyzers depend on the core, never
the other way round. Front-ends depend on both.

## Analysis pipeline

1. **Discovery** (`ProjectAnalyzer::discoverFiles`): recursive walk.
   - Skips VCS, vendor and build directories (`node_modules`, `.git`, `build*`, …) and
     doesn't follow directory symlinks.
   - Detects the language by extension, or by shebang for files without one.
2. **Change detection:** each file's size and mtime are compared with the cache.
   If they differ, the content is hashed (FNV-1a), so a file that was touched but not
   changed is not re-parsed.
3. **Per-file analysis** runs on a thread pool.
   - An analyzer can ask for batches (`preferredBatchSize`). The Python AST backend uses
     batches of 48 so one helper process handles many files.
   - Progress callbacks and a `CancellationToken` keep the UI responsive. A cancelled
     run keeps the finished files in the cache, so the next run is cheaper.
4. **Snapshot** (`buildSnapshot`): assigns symbol ids and builds the `DependencyGraph`
   and then the `CallGraph`. A snapshot is immutable and shared by pointer, so the UI
   can keep showing the old one while a new one is computed.

Every file produces a `FileAnalysis`: symbols, call sites, imports, language
bindings and diagnostics. Analyzers never throw for bad input. If an analyzer fails
anyway, `ProjectAnalyzer` falls back to the next backend for that language and
records an info diagnostic.

## Call resolution

A call site is `(caller, calleeName, receiver, isMember, line)`. Candidates are the
callables with the same short name in the same language. A qualified name such as
`ns::f` must also match as a suffix. A call to a class name (`Foo()` / `new Foo()`)
maps to that class's constructor. Candidates are ranked:

| Tier | Rule | Result |
|---|---|---|
| 0 | Method of the caller's own class (no receiver, or `self`/`this`/`cls`) | exact |
| 1 | Same file | exact |
| 2 | File imported by the caller's file (via the dependency graph) | likely |
| 3 | Anywhere else in the project | likely |

The best tier wins if it holds a single target. Overloads in one file count as a
single target. Otherwise the call is recorded as **unresolved**, together with the
number of candidates (`> 1` means ambiguous, `0` means external, such as the standard
library or npm). Calls that match nothing in their own language are checked against
**language bindings**; currently those are pybind11 `m.def("py_name", &cpp_fn)`
declarations found by the C++ analyzers. Such edges are marked `cross-language`.

## Dependency graph

Import resolution:

**C++**
- `#include "x"`: first relative to the including file, otherwise by path-suffix
  match in the project.
- `<x>`: suffix match only.

**Python**
- `a.b` → `a/b.py` or `a/b/__init__.py`, searched by suffix.
- Relative imports walk up `level` directories.
- `from pkg import sub` also links `pkg/sub.py`.

**JavaScript**
- Relative specifiers, with extension and `index.js` probing.
- Bare specifiers (npm packages) are external.

Cycles are the strongly connected components of size > 1, found with an iterative
Tarjan (no recursion limit). The same algorithm finds recursive call groups.

## Robustness

- The tokenizers and heuristic analyzers are iterative. They tolerate unterminated
  strings and comments, unbalanced braces and `#if 0` blocks, and mark the result
  `partial` with a diagnostic.
- libclang is skipped for files nested more than 256 brackets deep, because a stack
  overflow in Clang can't be recovered. Tree-sitter tree walking is depth-limited.
- The Python helper runs with `python -I` (isolated mode). It only calls `ast.parse`
  and never imports or executes project code.
- Files larger than `maxFileBytes` (4 MiB by default) and binary files are skipped
  with a warning.
- The test suite feeds every backend malformed input, pathological nesting and random
  token soup. The heuristic build is also tested under ASan/UBSan.

## Desktop application

- `MainWindow` runs `ProjectAnalyzer::analyze` through `QtConcurrent`. It shows
  progress and can cancel, and a newer request restarts the run.
- `QFileSystemWatcher` invalidates changed files and triggers an incremental re-run
  after a 700 ms debounce.
- `GraphView` (`QGraphicsView`) uses the core's `layeredLayout`: longest-path layering
  on the SCC condensation, then barycentric ordering. In neighbourhood mode, callers
  get negative layers and callees positive ones.
- `--screenshot FILE` renders the window after analysis and exits; it serves as a
  headless smoke test with `QT_QPA_PLATFORM=offscreen`.

# JSON-RPC protocol

`codetrace serve` speaks [JSON-RPC 2.0](https://www.jsonrpc.org/specification) over
stdin/stdout with LSP-style framing:

```text
Content-Length: 58\r\n
\r\n
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}
```

Batch requests and notifications (no `id`) are supported. Symbol ids are only valid
within the snapshot that produced them, so re-query after `project/update`.

## Methods

| Method | Params | Result |
|---|---|---|
| `initialize` | – | `{name, version, languages[], backends[{language, backend, available}], capabilities}` |
| `project/open` | `{root, includeDirs?[], defines?[]}` | summary |
| `project/update` | `{files?[]}` — changed files to invalidate | summary (incremental; `reanalyzedFiles` tells how many were parsed) |
| `project/summary` | – | summary |
| `project/symbols` | `{query?, language?, callablesOnly?, limit? = 500}` | `[symbol]`, best matches first |
| `graph/calls` | `{symbolId?, depth? = 2}` | without `symbolId`: whole graph + `unresolved` + `recursionCycles`; with it: neighbourhood + `focus` |
| `graph/dependencies` | – | `{files[], edges[{from,to,line,spec}], external[], cycles[[fileIndex]]}` |
| `project/diagnostics` | – | `[diagnostic]` |
| `shutdown` | – | `null` |
| `exit` (notification) | – | server terminates |

### Objects

```jsonc
// symbol
{ "id": 7, "name": "area", "qualifiedName": "geo::Polygon::area", "container": "geo::Polygon",
  "kind": "method",            // module | namespace | class | function | method | constructor | lambda
  "language": "cpp",           // cpp | python | javascript
  "file": "/abs/path/geometry.cpp",
  "range": { "line": 9, "column": 17, "endLine": 17, "endColumn": 1 },
  "signature": "double area()" }

// edge
{ "caller": 12, "callee": 7, "line": 6, "column": 33, "resolution": "exact" }  // exact | likely | cross-language

// summary
{ "root": "...", "fileCount": 11, "symbolCount": 26, "callCount": 25, "unresolvedCallCount": 7,
  "reanalyzedFiles": 11, "cancelled": false, "errors": 0, "warnings": 0, "dependencyCycles": 1,
  "languages": [{ "language": "python", "files": 5, "symbols": 7 }],
  "files": [{ "file": "...", "language": "cpp", "backend": "libclang", "symbols": 3, "calls": 5, "imports": 2, "partial": false }] }

// diagnostic
{ "severity": "warning", "file": "...", "line": 4, "column": 0, "message": "...", "source": "python-ast" }
```

### Errors

| Code | Meaning |
|---|---|
| -32700 | Parse error |
| -32600 | Invalid request |
| -32601 | Method not found |
| -32602 | Invalid params |
| -32603 | Internal error |
| -32001 | No project is open |

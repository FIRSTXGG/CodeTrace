# CodeTrace for Visual Studio (planned for v0.3)

This directory will hold the C# VSIX extension. Like the VS Code extension it is a
thin client: it launches `codetrace.exe serve` and uses the JSON-RPC protocol in
[docs/protocol.md](../../docs/protocol.md) (StreamJsonRpc can speak the same
`Content-Length` framing out of the box).

Planned scope for v0.3: a tool window with the call graph of the current function,
"Find callers / callees" commands, and CodeTrace diagnostics in the Error List.

// CodeTrace — JSON-RPC 2.0 service exposing the core to IDE extensions.
//
// Transport: LSP-style framing over a byte stream
//   Content-Length: <n>\r\n\r\n<json payload>
//
// Methods (v0.1):
//   initialize                -> { name, version, languages[], capabilities }
//   project/open   {root, includeDirs?, defines?}  -> summary
//   project/update {files?}   -> summary (incremental)
//   project/summary           -> summary
//   project/symbols {query?, language?, callablesOnly?, limit?} -> [symbol]
//   graph/calls    {symbolId?, depth?}  -> full graph or neighbourhood
//   graph/dependencies        -> file graph + cycles
//   project/diagnostics       -> [diagnostic]
//   shutdown / exit
#pragma once

#include "core/analysis/project.hpp"
#include "core/protocol/json.hpp"

#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>

namespace codetrace::protocol {

namespace rpc_error {
inline constexpr int ParseError = -32700;
inline constexpr int InvalidRequest = -32600;
inline constexpr int MethodNotFound = -32601;
inline constexpr int InvalidParams = -32602;
inline constexpr int InternalError = -32603;
inline constexpr int ProjectNotOpen = -32001;
}  // namespace rpc_error

class RpcService {
public:
    explicit RpcService(std::shared_ptr<AnalyzerRegistry> registry);

    /// Handle one JSON-RPC message (request or notification).
    /// Returns the response, or nullopt for notifications.
    std::optional<json::Value> handle(const json::Value& message);

    /// Handle raw text (parse errors produce a JSON-RPC error response).
    std::optional<std::string> handleText(std::string_view text);

    bool exitRequested() const { return exit_; }

    /// Serve requests from `in` and write responses to `out` until exit or EOF.
    void serve(std::istream& in, std::ostream& out);

private:
    json::Value dispatch(const std::string& method, const json::Value& params);
    json::Value summary() const;

    ProjectAnalyzer analyzer_;
    ProjectOptions options_;
    std::shared_ptr<const ProjectSnapshot> snapshot_;
    bool exit_ = false;
    bool shutdown_ = false;
};

/// Read one framed message; returns nullopt on EOF / malformed header.
std::optional<std::string> readFramed(std::istream& in);
void writeFramed(std::ostream& out, const std::string& payload);

}  // namespace codetrace::protocol

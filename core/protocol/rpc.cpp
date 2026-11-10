#include "core/protocol/rpc.hpp"

#include "core/protocol/serialize.hpp"

#include <istream>
#include <ostream>

namespace codetrace::protocol {

using json::Value;

namespace {

struct RpcException {
    int code;
    std::string message;
};

Value errorResponse(const Value& id, int code, const std::string& message) {
    Value r = Value::object();
    r["jsonrpc"] = "2.0";
    r["id"] = id;
    Value e = Value::object();
    e["code"] = code;
    e["message"] = message;
    r["error"] = e;
    return r;
}

std::vector<std::string> stringList(const Value& v) {
    std::vector<std::string> out;
    for (const auto& e : v.asArray())
        if (e.isString()) out.push_back(e.asString());
    return out;
}

}  // namespace

RpcService::RpcService(std::shared_ptr<AnalyzerRegistry> registry) : analyzer_(std::move(registry)) {}

Value RpcService::summary() const {
    if (!snapshot_) throw RpcException{rpc_error::ProjectNotOpen, "no project is open"};
    return summaryJson(*snapshot_);
}

Value RpcService::dispatch(const std::string& method, const Value& params) {
    if (method == "initialize") {
        Value r = Value::object();
        r["name"] = "CodeTrace";
        r["version"] = "0.1.0";
        Value langs = Value::array();
        Value backends = Value::array();
        for (const auto& a : analyzer_.registry().analyzers()) {
            Value b = Value::object();
            b["language"] = toString(a->language());
            b["backend"] = a->backendName();
            b["available"] = a->isAvailable();
            backends.push(b);
        }
        for (auto l : {Language::Cpp, Language::Python, Language::JavaScript})
            if (analyzer_.registry().analyzerFor(l)) langs.push(toString(l));
        r["languages"] = langs;
        r["backends"] = backends;
        Value caps = Value::object();
        caps["callGraph"] = true;
        caps["dependencyGraph"] = true;
        caps["incremental"] = true;
        caps["debugging"] = false;
        r["capabilities"] = caps;
        return r;
    }
    if (method == "project/open") {
        if (!params["root"].isString()) throw RpcException{rpc_error::InvalidParams, "'root' (string) is required"};
        options_ = ProjectOptions{};
        options_.root = params["root"].asString();
        options_.analyzer.includeDirs = stringList(params["includeDirs"]);
        options_.analyzer.defines = stringList(params["defines"]);
        analyzer_.clearCache();
        snapshot_ = analyzer_.analyze(options_);
        return summary();
    }
    if (method == "project/update") {
        if (!snapshot_) throw RpcException{rpc_error::ProjectNotOpen, "no project is open"};
        analyzer_.invalidate(stringList(params["files"]));
        snapshot_ = analyzer_.analyze(options_);
        return summary();
    }
    if (method == "project/summary") return summary();
    if (method == "project/symbols") {
        if (!snapshot_) throw RpcException{rpc_error::ProjectNotOpen, "no project is open"};
        auto ids = snapshot_->findSymbols(params["query"].asString(), languageFromString(params["language"].asString()),
                                          params["callablesOnly"].asBool(false));
        std::size_t limit = static_cast<std::size_t>(params["limit"].asInt(500));
        Value out = Value::array();
        for (std::size_t i = 0; i < ids.size() && i < limit; ++i) out.push(toJson(snapshot_->symbols[ids[i]]));
        return out;
    }
    if (method == "graph/calls") {
        if (!snapshot_) throw RpcException{rpc_error::ProjectNotOpen, "no project is open"};
        if (params["symbolId"].isNumber()) {
            auto id = static_cast<SymbolId>(params["symbolId"].asInt());
            if (!snapshot_->symbol(id)) throw RpcException{rpc_error::InvalidParams, "unknown symbolId"};
            return neighbourhoodJson(*snapshot_, id, static_cast<int>(params["depth"].asInt(2)));
        }
        return callGraphJson(*snapshot_);
    }
    if (method == "graph/dependencies") {
        if (!snapshot_) throw RpcException{rpc_error::ProjectNotOpen, "no project is open"};
        return dependencyGraphJson(*snapshot_);
    }
    if (method == "project/diagnostics") {
        if (!snapshot_) throw RpcException{rpc_error::ProjectNotOpen, "no project is open"};
        Value out = Value::array();
        for (const auto& d : snapshot_->diagnostics) out.push(toJson(d));
        return out;
    }
    if (method == "shutdown") {
        shutdown_ = true;
        return Value(nullptr);
    }
    throw RpcException{rpc_error::MethodNotFound, "method not found: " + method};
}

std::optional<Value> RpcService::handle(const Value& message) {
    if (!message.isObject() || !message["method"].isString()) {
        return errorResponse(message["id"], rpc_error::InvalidRequest, "invalid request");
    }
    const std::string& method = message["method"].asString();
    const bool isNotification = !message.contains("id");
    if (method == "exit") {
        exit_ = true;
        return std::nullopt;
    }
    try {
        Value result = dispatch(method, message["params"]);
        if (isNotification) return std::nullopt;
        Value r = Value::object();
        r["jsonrpc"] = "2.0";
        r["id"] = message["id"];
        r["result"] = std::move(result);
        return r;
    } catch (const RpcException& e) {
        if (isNotification) return std::nullopt;
        return errorResponse(message["id"], e.code, e.message);
    } catch (const std::exception& e) {
        if (isNotification) return std::nullopt;
        return errorResponse(message["id"], rpc_error::InternalError, e.what());
    }
}

std::optional<std::string> RpcService::handleText(std::string_view text) {
    Value msg;
    try {
        msg = Value::parse(text);
    } catch (const json::ParseError& e) {
        return errorResponse(Value(nullptr), rpc_error::ParseError, e.what()).dump();
    }
    if (msg.isArray()) {   // batch request
        Value out = Value::array();
        for (const auto& m : msg.asArray())
            if (auto r = handle(m)) out.push(*r);
        if (out.size() == 0) return std::nullopt;
        return out.dump();
    }
    if (auto r = handle(msg)) return r->dump();
    return std::nullopt;
}

std::optional<std::string> readFramed(std::istream& in) {
    std::size_t length = 0;
    bool haveLength = false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            if (haveLength) break;
            continue;
        }
        constexpr std::string_view header = "Content-Length:";
        if (line.size() > header.size() && line.compare(0, header.size(), header) == 0) {
            try {
                length = static_cast<std::size_t>(std::stoull(line.substr(header.size())));
                haveLength = true;
            } catch (...) {
                return std::nullopt;
            }
        }
    }
    if (!haveLength || !in) return std::nullopt;
    if (length > (256u << 20)) return std::nullopt;
    std::string payload(length, '\0');
    in.read(payload.data(), static_cast<std::streamsize>(length));
    if (static_cast<std::size_t>(in.gcount()) != length) return std::nullopt;
    return payload;
}

void writeFramed(std::ostream& out, const std::string& payload) {
    out << "Content-Length: " << payload.size() << "\r\n\r\n" << payload;
    out.flush();
}

void RpcService::serve(std::istream& in, std::ostream& out) {
    while (!exit_) {
        auto msg = readFramed(in);
        if (!msg) break;
        if (auto reply = handleText(*msg)) writeFramed(out, *reply);
    }
}

}  // namespace codetrace::protocol

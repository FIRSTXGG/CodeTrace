// codetrace — command line front-end and JSON-RPC server for IDE extensions.
//
//   codetrace analyze <path> [--json | --dot | --deps] [--query TEXT] [-I DIR] [-D NAME[=V]] [--threads N]
//   codetrace serve            JSON-RPC 2.0 over stdin/stdout (Content-Length framing)
//   codetrace --version

#include "analyzers/registry.hpp"
#include "core/analysis/project.hpp"
#include "core/protocol/rpc.hpp"
#include "core/protocol/serialize.hpp"

#include <algorithm>
#include <cstdio>
#include <tuple>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <fcntl.h>
#  include <io.h>
#endif

using namespace codetrace;

namespace {

void usage() {
    std::cout <<
        "CodeTrace 0.1.0 - static analysis and call graphs for C++, Python and JavaScript\n\n"
        "Usage:\n"
        "  codetrace analyze <path> [options]   analyze a project directory or a single file\n"
        "  codetrace serve                      JSON-RPC server on stdio (used by IDE extensions)\n"
        "  codetrace backends                   list analyzer backends and their availability\n"
        "  codetrace --version\n\n"
        "Options for analyze:\n"
        "  --json            full JSON report (summary, symbols, call graph, dependencies)\n"
        "  --dot             call graph in Graphviz DOT format\n"
        "  --deps            file dependency graph and cycles\n"
        "  --query TEXT      only list symbols matching TEXT\n"
        "  -I DIR            extra C/C++ include directory (repeatable)\n"
        "  -D NAME[=VALUE]   extra C/C++ define (repeatable)\n"
        "  --threads N       worker threads (default: all cores)\n"
        "  --quiet           no progress output\n";
}

std::string rel(const std::string& root, const std::string& path) {
    if (path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/')
        return path.substr(root.size() + 1);
    return path;
}

int analyzeCommand(const std::vector<std::string>& args) {
    ProjectOptions opts;
    std::string mode = "text", query;
    bool quiet = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= args.size()) {
                std::cerr << "missing value for " << a << "\n";
                std::exit(2);
            }
            return args[++i];
        };
        if (a == "--json") mode = "json";
        else if (a == "--dot") mode = "dot";
        else if (a == "--deps") mode = "deps";
        else if (a == "--quiet" || a == "-q") quiet = true;
        else if (a == "--query") query = next();
        else if (a == "-I") opts.analyzer.includeDirs.push_back(next());
        else if (a.rfind("-I", 0) == 0) opts.analyzer.includeDirs.push_back(a.substr(2));
        else if (a == "-D") opts.analyzer.defines.push_back(next());
        else if (a.rfind("-D", 0) == 0) opts.analyzer.defines.push_back(a.substr(2));
        else if (a == "--threads") opts.threads = static_cast<unsigned>(std::stoul(next()));
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "unknown option " << a << "\n";
            return 2;
        } else if (opts.root.empty()) opts.root = a;
        else {
            std::cerr << "unexpected argument " << a << "\n";
            return 2;
        }
    }
    if (opts.root.empty()) {
        std::cerr << "analyze: missing <path>\n";
        return 2;
    }

    ProjectAnalyzer analyzer(createDefaultRegistry());
    ProgressCallback progress;
    if (!quiet && mode == "text") {
        progress = [](const Progress& p) {
            if (p.total == 0) return;
            std::fprintf(stderr, "\ranalyzing %zu/%zu files", p.done, p.total);
            if (p.done == p.total) std::fprintf(stderr, "\n");
        };
    }
    auto snap = analyzer.analyze(opts, progress);

    if (mode == "json") {
        json::Value report = protocol::summaryJson(*snap);
        json::Value symbols = json::Value::array();
        for (const auto& s : snap->symbols) symbols.push(protocol::toJson(s));
        report["symbols"] = symbols;
        report["callGraph"] = protocol::callGraphJson(*snap);
        report["dependencies"] = protocol::dependencyGraphJson(*snap);
        json::Value diags = json::Value::array();
        for (const auto& d : snap->diagnostics) diags.push(protocol::toJson(d));
        report["diagnostics"] = diags;
        std::cout << report.dump(2) << "\n";
        return 0;
    }
    if (mode == "dot") {
        std::cout << protocol::callGraphDot(*snap);
        return 0;
    }
    const std::string& root = snap->root;
    if (mode == "deps") {
        const auto& g = snap->dependencies;
        for (const auto& e : g.edges())
            std::cout << rel(root, g.files()[e.from]) << ":" << e.line << " -> " << rel(root, g.files()[e.to]) << "\n";
        auto cycles = g.cycles();
        std::cout << "\n" << cycles.size() << " dependency cycle(s)\n";
        for (const auto& c : cycles) {
            std::cout << "  cycle:";
            for (auto f : c) std::cout << " " << rel(root, g.files()[f]);
            std::cout << "\n";
        }
        return 0;
    }

    // Text report.
    std::cout << "Project: " << root << "\n";
    std::cout << "Files: " << snap->files.size() << "   Symbols: " << snap->symbols.size()
              << "   Resolved calls: " << snap->callGraph.edges().size()
              << "   Unresolved calls: " << snap->callGraph.unresolved().size() << "\n";
    for (const auto& l : snap->languages)
        std::cout << "  " << toString(l.language) << ": " << l.files << " files, " << l.symbols << " symbols\n";
    std::cout << "\n";

    auto ids = snap->findSymbols(query, Language::Unknown, true);
    std::sort(ids.begin(), ids.end(), [&](SymbolId a, SymbolId b) {
        const Symbol &x = snap->symbols[a], &y = snap->symbols[b];
        return std::tie(x.file, x.range.line) < std::tie(y.file, y.range.line);
    });
    std::string lastFile;
    for (auto id : ids) {
        const Symbol& s = snap->symbols[id];
        if (s.file != lastFile) {
            std::cout << rel(root, s.file) << "\n";
            lastFile = s.file;
        }
        std::cout << "  " << s.range.line << "  " << toString(s.kind) << " " << s.qualifiedName << "\n";
        std::vector<SymbolId> shown;
        for (auto e : snap->callGraph.outgoing(id)) {
            const auto& edge = snap->callGraph.edges()[e];
            if (std::find(shown.begin(), shown.end(), edge.callee) != shown.end()) continue;
            shown.push_back(edge.callee);
            const Symbol& callee = snap->symbols[edge.callee];
            std::cout << "        -> " << callee.qualifiedName << "  (" << rel(root, callee.file) << ":" << callee.range.line
                      << ", call at line " << edge.line;
            if (edge.resolution != Resolution::Exact) std::cout << ", " << toString(edge.resolution);
            std::cout << ")\n";
        }
    }

    auto fileCycles = snap->dependencies.cycles();
    if (!fileCycles.empty()) {
        std::cout << "\nCyclic file dependencies:\n";
        for (const auto& c : fileCycles) {
            std::cout << " ";
            for (auto f : c) std::cout << " " << rel(root, snap->dependencies.files()[f]);
            std::cout << "\n";
        }
    }
    auto rec = snap->callGraph.recursionCycles();
    if (!rec.empty()) {
        std::cout << "\nRecursive call groups:\n";
        for (const auto& c : rec) {
            std::cout << " ";
            for (auto id : c) std::cout << " " << snap->symbols[id].qualifiedName;
            std::cout << "\n";
        }
    }
    if (!snap->diagnostics.empty()) {
        std::cout << "\nDiagnostics:\n";
        for (const auto& d : snap->diagnostics) std::cout << "  " << format(d) << "\n";
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
        usage();
        return args.empty() ? 2 : 0;
    }
    if (args[0] == "--version" || args[0] == "-v") {
        std::cout << "codetrace 0.1.0\n";
        return 0;
    }
    try {
        if (args[0] == "analyze") return analyzeCommand({args.begin() + 1, args.end()});
        if (args[0] == "backends") {
            auto reg = createDefaultRegistry();
            for (const auto& a : reg->analyzers())
                std::cout << toString(a->language()) << "\t" << a->backendName() << "\t"
                          << (a->isAvailable() ? "available" : "unavailable") << "\n";
            return 0;
        }
        if (args[0] == "serve") {
#ifdef _WIN32
            _setmode(_fileno(stdin), _O_BINARY);
            _setmode(_fileno(stdout), _O_BINARY);
#endif
            std::ios::sync_with_stdio(false);
            protocol::RpcService service(createDefaultRegistry());
            service.serve(std::cin, std::cout);
            return 0;
        }
    } catch (const std::exception& e) {
        std::cerr << "codetrace: " << e.what() << "\n";
        return 1;
    }
    std::cerr << "unknown command '" << args[0] << "'\n\n";
    usage();
    return 2;
}

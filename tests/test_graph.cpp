#include "core/analysis/project.hpp"
#include "core/graph/call_graph.hpp"
#include "core/graph/dependency_graph.hpp"
#include "core/graph/layout.hpp"

#include <gtest/gtest.h>

using namespace codetrace;

namespace {

Symbol fn(std::string file, std::string qn, Language lang, SymbolKind kind = SymbolKind::Function, std::string container = {}) {
    Symbol s;
    s.file = std::move(file);
    s.qualifiedName = qn;
    auto dot = qn.find_last_of(".:");
    s.name = dot == std::string::npos ? qn : qn.substr(dot + 1);
    s.kind = kind;
    s.language = lang;
    s.container = std::move(container);
    return s;
}

CallSite call(std::string caller, std::string callee, std::uint32_t line = 1, std::string receiver = {}) {
    CallSite c;
    c.callerQualifiedName = std::move(caller);
    c.calleeName = std::move(callee);
    c.line = line;
    c.receiver = receiver;
    c.isMemberCall = !receiver.empty();
    return c;
}

const Symbol* byName(const ProjectSnapshot& s, const std::string& qn, const std::string& file = {}) {
    for (const auto& sym : s.symbols)
        if (sym.qualifiedName == qn && (file.empty() || sym.file == file)) return &sym;
    return nullptr;
}

}  // namespace

TEST(CallGraph, PrefersSameFileThenImportedFile) {
    FileAnalysis a{"/p/a.py", Language::Python, "test", {fn("/p/a.py", "main", Language::Python), fn("/p/a.py", "util", Language::Python)},
                   {call("main", "util"), call("main", "shared")}, {}, {}, {}, false};
    a.imports.push_back({"b", {}, ImportKind::Module, 0, 1});
    FileAnalysis b{"/p/b.py", Language::Python, "test", {fn("/p/b.py", "util", Language::Python), fn("/p/b.py", "shared", Language::Python)}, {}, {}, {}, {}, false};
    FileAnalysis c{"/p/c.py", Language::Python, "test", {fn("/p/c.py", "shared", Language::Python)}, {}, {}, {}, {}, false};
    auto snap = buildSnapshot("/p", {a, b, c});

    const Symbol* main = byName(*snap, "main");
    ASSERT_NE(main, nullptr);
    auto callees = snap->callGraph.callees(main->id);
    ASSERT_EQ(callees.size(), 2u);
    EXPECT_EQ(snap->symbols[callees[0]].file, "/p/a.py");   // util: same file wins over b.py
    EXPECT_EQ(snap->symbols[callees[1]].file, "/p/b.py");   // shared: imported b.py wins over c.py
    EXPECT_EQ(snap->callGraph.edges()[snap->callGraph.outgoing(main->id)[0]].resolution, Resolution::Exact);
    EXPECT_EQ(snap->callGraph.edges()[snap->callGraph.outgoing(main->id)[1]].resolution, Resolution::Likely);
}

TEST(CallGraph, AmbiguousAndExternalCallsAreUnresolved) {
    FileAnalysis a{"/p/a.js", Language::JavaScript, "test", {fn("/p/a.js", "main", Language::JavaScript)},
                   {call("main", "dup"), call("main", "console.log")}, {}, {}, {}, false};
    FileAnalysis b{"/p/b.js", Language::JavaScript, "test", {fn("/p/b.js", "dup", Language::JavaScript)}, {}, {}, {}, {}, false};
    FileAnalysis c{"/p/c.js", Language::JavaScript, "test", {fn("/p/c.js", "dup", Language::JavaScript)}, {}, {}, {}, {}, false};
    auto snap = buildSnapshot("/p", {a, b, c});
    EXPECT_TRUE(snap->callGraph.edges().empty());
    ASSERT_EQ(snap->callGraph.unresolved().size(), 2u);
    for (const auto& u : snap->callGraph.unresolved()) {
        if (u.calleeName == "dup") EXPECT_EQ(u.candidates, 2u);
        else EXPECT_EQ(u.candidates, 0u);
    }
}

TEST(CallGraph, SelfCallsResolveToOwnClass) {
    FileAnalysis a{"/p/m.py", Language::Python, "test",
                   {fn("/p/m.py", "A.run", Language::Python, SymbolKind::Method, "A"), fn("/p/m.py", "A.step", Language::Python, SymbolKind::Method, "A"),
                    fn("/p/m.py", "B.step", Language::Python, SymbolKind::Method, "B")},
                   {call("A.run", "step", 3, "self")}, {}, {}, {}, false};
    auto snap = buildSnapshot("/p", {a});
    auto callees = snap->callGraph.callees(byName(*snap, "A.run")->id);
    ASSERT_EQ(callees.size(), 1u);
    EXPECT_EQ(snap->symbols[callees[0]].qualifiedName, "A.step");
}

TEST(CallGraph, CrossLanguageBindings) {
    FileAnalysis cpp{"/p/ext.cpp", Language::Cpp, "test", {fn("/p/ext.cpp", "fast_add", Language::Cpp)}, {}, {}, {}, {}, false};
    cpp.bindings.push_back({Language::Cpp, Language::Python, "add", "fast_add", 5});
    FileAnalysis py{"/p/use.py", Language::Python, "test", {fn("/p/use.py", "main", Language::Python)},
                    {call("main", "add", 2, "ext")}, {}, {}, {}, false};
    auto snap = buildSnapshot("/p", {cpp, py});
    auto out = snap->callGraph.outgoing(byName(*snap, "main")->id);
    ASSERT_EQ(out.size(), 1u);
    const auto& e = snap->callGraph.edges()[out[0]];
    EXPECT_EQ(snap->symbols[e.callee].qualifiedName, "fast_add");
    EXPECT_EQ(e.resolution, Resolution::CrossLanguage);
}

TEST(CallGraph, ChainsReachabilityAndRecursion) {
    FileAnalysis f{"/p/x.cpp", Language::Cpp, "test",
                   {fn("/p/x.cpp", "a", Language::Cpp), fn("/p/x.cpp", "b", Language::Cpp), fn("/p/x.cpp", "c", Language::Cpp),
                    fn("/p/x.cpp", "d", Language::Cpp), fn("/p/x.cpp", "rec", Language::Cpp)},
                   {call("a", "b"), call("b", "c"), call("c", "a"), call("c", "d"), call("rec", "rec")}, {}, {}, {}, false};
    auto snap = buildSnapshot("/p", {f});
    auto id = [&](const char* n) { return byName(*snap, n)->id; };
    auto chain = snap->callGraph.shortestChain(id("a"), id("d"));
    ASSERT_EQ(chain.size(), 4u);
    EXPECT_EQ(chain.front(), id("a"));
    EXPECT_EQ(chain.back(), id("d"));
    EXPECT_TRUE(snap->callGraph.shortestChain(id("d"), id("a")).empty());
    EXPECT_EQ(snap->callGraph.reachable(id("a"), 1, true).size(), 1u);
    EXPECT_EQ(snap->callGraph.reachable(id("a"), -1, true).size(), 3u);   // b, c, d
    auto cycles = snap->callGraph.recursionCycles();
    ASSERT_EQ(cycles.size(), 2u);   // {a,b,c} and {rec}
}

TEST(Scc, HandlesLargeChainsWithoutRecursionLimits) {
    const std::uint32_t n = 200000;
    std::vector<std::vector<std::uint32_t>> adj(n);
    for (std::uint32_t i = 0; i + 1 < n; ++i) adj[i].push_back(i + 1);
    adj[n - 1].push_back(0);
    auto comps = stronglyConnectedComponents(adj);
    ASSERT_EQ(comps.size(), 1u);
    EXPECT_EQ(comps[0].size(), n);
}

TEST(DependencyGraph, ResolvesImportsPerLanguageAndFindsCycles) {
    std::vector<FileAnalysis> files(6);
    files[0] = {"/p/src/main.cpp", Language::Cpp, "t", {}, {}, {{"util.h", {}, ImportKind::Include, 0, 1}, {"vector", {}, ImportKind::SystemInclude, 0, 2}}, {}, {}, false};
    files[1] = {"/p/include/util.h", Language::Cpp, "t", {}, {}, {}, {}, {}, false};
    files[2] = {"/p/pkg/a.py", Language::Python, "t", {}, {}, {{"b", {}, ImportKind::Relative, 1, 1}, {"", {"c"}, ImportKind::Relative, 1, 2}}, {}, {}, false};
    files[3] = {"/p/pkg/b.py", Language::Python, "t", {}, {}, {{"pkg.a", {}, ImportKind::Module, 0, 1}}, {}, {}, false};
    files[4] = {"/p/pkg/c.py", Language::Python, "t", {}, {}, {}, {}, {}, false};
    files[5] = {"/p/web/app.js", Language::JavaScript, "t", {}, {}, {{"./lib", {}, ImportKind::Relative, 0, 1}, {"react", {}, ImportKind::Module, 0, 2}}, {}, {}, false};
    files.push_back({"/p/web/lib/index.js", Language::JavaScript, "t", {}, {}, {}, {}, {}, false});
    DependencyGraph g;
    g.build(files);
    auto idx = [&](const char* f) { return static_cast<std::uint32_t>(g.indexOf(f)); };
    EXPECT_TRUE(g.dependsOn(idx("/p/src/main.cpp"), idx("/p/include/util.h")));
    EXPECT_TRUE(g.dependsOn(idx("/p/pkg/a.py"), idx("/p/pkg/b.py")));
    EXPECT_TRUE(g.dependsOn(idx("/p/pkg/a.py"), idx("/p/pkg/c.py")));
    EXPECT_TRUE(g.dependsOn(idx("/p/pkg/b.py"), idx("/p/pkg/a.py")));
    EXPECT_TRUE(g.dependsOn(idx("/p/web/app.js"), idx("/p/web/lib/index.js")));
    EXPECT_EQ(g.external().size(), 2u);   // <vector>, react
    auto cycles = g.cycles();
    ASSERT_EQ(cycles.size(), 1u);
    EXPECT_EQ(cycles[0].size(), 2u);
}

TEST(Layout, LayersFollowEdgesAndToleratesCycles) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edges = {{0, 1}, {1, 2}, {2, 1}, {0, 3}};
    auto nodes = layeredLayout(4, edges);
    ASSERT_EQ(nodes.size(), 4u);
    EXPECT_LT(nodes[0].x, nodes[1].x);
    EXPECT_LT(nodes[0].x, nodes[3].x);
    EXPECT_EQ(nodes[1].layer, nodes[2].layer);   // the 1<->2 cycle shares a layer
    for (std::size_t i = 0; i < nodes.size(); ++i)
        for (std::size_t j = i + 1; j < nodes.size(); ++j)
            EXPECT_FALSE(nodes[i].x == nodes[j].x && nodes[i].y == nodes[j].y) << "overlap " << i << " " << j;
}

TEST(Layout, FixedLayersAreRespected) {
    auto nodes = layeredLayout(3, {{0, 1}, {2, 0}}, {}, {0, 1, -1});
    EXPECT_LT(nodes[2].x, nodes[0].x);
    EXPECT_LT(nodes[0].x, nodes[1].x);
    EXPECT_EQ(nodes[2].layer, -1);
}

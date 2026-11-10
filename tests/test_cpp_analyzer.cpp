#include "analyzers/cpp/cpp_analyzer.hpp"
#include "test_util.hpp"

using namespace codetrace;

namespace {
const char* kCode = R"CPP(
#include "util.h"
#include <vector>

namespace app {
namespace detail { int helper(int x) { return x * 2; } }

class Engine : public Base {
public:
    Engine(int n) : count_(n), items_{1, 2} { init(); }
    ~Engine() { shutdown(); }
    int run() const { return detail::helper(count_) + step(); }
    bool operator==(const Engine& o) const { return count_ == o.count_; }
private:
    void init();
    int step() const;
    int count_;
    std::vector<int> items_;
};

void Engine::init() { log("init"); }
int Engine::step() const { return compute<int>(count_); }

template <typename T>
T compute(T v) { return v; }
}  // namespace app

int main(int argc, char** argv) {
    app::Engine e(3);
    auto twice = [](int v) { return app::detail::helper(v); };
    return e.run() + twice(argc);
}
)CPP";
}

TEST(CppAnalyzer, FindsFunctionsClassesAndMethods) {
    FOR_EACH_BACKEND(Language::Cpp) {
        auto fa = analyzeWith(*backend, kCode, "/virtual/engine.cpp");
        SCOPED_TRACE(dumpSymbols(fa));
        ASSERT_NE(findSymbol(fa, "app::Engine"), nullptr);
        EXPECT_EQ(findSymbol(fa, "app::Engine")->kind, SymbolKind::Class);
        ASSERT_NE(findSymbol(fa, "app::detail::helper"), nullptr);
        EXPECT_EQ(findSymbol(fa, "app::detail::helper")->kind, SymbolKind::Function);
        ASSERT_NE(findSymbol(fa, "app::Engine::Engine"), nullptr);
        EXPECT_EQ(findSymbol(fa, "app::Engine::Engine")->kind, SymbolKind::Constructor);
        ASSERT_NE(findSymbol(fa, "app::Engine::run"), nullptr);
        EXPECT_EQ(findSymbol(fa, "app::Engine::run")->kind, SymbolKind::Method);
        EXPECT_NE(findSymbol(fa, "app::Engine::init"), nullptr);
        EXPECT_NE(findSymbol(fa, "app::Engine::step"), nullptr);
        EXPECT_NE(findSymbol(fa, "app::compute"), nullptr);
        EXPECT_NE(findSymbol(fa, "main"), nullptr);
        EXPECT_EQ(findSymbol(fa, "main")->range.line, 28u);
    }
}

TEST(CppAnalyzer, FindsCalls) {
    FOR_EACH_BACKEND(Language::Cpp) {
        auto fa = analyzeWith(*backend, kCode, "/virtual/engine.cpp");
        SCOPED_TRACE(dumpSymbols(fa));
        EXPECT_TRUE(hasCall(fa, "app::Engine::Engine", "init"));
        EXPECT_TRUE(hasCall(fa, "app::Engine::run", "helper"));
        EXPECT_TRUE(hasCall(fa, "app::Engine::run", "step"));
        EXPECT_TRUE(hasCall(fa, "app::Engine::step", "compute"));
        EXPECT_TRUE(hasCall(fa, "main", "run"));
        EXPECT_TRUE(hasCall(fa, "main", "helper"));   // inside a lambda: attributed to main
    }
}

TEST(CppAnalyzer, RecordsIncludes) {
    FOR_EACH_BACKEND(Language::Cpp) {
        auto fa = analyzeWith(*backend, kCode, "/virtual/engine.cpp");
        ASSERT_GE(fa.imports.size(), 2u);
        bool local = false, system = false;
        for (const auto& i : fa.imports) {
            if (i.module == "util.h" && i.kind == ImportKind::Include) local = true;
            if (i.module == "vector" && i.kind == ImportKind::SystemInclude) system = true;
        }
        EXPECT_TRUE(local);
        EXPECT_TRUE(system);
    }
}

TEST(CppHeuristic, DoesNotTreatDeclarationsOrControlFlowAsCalls) {
    cpp::HeuristicCppAnalyzer a;
    auto fa = analyzeWith(a, R"(
void f() {
    if (ready()) { while (more()) work(); }
    std::string s("x");
    Widget w(1, 2);
    for (int i = 0; i < n(); ++i) {}
    return;
}
)");
    SCOPED_TRACE(dumpSymbols(fa));
    EXPECT_TRUE(hasCall(fa, "f", "ready"));
    EXPECT_TRUE(hasCall(fa, "f", "more"));
    EXPECT_TRUE(hasCall(fa, "f", "work"));
    EXPECT_TRUE(hasCall(fa, "f", "n"));
    EXPECT_FALSE(hasCall(fa, "f", "if"));
    EXPECT_FALSE(hasCall(fa, "f", "while"));
    EXPECT_FALSE(hasCall(fa, "f", "w"));
    EXPECT_FALSE(hasCall(fa, "f", "s"));
}

TEST(CppHeuristic, SkipsIfZeroBlocksCommentsAndStrings) {
    cpp::HeuristicCppAnalyzer a;
    auto fa = analyzeWith(a, R"(
#if 0
void dead() { gone(); }
#endif
// void commented() {}
const char* s = "void fake() {}";
const char* r = R"x(void raw() { })x";
void alive() { /* call(); */ real(); }
)");
    SCOPED_TRACE(dumpSymbols(fa));
    EXPECT_EQ(findSymbol(fa, "dead"), nullptr);
    EXPECT_EQ(findSymbol(fa, "commented"), nullptr);
    EXPECT_EQ(findSymbol(fa, "fake"), nullptr);
    EXPECT_EQ(findSymbol(fa, "raw"), nullptr);
    ASSERT_NE(findSymbol(fa, "alive"), nullptr);
    EXPECT_TRUE(hasCall(fa, "alive", "real"));
    EXPECT_FALSE(hasCall(fa, "alive", "call"));
}

TEST(CppHeuristic, DetectsPybind11Bindings) {
    cpp::HeuristicCppAnalyzer a;
    auto fa = analyzeWith(a, R"(
int add(int a, int b) { return a + b; }
PYBIND11_MODULE(fastmath, m) {
    m.def("add_ints", &add, "Add two ints");
}
)");
    ASSERT_EQ(fa.bindings.size(), 1u);
    EXPECT_EQ(fa.bindings[0].exportedName, "add_ints");
    EXPECT_EQ(fa.bindings[0].targetName, "add");
    EXPECT_EQ(fa.bindings[0].toLanguage, Language::Python);
}

TEST(CppHeuristic, ReportsUnbalancedBracesWithoutFailing) {
    cpp::HeuristicCppAnalyzer a;
    auto fa = analyzeWith(a, "void ok() { a(); }\nvoid broken() { if (x) { b();\n");
    EXPECT_NE(findSymbol(fa, "ok"), nullptr);
    EXPECT_NE(findSymbol(fa, "broken"), nullptr);
    EXPECT_TRUE(fa.partial);
    EXPECT_FALSE(fa.diagnostics.empty());
}

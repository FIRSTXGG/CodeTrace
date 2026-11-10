#include "analyzers/python/python_analyzer.hpp"
#include "test_util.hpp"

using namespace codetrace;

namespace {
const char* kCode = R"PY(
import os, sys as system
from collections import defaultdict
from .models import Account, Ledger as L
from .. import config


def helper(x, *args, **kw):
    return os.path.join(str(x), "y")


class Bank(Base):
    """A bank.  def fake(): pass"""

    def __init__(self, name):
        self.name = name
        self.setup()

    def setup(self):
        helper(self.name)
        acc = Account(self.name)

    @staticmethod
    def build(items=sorted([])):
        def inner():
            return helper(1)
        return inner()

    async def fetch(self):
        await self.setup()


if __name__ == "__main__":
    Bank("x").setup()
)PY";
}

TEST(PythonAnalyzer, FindsSymbolsWithQualifiedNames) {
    FOR_EACH_BACKEND(Language::Python) {
        auto fa = analyzeWith(*backend, kCode, "/virtual/bank.py");
        SCOPED_TRACE(dumpSymbols(fa));
        ASSERT_NE(findSymbol(fa, "helper"), nullptr);
        EXPECT_EQ(findSymbol(fa, "helper")->kind, SymbolKind::Function);
        EXPECT_EQ(findSymbol(fa, "helper")->range.line, 8u);
        ASSERT_NE(findSymbol(fa, "Bank"), nullptr);
        EXPECT_EQ(findSymbol(fa, "Bank")->kind, SymbolKind::Class);
        ASSERT_NE(findSymbol(fa, "Bank.__init__"), nullptr);
        EXPECT_EQ(findSymbol(fa, "Bank.__init__")->kind, SymbolKind::Constructor);
        ASSERT_NE(findSymbol(fa, "Bank.setup"), nullptr);
        EXPECT_EQ(findSymbol(fa, "Bank.setup")->kind, SymbolKind::Method);
        EXPECT_NE(findSymbol(fa, "Bank.build.inner"), nullptr);
        EXPECT_NE(findSymbol(fa, "Bank.fetch"), nullptr);
        EXPECT_EQ(findSymbol(fa, "fake"), nullptr);   // inside a docstring
    }
}

TEST(PythonAnalyzer, FindsCallsAndAttributesThem) {
    FOR_EACH_BACKEND(Language::Python) {
        auto fa = analyzeWith(*backend, kCode, "/virtual/bank.py");
        SCOPED_TRACE(dumpSymbols(fa));
        EXPECT_TRUE(hasCall(fa, "Bank.__init__", "setup"));
        EXPECT_TRUE(hasCall(fa, "Bank.setup", "helper"));
        EXPECT_TRUE(hasCall(fa, "Bank.setup", "Account"));
        EXPECT_TRUE(hasCall(fa, "Bank.build.inner", "helper"));
        EXPECT_TRUE(hasCall(fa, "Bank.build", "inner"));
        EXPECT_TRUE(hasCall(fa, "helper", "join"));
        EXPECT_TRUE(hasCall(fa, "<module>", "setup"));
        EXPECT_TRUE(hasCall(fa, "<module>", "sorted"));   // default argument evaluated at module level
        EXPECT_NE(findSymbol(fa, "<module>"), nullptr);
        for (const auto& c : fa.calls)
            if (c.calleeName == "setup" && c.callerQualifiedName == "Bank.__init__") {
                EXPECT_TRUE(c.isMemberCall);
                EXPECT_EQ(c.receiver, "self");
            }
    }
}

TEST(PythonAnalyzer, RecordsImports) {
    FOR_EACH_BACKEND(Language::Python) {
        auto fa = analyzeWith(*backend, kCode, "/virtual/bank.py");
        auto find = [&](const std::string& m, int level) -> const Import* {
            for (const auto& i : fa.imports)
                if (i.module == m && i.relativeLevel == level) return &i;
            return nullptr;
        };
        EXPECT_NE(find("os", 0), nullptr);
        EXPECT_NE(find("sys", 0), nullptr);
        ASSERT_NE(find("collections", 0), nullptr);
        ASSERT_NE(find("models", 1), nullptr);
        EXPECT_EQ(find("models", 1)->names.size(), 2u);
        ASSERT_NE(find("", 2), nullptr);
        EXPECT_EQ(find("", 2)->names, std::vector<std::string>{"config"});
    }
}

TEST(PythonAnalyzer, SyntaxErrorFallsBackToHeuristicWithDiagnostic) {
    python::AstPythonAnalyzer ast;
    if (!ast.isAvailable()) GTEST_SKIP() << "python3 not available";
    auto fa = analyzeWith(ast, "def ok():\n    pass\n\ndef broken(:\n    call()\n", "/virtual/broken.py");
    EXPECT_EQ(fa.backend, "heuristic-python");
    EXPECT_TRUE(fa.partial);
    ASSERT_FALSE(fa.diagnostics.empty());
    EXPECT_NE(fa.diagnostics[0].message.find("SyntaxError"), std::string::npos);
    EXPECT_EQ(fa.diagnostics[0].line, 4u);
    EXPECT_NE(findSymbol(fa, "ok"), nullptr);
}

TEST(PythonAnalyzer, BatchKeepsFilesApart) {
    python::AstPythonAnalyzer ast;
    if (!ast.isAvailable()) GTEST_SKIP() << "python3 not available";
    std::vector<SourceFile> files = {{"/v/a.py", "def a():\n    b()\n"},
                                     {"/v/b.py", "def b():\n    pass\n"},
                                     {"/v/c.py", "def c(:\n"}};
    auto out = ast.analyzeBatch(files, {});
    ASSERT_EQ(out.size(), 3u);
    EXPECT_NE(findSymbol(out[0], "a"), nullptr);
    EXPECT_EQ(findSymbol(out[0], "b"), nullptr);
    EXPECT_NE(findSymbol(out[1], "b"), nullptr);
    EXPECT_EQ(out[0].backend, "python-ast");
    EXPECT_EQ(out[2].backend, "heuristic-python");
}

TEST(PythonHeuristic, HandlesContinuationLinesAndStrings) {
    python::HeuristicPythonAnalyzer a;
    auto fa = analyzeWith(a, "def f(a,\n      b):\n    x = '''\ndef nope():\n'''\n    return g(a,\n             b)\n");
    SCOPED_TRACE(dumpSymbols(fa));
    ASSERT_NE(findSymbol(fa, "f"), nullptr);
    EXPECT_EQ(findSymbol(fa, "nope"), nullptr);
    EXPECT_TRUE(hasCall(fa, "f", "g"));
}

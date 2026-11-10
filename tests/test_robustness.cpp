// Analysis errors must never crash the application: every analyzer must
// survive arbitrary input, truncated files and pathological nesting.
#include "test_util.hpp"

#include <random>

using namespace codetrace;

namespace {

std::vector<std::string> nastyInputs() {
    std::vector<std::string> v = {
        "", "{", "}", "}}}}", "((((", "\"", "'", "/*", "`${", "#if 0", "#include <", "R\"x(", "def", "class",
        "def f(:", "function (", "=>", "class { constructor(", "template <", "operator", "a::b::(", "\\",
        "\xEF\xBB\xBF void f() {}", std::string("\0\0\0", 3), "\xff\xfe\xfd", "/regex", "a /= b / c /",
        std::string(5000, '{'), std::string(5000, '('), std::string(5000, '['), std::string(3000, ' ') + "x",
    };
    // Deeply nested but valid-looking code.
    std::string deepPy;
    for (int i = 0; i < 300; ++i) deepPy += std::string(static_cast<std::size_t>(i), ' ') + "if x:\n";
    v.push_back(deepPy);
    std::string deepCall = "f(";
    for (int i = 0; i < 3000; ++i) deepCall += "g(";
    v.push_back(deepCall);
    return v;
}

std::string randomSource(std::mt19937& rng, std::size_t len) {
    static const std::string alphabet =
        "abcdefghijklmnopqrstuvwxyz_ABC0123456789(){}[]<>;:,.=+-*/%!&|^~?#\"'`\\@$ \t\n";
    std::uniform_int_distribution<std::size_t> pick(0, alphabet.size() - 1);
    std::string s;
    for (std::size_t i = 0; i < len; ++i) s.push_back(alphabet[pick(rng)]);
    return s;
}

}  // namespace

TEST(Robustness, AnalyzersSurviveMalformedInput) {
    for (auto lang : {Language::Cpp, Language::Python, Language::JavaScript}) {
        FOR_EACH_BACKEND(lang) {
            for (const auto& input : nastyInputs()) {
                SCOPED_TRACE(input.substr(0, 40));
                FileAnalysis fa;
                EXPECT_NO_THROW(fa = analyzeWith(*backend, input, "/virtual/nasty"));
            }
        }
    }
}

TEST(Robustness, AnalyzersSurviveRandomTokenSoup) {
    std::mt19937 rng(12345);
    std::vector<std::string> inputs;
    for (int i = 0; i < 60; ++i) inputs.push_back(randomSource(rng, 50 + static_cast<std::size_t>(i) * 40));
    for (auto lang : {Language::Cpp, Language::Python, Language::JavaScript}) {
        FOR_EACH_BACKEND(lang) {
            // The python-ast backend spawns a process per call; batch it.
            if (backend->preferredBatchSize() > 1) {
                std::vector<SourceFile> files;
                for (std::size_t i = 0; i < inputs.size(); ++i) files.push_back({"/virtual/r" + std::to_string(i) + ".py", inputs[i]});
                EXPECT_NO_THROW(backend->analyzeBatch(files, {}));
                continue;
            }
            for (const auto& input : inputs) EXPECT_NO_THROW(analyzeWith(*backend, input, "/virtual/random"));
        }
    }
}

TEST(Robustness, BrokenFilesStillYieldPartialResults) {
    FOR_EACH_BACKEND(Language::Cpp) {
        auto fa = analyzeWith(*backend, "int good() { return 1; }\nint bad( { \n", "/virtual/broken.cpp");
        EXPECT_NE(findSymbol(fa, "good"), nullptr);
    }
    FOR_EACH_BACKEND(Language::JavaScript) {
        auto fa = analyzeWith(*backend, "function good() { return 1; }\nfunction bad( {\n", "/virtual/broken.js");
        EXPECT_NE(findSymbol(fa, "good"), nullptr);
        EXPECT_TRUE(fa.partial || !fa.diagnostics.empty());
    }
}

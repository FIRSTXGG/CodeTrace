#include "core/protocol/serialize.hpp"
#include "test_util.hpp"

#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
using namespace codetrace;

namespace {

const std::string kSample = std::string(CODETRACE_FIXTURES_DIR) + "/sample_project";

class TempProject {
public:
    TempProject() {
        dir_ = fs::temp_directory_path() / ("codetrace-test-" + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
                                            "-" + std::to_string(counter_++));
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    ~TempProject() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    void write(const std::string& rel, const std::string& content) {
        fs::create_directories((dir_ / rel).parent_path());
        std::ofstream(dir_ / rel, std::ios::binary) << content;
    }
    std::string root() const { return dir_.generic_string(); }

private:
    fs::path dir_;
    static inline int counter_ = 0;
};

const Symbol* find(const ProjectSnapshot& s, const std::string& qn) {
    for (const auto& sym : s.symbols)
        if (sym.qualifiedName == qn) return &sym;
    return nullptr;
}

bool calls(const ProjectSnapshot& s, const std::string& from, const std::string& to) {
    const Symbol* a = find(s, from);
    const Symbol* b = find(s, to);
    if (!a || !b) return false;
    auto c = s.callGraph.callees(a->id);
    return std::find(c.begin(), c.end(), b->id) != c.end();
}

}  // namespace

// The MVP acceptance criterion: open a C++/Python/JS project, list functions and
// their calls, and know the source line of each.
TEST(Project, AnalyzesMultiLanguageSampleProject) {
    ProjectAnalyzer analyzer(createDefaultRegistry());
    ProjectOptions opts;
    opts.root = kSample;
    auto snap = analyzer.analyze(opts);

    EXPECT_EQ(snap->files.size(), 11u);
    EXPECT_EQ(snap->languages.size(), 3u);

    EXPECT_TRUE(calls(*snap, "main", "report"));
    EXPECT_TRUE(calls(*snap, "report", "geo::Polygon::area"));
    EXPECT_TRUE(calls(*snap, "geo::Polygon::perimeter", "geo::distance"));
    EXPECT_TRUE(calls(*snap, "open_account", "Account.deposit"));
    EXPECT_TRUE(calls(*snap, "open_account", "initial_bonus"));
    EXPECT_TRUE(calls(*snap, "Account.deposit", "Account._validate"));
    EXPECT_TRUE(calls(*snap, "Stats.sum", "add"));
    EXPECT_TRUE(calls(*snap, "square", "multiply"));

    const Symbol* area = find(*snap, "geo::Polygon::area");
    ASSERT_NE(area, nullptr);
    EXPECT_EQ(area->range.line, 9u);
    EXPECT_NE(area->file.find("cpp/src/geometry.cpp"), std::string::npos);

    // py/app/service.py <-> py/app/utils.py import each other.
    ASSERT_EQ(snap->dependencies.cycles().size(), 1u);
    // fib is recursive.
    bool fibCycle = false;
    for (const auto& c : snap->callGraph.recursionCycles())
        if (c.size() == 1 && snap->symbols[c[0]].name == "fib") fibCycle = true;
    EXPECT_TRUE(fibCycle);
}

TEST(Project, IncrementalUpdateReanalyzesOnlyChangedFiles) {
    TempProject p;
    p.write("a.py", "def a():\n    b()\n");
    p.write("b.py", "def b():\n    pass\n");
    p.write("c.js", "function c() { d(); }\nfunction d() {}\n");

    ProjectAnalyzer analyzer(createDefaultRegistry());
    ProjectOptions opts;
    opts.root = p.root();
    auto first = analyzer.analyze(opts);
    EXPECT_EQ(first->reanalyzedFiles, 3u);

    auto second = analyzer.analyze(opts);
    EXPECT_EQ(second->reanalyzedFiles, 0u);
    EXPECT_EQ(second->symbols.size(), first->symbols.size());

    p.write("b.py", "def b():\n    pass\n\ndef e():\n    b()\n");
    analyzer.invalidate({p.root() + "/b.py"});   // as a file watcher would
    auto third = analyzer.analyze(opts);
    EXPECT_EQ(third->reanalyzedFiles, 1u);
    EXPECT_NE(find(*third, "e"), nullptr);
    EXPECT_TRUE(calls(*third, "e", "b"));

    fs::remove(fs::path(p.root()) / "c.js");
    auto fourth = analyzer.analyze(opts);
    EXPECT_EQ(find(*fourth, "c"), nullptr);
    EXPECT_EQ(fourth->files.size(), 2u);
}

TEST(Project, IgnoresVendorAndBuildDirectoriesAndUnknownFiles) {
    TempProject p;
    p.write("src/main.py", "def main():\n    pass\n");
    p.write("node_modules/lib/index.js", "function vendored() {}\n");
    p.write("build/gen.cpp", "void generated() {}\n");
    p.write(".git/hooks/x.py", "def hook(): pass\n");
    p.write("README.md", "# readme\n");
    p.write("bin/tool", "#!/usr/bin/env python3\ndef tool():\n    pass\n");
    ProjectAnalyzer analyzer(createDefaultRegistry());
    ProjectOptions opts;
    opts.root = p.root();
    auto snap = analyzer.analyze(opts);
    EXPECT_NE(find(*snap, "main"), nullptr);
    EXPECT_NE(find(*snap, "tool"), nullptr);   // detected by shebang
    EXPECT_EQ(find(*snap, "vendored"), nullptr);
    EXPECT_EQ(find(*snap, "generated"), nullptr);
    EXPECT_EQ(find(*snap, "hook"), nullptr);
}

TEST(Project, CancellationStopsEarlyAndReportsIt) {
    TempProject p;
    for (int i = 0; i < 200; ++i) p.write("f" + std::to_string(i) + ".js", "function f" + std::to_string(i) + "() { g(); }\n");
    ProjectAnalyzer analyzer(createDefaultRegistry());
    ProjectOptions opts;
    opts.root = p.root();
    opts.threads = 1;
    CancellationToken cancel;
    std::size_t calls = 0;
    auto snap = analyzer.analyze(opts, [&](const Progress& pr) {
        if (++calls == 5) cancel.cancel();
        (void)pr;
    }, &cancel);
    EXPECT_TRUE(snap->cancelled);
    EXPECT_LT(snap->files.size(), 200u);

    // Next run reuses what was finished and completes.
    auto full = analyzer.analyze(opts);
    EXPECT_FALSE(full->cancelled);
    EXPECT_EQ(full->files.size(), 200u);
    EXPECT_LT(full->reanalyzedFiles, 200u);
}

TEST(Project, MissingRootIsADiagnosticNotACrash) {
    ProjectAnalyzer analyzer(createDefaultRegistry());
    ProjectOptions opts;
    opts.root = "/definitely/not/here/codetrace";
    auto snap = analyzer.analyze(opts);
    EXPECT_TRUE(snap->files.empty());
    ASSERT_FALSE(snap->diagnostics.empty());
    EXPECT_EQ(snap->diagnostics[0].severity, Severity::Error);
}

TEST(Project, OversizedFilesAreSkippedWithWarning) {
    TempProject p;
    p.write("big.js", std::string(2000, 'x'));
    p.write("small.js", "function s() {}\n");
    ProjectAnalyzer analyzer(createDefaultRegistry());
    ProjectOptions opts;
    opts.root = p.root();
    opts.maxFileBytes = 1000;
    auto snap = analyzer.analyze(opts);
    EXPECT_EQ(snap->files.size(), 1u);
    bool warned = false;
    for (const auto& d : snap->diagnostics) warned |= d.message.find("too large") != std::string::npos;
    EXPECT_TRUE(warned);
}

TEST(Project, SerializationProducesValidJson) {
    ProjectAnalyzer analyzer(createDefaultRegistry());
    ProjectOptions opts;
    opts.root = kSample;
    auto snap = analyzer.analyze(opts);
    auto text = protocol::callGraphJson(*snap).dump();
    auto parsed = json::Value::parse(text);
    EXPECT_EQ(parsed["nodes"].size(), static_cast<std::size_t>(std::count_if(snap->symbols.begin(), snap->symbols.end(),
                                                                             [](const Symbol& s) { return isCallable(s.kind); })));
    EXPECT_EQ(parsed["edges"].size(), snap->callGraph.edges().size());
    EXPECT_NE(protocol::callGraphDot(*snap).find("digraph"), std::string::npos);
}

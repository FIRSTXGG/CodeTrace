#include "analyzers/registry.hpp"

#include "analyzers/cpp/cpp_analyzer.hpp"
#include "analyzers/javascript/js_analyzer.hpp"
#include "analyzers/python/python_analyzer.hpp"

namespace codetrace {

std::shared_ptr<AnalyzerRegistry> createDefaultRegistry() {
    auto reg = std::make_shared<AnalyzerRegistry>();

    for (const char* ext : {".cpp", ".cc", ".cxx", ".c++", ".c", ".h", ".hpp", ".hh", ".hxx", ".h++", ".ipp", ".inl",
                            ".tpp", ".cppm", ".ixx"})
        reg->registerExtension(ext, Language::Cpp);
    for (const char* ext : {".py", ".pyw", ".pyi"}) reg->registerExtension(ext, Language::Python);
    for (const char* ext : {".js", ".mjs", ".cjs", ".jsx"}) reg->registerExtension(ext, Language::JavaScript);

    reg->registerAnalyzer(cpp::createLibclangAnalyzer());   // nullptr if not built: ignored
    reg->registerAnalyzer(std::make_shared<cpp::HeuristicCppAnalyzer>());

    reg->registerAnalyzer(std::make_shared<python::AstPythonAnalyzer>());
    reg->registerAnalyzer(std::make_shared<python::HeuristicPythonAnalyzer>());

    reg->registerAnalyzer(js::createTreeSitterAnalyzer());
    reg->registerAnalyzer(std::make_shared<js::HeuristicJsAnalyzer>());
    return reg;
}

}  // namespace codetrace

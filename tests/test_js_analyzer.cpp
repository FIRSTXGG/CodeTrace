#include "analyzers/javascript/js_analyzer.hpp"
#include "test_util.hpp"

using namespace codetrace;

namespace {
const char* kCode = R"JS(
import fs from 'fs';
import { helper, other as alias } from './util.js';
const path = require('path');

export function load(file) {
  const data = fs.readFileSync(path.join('/', file));
  return parse(data);
}

function parse(text) {
  return text.split('\n').map((line) => helper(line));
}

const double = (x) => multiply(x, 2);
const handler = async function () { await load('a'); };

class Store extends Base {
  constructor(items) {
    super(items);
    this.items = items;
  }
  add(item) {
    this.items.push(item);
    this.#notify();
  }
  #notify() { emit(`changed ${this.items.length}`); }
  static create() { return new Store([]); }
}

const api = {
  get(id) { return fetchItem(id); },
  remove: (id) => deleteItem(id),
};

load('x');
)JS";
}

TEST(JsAnalyzer, FindsFunctionsClassesAndMethods) {
    FOR_EACH_BACKEND(Language::JavaScript) {
        auto fa = analyzeWith(*backend, kCode, "/virtual/app.js");
        SCOPED_TRACE(dumpSymbols(fa));
        ASSERT_NE(findSymbol(fa, "load"), nullptr);
        EXPECT_EQ(findSymbol(fa, "load")->range.line, 6u);
        EXPECT_NE(findSymbol(fa, "parse"), nullptr);
        EXPECT_NE(findSymbol(fa, "double"), nullptr);
        EXPECT_NE(findSymbol(fa, "handler"), nullptr);
        ASSERT_NE(findSymbol(fa, "Store"), nullptr);
        EXPECT_EQ(findSymbol(fa, "Store")->kind, SymbolKind::Class);
        ASSERT_NE(findSymbol(fa, "Store.constructor"), nullptr);
        EXPECT_EQ(findSymbol(fa, "Store.constructor")->kind, SymbolKind::Constructor);
        EXPECT_NE(findSymbol(fa, "Store.add"), nullptr);
        EXPECT_NE(findSymbol(fa, "Store.#notify"), nullptr);
        EXPECT_NE(findSymbol(fa, "Store.create"), nullptr);
        EXPECT_NE(findSymbol(fa, "api.get"), nullptr);
        EXPECT_NE(findSymbol(fa, "api.remove"), nullptr);
    }
}

TEST(JsAnalyzer, FindsCalls) {
    FOR_EACH_BACKEND(Language::JavaScript) {
        auto fa = analyzeWith(*backend, kCode, "/virtual/app.js");
        SCOPED_TRACE(dumpSymbols(fa));
        EXPECT_TRUE(hasCall(fa, "load", "readFileSync"));
        EXPECT_TRUE(hasCall(fa, "load", "parse"));
        EXPECT_TRUE(hasCall(fa, "parse", "helper"));      // inside an arrow callback
        EXPECT_TRUE(hasCall(fa, "double", "multiply"));   // expression-bodied arrow
        EXPECT_TRUE(hasCall(fa, "handler", "load"));
        EXPECT_TRUE(hasCall(fa, "Store.add", "#notify"));
        EXPECT_TRUE(hasCall(fa, "Store.#notify", "emit"));
        EXPECT_TRUE(hasCall(fa, "Store.create", "Store"));   // new Store()
        EXPECT_TRUE(hasCall(fa, "api.get", "fetchItem"));
        EXPECT_TRUE(hasCall(fa, "api.remove", "deleteItem"));
        EXPECT_TRUE(hasCall(fa, "<module>", "load"));
        EXPECT_FALSE(hasCall(fa, "<module>", "parse"));
    }
}

TEST(JsAnalyzer, RecordsImports) {
    FOR_EACH_BACKEND(Language::JavaScript) {
        auto fa = analyzeWith(*backend, kCode, "/virtual/app.js");
        std::vector<std::string> modules;
        for (const auto& i : fa.imports) modules.push_back(i.module);
        EXPECT_NE(std::find(modules.begin(), modules.end(), "fs"), modules.end());
        EXPECT_NE(std::find(modules.begin(), modules.end(), "./util.js"), modules.end());
        EXPECT_NE(std::find(modules.begin(), modules.end(), "path"), modules.end());
        for (const auto& i : fa.imports)
            if (i.module == "./util.js") {
                EXPECT_EQ(i.kind, ImportKind::Relative);
                EXPECT_NE(std::find(i.names.begin(), i.names.end(), "helper"), i.names.end());
            }
    }
}

TEST(JsHeuristic, RegexAndTemplateLiteralsDoNotConfuseTheLexer) {
    js::HeuristicJsAnalyzer a;
    auto fa = analyzeWith(a, R"JS(
function f(s) {
  const re = /function fake() {/g;
  const t = `${g(s)} and } { ${ {a: 1}.a }`;
  return s.replace(re, '') / 2 / h();
}
function after() { k(); }
)JS");
    SCOPED_TRACE(dumpSymbols(fa));
    EXPECT_EQ(findSymbol(fa, "fake"), nullptr);
    EXPECT_TRUE(hasCall(fa, "f", "g"));
    EXPECT_TRUE(hasCall(fa, "f", "h"));
    ASSERT_NE(findSymbol(fa, "after"), nullptr);
    EXPECT_TRUE(hasCall(fa, "after", "k"));
    EXPECT_FALSE(fa.partial);
}

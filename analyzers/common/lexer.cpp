#include "analyzers/common/lexer.hpp"

#include <array>
#include <vector>

namespace codetrace::lex {

namespace {

bool isIdentStart(unsigned char c, Dialect d) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80 ||
           (d == Dialect::JavaScript && c == '$');
}

bool isIdentChar(unsigned char c, Dialect d) { return isIdentStart(c, d) || (c >= '0' && c <= '9'); }

constexpr std::array<std::string_view, 25> kMultiPunct = {
    "...", "===", "!==", "**=", "<<=", "&&=", "||=", "?\?=", "::", "->", "=>", "?.", "??", "==", "!=", "<=",
    "&&",  "||",  "++",  "--",  "+=",  "-=",  "*=",  "/=",  "%=",
};

bool regexAllowedAfter(const Token* prev) {
    if (!prev) return true;
    if (prev->kind == TokenKind::Number || prev->kind == TokenKind::String || prev->kind == TokenKind::Regex) return false;
    if (prev->kind == TokenKind::Identifier) {
        static constexpr std::array<std::string_view, 14> kw = {"return", "typeof", "instanceof", "in", "of", "new",
                                                               "delete", "void", "throw", "case", "do", "else",
                                                               "yield", "await"};
        for (auto k : kw)
            if (prev->text == k) return true;
        return false;
    }
    // Punctuation: ")" and "]" end expressions, everything else starts one.
    return !(prev->text == ")" || prev->text == "]" || prev->text == "}" || prev->text == "++" || prev->text == "--");
}

class Lexer {
public:
    Lexer(std::string_view src, Dialect d) : s_(src), d_(d) {}

    std::vector<Token> run() {
        bool lineStart = true;
        while (pos_ < s_.size()) {
            unsigned char c = static_cast<unsigned char>(s_[pos_]);
            if (c == '\n') {
                advance();
                lineStart = true;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
                advance();
                continue;
            }
            if (c == '/' && peek(1) == '/') { skipLineComment(); continue; }
            if (c == '/' && peek(1) == '*') { skipBlockComment(); continue; }
            if (d_ == Dialect::Cpp && c == '#' && lineStart) {
                lexDirective();
                continue;
            }
            if (d_ == Dialect::Cpp && c == '\\' && peek(1) == '\n') {   // stray line continuation
                advance(); advance();
                continue;
            }
            lineStart = false;

            // Template literal continuation: "}" closing a ${ ... } substitution.
            if (d_ == Dialect::JavaScript && c == '}' && !templateDepth_.empty() && templateDepth_.back() == braceDepth_) {
                templateDepth_.pop_back();
                lexTemplate(pos_, true);
                continue;
            }

            if (isIdentStart(c, d_) || (d_ == Dialect::JavaScript && c == '#' && isIdentStart(static_cast<unsigned char>(peek(1)), d_))) {
                if (d_ == Dialect::Cpp && tryStringPrefix()) continue;
                lexIdentifier();
                continue;
            }
            if ((c >= '0' && c <= '9') || (c == '.' && peek(1) >= '0' && peek(1) <= '9')) { lexNumber(); continue; }
            if (c == '"' || c == '\'') { lexQuoted(pos_, c); continue; }
            if (d_ == Dialect::JavaScript && c == '`') { lexTemplate(pos_, false); continue; }
            if (d_ == Dialect::JavaScript && c == '/' && regexAllowedAfter(tokens_.empty() ? nullptr : &tokens_.back())) {
                lexRegex();
                continue;
            }
            lexPunct();
        }
        return std::move(tokens_);
    }

private:
    char peek(std::size_t off) const { return pos_ + off < s_.size() ? s_[pos_ + off] : '\0'; }

    void advance() {
        if (s_[pos_] == '\n') {
            ++line_;
            col_ = 1;
        } else {
            ++col_;
        }
        ++pos_;
    }

    void emit(TokenKind k, std::size_t start, std::uint32_t line, std::uint32_t col) {
        tokens_.push_back({k, s_.substr(start, pos_ - start), line, col});
    }

    void skipLineComment() {
        while (pos_ < s_.size() && s_[pos_] != '\n') {
            if (d_ == Dialect::Cpp && s_[pos_] == '\\' && peek(1) == '\n') advance();
            advance();
        }
    }

    void skipBlockComment() {
        advance(); advance();
        while (pos_ < s_.size() && !(s_[pos_] == '*' && peek(1) == '/')) advance();
        if (pos_ < s_.size()) { advance(); advance(); }
    }

    void lexDirective() {
        std::size_t start = pos_;
        auto line = line_, col = col_;
        while (pos_ < s_.size() && s_[pos_] != '\n') {
            if (s_[pos_] == '\\' && (peek(1) == '\n' || (peek(1) == '\r' && peek(2) == '\n'))) {
                advance();
                if (s_[pos_] == '\r') advance();
                advance();
                continue;
            }
            if (s_[pos_] == '/' && peek(1) == '*') {   // comment inside directive
                skipBlockComment();
                continue;
            }
            if (s_[pos_] == '/' && peek(1) == '/') break;
            advance();
        }
        emit(TokenKind::Preprocessor, start, line, col);
    }

    bool tryStringPrefix() {
        // u8"..", u"..", U"..", L"..", R"(..)", u8R"..", LR"..", etc.
        std::size_t p = pos_;
        auto at = [&](std::size_t i) { return i < s_.size() ? s_[i] : '\0'; };
        if (at(p) == 'u' && at(p + 1) == '8') p += 2;
        else if (at(p) == 'u' || at(p) == 'U' || at(p) == 'L') p += 1;
        bool raw = false;
        if (at(p) == 'R') { raw = true; ++p; }
        if (p == pos_) return false;
        char q = at(p);
        if (q != '"' && !(q == '\'' && !raw)) return false;
        std::size_t start = pos_;
        auto line = line_, col = col_;
        while (pos_ < p) advance();
        if (raw) lexRawString(start, line, col);
        else lexQuoted(start, q, line, col);
        return true;
    }

    void lexRawString(std::size_t start, std::uint32_t line, std::uint32_t col) {
        advance();   // opening quote
        std::size_t delimStart = pos_;
        while (pos_ < s_.size() && s_[pos_] != '(' && s_[pos_] != '\n' && pos_ - delimStart < 16) advance();
        std::string close = ")" + std::string(s_.substr(delimStart, pos_ - delimStart)) + "\"";
        if (pos_ < s_.size() && s_[pos_] == '(') advance();
        auto end = s_.find(close, pos_);
        std::size_t stop = end == std::string_view::npos ? s_.size() : end + close.size();
        while (pos_ < stop) advance();
        emit(TokenKind::String, start, line, col);
    }

    void lexQuoted(std::size_t start, char quote, std::uint32_t line = 0, std::uint32_t col = 0) {
        if (line == 0) { line = line_; col = col_; }
        advance();
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == '\\') {
                advance();
                if (pos_ < s_.size()) advance();
                continue;
            }
            if (c == '\n') break;   // unterminated: stop at end of line
            advance();
            if (c == quote) break;
        }
        emit(TokenKind::String, start, line, col);
    }

    void lexTemplate(std::size_t start, bool continuation) {
        auto line = line_, col = col_;
        advance();   // ` or }
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == '\\') {
                advance();
                if (pos_ < s_.size()) advance();
                continue;
            }
            if (c == '`') {
                advance();
                break;
            }
            if (c == '$' && peek(1) == '{') {
                advance(); advance();
                emit(TokenKind::String, start, line, col);
                templateDepth_.push_back(braceDepth_);
                return;
            }
            advance();
        }
        (void)continuation;
        emit(TokenKind::String, start, line, col);
    }

    void lexRegex() {
        std::size_t start = pos_;
        auto line = line_, col = col_;
        advance();
        bool inClass = false;
        while (pos_ < s_.size() && s_[pos_] != '\n') {
            char c = s_[pos_];
            if (c == '\\') {
                advance();
                if (pos_ < s_.size() && s_[pos_] != '\n') advance();
                continue;
            }
            advance();
            if (c == '[') inClass = true;
            else if (c == ']') inClass = false;
            else if (c == '/' && !inClass) break;
        }
        while (pos_ < s_.size() && isIdentChar(static_cast<unsigned char>(s_[pos_]), d_)) advance();   // flags
        emit(TokenKind::Regex, start, line, col);
    }

    void lexIdentifier() {
        std::size_t start = pos_;
        auto line = line_, col = col_;
        advance();
        while (pos_ < s_.size() && isIdentChar(static_cast<unsigned char>(s_[pos_]), d_)) advance();
        emit(TokenKind::Identifier, start, line, col);
    }

    void lexNumber() {
        std::size_t start = pos_;
        auto line = line_, col = col_;
        advance();
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            bool sep = d_ == Dialect::Cpp && c == '\'' && pos_ + 1 < s_.size() && isIdentChar(static_cast<unsigned char>(peek(1)), d_);
            if (isIdentChar(static_cast<unsigned char>(c), d_) || c == '.' || sep ||
                ((c == '+' || c == '-') && (s_[pos_ - 1] == 'e' || s_[pos_ - 1] == 'E' || s_[pos_ - 1] == 'p' || s_[pos_ - 1] == 'P'))) {
                advance();
                continue;
            }
            break;
        }
        emit(TokenKind::Number, start, line, col);
    }

    void lexPunct() {
        std::size_t start = pos_;
        auto line = line_, col = col_;
        for (auto p : kMultiPunct) {
            if (s_.substr(pos_, p.size()) == p) {
                if (p == "?." && peek(2) >= '0' && peek(2) <= '9') continue;   // a?.5:b
                if (d_ == Dialect::Cpp && (p == "=>" || p == "?." || p == "??" || p == "===" || p == "!==" || p == "**=" || p == "&&=" || p == "||=" || p == "?\?=")) continue;
                if (d_ == Dialect::JavaScript && p == "::") continue;
                for (std::size_t i = 0; i < p.size(); ++i) advance();
                emit(TokenKind::Punct, start, line, col);
                return;
            }
        }
        char c = s_[pos_];
        if (c == '{') ++braceDepth_;
        if (c == '}' && braceDepth_ > 0) --braceDepth_;
        advance();
        emit(TokenKind::Punct, start, line, col);
    }

    std::string_view s_;
    Dialect d_;
    std::size_t pos_ = 0;
    std::uint32_t line_ = 1;
    std::uint32_t col_ = 1;
    std::vector<Token> tokens_;
    int braceDepth_ = 0;
    std::vector<int> templateDepth_;
};

}  // namespace

std::vector<Token> tokenize(std::string_view source, Dialect dialect) { return Lexer(source, dialect).run(); }

std::string unquote(std::string_view lit) {
    // Raw string R"delim(...)delim"
    auto r = lit.find("R\"");
    if (r != std::string_view::npos && r <= 2) {
        auto open = lit.find('(', r);
        auto close = lit.rfind(')');
        if (open != std::string_view::npos && close != std::string_view::npos && close > open)
            return std::string(lit.substr(open + 1, close - open - 1));
    }
    std::size_t b = 0;
    while (b < lit.size() && lit[b] != '"' && lit[b] != '\'' && lit[b] != '`') ++b;
    if (b >= lit.size()) return std::string(lit);
    char q = lit[b];
    std::size_t e = lit.size();
    if (e > b + 1 && lit[e - 1] == q) --e;
    std::string out;
    for (std::size_t i = b + 1; i < e; ++i) {
        if (lit[i] == '\\' && i + 1 < e) {
            ++i;
            switch (lit[i]) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                default: out.push_back(lit[i]);
            }
            continue;
        }
        out.push_back(lit[i]);
    }
    return out;
}

}  // namespace codetrace::lex

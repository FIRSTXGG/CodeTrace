// CodeTrace — tolerant tokenizer for C-family languages (C++, JavaScript).
// Never fails: unterminated strings/comments end at EOF (or end of line for
// ordinary C++ string literals), unknown bytes become punctuation tokens.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace codetrace::lex {

enum class TokenKind : std::uint8_t {
    Identifier,
    Number,
    String,         // includes char literals, raw strings and template literal chunks
    Punct,
    Preprocessor,   // C++ only: whole directive line, continuation lines joined
    Regex,          // JavaScript only
};

struct Token {
    TokenKind kind;
    std::string_view text;
    std::uint32_t line;
    std::uint32_t column;

    bool is(std::string_view s) const { return text == s; }
    bool isIdent() const { return kind == TokenKind::Identifier; }
    bool isPunct(std::string_view s) const { return kind == TokenKind::Punct && text == s; }
};

enum class Dialect : std::uint8_t { Cpp, JavaScript };

/// Tokenizes `source`. Token text views point into `source`, which must outlive them.
std::vector<Token> tokenize(std::string_view source, Dialect dialect);

/// Strip quotes / prefixes from a string literal token ("abc", 'abc', `abc`, R"x(abc)x", u8"abc").
std::string unquote(std::string_view literal);

}  // namespace codetrace::lex

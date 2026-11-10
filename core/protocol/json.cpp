#include "core/protocol/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace codetrace::json {

namespace {
const Value kNull;
const std::string kEmptyString;
const Array kEmptyArray;
const Object kEmptyObject;
constexpr int kMaxDepth = 512;
}  // namespace

bool Value::asBool(bool def) const {
    if (auto* b = std::get_if<bool>(&v_)) return *b;
    return def;
}

double Value::asDouble(double def) const {
    if (auto* d = std::get_if<double>(&v_)) return *d;
    if (auto* i = std::get_if<std::int64_t>(&v_)) return static_cast<double>(*i);
    return def;
}

std::int64_t Value::asInt(std::int64_t def) const {
    if (auto* i = std::get_if<std::int64_t>(&v_)) return *i;
    if (auto* d = std::get_if<double>(&v_)) return static_cast<std::int64_t>(*d);
    return def;
}

const std::string& Value::asString() const {
    if (auto* s = std::get_if<std::string>(&v_)) return *s;
    return kEmptyString;
}

std::string Value::asString(std::string def) const {
    if (auto* s = std::get_if<std::string>(&v_)) return *s;
    return def;
}

const Array& Value::asArray() const {
    if (auto* a = std::get_if<std::shared_ptr<Array>>(&v_)) return **a;
    return kEmptyArray;
}

const Object& Value::asObject() const {
    if (auto* o = std::get_if<std::shared_ptr<Object>>(&v_)) return **o;
    return kEmptyObject;
}

Array& Value::mutableArray() {
    if (!isArray()) v_ = std::make_shared<Array>();
    auto& p = std::get<std::shared_ptr<Array>>(v_);
    if (p.use_count() > 1) p = std::make_shared<Array>(*p);   // copy-on-write
    return *p;
}

Object& Value::mutableObject() {
    if (!isObject()) v_ = std::make_shared<Object>();
    auto& p = std::get<std::shared_ptr<Object>>(v_);
    if (p.use_count() > 1) p = std::make_shared<Object>(*p);
    return *p;
}

const Value& Value::operator[](const std::string& key) const {
    const auto& o = asObject();
    auto it = o.find(key);
    return it == o.end() ? kNull : it->second;
}

Value& Value::operator[](const std::string& key) { return mutableObject()[key]; }

bool Value::contains(const std::string& key) const { return asObject().count(key) != 0; }

std::size_t Value::size() const {
    if (isArray()) return asArray().size();
    if (isObject()) return asObject().size();
    return 0;
}

bool Value::operator==(const Value& other) const {
    if (isNumber() && other.isNumber()) {
        if (isInt() && other.isInt()) return asInt() == other.asInt();
        return asDouble() == other.asDouble();
    }
    if (v_.index() != other.v_.index()) return false;
    if (isArray()) return asArray() == other.asArray();
    if (isObject()) return asObject() == other.asObject();
    return v_ == other.v_;
}

void escapeString(std::string& out, std::string_view s) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void Value::dumpTo(std::string& out, int indent, int depth) const {
    auto newline = [&](int d) {
        if (indent < 0) return;
        out.push_back('\n');
        out.append(static_cast<std::size_t>(indent * d), ' ');
    };
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) {
                out += "null";
            } else if constexpr (std::is_same_v<T, bool>) {
                out += v ? "true" : "false";
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                out += std::to_string(v);
            } else if constexpr (std::is_same_v<T, double>) {
                if (!std::isfinite(v)) {
                    out += "null";
                } else {
                    char buf[32];
                    auto res = std::to_chars(buf, buf + sizeof buf, v);
                    out.append(buf, res.ptr);
                }
            } else if constexpr (std::is_same_v<T, std::string>) {
                escapeString(out, v);
            } else if constexpr (std::is_same_v<T, std::shared_ptr<Array>>) {
                out.push_back('[');
                bool first = true;
                for (const auto& e : *v) {
                    if (!first) out.push_back(',');
                    first = false;
                    newline(depth + 1);
                    e.dumpTo(out, indent, depth + 1);
                }
                if (!v->empty()) newline(depth);
                out.push_back(']');
            } else {
                out.push_back('{');
                bool first = true;
                for (const auto& [k, e] : *v) {
                    if (!first) out.push_back(',');
                    first = false;
                    newline(depth + 1);
                    escapeString(out, k);
                    out.push_back(':');
                    if (indent >= 0) out.push_back(' ');
                    e.dumpTo(out, indent, depth + 1);
                }
                if (!v->empty()) newline(depth);
                out.push_back('}');
            }
        },
        v_);
}

std::string Value::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

namespace {

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    Value parseDocument() {
        Value v = parseValue(0);
        skipWs();
        if (pos_ != s_.size()) fail("trailing characters");
        return v;
    }

private:
    [[noreturn]] void fail(const char* msg) const { throw ParseError(msg, pos_); }

    void skipWs() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r'))
            ++pos_;
    }

    bool consume(std::string_view lit) {
        if (s_.substr(pos_, lit.size()) == lit) {
            pos_ += lit.size();
            return true;
        }
        return false;
    }

    Value parseValue(int depth) {
        if (depth > kMaxDepth) fail("nesting too deep");
        skipWs();
        if (pos_ >= s_.size()) fail("unexpected end of input");
        char c = s_[pos_];
        if (c == '{') return parseObject(depth);
        if (c == '[') return parseArray(depth);
        if (c == '"') return Value(parseString());
        if (consume("true")) return Value(true);
        if (consume("false")) return Value(false);
        if (consume("null")) return Value(nullptr);
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
        fail("unexpected character");
    }

    Value parseObject(int depth) {
        ++pos_;
        Object obj;
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return Value(std::move(obj));
        }
        while (true) {
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != '"') fail("expected string key");
            std::string key = parseString();
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') fail("expected ':'");
            ++pos_;
            obj[std::move(key)] = parseValue(depth + 1);
            skipWs();
            if (pos_ >= s_.size()) fail("unterminated object");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == '}') { ++pos_; break; }
            fail("expected ',' or '}'");
        }
        return Value(std::move(obj));
    }

    Value parseArray(int depth) {
        ++pos_;
        Array arr;
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return Value(std::move(arr));
        }
        while (true) {
            arr.push_back(parseValue(depth + 1));
            skipWs();
            if (pos_ >= s_.size()) fail("unterminated array");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == ']') { ++pos_; break; }
            fail("expected ',' or ']'");
        }
        return Value(std::move(arr));
    }

    static void appendUtf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    std::uint32_t parseHex4() {
        if (pos_ + 4 > s_.size()) fail("bad unicode escape");
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
            else fail("bad hex digit");
        }
        return v;
    }

    std::string parseString() {
        ++pos_;   // opening quote
        std::string out;
        while (true) {
            if (pos_ >= s_.size()) fail("unterminated string");
            char c = s_[pos_++];
            if (c == '"') break;
            if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= s_.size()) fail("unterminated escape");
            char e = s_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    std::uint32_t cp = parseHex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && s_.substr(pos_, 2) == "\\u") {
                        pos_ += 2;
                        std::uint32_t lo = parseHex4();
                        if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else cp = 0xFFFD;
                    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                        cp = 0xFFFD;
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: fail("invalid escape");
            }
        }
        return out;
    }

    Value parseNumber() {
        std::size_t start = pos_;
        bool isFloat = false;
        if (s_[pos_] == '-') ++pos_;
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c >= '0' && c <= '9') { ++pos_; continue; }
            if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') { isFloat = true; ++pos_; continue; }
            break;
        }
        std::string_view num = s_.substr(start, pos_ - start);
        if (!isFloat) {
            std::int64_t i = 0;
            auto res = std::from_chars(num.data(), num.data() + num.size(), i);
            if (res.ec == std::errc() && res.ptr == num.data() + num.size()) return Value(i);
        }
        // strtod needs a NUL-terminated buffer.
        std::string tmp(num);
        char* end = nullptr;
        double d = std::strtod(tmp.c_str(), &end);
        if (end != tmp.c_str() + tmp.size() || tmp == "-") fail("invalid number");
        return Value(d);
    }

    std::string_view s_;
    std::size_t pos_ = 0;
};

}  // namespace

Value Value::parse(std::string_view text) { return Parser(text).parseDocument(); }

}  // namespace codetrace::json

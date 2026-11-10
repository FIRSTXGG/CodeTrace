// CodeTrace — small dependency-free JSON value, parser and serializer.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace codetrace::json {

class Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

class ParseError : public std::runtime_error {
public:
    ParseError(const std::string& msg, std::size_t offset)
        : std::runtime_error(msg + " at offset " + std::to_string(offset)), offset_(offset) {}
    std::size_t offset() const noexcept { return offset_; }

private:
    std::size_t offset_;
};

class Value {
public:
    using Storage = std::variant<std::nullptr_t, bool, double, std::int64_t, std::string,
                                 std::shared_ptr<Array>, std::shared_ptr<Object>>;

    Value() : v_(nullptr) {}
    Value(std::nullptr_t) : v_(nullptr) {}
    Value(bool b) : v_(b) {}
    Value(int i) : v_(static_cast<std::int64_t>(i)) {}
    Value(unsigned i) : v_(static_cast<std::int64_t>(i)) {}
    Value(long i) : v_(static_cast<std::int64_t>(i)) {}
    Value(long long i) : v_(static_cast<std::int64_t>(i)) {}
    Value(unsigned long i) : v_(static_cast<std::int64_t>(i)) {}
    Value(unsigned long long i) : v_(static_cast<std::int64_t>(i)) {}
    Value(double d) : v_(d) {}
    Value(const char* s) : v_(std::string(s)) {}
    Value(std::string s) : v_(std::move(s)) {}
    Value(std::string_view s) : v_(std::string(s)) {}
    Value(Array a) : v_(std::make_shared<Array>(std::move(a))) {}
    Value(Object o) : v_(std::make_shared<Object>(std::move(o))) {}

    static Value array() { return Value(Array{}); }
    static Value object() { return Value(Object{}); }

    bool isNull() const { return std::holds_alternative<std::nullptr_t>(v_); }
    bool isBool() const { return std::holds_alternative<bool>(v_); }
    bool isNumber() const { return std::holds_alternative<double>(v_) || std::holds_alternative<std::int64_t>(v_); }
    bool isInt() const { return std::holds_alternative<std::int64_t>(v_); }
    bool isString() const { return std::holds_alternative<std::string>(v_); }
    bool isArray() const { return std::holds_alternative<std::shared_ptr<Array>>(v_); }
    bool isObject() const { return std::holds_alternative<std::shared_ptr<Object>>(v_); }

    bool asBool(bool def = false) const;
    double asDouble(double def = 0) const;
    std::int64_t asInt(std::int64_t def = 0) const;
    const std::string& asString() const;          // empty string if not a string
    std::string asString(std::string def) const;

    const Array& asArray() const;                 // empty array if not an array
    const Object& asObject() const;               // empty object if not an object
    Array& mutableArray();                        // converts to array if needed
    Object& mutableObject();                      // converts to object if needed

    /// Object member access. const version returns null Value if missing.
    const Value& operator[](const std::string& key) const;
    Value& operator[](const std::string& key);
    bool contains(const std::string& key) const;

    void push(Value v) { mutableArray().push_back(std::move(v)); }
    std::size_t size() const;

    std::string dump(int indent = -1) const;
    static Value parse(std::string_view text);    // throws ParseError

    bool operator==(const Value& other) const;

private:
    void dumpTo(std::string& out, int indent, int depth) const;
    Storage v_;
};

void escapeString(std::string& out, std::string_view s);

}  // namespace codetrace::json

#include "core/protocol/json.hpp"

#include <gtest/gtest.h>

using codetrace::json::ParseError;
using codetrace::json::Value;

TEST(Json, RoundTripsNestedValues) {
    Value v = Value::object();
    v["name"] = "CodeTrace";
    v["version"] = 1;
    v["ratio"] = 0.5;
    v["ok"] = true;
    v["none"] = nullptr;
    Value arr = Value::array();
    arr.push(1);
    arr.push("two");
    v["list"] = arr;
    Value back = Value::parse(v.dump());
    EXPECT_EQ(back, v);
    EXPECT_EQ(back["list"].asArray().size(), 2u);
    EXPECT_EQ(back["name"].asString(), "CodeTrace");
    EXPECT_EQ(back["version"].asInt(), 1);
    EXPECT_DOUBLE_EQ(back["ratio"].asDouble(), 0.5);
}

TEST(Json, EscapesAndUnicode) {
    Value v = Value::parse(R"({"s":"a\"b\\c\n\u00e9\ud83d\ude00"})");
    EXPECT_EQ(v["s"].asString(), "a\"b\\c\n\xC3\xA9\xF0\x9F\x98\x80");
    EXPECT_EQ(Value::parse(v.dump()), v);
    EXPECT_EQ(Value(std::string("\x01")).dump(), "\"\\u0001\"");
}

TEST(Json, RejectsMalformedInput) {
    for (const char* bad : {"", "{", "[1,]", "{\"a\" 1}", "tru", "\"unterminated", "01x", "[1] 2", "{\"a\":}"})
        EXPECT_THROW(Value::parse(bad), ParseError) << bad;
}

TEST(Json, DeepNestingIsRejectedNotCrashing) {
    std::string deep(100000, '[');
    EXPECT_THROW(Value::parse(deep), ParseError);
}

TEST(Json, MissingMembersAreNull) {
    Value v = Value::parse(R"({"a":1})");
    EXPECT_TRUE(v["missing"].isNull());
    EXPECT_EQ(v["missing"]["deeper"].asInt(7), 7);
    EXPECT_EQ(v["a"].asString(), "");
}

TEST(Json, CopyOnWrite) {
    Value a = Value::object();
    a["x"] = 1;
    Value b = a;
    b["x"] = 2;
    EXPECT_EQ(a["x"].asInt(), 1);
    EXPECT_EQ(b["x"].asInt(), 2);
}

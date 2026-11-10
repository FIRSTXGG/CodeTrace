#include "analyzers/registry.hpp"
#include "core/protocol/rpc.hpp"

#include <gtest/gtest.h>

#include <sstream>

using namespace codetrace;
using json::Value;

namespace {
const std::string kSample = std::string(CODETRACE_FIXTURES_DIR) + "/sample_project";

Value request(int id, const std::string& method, Value params = Value()) {
    Value v = Value::object();
    v["jsonrpc"] = "2.0";
    v["id"] = id;
    v["method"] = method;
    if (!params.isNull()) v["params"] = params;
    return v;
}
}  // namespace

TEST(Rpc, OpenQueryAndGraph) {
    protocol::RpcService rpc(createDefaultRegistry());
    auto init = rpc.handle(request(1, "initialize"));
    ASSERT_TRUE(init);
    EXPECT_EQ((*init)["result"]["languages"].size(), 3u);

    Value params = Value::object();
    params["root"] = kSample;
    auto open = rpc.handle(request(2, "project/open", params));
    ASSERT_TRUE(open && (*open)["result"].isObject()) << open->dump();
    EXPECT_EQ((*open)["result"]["fileCount"].asInt(), 11);

    Value q = Value::object();
    q["query"] = "perimeter";
    auto syms = rpc.handle(request(3, "project/symbols", q));
    ASSERT_EQ((*syms)["result"].size(), 1u);
    auto id = (*syms)["result"].asArray()[0]["id"].asInt();

    Value g = Value::object();
    g["symbolId"] = id;
    g["depth"] = 1;
    auto graph = rpc.handle(request(4, "graph/calls", g));
    EXPECT_EQ((*graph)["result"]["focus"].asInt(), id);
    EXPECT_GE((*graph)["result"]["nodes"].size(), 3u);

    auto deps = rpc.handle(request(5, "graph/dependencies"));
    EXPECT_EQ((*deps)["result"]["cycles"].size(), 1u);
}

TEST(Rpc, ErrorsFollowJsonRpc) {
    protocol::RpcService rpc(createDefaultRegistry());
    auto r = rpc.handle(request(1, "project/symbols"));
    EXPECT_EQ((*r)["error"]["code"].asInt(), protocol::rpc_error::ProjectNotOpen);
    r = rpc.handle(request(2, "no/such/method"));
    EXPECT_EQ((*r)["error"]["code"].asInt(), protocol::rpc_error::MethodNotFound);
    r = rpc.handle(request(3, "project/open", Value::object()));
    EXPECT_EQ((*r)["error"]["code"].asInt(), protocol::rpc_error::InvalidParams);
    auto text = rpc.handleText("{not json");
    ASSERT_TRUE(text);
    EXPECT_EQ(Value::parse(*text)["error"]["code"].asInt(), protocol::rpc_error::ParseError);
    // Notifications get no response.
    Value note = Value::object();
    note["jsonrpc"] = "2.0";
    note["method"] = "initialize";
    EXPECT_FALSE(rpc.handle(note));
}

TEST(Rpc, ServesFramedStream) {
    protocol::RpcService rpc(createDefaultRegistry());
    std::stringstream in, out;
    protocol::writeFramed(in, request(1, "initialize").dump());
    protocol::writeFramed(in, request(2, "shutdown").dump());
    Value exit = Value::object();
    exit["jsonrpc"] = "2.0";
    exit["method"] = "exit";
    protocol::writeFramed(in, exit.dump());
    rpc.serve(in, out);
    EXPECT_TRUE(rpc.exitRequested());
    auto first = protocol::readFramed(out);
    auto second = protocol::readFramed(out);
    ASSERT_TRUE(first && second);
    EXPECT_EQ(Value::parse(*first)["id"].asInt(), 1);
    EXPECT_EQ(Value::parse(*second)["id"].asInt(), 2);
    EXPECT_FALSE(protocol::readFramed(out));
}

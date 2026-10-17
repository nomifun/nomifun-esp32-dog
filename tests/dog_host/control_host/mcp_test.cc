#include <algorithm>
#include <cassert>
#include <climits>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include "mcp_server.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)

static int calls = 0, scheduled = 0, successes = 0, failures = 0;
static int steps = 0;
static bool enabled = false, provided = false;
class Application {
public:
    static Application& GetInstance() { static Application app; return app; }
    void Schedule(std::function<void()> callback) { ++scheduled; callback(); }
};
McpServer::McpServer() = default;
McpServer::~McpServer() { for (auto* tool : tools_) delete tool; }
void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties,
                        std::function<ReturnValue(const PropertyList&)> callback) {
    tools_.push_back(new McpTool(name, description, properties, callback));
}
void McpServer::ReplyError(int, const std::string&) { ++failures; }
void McpServer::ReplyResult(int, const std::string&) { ++successes; }
// Entry shim exercises the real private DoToolCall body through the public API.
void McpServer::ParseMessage(const cJSON* json) {
    auto name = cJSON_GetObjectItemCaseSensitive(json, "name");
    DoToolCall(1, name->valuestring, cJSON_GetObjectItemCaseSensitive(json, "arguments"));
}

// INSERT_PRODUCTION_FUNCTIONS

int main() {
    auto& server = McpServer::GetInstance();
    server.AddTool("dog", "test", PropertyList({
        Property("action", kPropertyTypeString), Property("steps", kPropertyTypeInteger, 3, 1, 100),
        Property("enabled", kPropertyTypeBoolean, false),
    }), [](const PropertyList& values) -> ReturnValue {
        ++calls; steps = values["steps"].value<int>(); enabled = values["enabled"].value<bool>();
        provided = values["steps"].provided(); return true;
    });
    int cases = 0;
    auto invoke = [&](const std::string& arguments, bool accepted) {
        const int before_calls = calls, before_schedule = scheduled, before_errors = failures;
        auto request = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(
            cJSON_Parse(("{\"name\":\"dog\",\"arguments\":" + arguments + "}").c_str()), cJSON_Delete);
        assert(request);
        server.ParseMessage(request.get());
        assert(calls == before_calls + (accepted ? 1 : 0));
        assert(scheduled == before_schedule + (accepted ? 1 : 0));
        assert(failures == before_errors + (accepted ? 0 : 1));
        ++cases;
    };
    invoke(R"({"action":"stand"})", true); assert(steps == 3 && !provided && !enabled);
    invoke(R"({"action":"forward","steps":100,"enabled":true})", true); assert(steps == 100 && provided && enabled);
    invoke(R"({"action":"stand"})", true); assert(steps == 3 && !provided && !enabled);
    for (const char* number : {"0", "101", "3.9", "100.9", "1e100", "2147483648", "-2147483649", "1e999"})
        invoke(std::string("{\"action\":\"forward\",\"steps\":") + number + "}", false);
    for (const char* value : {"\"100\"", "null", "false", "[]", "{}"})
        invoke(std::string("{\"action\":\"forward\",\"steps\":") + value + "}", false);
    for (const char* value : {"\"false\"", "null", "0", "[]", "{}"})
        invoke(std::string("{\"action\":\"stand\",\"enabled\":") + value + "}", false);
    for (const char* args : {R"({})", R"({"action":null})", R"({"action":1})", R"({"action":false})", R"({"Action":"stand"})"})
        invoke(args, false);
    invoke(R"({"action":"forward","steps":1e2})", true); assert(steps == 100);
    // A cJSON tree supplied by another caller can hold non-JSON NaN/Infinity.
    for (double value : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        auto request = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(cJSON_Parse(R"({"name":"dog","arguments":{"action":"forward","steps":1}})"), cJSON_Delete);
        cJSON_GetObjectItem(cJSON_GetObjectItem(request.get(), "arguments"), "steps")->valuedouble = value;
        const int before = failures, before_calls = calls;
        server.ParseMessage(request.get()); assert(failures == before + 1 && calls == before_calls); ++cases;
    }
    assert(successes == 4);
    std::cout << cases << " MCP type/integer/default/no-side-effect cases passed\n";
}

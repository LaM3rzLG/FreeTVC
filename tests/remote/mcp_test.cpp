// SPDX-License-Identifier: GPL-3.0-or-later
// MCP server: both protocol eras and the tools on a RAM program, HTTP over a real socket, and BASIC on the ROM.
// usage: mcp_test [<rom dir>]; the ROM part is skipped without the 2.2 ROM files.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/machine/machine.hpp"
#include "debug/debugger.hpp"
#include "frontend/common/presets.hpp"
#include "json.hpp"
#include "remote/mcp.hpp"
#include "remote/mcp_host.hpp"
#include "remote/monitor.hpp"
#include "remote/net.hpp"

namespace {

using nlohmann::json;
using freetvc::Machine;
using freetvc::remote::McpServer;

int g_failures = 0;

void expect(bool ok, int line, const std::string& what, const std::string& detail = {}) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL line %d: %s\n%s\n", line, what.c_str(), detail.substr(0, 600).c_str());
    }
}
#define EXPECT(cond, detail) expect((cond), __LINE__, #cond, (detail))

const json kMeta{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}, {"io.modelcontextprotocol/clientCapabilities", json::object()},
                 {"io.modelcontextprotocol/clientInfo", {{"name", "mcp_test"}, {"version", "1"}}}};

json request(McpServer& s, int id, const std::string& method, json params, bool modern = true, int* status = nullptr) {
    if (modern) {
        params["_meta"] = kMeta;
    }
    const freetvc::remote::McpReply r = s.handle(json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}.dump());
    if (status) {
        *status = r.http_status;
    }
    return r.body.empty() ? json() : json::parse(r.body);
}

json call(McpServer& s, const std::string& tool, json args, bool modern = true) {
    return request(s, 99, "tools/call", {{"name", tool}, {"arguments", std::move(args)}}, modern)["result"];
}

// 8000 LD A,5 / INC A / CALL 8010h / NOP / JR $ ; 8010 INC A / RET
std::unique_ptr<Machine> program() {
    freetvc::MachineConfig config;
    config.audio_rate = 0;
    auto m = std::make_unique<Machine>(config);
    m->io_write(0x02, 0xB0);
    m->load(0x8000, std::vector<std::uint8_t>{0x3E, 0x05, 0x3C, 0xCD, 0x10, 0x80, 0x00, 0x18, 0xFE});
    m->load(0x8010, std::vector<std::uint8_t>{0x3C, 0xC9});
    freetvc::Z80Registers r = m->cpu().registers();
    r.sp = 0xA000;
    m->cpu().set_registers(r);
    m->jump(0x8000);
    return m;
}

void protocol() {
    auto m = program();
    freetvc::debug::Debugger dbg(*m);
    freetvc::remote::Monitor mon(dbg);
    mon.enter("test");
    McpServer s(mon, {"ram", false});
    int status = 0;

    json d = request(s, 1, "server/discover", json::object(), true, &status);
    EXPECT(status == 200 && d["result"]["resultType"] == "complete" && d["result"]["supportedVersions"][0] == "2026-07-28" &&
               d["result"].contains("ttlMs") && d["result"]["cacheScope"] == "public" &&
               d["result"]["_meta"]["io.modelcontextprotocol/serverInfo"]["name"] == "freeTVC",
           d.dump());
    json init = request(s, 2, "initialize", {{"protocolVersion", "2025-06-18"}, {"capabilities", json::object()},
                                             {"clientInfo", {{"name", "old"}, {"version", "1"}}}}, false);
    EXPECT(init["result"]["protocolVersion"] == "2025-06-18" && init["result"]["capabilities"].contains("tools"), init.dump());
    json future = request(s, 3, "initialize", {{"protocolVersion", "2099-01-01"}}, false);
    EXPECT(future["result"]["protocolVersion"] == "2025-11-25", future.dump());
    EXPECT(s.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").http_status == 202, "");
    EXPECT(request(s, 4, "ping", json::object(), false)["result"].is_object(), "");

    json list = request(s, 5, "tools/list", json::object());
    EXPECT(list["result"]["tools"].size() == s.tool_count() && s.tool_count() == 20 && list["result"]["ttlMs"].is_number(), list.dump());
    json legacy_list = request(s, 6, "tools/list", json::object(), false);
    EXPECT(legacy_list["result"]["tools"] == list["result"]["tools"] && !legacy_list["result"].contains("resultType"), "");

    // Errors
    json bad_version = request(s, 7, "tools/list", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "1900-01-01"},
                                                                {"io.modelcontextprotocol/clientCapabilities", json::object()}}}},
                               false, &status);
    EXPECT(status == 400 && bad_version["error"]["code"] == -32022 && bad_version["error"]["data"]["requested"] == "1900-01-01", bad_version.dump());
    json no_caps = request(s, 8, "tools/list", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}}}}, false, &status);
    EXPECT(status == 400 && no_caps["error"]["code"] == -32602, no_caps.dump());
    json unknown_method = request(s, 9, "resources/list", json::object(), true, &status);
    EXPECT(status == 404 && unknown_method["error"]["code"] == -32601, unknown_method.dump());
    json unknown_tool = request(s, 10, "tools/call", {{"name", "nope"}});
    EXPECT(unknown_tool["error"]["code"] == -32602, unknown_tool.dump());
    EXPECT(json::parse(s.handle("{not json").body)["error"]["code"] == -32700, "");
    EXPECT(s.handle(R"({"jsonrpc":"2.0","id":1,"result":{}})").body.empty(), "client responses are ignored");

    // Tools
    json r = call(s, "registers", json::object());
    EXPECT(r["structuredContent"]["pc"] == "8000" && r["resultType"] == "complete", r.dump());
    r = call(s, "step", {{"mode", "into"}, {"count", 2}});
    EXPECT(r["structuredContent"]["pc"] == "8003", r.dump());
    r = call(s, "step", {{"mode", "over"}});
    EXPECT(r["structuredContent"]["pc"] == "8006" && r["structuredContent"]["registers"]["af"].get<std::string>().substr(0, 2) == "07", r.dump());
    r = call(s, "registers", {{"set", {{"hl", "9000"}, {"a", 66}}}});
    EXPECT(r["structuredContent"]["hl"] == "9000" && r["structuredContent"]["af"].get<std::string>().substr(0, 2) == "42", r.dump());
    r = call(s, "memory", {{"action", "write"}, {"address", "9000"}, {"data", "11 22 33"}});
    r = call(s, "memory", {{"action", "read"}, {"address", "U2:1000"}, {"length", 3}});
    EXPECT(r["structuredContent"]["data"] == "11 22 33" && r["structuredContent"]["address"] == "U2:1000", r.dump());
    r = call(s, "memory", {{"action", "read"}, {"address", 36864}, {"length", 3}, {"format", "base64"}});
    EXPECT(r["structuredContent"]["data"] == "ESIz", r.dump());
    r = call(s, "memory", {{"action", "search"}, {"address", "8000"}, {"length", 32}, {"data", {205, 16, 128}}});
    EXPECT(r["structuredContent"]["matches"] == json::array({"8003"}), r.dump());
    r = call(s, "disassemble", {{"address", "8003"}, {"count", 2}});
    EXPECT(r["structuredContent"]["lines"][0]["text"] == "CALL 8010h" && r["structuredContent"]["next"] == "8007", r.dump());
    r = call(s, "evaluate", {{"expression", "HL + #2"}});
    EXPECT(r["structuredContent"]["value"] == 0x9002, r.dump());
    // Call log for Bench's agent lane
    EXPECT(!s.calls().empty() && s.calls().back().tool == "evaluate" && !s.calls().back().error &&
               s.calls().back().arguments.find("HL + #2") != std::string::npos && s.calls().back().number == s.calls().size(),
           s.calls().empty() ? std::string() : s.calls().back().arguments);
    call(s, "memory", {{"action", "read"}, {"address", "NOPE:0"}});
    EXPECT(s.calls().back().tool == "memory" && s.calls().back().error && !s.calls().back().result.empty(), s.calls().back().result);
    r = call(s, "breakpoints", {{"action", "add"}, {"address", "8010"}, {"condition", "A == 43"}});
    EXPECT(r["structuredContent"]["id"] == 1 && r["structuredContent"]["condition"] == "A == 43", r.dump());
    r = call(s, "registers", {{"set", {{"pc", "8000"}}}});
    r = call(s, "run", {{"frames", 5}});
    EXPECT(r["structuredContent"]["stopped_by"] == "budget", r.dump());  // A is 6 at 8010h, not 43
    call(s, "breakpoints", {{"action", "clear"}});
    call(s, "breakpoints", {{"action", "add"}, {"address", "8010"}});
    call(s, "registers", {{"set", {{"pc", "8000"}}}});
    r = call(s, "run", {{"frames", 5}});
    EXPECT(r["structuredContent"]["stopped_by"] == "checkpoint" && r["structuredContent"]["hits"][0]["bank"] == "U2" &&
               r["structuredContent"]["pc"] == "8010",
           r.dump());
    call(s, "breakpoints", {{"action", "clear"}});
    r = call(s, "run", {{"until", "8007"}});
    EXPECT(r["structuredContent"]["stopped_by"] == "step" && r["structuredContent"]["pc"] == "8007", r.dump());
    r = call(s, "snapshot", {{"action", "save"}, {"name", "a"}});
    call(s, "run", {{"frames", 3}});
    r = call(s, "snapshot", {{"action", "load"}, {"name", "a"}});
    EXPECT(r["structuredContent"]["pc"] == "8007", r.dump());
    r = call(s, "screenshot", {{"area", "full"}});
    EXPECT(r["content"][0]["type"] == "image" && r["content"][0]["data"].get<std::string>().rfind("iVBORw0KGgo", 0) == 0, r.dump().substr(0, 200));
    r = call(s, "io_state", json::object());
    EXPECT(r["structuredContent"]["paging"]["port02"] == "B0", r.dump());
    r = call(s, "control", {{"action", "resume"}});
    EXPECT(r["structuredContent"]["state"] == "running" && !mon.active(), r.dump());
    r = call(s, "control", {{"action", "pause"}});
    EXPECT(r["structuredContent"]["state"] == "paused", r.dump());
    // Tool errors come back with isError, not as protocol errors
    r = call(s, "memory", {{"action", "read"}, {"address", "FF"}});
    EXPECT(r["isError"] == true && r["content"][0]["text"].get<std::string>().find("$FF") != std::string::npos, r.dump());
    r = call(s, "monitor_command", {{"command", "r"}});
    EXPECT(r["isError"] == true, r.dump());
    r = call(s, "run", {{"frames", 1}, {"tstates", 5}});
    EXPECT(r["isError"] == true, r.dump());
    r = call(s, "machine_info", json::object(), false);
    EXPECT(r["structuredContent"]["machine"] == "ram" && !r.contains("resultType"), r.dump());
}

// Minimal HTTP client over our socket layer: one request, Connection: close
std::pair<int, std::string> http(std::uint16_t port, const std::string& headers, const std::string& body, const char* method = "POST") {
    std::string error;
    freetvc::remote::Connection c = freetvc::remote::Connection::connect_loopback(port, error);
    if (!c.open()) {
        return {0, error};
    }
    c.send_all(std::string(method) + " /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\nContent-Length: " +
               std::to_string(body.size()) + "\r\n" + headers + "\r\n" + body);
    std::string reply;
    char buf[4096];
    for (long n; (n = c.receive(buf, sizeof buf)) > 0;) {
        reply.append(buf, static_cast<std::size_t>(n));
    }
    const int status = reply.size() > 12 ? std::atoi(reply.substr(9, 3).c_str()) : 0;
    const std::size_t at = reply.find("\r\n\r\n");
    return {status, at == std::string::npos ? std::string() : reply.substr(at + 4)};
}

void over_http() {
    auto m = program();
    freetvc::debug::Debugger dbg(*m);
    freetvc::remote::Monitor mon(dbg);
    mon.enter("test");
    McpServer s(mon, {"ram", false});
    auto host = std::make_unique<freetvc::remote::McpHost>(s);
    std::string error;
    expect(host->listen_http(0, "tok", error), __LINE__, "listen: " + error);
    const std::uint16_t port = host->http_port();
    std::vector<std::pair<int, std::string>> results;
    std::thread client([&] {
        const std::string body = json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
                                      {"params", {{"name", "registers"}, {"arguments", json::object()}, {"_meta", kMeta}}}}.dump();
        const std::string good = "Authorization: Bearer tok\r\nMCP-Protocol-Version: 2026-07-28\r\nMcp-Method: tools/call\r\nMcp-Name: registers\r\n";
        results.push_back(http(port, good, body));
        results.push_back(http(port, "MCP-Protocol-Version: 2026-07-28\r\nMcp-Method: tools/call\r\nMcp-Name: registers\r\n", body));
        results.push_back(http(port, good + "Origin: http://example.com\r\n", body));
        results.push_back(http(port, "Authorization: Bearer tok\r\nMcp-Method: tools/call\r\nMcp-Name: registers\r\n", body));
        results.push_back(http(port, "Authorization: Bearer tok\r\nMCP-Protocol-Version: 2026-07-28\r\nMcp-Method: tools/call\r\nMcp-Name: =?base64?cmVnaXN0ZXJz?=\r\n", body));
        results.push_back(http(port, "Authorization: Bearer tok\r\n", "", "GET"));
        results.push_back(http(port, "Authorization: Bearer tok\r\n", R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
        results.push_back(http(port, "Authorization: Bearer tok\r\n",
                               R"({"jsonrpc":"2.0","id":7,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})"));
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (results.size() < 8 && std::chrono::steady_clock::now() < deadline) {
        host->service();
        host->wait(std::chrono::milliseconds(5));
    }
    host.reset();
    client.join();
    expect(results.size() == 8, __LINE__, "all HTTP requests answered");
    if (results.size() == 8) {
        EXPECT(results[0].first == 200 && json::parse(results[0].second)["result"]["structuredContent"]["pc"] == "8000", results[0].second);
        EXPECT(results[1].first == 401, results[1].second);
        EXPECT(results[2].first == 403, results[2].second);
        EXPECT(results[3].first == 400 && json::parse(results[3].second)["error"]["code"] == -32020, results[3].second);
        EXPECT(results[4].first == 200, results[4].second);
        EXPECT(results[5].first == 405, results[5].second);
        EXPECT(results[6].first == 202 && results[6].second.empty(), results[6].second);
        EXPECT(results[7].first == 200 && json::parse(results[7].second)["result"]["protocolVersion"] == "2025-03-26", results[7].second);
    }
    std::printf("HTTP on 127.0.0.1:%u: %zu requests answered\n", static_cast<unsigned>(port), results.size());
}

int basic_on_rom(const std::string& rom_dir) {
    const freetvc::Preset* preset = freetvc::find_preset("64k+-2.2");
    std::string error;
    const auto config = freetvc::load_preset(*preset, rom_dir, error);
    if (!config) {
        std::printf("ROM part skipped: %s\n", error.c_str());
        return 0;
    }
    Machine m(*config);
    freetvc::debug::Debugger dbg(m);
    freetvc::remote::Monitor mon(dbg);
    mon.enter("test");
    McpServer s(mon, {"64k+-2.2", false});
    call(s, "run", {{"frames", 330}});
    call(s, "press_keys", {{"keys", {"SPACE"}}, {"settle_frames", 100}});
    const json typed = call(s, "type_text", {{"text", "print 6*7\n"}});
    const json screen = call(s, "screen_text", json::object());
    const std::string text = screen["content"][0]["text"];
    std::printf("BASIC over MCP:\n%s", text.c_str());
    EXPECT(text.find("print 6*7\n 42\nok") != std::string::npos, text);
    EXPECT(typed["structuredContent"]["screen"].size() >= 8, typed.dump());
    const json info = call(s, "machine_info", json::object());
    EXPECT(info["structuredContent"]["sys_rom"] == "SYS 2.2" && info["structuredContent"]["state"] == "paused", info.dump());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    protocol();
    over_http();
    if (argc > 1) {
        basic_on_rom(argv[1]);
    }
    if (g_failures == 0) {
        std::printf("mcp_test: all checks passed\n");
        return 0;
    }
    std::printf("mcp_test: %d check(s) failed\n", g_failures);
    return 1;
}

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    const auto request = nlohmann::json::parse(line);
    if (!request.contains("id")) continue;
    nlohmann::json result;
    const auto method = request.value("method", "");
    if (method == "initialize") {
      result = {{"protocolVersion", "2025-06-18"},
                {"capabilities", {{"tools", nlohmann::json::object()}}},
                {"serverInfo", {{"name", "fixture"}, {"version", "1.0"}}}};
    } else if (method == "tools/list") {
      result = {{"tools",
                 {{{"name", "echo"},
                   {"description", "Echo fixture input"},
                   {"inputSchema",
                    {{"type", "object"},
                     {"properties", {{"text", {{"type", "string"}}}}},
                     {"required", {"text"}},
                     {"additionalProperties", false}}},
                   {"annotations", {{"readOnlyHint", true}}}}}}};
    } else if (method == "tools/call") {
      const auto text = request.at("params").at("arguments").at("text")
                            .get<std::string>();
      result = {{"content", {{{"type", "text"}, {"text", "mcp:" + text}}}},
                {"structuredContent", {{"echo", text}}},
                {"isError", false}};
    } else {
      std::cout << nlohmann::json{{"jsonrpc", "2.0"},
                                  {"id", request.at("id")},
                                  {"error", {{"code", -32601},
                                             {"message", "method not found"}}}}
                       .dump()
                << '\n'
                << std::flush;
      continue;
    }
    std::cout << nlohmann::json{{"jsonrpc", "2.0"},
                                {"id", request.at("id")},
                                {"result", std::move(result)}}
                     .dump()
              << '\n'
              << std::flush;
  }
}

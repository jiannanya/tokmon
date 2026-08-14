#include <snow/model.hpp>

#define NOMINMAX
#include <curl/curl.h>

#include <algorithm>
#include <exception>
#include <map>
#include <mutex>

namespace snow {
namespace {

int transfer_progress(void* user, curl_off_t, curl_off_t, curl_off_t,
                      curl_off_t) {
  return static_cast<std::stop_token*>(user)->stop_requested() ? 1 : 0;
}

void emit(const ModelChunkCallback& callback, std::string_view kind,
          std::string_view value) {
  if (callback && !value.empty()) callback(kind, value);
}

void apply_usage(const tokmon::Json& document, ModelResponse& result) {
  if (!document.contains("usage") || document["usage"].is_null()) return;
  const auto& usage = document["usage"];
  result.usage.input_tokens = usage.value("prompt_tokens", 0ULL);
  result.usage.output_tokens = usage.value("completion_tokens", 0ULL);
  result.usage.cached_tokens =
      usage.value("cached_tokens",
                  usage.value("prompt_tokens_details", tokmon::Json::object())
                      .value("cached_tokens", 0ULL));
}

ModelResponse parse_complete_response(const tokmon::Json& document,
                                      const ModelChunkCallback& on_chunk) {
  if (!document.contains("choices") || document["choices"].empty()) {
    throw tokmon::Error("snow.model.response",
                        "provider response has no choices");
  }
  const auto& choice = document["choices"][0];
  const auto& message = choice.at("message");
  ModelResponse result;
  if (message.contains("content") && !message["content"].is_null())
    result.content = message["content"].get<std::string>();
  const auto reasoning_key = message.contains("reasoning_content")
                                 ? "reasoning_content"
                                 : (message.contains("reasoning") ? "reasoning"
                                                                   : "");
  if (*reasoning_key && !message[reasoning_key].is_null())
    result.reasoning = message[reasoning_key].get<std::string>();
  result.finish_reason = choice.value("finish_reason", "stop");
  if (message.contains("tool_calls")) {
    for (const auto& value : message["tool_calls"]) {
      ToolCall call;
      call.id = tokmon::ToolCallId(value.value("id", tokmon::make_uuid()));
      const auto& function = value.at("function");
      call.name = function.value("name", "");
      const auto arguments = function.value("arguments", tokmon::Json::object());
      call.arguments = arguments.is_string()
                           ? tokmon::Json::parse(arguments.get<std::string>())
                           : arguments;
      result.tool_calls.push_back(std::move(call));
    }
  }
  apply_usage(document, result);
  if (result.reasoning) emit(on_chunk, "reasoning", *result.reasoning);
  emit(on_chunk, "text", result.content);
  return result;
}

struct CollectState {
  std::string body;
  ModelChunkCallback callback;
  std::exception_ptr callback_error;
};

std::size_t collect_callback(char* data, std::size_t size, std::size_t count,
                             void* user) noexcept {
  auto& state = *static_cast<CollectState*>(user);
  const auto bytes = size * count;
  try {
    emit(state.callback, "provider.raw", std::string_view(data, bytes));
    state.body.append(data, bytes);
    return bytes;
  } catch (...) {
    state.callback_error = std::current_exception();
    return 0;
  }
}

struct StreamTool {
  std::string id;
  std::string name;
  std::string arguments;
};

struct StreamState {
  ModelChunkCallback callback;
  ModelResponse response;
  std::string raw_body;
  std::string line_buffer;
  std::string event_data;
  std::map<std::size_t, StreamTool> tools;
  std::exception_ptr callback_error;
  bool saw_event{false};

  void consume_event() {
    if (event_data.empty()) return;
    auto payload = std::move(event_data);
    event_data.clear();
    if (payload == "[DONE]") return;
    const auto document = tokmon::Json::parse(payload);
    saw_event = true;
    apply_usage(document, response);
    if (!document.contains("choices")) return;
    for (const auto& choice : document["choices"]) {
      if (!choice.value("finish_reason", tokmon::Json(nullptr)).is_null())
        response.finish_reason = choice["finish_reason"].get<std::string>();
      const auto delta = choice.value("delta", tokmon::Json::object());
      if (delta.contains("content") && !delta["content"].is_null()) {
        const auto content = delta["content"].get<std::string>();
        response.content += content;
        emit(callback, "text", content);
      }
      const auto reasoning_key = delta.contains("reasoning_content")
                                     ? "reasoning_content"
                                     : (delta.contains("reasoning")
                                            ? "reasoning"
                                            : "");
      if (*reasoning_key && !delta[reasoning_key].is_null()) {
        const auto content = delta[reasoning_key].get<std::string>();
        if (!response.reasoning) response.reasoning.emplace();
        *response.reasoning += content;
        emit(callback, "reasoning", content);
      }
      if (delta.contains("tool_calls")) {
        for (const auto& fragment : delta["tool_calls"]) {
          const auto index = fragment.value("index", tools.size());
          auto& tool = tools[index];
          if (fragment.contains("id") && !fragment["id"].is_null())
            tool.id = fragment["id"].get<std::string>();
          if (fragment.contains("function")) {
            const auto& function = fragment["function"];
            if (function.contains("name") && !function["name"].is_null())
              tool.name += function["name"].get<std::string>();
            if (function.contains("arguments") &&
                !function["arguments"].is_null())
              tool.arguments += function["arguments"].get<std::string>();
          }
        }
      }
    }
  }

  void consume_line(std::string line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) {
      consume_event();
      return;
    }
    if (line.starts_with("data:")) {
      auto data = std::string_view(line).substr(5);
      if (!data.empty() && data.front() == ' ') data.remove_prefix(1);
      if (!event_data.empty()) event_data.push_back('\n');
      event_data.append(data);
    }
  }

  void consume(char* data, std::size_t bytes) {
    emit(callback, "provider.raw", std::string_view(data, bytes));
    if (raw_body.size() < 1024U * 1024U) {
      const auto remaining = 1024U * 1024U - raw_body.size();
      raw_body.append(data, std::min(bytes, remaining));
    }
    line_buffer.append(data, bytes);
    for (;;) {
      const auto newline = line_buffer.find('\n');
      if (newline == std::string::npos) break;
      auto line = line_buffer.substr(0, newline);
      line_buffer.erase(0, newline + 1);
      consume_line(std::move(line));
    }
  }

  void finish() {
    if (!line_buffer.empty()) {
      consume_line(std::move(line_buffer));
      line_buffer.clear();
    }
    consume_event();
    for (auto& [_, value] : tools) {
      ToolCall call;
      call.id = tokmon::ToolCallId(value.id.empty() ? tokmon::make_uuid()
                                                     : value.id);
      call.name = std::move(value.name);
      try {
        call.arguments = value.arguments.empty()
                             ? tokmon::Json::object()
                             : tokmon::Json::parse(value.arguments);
      } catch (const nlohmann::json::exception& error) {
        throw tokmon::Error(
            "snow.model.tool_arguments",
            "stream ended with invalid tool arguments: " +
                std::string(error.what()),
            {{"tool", call.name}, {"arguments", value.arguments}});
      }
      response.tool_calls.push_back(std::move(call));
    }
  }
};

std::size_t stream_callback(char* data, std::size_t size, std::size_t count,
                            void* user) noexcept {
  auto& state = *static_cast<StreamState*>(user);
  const auto bytes = size * count;
  try {
    state.consume(data, bytes);
    return bytes;
  } catch (...) {
    state.callback_error = std::current_exception();
    return 0;
  }
}

} // namespace

ScriptedModelProvider::ScriptedModelProvider(
    std::vector<ModelResponse> responses)
    : responses_(std::move(responses)) {}

ModelResponse ScriptedModelProvider::complete(const ModelRequest& request,
                                              ModelChunkCallback on_chunk,
                                              std::stop_token stop) {
  std::lock_guard lock(mutex_);
  if (stop.stop_requested())
    throw tokmon::Error("snow.cancelled", "model request was cancelled");
  requests_.push_back(request);
  if (cursor_ >= responses_.size())
    throw tokmon::Error("snow.model.script",
                        "scripted model has no remaining response");
  auto response = responses_[cursor_++];
  if (response.reasoning) emit(on_chunk, "reasoning", *response.reasoning);
  constexpr std::size_t chunk_size = 16;
  for (std::size_t offset = 0; offset < response.content.size();
       offset += chunk_size) {
    if (stop.stop_requested())
      throw tokmon::Error("snow.cancelled", "model stream was cancelled");
    emit(on_chunk, "text", std::string_view(response.content).substr(
                                offset, chunk_size));
  }
  return response;
}

std::vector<ModelRequest> ScriptedModelProvider::requests() const {
  std::lock_guard lock(mutex_);
  return requests_;
}

OpenAICompatibleProvider::OpenAICompatibleProvider(
    OpenAICompatibleConfig config)
    : config_(std::move(config)) {
  static std::once_flag initialized;
  std::call_once(initialized,
                 [] { (void)curl_global_init(CURL_GLOBAL_DEFAULT); });
}

ModelResponse OpenAICompatibleProvider::complete(
    const ModelRequest& request, ModelChunkCallback on_chunk,
    std::stop_token stop) {
  if (stop.stop_requested())
    throw tokmon::Error("snow.cancelled", "model request was cancelled");
  CURL* curl = curl_easy_init();
  if (!curl)
    throw tokmon::Error("snow.model.curl", "failed to initialize libcurl");

  tokmon::Json body{{"model", request.model.empty() ? config_.model
                                                    : request.model},
                    {"messages", request.messages},
                    {"tools", request.tools},
                    {"stream", config_.stream}};
  if (config_.stream) body["stream_options"] = {{"include_usage", true}};
  for (const auto& [key, value] : request.parameters.items()) body[key] = value;
  const auto encoded = body.dump();
  curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: text/event-stream");
  if (!config_.api_key.empty()) {
    const auto authorization = "Authorization: Bearer " + config_.api_key;
    headers = curl_slist_append(headers, authorization.c_str());
  }
  for (const auto& [name, value] : config_.extra_headers.items()) {
    const auto header = name + ": " + value.get<std::string>();
    headers = curl_slist_append(headers, header.c_str());
  }

  CollectState collected{{}, on_chunk};
  StreamState streamed{on_chunk};
  curl_easy_setopt(curl, CURLOPT_URL, config_.endpoint.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, encoded.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                   static_cast<long>(encoded.size()));
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS,
                   static_cast<long>(config_.timeout.count()));
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                   config_.stream ? &stream_callback : &collect_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA,
                   config_.stream ? static_cast<void*>(&streamed)
                                  : static_cast<void*>(&collected));
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &transfer_progress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &stop);

  const auto status = curl_easy_perform(curl);
  long http_status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  const auto callback_error =
      config_.stream ? streamed.callback_error : collected.callback_error;
  if (callback_error) std::rethrow_exception(callback_error);
  if (status != CURLE_OK) {
    if (status == CURLE_ABORTED_BY_CALLBACK && stop.stop_requested())
      throw tokmon::Error("snow.cancelled", "model request was cancelled");
    throw tokmon::Error("snow.model.transport", curl_easy_strerror(status));
  }
  const auto& response_body = config_.stream ? streamed.raw_body : collected.body;
  if (http_status < 200 || http_status >= 300)
    throw tokmon::Error("snow.model.http",
                        "provider returned HTTP " +
                            std::to_string(http_status),
                        {{"body", response_body}});

  ModelResponse result;
  if (config_.stream) {
    streamed.finish();
    if (streamed.saw_event) {
      result = std::move(streamed.response);
    } else {
      result = parse_complete_response(tokmon::Json::parse(response_body),
                                       on_chunk);
    }
  } else {
    result = parse_complete_response(tokmon::Json::parse(response_body),
                                     on_chunk);
  }
  result.provider_metadata =
      {{"http_status", http_status}, {"stream", config_.stream}};
  return result;
}

} // namespace snow

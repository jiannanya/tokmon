#include <snow/surface.hpp>

#include <algorithm>

namespace snow {

void to_json(tokmon::Json& out, const SurfaceItem& item) {
  out = {{"id", item.id},
         {"role", item.role},
         {"content", item.content},
         {"source_event_seqs", item.source_event_seqs}};
}

std::vector<SurfaceItem> SurfaceProjection::project(
    const std::vector<TrajectoryEvent>& events) const {
  std::vector<SurfaceItem> result;
  for (const auto& event : events) {
    if (event.type == "user/message" || event.type == "user/steer") {
      auto content = event.data.value("content", "");
      for (const auto& attachment :
           event.data.value("attachments", tokmon::Json::array())) {
        content += "\n\n<attachment name=\"" +
                   attachment.value("name", "attachment") + "\" sha256=\"" +
                   attachment.value("sha256", "") + "\">\n" +
                   attachment.value("content", "") + "\n</attachment>";
      }
      result.push_back({"event-" + std::to_string(event.seq), "user",
                        std::move(content), {event.seq}});
    } else if (event.type == "assistant/message") {
      auto content = tokmon::Json::object();
      content["text"] = event.data.value("content", "");
      content["tool_calls"] =
          event.data.value("tool_calls", tokmon::Json::array());
      result.push_back({"event-" + std::to_string(event.seq), "assistant",
                        content, {event.seq}});
    } else if (event.type == "tool/result") {
      auto content = event.data;
      result.push_back({event.data.value(
                            "tool_call_id",
                            "event-" + std::to_string(event.seq)),
                        "tool", std::move(content), {event.seq}});
    } else if (event.type == "context/compaction") {
      const auto start = event.data.value("start", 0U);
      const auto end = event.data.value("end", 0U);
      if (start <= end && end <= result.size()) {
        SurfaceItem replacement{
            event.data.value("id", tokmon::make_uuid()),
            "system",
            event.data.value("summary", tokmon::Json("")),
            event.source_event_seqs};
        result.erase(result.begin() + static_cast<std::ptrdiff_t>(start),
                     result.begin() + static_cast<std::ptrdiff_t>(end));
        result.insert(result.begin() + static_cast<std::ptrdiff_t>(start),
                      std::move(replacement));
      }
    }
  }
  return result;
}

tokmon::Json SurfaceProjection::model_messages(
    const std::vector<TrajectoryEvent>& events) const {
  tokmon::Json messages = tokmon::Json::array();
  for (const auto& item : project(events)) {
    if (item.role == "assistant" && item.content.is_object()) {
      tokmon::Json message{{"role", item.role},
                           {"content", item.content.value("text", "")}};
      if (!item.content["tool_calls"].empty()) {
        message["tool_calls"] = item.content["tool_calls"];
      }
      messages.push_back(std::move(message));
    } else if (item.role == "tool") {
      messages.push_back(
          {{"role", "tool"},
           {"tool_call_id", item.content.value("tool_call_id", "")},
           {"content", item.content.value("content", "")}});
    } else {
      messages.push_back({{"role", item.role}, {"content", item.content}});
    }
  }
  return messages;
}

} // namespace snow

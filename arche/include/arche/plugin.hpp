#pragma once

#include <arche/context.hpp>
#include <arche/types.hpp>

#include <memory>

namespace arche {

class Plugin {
public:
  virtual ~Plugin() = default;
  [[nodiscard]] virtual const PluginDescriptor& descriptor() const = 0;
  virtual void apply(Context& context) = 0;
  virtual void quiesce(Context&) {}
};

using PluginPtr = std::shared_ptr<Plugin>;

} // namespace arche


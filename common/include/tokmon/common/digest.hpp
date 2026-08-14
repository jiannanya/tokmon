#pragma once

#include <tokmon/common/types.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace tokmon {

[[nodiscard]] std::string sha256_hex(std::span<const std::byte> data);
[[nodiscard]] std::string sha256_hex(std::string_view data);
[[nodiscard]] std::string canonical_json(const Json& value);
[[nodiscard]] std::string canonical_json_sha256(const Json& value);

} // namespace tokmon

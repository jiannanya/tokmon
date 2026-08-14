#include <tokmon/common/types.hpp>

#include <array>
#include <format>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace tokmon {

Error::Error(std::string code, std::string message, Json details)
    : std::runtime_error(std::move(message)),
      code_(std::move(code)),
      details_(std::move(details)) {}

std::string make_uuid() {
  static std::mutex mutex;
  static std::mt19937_64 generator(std::random_device{}());
  std::lock_guard lock(mutex);

  std::array<std::uint8_t, 16> bytes{};
  for (auto& byte : bytes) {
    byte = static_cast<std::uint8_t>(generator() & 0xffU);
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);

  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out << '-';
    }
    out << std::setw(2) << static_cast<unsigned>(bytes[i]);
  }
  return out.str();
}

std::int64_t unix_millis(Clock::time_point time) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             time.time_since_epoch())
      .count();
}

std::string iso8601(Clock::time_point time) {
  const auto value = Clock::to_time_t(time);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &value);
#else
  gmtime_r(&value, &utc);
#endif
  const auto millis = unix_millis(time) % 1000;
  return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
                     utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                     utc.tm_hour, utc.tm_min, utc.tm_sec, millis);
}

} // namespace tokmon


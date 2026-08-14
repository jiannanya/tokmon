#include <tokmon/common/digest.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <vector>

namespace tokmon {
namespace {

constexpr std::array<std::uint32_t, 64> constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

std::uint32_t read_be(const std::byte* value) {
  return (std::to_integer<std::uint32_t>(value[0]) << 24U) |
         (std::to_integer<std::uint32_t>(value[1]) << 16U) |
         (std::to_integer<std::uint32_t>(value[2]) << 8U) |
         std::to_integer<std::uint32_t>(value[3]);
}

} // namespace

std::string sha256_hex(std::span<const std::byte> data) {
  std::vector<std::byte> message(data.begin(), data.end());
  const auto bit_length = static_cast<std::uint64_t>(message.size()) * 8ULL;
  message.push_back(std::byte{0x80});
  while ((message.size() % 64U) != 56U) message.push_back(std::byte{0});
  for (int shift = 56; shift >= 0; shift -= 8) {
    message.push_back(static_cast<std::byte>((bit_length >> shift) & 0xffULL));
  }

  std::array<std::uint32_t, 8> hash{
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::array<std::uint32_t, 64> words{};
  for (std::size_t offset = 0; offset < message.size(); offset += 64U) {
    for (std::size_t index = 0; index < 16U; ++index) {
      words[index] = read_be(message.data() + offset + index * 4U);
    }
    for (std::size_t index = 16U; index < 64U; ++index) {
      const auto s0 = std::rotr(words[index - 15U], 7) ^
                      std::rotr(words[index - 15U], 18) ^
                      (words[index - 15U] >> 3U);
      const auto s1 = std::rotr(words[index - 2U], 17) ^
                      std::rotr(words[index - 2U], 19) ^
                      (words[index - 2U] >> 10U);
      words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }

    auto a = hash[0];
    auto b = hash[1];
    auto c = hash[2];
    auto d = hash[3];
    auto e = hash[4];
    auto f = hash[5];
    auto g = hash[6];
    auto h = hash[7];
    for (std::size_t index = 0; index < 64U; ++index) {
      const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const auto choose = (e & f) ^ ((~e) & g);
      const auto temporary1 =
          h + sum1 + choose + constants[index] + words[index];
      const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temporary2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
  }

  std::string result;
  result.reserve(64);
  for (const auto value : hash) result += std::format("{:08x}", value);
  return result;
}

std::string sha256_hex(std::string_view data) {
  return sha256_hex(std::as_bytes(std::span(data.data(), data.size())));
}

std::string canonical_json(const Json& value) {
  return value.dump(-1, ' ', false, Json::error_handler_t::strict);
}

std::string canonical_json_sha256(const Json& value) {
  return sha256_hex(canonical_json(value));
}

} // namespace tokmon

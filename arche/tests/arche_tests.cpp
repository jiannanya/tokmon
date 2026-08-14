#include <arche/manifest.hpp>
#include <arche/composition.hpp>
#include <arche/native_loader.hpp>
#include <arche/runtime.hpp>
#include <arche/package.hpp>
#include <tokmon/common/digest.hpp>
#include <tokmon/common/files.hpp>

#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <filesystem>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#endif

namespace {

std::string encode_base64(std::span<const std::byte> bytes) {
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
    const auto a = std::to_integer<unsigned char>(bytes[offset]);
    const auto b = offset + 1 < bytes.size()
                       ? std::to_integer<unsigned char>(bytes[offset + 1]) : 0;
    const auto c = offset + 2 < bytes.size()
                       ? std::to_integer<unsigned char>(bytes[offset + 2]) : 0;
    result.push_back(alphabet[a >> 2]);
    result.push_back(alphabet[((a & 3U) << 4U) | (b >> 4U)]);
    result.push_back(offset + 1 < bytes.size()
                         ? alphabet[((b & 15U) << 2U) | (c >> 6U)] : '=');
    result.push_back(offset + 2 < bytes.size() ? alphabet[c & 63U] : '=');
  }
  return result;
}

std::vector<std::byte> hex_bytes(std::string_view input) {
  auto digit = [](char value) {
    return static_cast<unsigned>(value <= '9' ? value - '0'
                                               : value - 'a' + 10);
  };
  std::vector<std::byte> result(input.size() / 2);
  for (std::size_t index = 0; index < result.size(); ++index)
    result[index] = static_cast<std::byte>(
        (digit(input[index * 2]) << 4U) | digit(input[index * 2 + 1]));
  return result;
}

struct Counter {
  int value{0};
};

class Provider final : public arche::Plugin {
public:
  explicit Provider(std::vector<std::string>& order) : order_(order) {
    descriptor_.id = "test.provider";
    descriptor_.version = "1.0.0";
    descriptor_.provides.push_back(
        {"test.counter", "1.0.0", "counter-v1", false});
  }

  const arche::PluginDescriptor& descriptor() const override {
    return descriptor_;
  }

  void apply(arche::Context& context) override {
    order_.push_back("provider.apply");
    context.provide<Counter>("test.counter", "1.0.0",
                             std::make_shared<Counter>());
    context.on_unload("provider.cleanup",
                      [this] { order_.push_back("provider.cleanup"); });
  }

private:
  std::vector<std::string>& order_;
  arche::PluginDescriptor descriptor_;
};

class Consumer final : public arche::Plugin {
public:
  explicit Consumer(std::vector<std::string>& order) : order_(order) {
    descriptor_.id = "test.consumer";
    descriptor_.version = "1.0.0";
    descriptor_.requirements.push_back({"test.counter", "^1.0", false});
  }

  const arche::PluginDescriptor& descriptor() const override {
    return descriptor_;
  }

  void apply(arche::Context& context) override {
    auto counter = context.require<Counter>("test.counter", "^1.0");
    ++counter->value;
    order_.push_back("consumer.apply");
    context.on_unload("consumer.cleanup", [this, counter] {
      order_.push_back("consumer.cleanup");
      --counter->value;
    });
  }

private:
  std::vector<std::string>& order_;
  arche::PluginDescriptor descriptor_;
};

class Failing final : public arche::Plugin {
public:
  explicit Failing(int& balance) : balance_(balance) {
    descriptor_.id = "test.failing";
    descriptor_.version = "1.0.0";
  }
  const arche::PluginDescriptor& descriptor() const override {
    return descriptor_;
  }
  void apply(arche::Context& context) override {
    ++balance_;
    context.on_unload("balance", [this] { --balance_; });
    throw std::runtime_error("expected failure");
  }

private:
  int& balance_;
  arche::PluginDescriptor descriptor_;
};

class DescriptorOnly final : public arche::Plugin {
public:
  explicit DescriptorOnly(arche::PluginDescriptor descriptor)
      : descriptor_(std::move(descriptor)) {}
  const arche::PluginDescriptor& descriptor() const override {
    return descriptor_;
  }
  void apply(arche::Context&) override {}

private:
  arche::PluginDescriptor descriptor_;
};

class ConfigReader final : public arche::Plugin {
public:
  explicit ConfigReader(int& value) : value_(value) {
    descriptor_.id = "test.config-reader";
    descriptor_.version = "1.0.0";
  }
  const arche::PluginDescriptor& descriptor() const override {
    return descriptor_;
  }
  void apply(arche::Context& context) override {
    value_ = context.config().at("answer").get<int>();
  }

private:
  int& value_;
  arche::PluginDescriptor descriptor_;
};

class SneakyConsumer final : public arche::Plugin {
public:
  SneakyConsumer() {
    descriptor_.id = "test.sneaky";
    descriptor_.version = "1.0.0";
    descriptor_.requirements.push_back({"test.counter", "^1.0", false,
                                         "counter-v1"});
  }
  const arche::PluginDescriptor& descriptor() const override {
    return descriptor_;
  }
  void apply(arche::Context& context) override {
    context_ = &context;
    (void)context.require<Counter>("test.counter", "^1.0");
  }
  bool undeclared_is_denied() const {
    try {
      (void)context_->require<Counter>("test.not-declared");
      return false;
    } catch (const tokmon::Error& error) {
      return error.code() == "arche.capability.undeclared_requirement";
    }
  }

private:
  arche::PluginDescriptor descriptor_;
  arche::Context* context_{};
};

class Stubborn final : public arche::Plugin {
public:
  Stubborn() {
    descriptor_.id = "test.stubborn";
    descriptor_.version = "1.0.0";
  }
  const arche::PluginDescriptor& descriptor() const override {
    return descriptor_;
  }
  void apply(arche::Context& context) override {
    context.tasks().spawn([](std::stop_token) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    });
  }

private:
  arche::PluginDescriptor descriptor_;
};

} // namespace

int main() {
  {
    const auto descriptor = arche::parse_plugin_manifest({
        {"id", "test.manifest"},
        {"version", "1.2.3"},
        {"requires",
         {{{"capability", "test.input"}, {"range", "^1.0"}}}},
        {"provides",
         {{{"capability", "test.output"}, {"version", "2.0.0"}}}},
    });
    assert(descriptor.id == "test.manifest");
    assert(arche::version_satisfies("1.4.2", "^1.0"));
    assert(!arche::version_satisfies("2.0.0", "^1.0"));
  }

  {
    arche::Runtime runtime;
    runtime.install("native", arche::load_native_plugin(ARCHE_TEST_PLUGIN_PATH));
    runtime.mount("native");
    auto value = runtime.root_context()->require<tokmon::Json>(
        "test.native.value", "^1.0");
    assert((*value)["value"] == 7);
    runtime.unmount("native");
    assert(runtime.fiber("native")->state == arche::FiberState::inactive);
  }

  {
    int configured = 0;
    arche::Runtime runtime;
    runtime.install("config", std::make_shared<ConfigReader>(configured),
                    {{"answer", 42}}, "workspace");
    runtime.mount("config");
    assert(configured == 42);
    const auto snapshot = runtime.fiber("config");
    assert(snapshot->realm == "workspace");
    assert(!snapshot->context_id.empty());
  }

  {
    std::vector<std::string> order;
    arche::Runtime runtime;
    runtime.install("provider", std::make_shared<Provider>(order));
    auto sneaky = std::make_shared<SneakyConsumer>();
    runtime.install("consumer", sneaky);
    runtime.fiber_context("provider")->isolate("test.counter", "private-a");
    runtime.fiber_context("consumer")->isolate("test.counter", "private-a");
    runtime.mount("provider");
    runtime.mount("consumer");
    assert(runtime.fiber("consumer")->state == arche::FiberState::active);
    assert(sneaky->undeclared_is_denied());

    arche::PluginDescriptor blocked_descriptor;
    blocked_descriptor.id = "test.blocked";
    blocked_descriptor.version = "1.0.0";
    blocked_descriptor.requirements.push_back(
        {"test.counter", "^1.0", false, "counter-v1"});
    runtime.install("blocked",
                    std::make_shared<DescriptorOnly>(blocked_descriptor));
    auto blocked_context = runtime.fiber_context("blocked");
    blocked_context->isolate("test.counter", "private-a");
    blocked_context->intercept("test.counter", [](std::string_view) {
      return arche::CapabilityDecision{false, "test policy denied"};
    });
    runtime.mount("blocked");
    assert(runtime.fiber("blocked")->state == arche::FiberState::inactive);
    const auto explanation =
        blocked_context->explain_resolution("test.counter", "^1.0");
    assert(!explanation["resolved"].get<bool>());
    assert(explanation.dump().find("test policy denied") != std::string::npos);
  }

  {
    arche::PluginDescriptor left;
    left.id = "test.cycle-left";
    left.version = "1.0.0";
    left.requirements.push_back({"test.right", "^1.0", false});
    left.provides.push_back({"test.left", "1.0.0", "left-v1", false});
    arche::PluginDescriptor right;
    right.id = "test.cycle-right";
    right.version = "1.0.0";
    right.requirements.push_back({"test.left", "^1.0", false});
    right.provides.push_back({"test.right", "1.0.0", "right-v1", false});
    arche::Runtime runtime;
    runtime.install("left", std::make_shared<DescriptorOnly>(left));
    runtime.install("right", std::make_shared<DescriptorOnly>(right));
    runtime.mount("left");
    bool cycle_rejected = false;
    try {
      runtime.mount("right");
    } catch (const tokmon::Error& error) {
      cycle_rejected = error.code() == "arche.dependency.cycle";
    }
    assert(cycle_rejected);
  }

  {
    arche::Runtime runtime("root", std::chrono::milliseconds(10));
    runtime.install("stubborn", std::make_shared<Stubborn>());
    runtime.mount("stubborn");
    runtime.unmount("stubborn");
    assert(runtime.fiber("stubborn")->stuck);
    assert(runtime.fiber("stubborn")->state == arche::FiberState::unloading);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    runtime.unmount("stubborn");
    assert(!runtime.fiber("stubborn")->stuck);
    assert(runtime.fiber("stubborn")->state == arche::FiberState::inactive);
    runtime.uninstall("stubborn");
  }

  {
    std::vector<std::string> order;
    arche::PluginCatalog catalog;
    catalog.add("test.provider", "1.0.0",
                [&](const tokmon::Json&) {
                  return std::make_shared<Provider>(order);
                });
    catalog.add("test.consumer", "1.0.0",
                [&](const tokmon::Json&) {
                  return std::make_shared<Consumer>(order);
                });
    const auto composition = arche::DesiredComposition::parse(
        {{"schema", "org.tokmon.arche.composition/v1"},
         {"id", "test.full"},
         {"plugins",
          {{{"instance", "consumer"},
            {"package", "test.consumer@^1.0"}},
           {{"instance", "provider"},
            {"package", "test.provider@1.0.0"}}}}});
    arche::Runtime runtime;
    arche::Reconciler reconciler(catalog);
    const auto report = reconciler.apply(runtime, composition);
    assert(report.actions.size() == 2);
    assert(report.epoch_after == report.epoch_before + 1);
    assert(runtime.fiber("provider")->state == arche::FiberState::active);
    assert(runtime.fiber("consumer")->state == arche::FiberState::active);

    const auto empty = arche::DesiredComposition::parse(
        {{"id", "test.empty"}, {"plugins", tokmon::Json::array()}});
    const auto removed = reconciler.apply(runtime, empty);
    assert(removed.actions.size() == 2);
    assert(removed.epoch_after == report.epoch_after + 1);
    assert(runtime.fibers().empty());
  }

  {
    std::vector<std::string> order;
    arche::Runtime runtime;
    runtime.install("consumer", std::make_shared<Consumer>(order));
    runtime.mount("consumer");
    assert(runtime.fiber("consumer")->state == arche::FiberState::inactive);

    runtime.install("provider", std::make_shared<Provider>(order));
    runtime.mount("provider");
    assert(runtime.fiber("provider")->state == arche::FiberState::active);
    assert(runtime.fiber("consumer")->state == arche::FiberState::active);

    runtime.unmount("provider");
    assert(runtime.fiber("provider")->state == arche::FiberState::inactive);
    assert(runtime.fiber("consumer")->state == arche::FiberState::inactive);
    const auto consumer_cleanup =
        std::find(order.begin(), order.end(), "consumer.cleanup");
    const auto provider_cleanup =
        std::find(order.begin(), order.end(), "provider.cleanup");
    assert(consumer_cleanup < provider_cleanup);
  }

  {
    int balance = 0;
    arche::Runtime runtime;
    runtime.install("failing", std::make_shared<Failing>(balance));
    runtime.mount("failing");
    assert(balance == 0);
    assert(runtime.fiber("failing")->state == arche::FiberState::inactive);
    assert(!runtime.fiber("failing")->error.empty());
  }

#ifdef _WIN32
  {
    const auto root = std::filesystem::temp_directory_path() /
                      ("arche-package-" + tokmon::make_uuid());
    const auto package = root / "candidate";
    std::filesystem::create_directories(package);
    tokmon::write_text_file_atomic(package / "plugin.bin",
                                   "signed plugin artifact");
    tokmon::Json manifest{
        {"id", "test.signed"},
        {"version", "2.0.0"},
        {"abi", "arche-c/1"},
        {"provides", {{{"capability", "test.new-capability"},
                         {"version", "1.0.0"},
                         {"interface_hash", "new-capability-v1"}}}},
        {"permissions", {{"filesystem_write", true}}},
        {"artifacts", {{{"path", "plugin.bin"},
                          {"sha256", tokmon::sha256_hex(
                                         "signed plugin artifact")}}}}};
    const auto package_digest = tokmon::canonical_json_sha256(manifest);
    manifest["content_hash"] = "sha256:" + package_digest;
    const auto digest = hex_bytes(package_digest);

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    assert(BCryptOpenAlgorithmProvider(&algorithm,
                                       BCRYPT_ECDSA_P256_ALGORITHM,
                                       nullptr, 0) >= 0);
    assert(BCryptGenerateKeyPair(algorithm, &key, 256, 0) >= 0);
    assert(BCryptFinalizeKeyPair(key, 0) >= 0);
    ULONG public_size = 0;
    assert(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0,
                           &public_size, 0) >= 0);
    std::vector<std::byte> public_key(public_size);
    assert(BCryptExportKey(
               key, nullptr, BCRYPT_ECCPUBLIC_BLOB,
               reinterpret_cast<PUCHAR>(public_key.data()), public_size,
               &public_size, 0) >= 0);
    ULONG signature_size = 0;
    assert(BCryptSignHash(
               key, nullptr, reinterpret_cast<PUCHAR>(
                                 const_cast<std::byte*>(digest.data())),
               static_cast<ULONG>(digest.size()), nullptr, 0,
               &signature_size, 0) >= 0);
    std::vector<std::byte> signature(signature_size);
    assert(BCryptSignHash(
               key, nullptr, reinterpret_cast<PUCHAR>(
                                 const_cast<std::byte*>(digest.data())),
               static_cast<ULONG>(digest.size()),
               reinterpret_cast<PUCHAR>(signature.data()), signature_size,
               &signature_size, 0) >= 0);
    BCryptDestroyKey(key);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    manifest["signature"] =
        {{"signer", "test-root"},
         {"algorithm", "ecdsa-p256-sha256"},
         {"value", encode_base64(signature)}};
    tokmon::write_text_file_atomic(package / "plugin.json", manifest.dump(2));
    const auto trust_path = root / "trust.json";
    tokmon::write_text_file_atomic(
        trust_path,
        tokmon::Json{{"schema", "org.tokmon.arche.trust/v1"},
                     {"signers",
                      {{"test-root",
                        {{"enabled", true},
                         {"algorithm", "ecdsa-p256-sha256"},
                         {"public_key", encode_base64(public_key)}}}}}}
            .dump(2));
    arche::PackageTrustStore trust(trust_path);
    const auto verified = trust.verify(package);
    assert(verified.signature_valid);
    assert(verified.package_hash == "sha256:" + package_digest);
    {
      auto uncovered = manifest;
      uncovered["entrypoint"] =
          {{"kind", "native"}, {"path", "uncovered.dll"}};
      tokmon::write_text_file_atomic(package / "plugin.json",
                                     uncovered.dump(2));
      bool uncovered_rejected = false;
      try {
        (void)trust.verify(package);
      } catch (const tokmon::Error& error) {
        uncovered_rejected =
            error.code() == "arche.package.entrypoint-integrity";
      }
      assert(uncovered_rejected);
      tokmon::write_text_file_atomic(package / "plugin.json",
                                     manifest.dump(2));
    }

    arche::PluginDescriptor current;
    current.id = "test.signed";
    current.version = "1.0.0";
    const auto proposal = arche::EvolutionProposal::parse(
        {{"schema", "org.tokmon.arche.evolution-proposal/v1"},
         {"id", "upgrade-signed"},
         {"instance", "signed"},
         {"package_hash", verified.package_hash},
         {"capability_delta", {{"added", {"test.new-capability"}}}},
         {"permission_delta", {{"filesystem_write", true}}},
         {"evidence", {{{"name", "fixture"}, {"status", "passed"}},
                         {{"name", "health"}, {"status", "passed"}}}},
         {"rollback", {{"strategy", "restore-previous-epoch"}}}});
    arche::EvolutionGate gate;
    assert(!gate.validate(proposal, &current, verified).accepted);
    assert(gate.validate(proposal, &current, verified, true).accepted);
    assert(!gate.validate(proposal, nullptr, verified).accepted);
    assert(gate.validate(proposal, nullptr, verified, true).accepted);

    arche::PackageStore store(root / "store");
    const auto installed = store.install_verified(package, verified);
    assert(std::filesystem::exists(installed / "plugin.json"));
    tokmon::write_text_file_atomic(package / "plugin.bin", "tampered");
    bool tamper_rejected = false;
    try {
      (void)trust.verify(package);
    } catch (const tokmon::Error& error) {
      tamper_rejected = error.code() == "arche.package.integrity";
    }
    assert(tamper_rejected);
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
  }
#endif

  std::cout << "arche_tests: ok\n";
  return 0;
}

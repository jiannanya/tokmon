#include <snow/assembly.hpp>
#include <snow/mcp.hpp>

#include <arche/plugin.hpp>
#include <arche/native_loader.hpp>

#include <tokmon/common/files.hpp>

#include <algorithm>

namespace snow {
namespace {

tokmon::Json composition_json(const arche::DesiredComposition& desired) {
  tokmon::Json plugins = tokmon::Json::array();
  for (const auto& entry : desired.plugins) {
    plugins.push_back({{"instance", entry.instance},
                       {"package", entry.package},
                       {"realm", entry.realm},
                       {"config", entry.config},
                       {"disabled", entry.disabled}});
  }
  return {{"schema", desired.schema}, {"id", desired.id},
          {"plugins", std::move(plugins)}, {"locks", desired.locks}};
}

class ExternalWorkerPlugin final : public arche::Plugin {
public:
  ExternalWorkerPlugin(arche::PluginDescriptor descriptor,
                       std::filesystem::path package_root,
                       tokmon::Json entrypoint)
      : descriptor_(std::move(descriptor)),
        package_root_(std::move(package_root)),
        entrypoint_(std::move(entrypoint)) {
    const auto has_registry =
        std::ranges::any_of(descriptor_.requirements, [](const auto& value) {
          return value.capability == "snow.tools";
        });
    if (!has_registry)
      throw tokmon::Error(
          "snow.worker.contract",
          "worker plugin must declare a snow.tools requirement");
    if (descriptor_.provides.empty())
      throw tokmon::Error("snow.worker.contract",
                          "worker plugin must provide a tool-provider capability");
    for (const auto& provision : descriptor_.provides) {
      if (!provision.capability.starts_with("snow.tool-provider.") ||
          provision.interface_hash != "snow-mcp-tool-provider-v1")
        throw tokmon::Error(
            "snow.worker.contract",
            "worker provisions must use snow.tool-provider.* and the "
            "snow-mcp-tool-provider-v1 contract");
    }
    if (entrypoint_.value("protocol", "") != "mcp")
      throw tokmon::Error("snow.worker.protocol",
                          "worker plugin protocol must be mcp");
  }

  const arche::PluginDescriptor& descriptor() const override {
    return descriptor_;
  }

  void apply(arche::Context& context) override {
    registry_ = context.require<ToolRegistry>("snow.tools", "^1.0");
    McpServerConfig config;
    config.id = worker_id(descriptor_.id);
    config.cwd = package_root_;
    config.request_timeout = std::chrono::milliseconds(
        entrypoint_.value("request_timeout_ms", 30000LL));
    if (config.request_timeout < std::chrono::milliseconds(100) ||
        config.request_timeout > std::chrono::minutes(10))
      throw tokmon::Error("snow.worker.timeout",
                          "worker request timeout is outside 100..600000 ms");
    const auto script = tokmon::canonical_within(
        package_root_, entrypoint_.at("path").get<std::string>(), true);
    const auto runtime = entrypoint_.value("runtime", "");
    if (runtime == "node") {
      config.command = "node";
      config.arguments.push_back(script.generic_string());
    } else if (runtime == "python") {
#ifdef _WIN32
      config.command = "python";
#else
      config.command = "python3";
#endif
      config.arguments.push_back(script.generic_string());
    } else if (runtime == "quickjs") {
      config.command = "qjs";
      config.arguments.push_back(script.generic_string());
    } else if (runtime == "executable") {
      config.command = script;
    } else {
      throw tokmon::Error(
          "snow.worker.runtime",
          "worker runtime must be node, python, quickjs, or executable");
    }
    for (const auto& argument :
         entrypoint_.value("args", std::vector<std::string>{}))
      config.arguments.push_back(argument);

    provider_ = std::make_shared<McpToolProvider>(std::move(config));
    context.on_unload("snow.worker.stop", [this] {
      if (registry_) {
        for (const auto& name : registered_tools_) registry_->remove(name);
      }
      registered_tools_.clear();
      if (provider_) provider_->stop();
      provider_.reset();
      registry_ = {};
    });
    for (const auto& tool : provider_->tools()) {
      registry_->add(tool);
      registered_tools_.push_back(tool->definition().name);
    }
    for (const auto& provision : descriptor_.provides)
      context.provide<McpToolProvider>(provision.capability,
                                       provision.version, provider_);
  }

private:
  static std::string worker_id(std::string value) {
    for (auto& character : value)
      if (!std::isalnum(static_cast<unsigned char>(character)) &&
          character != '_' && character != '-')
        character = '_';
    return value;
  }

  arche::PluginDescriptor descriptor_;
  std::filesystem::path package_root_;
  tokmon::Json entrypoint_;
  arche::CapabilityLease<ToolRegistry> registry_;
  std::shared_ptr<McpToolProvider> provider_;
  std::vector<std::string> registered_tools_;
};

void register_verified_package(
    arche::PluginCatalog& catalog,
    const arche::PackageVerificationReport& verified,
    const std::filesystem::path& package_root,
    const tokmon::Json& manifest) {
  if (catalog.contains(verified.descriptor.id,
                       verified.descriptor.version))
    return;
  const auto entrypoint =
      manifest.value("entrypoint", tokmon::Json::object());
  const auto kind = entrypoint.value("kind", "");
  if (kind == "native") {
    if (verified.descriptor.abi != "arche-c/1")
      throw tokmon::Error("snow.evolution.abi",
                          "native package must declare arche-c/1 ABI");
    const auto library = tokmon::canonical_within(
        package_root, entrypoint.at("path").get<std::string>(), true);
    const auto probe = arche::load_native_plugin(library);
    if (probe->descriptor().id != verified.descriptor.id ||
        probe->descriptor().version != verified.descriptor.version)
      throw tokmon::Error(
          "snow.evolution.descriptor",
          "installed signed package and native descriptor disagree");
    catalog.add(verified.descriptor.id, verified.descriptor.version,
                [library](const tokmon::Json&) {
                  return arche::load_native_plugin(library);
                });
    return;
  }
  if (kind == "worker") {
    if (verified.descriptor.abi != "arche-worker/1")
      throw tokmon::Error("snow.evolution.abi",
                          "worker package must declare arche-worker/1 ABI");
    const auto descriptor = verified.descriptor;
    catalog.add(verified.descriptor.id, verified.descriptor.version,
                [descriptor, package_root, entrypoint](const tokmon::Json&) {
                  return std::make_shared<ExternalWorkerPlugin>(
                      descriptor, package_root, entrypoint);
                });
    return;
  }
  throw tokmon::Error(
      "snow.evolution.entrypoint",
      "signed package entrypoint kind must be native or worker",
      {{"package", verified.descriptor.id}, {"kind", kind}});
}

void register_installed_packages(
    arche::PluginCatalog& catalog, const BootstrapConfig& config,
    const std::filesystem::path& trust_json) {
  const auto packages_root = config.data_root / "plugins" / "packages";
  if (!std::filesystem::exists(packages_root)) return;

  std::vector<std::filesystem::path> manifests;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(packages_root)) {
    if (entry.is_regular_file() && entry.path().filename() == "plugin.json")
      manifests.push_back(entry.path());
  }
  std::ranges::sort(manifests);
  if (manifests.empty()) return;

  // Installed code is never trusted merely because it is in the data
  // directory. Revalidate the complete manifest, every artifact hash and the
  // signer on each process start before exposing a factory to the catalog.
  const arche::PackageTrustStore trust(trust_json);
  for (const auto& manifest_path : manifests) {
    const auto package_root = manifest_path.parent_path();
    const auto verified = trust.verify(package_root);
    const auto manifest = load_json_config(manifest_path);
    register_verified_package(catalog, verified, package_root, manifest);
  }
}

void record_evolution(arche::Runtime& runtime,
                      const tokmon::SessionId& session,
                      std::string type, tokmon::Json data) {
  auto journal = runtime.root_context()->require<TrajectoryJournal>(
      "snow.trajectory", "^1.0");
  TrajectoryEvent event;
  event.type = std::move(type);
  event.session_id = session;
  event.trace_id = tokmon::TraceId(tokmon::make_uuid());
  event.producer_fiber = arche::FiberId("snow.evolution");
  event.composition_epoch = runtime.epoch();
  event.data = std::move(data);
  journal->append(std::move(event));
}

class StoragePlugin final : public arche::Plugin {
public:
  explicit StoragePlugin(const BootstrapConfig& config) {
    descriptor_.id = "org.tokmon.snow.storage.default";
    descriptor_.version = "1.0.0";
    descriptor_.provides = {
        {"snow.artifacts", "1.0.0", "artifact-store-v1", false},
        {"snow.raw-trace-vault", "1.0.0", "raw-trace-vault-v1", false}};
    artifacts_ =
        std::make_shared<ArtifactStore>(config.data_root / "artifacts");
    raw_vault_ = std::make_shared<RawTraceVault>(
        config.data_root / "trace-vault", config.raw_trace_enabled);
  }
  const arche::PluginDescriptor& descriptor() const override {
    return descriptor_;
  }
  void apply(arche::Context& context) override {
    context.provide<ArtifactStore>("snow.artifacts", "1.0.0", artifacts_);
    context.provide<RawTraceVault>("snow.raw-trace-vault", "1.0.0",
                                   raw_vault_);
  }

private:
  arche::PluginDescriptor descriptor_;
  std::shared_ptr<ArtifactStore> artifacts_;
  std::shared_ptr<RawTraceVault> raw_vault_;
};

class SessionPlugin final : public arche::Plugin {
public:
  explicit SessionPlugin(const BootstrapConfig& config) {
    descriptor_.id = "org.tokmon.snow.session.sqlite";
    descriptor_.version = "1.0.0";
    descriptor_.provides.push_back(
        {"snow.trajectory", "1.0.0", "trajectory-v1", false});
    database_ = config.data_root / "snow.db";
  }
  const arche::PluginDescriptor& descriptor() const override {
    return descriptor_;
  }
  void apply(arche::Context& context) override {
    journal_ = std::make_shared<TrajectoryJournal>(database_);
    journal_->repair_interrupted_sessions();
    context.provide<TrajectoryJournal>("snow.trajectory", "1.0.0", journal_);
    context.on_unload("snow.trajectory.close", [this] { journal_.reset(); });
  }

private:
  arche::PluginDescriptor descriptor_;
  std::filesystem::path database_;
  std::shared_ptr<TrajectoryJournal> journal_;
};

class ModelPlugin final : public arche::Plugin {
public:
  explicit ModelPlugin(std::shared_ptr<ModelProvider> model)
      : model_(std::move(model)) {
    descriptor_.id = "org.tokmon.snow.model.configured";
    descriptor_.version = "1.0.0";
    descriptor_.provides.push_back(
        {"snow.model-provider", "1.0.0", "model-provider-v1", false});
  }
  const arche::PluginDescriptor& descriptor() const override {
    return descriptor_;
  }
  void apply(arche::Context& context) override {
    context.provide<ModelProvider>("snow.model-provider", "1.0.0", model_);
  }

private:
  arche::PluginDescriptor descriptor_;
  std::shared_ptr<ModelProvider> model_;
};

class ToolsPlugin final : public arche::Plugin {
public:
  explicit ToolsPlugin(const BootstrapConfig& config) : config_(config) {
    descriptor_.id = "org.tokmon.snow.tools.default";
    descriptor_.version = "1.0.0";
    descriptor_.provides.push_back(
        {"snow.tools", "1.0.0", "tools-v1", false});
    descriptor_.requirements.push_back(
        {"snow.artifacts", "^1.0", false});
  }
  const arche::PluginDescriptor& descriptor() const override {
    return descriptor_;
  }
  void apply(arche::Context& context) override {
    artifacts_ = context.require<ArtifactStore>("snow.artifacts", "^1.0");
    tools_ = std::make_shared<ToolRegistry>();
    tools_->add(std::make_shared<ReadFileTool>(config_.workspace));
    tools_->add(std::make_shared<SearchFilesTool>(config_.workspace));
    tools_->add(std::make_shared<WriteFileTool>(config_.workspace,
                                                artifacts_.shared()));
    tools_->add(std::make_shared<ShellTool>(config_.workspace));
    const auto layout =
        ConfigLayout::resolve(config_.workspace, config_.config_dir_name);
    for (auto server : load_mcp_config(layout.mcp, config_.workspace)) {
      auto provider = std::make_shared<McpToolProvider>(std::move(server));
      for (const auto& tool : provider->tools()) tools_->add(tool);
      mcp_.push_back(std::move(provider));
    }
    context.on_unload("snow.mcp.stop", [this] {
      for (const auto& provider : mcp_) provider->stop();
      mcp_.clear();
    });
    context.provide<ToolRegistry>("snow.tools", "1.0.0", tools_);
  }

private:
  BootstrapConfig config_;
  arche::PluginDescriptor descriptor_;
  arche::CapabilityLease<ArtifactStore> artifacts_;
  std::shared_ptr<ToolRegistry> tools_;
  std::vector<std::shared_ptr<McpToolProvider>> mcp_;
};

class PolicyPlugin final : public arche::Plugin {
public:
  PolicyPlugin(const BootstrapConfig& config,
               std::shared_ptr<ApprovalService> approvals)
      : approvals_(std::move(approvals)) {
    descriptor_.id = "org.tokmon.snow.policy.default";
    descriptor_.version = "1.0.0";
    descriptor_.provides = {
        {"snow.policy", "1.0.0", "policy-v1", false},
        {"snow.approvals", "1.0.0", "approval-v1", false}};
    const auto layout =
        ConfigLayout::resolve(config.workspace, config.config_dir_name);
    if (std::filesystem::exists(layout.policy))
      policy_ = std::make_shared<JsonPolicyEngine>(
          load_json_config(layout.policy));
    else
      policy_ = std::make_shared<DefaultPolicy>();
    if (!approvals_) {
      approvals_ = std::make_shared<CallbackApproval>(
          [](const ToolDefinition&, const tokmon::Json&, std::string_view) {
            return false;
          });
    }
  }
  const arche::PluginDescriptor& descriptor() const override {
    return descriptor_;
  }
  void apply(arche::Context& context) override {
    context.provide<PolicyEngine>("snow.policy", "1.0.0", policy_);
    context.provide<ApprovalService>("snow.approvals", "1.0.0", approvals_);
  }

private:
  arche::PluginDescriptor descriptor_;
  std::shared_ptr<PolicyEngine> policy_;
  std::shared_ptr<ApprovalService> approvals_;
};

class AgentPlugin final : public arche::Plugin {
public:
  AgentPlugin(BootstrapConfig config,
              std::function<arche::CompositionEpoch()> epoch)
      : config_(std::move(config)), epoch_(std::move(epoch)) {
    descriptor_.id = "org.tokmon.snow.loop.direct";
    descriptor_.version = "1.0.0";
    descriptor_.requirements = {
        {"snow.trajectory", "^1.0", false},
        {"snow.model-provider", "^1.0", false},
        {"snow.tools", "^1.0", false},
        {"snow.policy", "^1.0", false},
        {"snow.approvals", "^1.0", false},
        {"snow.raw-trace-vault", "^1.0", false}};
    descriptor_.provides.push_back(
        {"snow.agent", "1.0.0", "agent-v1", false});
  }
  const arche::PluginDescriptor& descriptor() const override {
    return descriptor_;
  }
  void apply(arche::Context& context) override {
    auto journal =
        context.require<TrajectoryJournal>("snow.trajectory", "^1.0");
    auto model =
        context.require<ModelProvider>("snow.model-provider", "^1.0");
    auto tools = context.require<ToolRegistry>("snow.tools", "^1.0");
    auto policy = context.require<PolicyEngine>("snow.policy", "^1.0");
    auto approvals =
        context.require<ApprovalService>("snow.approvals", "^1.0");
    auto raw_vault =
        context.require<RawTraceVault>("snow.raw-trace-vault", "^1.0");
    journal_ = journal;
    model_ = model;
    tools_ = tools;
    policy_ = policy;
    approvals_ = approvals;
    raw_vault_ = raw_vault;
    agent_ = std::make_shared<Agent>(
        journal_.shared(), model_.shared(), tools_.shared(), policy_.shared(),
        approvals_.shared(), config_, epoch_,
        arche::FiberId("snow.loop.direct"), raw_vault_.shared());
    context.provide<Agent>("snow.agent", "1.0.0", agent_);
  }

private:
  BootstrapConfig config_;
  std::function<arche::CompositionEpoch()> epoch_;
  arche::PluginDescriptor descriptor_;
  std::shared_ptr<Agent> agent_;
  arche::CapabilityLease<TrajectoryJournal> journal_;
  arche::CapabilityLease<ModelProvider> model_;
  arche::CapabilityLease<ToolRegistry> tools_;
  arche::CapabilityLease<PolicyEngine> policy_;
  arche::CapabilityLease<ApprovalService> approvals_;
  arche::CapabilityLease<RawTraceVault> raw_vault_;
};

} // namespace

Assembly::Assembly(BootstrapConfig config,
                   std::shared_ptr<ModelProvider> model,
                   std::shared_ptr<ApprovalService> approvals)
    : config_(std::move(config)) {
  if (config_.data_root.empty()) {
    config_.data_root = config_.workspace / config_.config_dir_name / "data";
  }
  std::filesystem::create_directories(config_.data_root);
  const auto configured_model = std::move(model);
  const auto configured_approvals = std::move(approvals);
  catalog_.add("org.tokmon.snow.session.sqlite", "1.0.0",
               [this](const tokmon::Json&) {
                 return std::make_shared<SessionPlugin>(config_);
               });
  catalog_.add("org.tokmon.snow.storage.default", "1.0.0",
               [this](const tokmon::Json&) {
                 return std::make_shared<StoragePlugin>(config_);
               });
  catalog_.add("org.tokmon.snow.model.configured", "1.0.0",
               [configured_model](const tokmon::Json&) {
                 return std::make_shared<ModelPlugin>(configured_model);
               });
  catalog_.add("org.tokmon.snow.tools.default", "1.0.0",
               [this](const tokmon::Json&) {
                 return std::make_shared<ToolsPlugin>(config_);
               });
  catalog_.add("org.tokmon.snow.policy.default", "1.0.0",
               [this, configured_approvals](const tokmon::Json&) {
                 return std::make_shared<PolicyPlugin>(config_,
                                                       configured_approvals);
               });
  catalog_.add("org.tokmon.snow.loop.direct", "1.0.0",
               [this](const tokmon::Json&) {
                 return std::make_shared<AgentPlugin>(
                     config_, [this] { return runtime_.epoch(); });
               });
  const auto layout =
      ConfigLayout::resolve(config_.workspace, config_.config_dir_name);
  arche::DesiredComposition desired;
  const auto lock_path =
      config_.data_root / "plugins" / "composition.lock.json";
  if (std::filesystem::exists(lock_path)) {
    const auto lock = load_json_config(lock_path);
    desired = arche::DesiredComposition::parse(lock.at("composition"));
  } else if (std::filesystem::exists(layout.composition)) {
    desired = arche::DesiredComposition::parse(
        load_json_config(layout.composition));
  } else {
    desired.id = "org.tokmon.snow.default";
    desired.plugins = {
        {"session", "org.tokmon.snow.session.sqlite@1.0.0", "storage"},
        {"storage", "org.tokmon.snow.storage.default@1.0.0", "storage"},
        {"model", "org.tokmon.snow.model.configured@1.0.0", "model"},
        {"tools", "org.tokmon.snow.tools.default@1.0.0", "tools"},
        {"policy", "org.tokmon.snow.policy.default@1.0.0", "policy"},
        {"agent", "org.tokmon.snow.loop.direct@1.0.0", "agent"}};
  }
  register_installed_packages(catalog_, config_,
                              layout.config_root / "trust.json");
  reconciler_ = std::make_unique<arche::Reconciler>(catalog_);
  composition_report_ = reconciler_->apply(runtime_, desired);
  agent_ = runtime_.root_context()->require<Agent>("snow.agent", "^1.0");
}

Assembly::~Assembly() = default;

Agent& Assembly::agent() {
  if (!agent_) {
    throw tokmon::Error("snow.assembly.agent",
                        "Snow agent capability is unavailable");
  }
  return *agent_;
}

tokmon::Json Assembly::stage_package(
    const tokmon::SessionId& session,
    const std::filesystem::path& package_root,
    const arche::EvolutionProposal& proposal,
    bool permission_increase_approved) {
  std::lock_guard lock(composition_mutex_);
  record_evolution(runtime_, session, "evolution/proposed",
                   {{"proposal_id", proposal.id},
                    {"instance", proposal.instance},
                    {"package_hash", proposal.package_hash},
                    {"capability_delta", proposal.capability_delta},
                    {"permission_delta", proposal.permission_delta},
                    {"evidence", proposal.evidence},
                    {"rollback", proposal.rollback}});
  try {
    const auto layout =
        ConfigLayout::resolve(config_.workspace, config_.config_dir_name);
    arche::PackageTrustStore trust(layout.config_root / "trust.json");
    const auto verified = trust.verify(package_root);
    const auto current = runtime_.fiber(proposal.instance);
    const auto gate = arche::EvolutionGate{}.validate(
        proposal, current ? &current->descriptor : nullptr, verified,
        permission_increase_approved);
    if (!gate.accepted)
      throw tokmon::Error("snow.evolution.rejected",
                          "evolution proposal failed validation gates",
                          {{"rejections", gate.rejections}});
    const auto manifest = load_json_config(package_root / "plugin.json");
    const auto entrypoint =
        manifest.value("entrypoint", tokmon::Json::object());
    if (entrypoint.value("kind", "") == "native") {
      const auto candidate_library = tokmon::canonical_within(
          package_root, entrypoint.at("path").get<std::string>(), true);
      const auto probe = arche::load_native_plugin(candidate_library);
      if (probe->descriptor().id != verified.descriptor.id ||
          probe->descriptor().version != verified.descriptor.version)
        throw tokmon::Error("snow.evolution.descriptor",
                            "signed package and native descriptor disagree");
    } else if (entrypoint.value("kind", "") != "worker") {
      throw tokmon::Error(
          "snow.evolution.entrypoint",
          "signed package entrypoint kind must be native or worker");
    }
    arche::PackageStore store(config_.data_root / "plugins");
    const auto installed = store.install_verified(package_root, verified);
    register_verified_package(catalog_, verified, installed, manifest);
    record_evolution(runtime_, session, "evolution/staged",
                     {{"proposal_id", proposal.id},
                      {"package_hash", verified.package_hash},
                      {"signer", verified.signer},
                      {"installed", installed.generic_string()},
                      {"descriptor", verified.descriptor}});
    return {{"accepted", true},
            {"proposal_id", proposal.id},
            {"package_hash", verified.package_hash},
            {"signer", verified.signer},
            {"installed", installed.generic_string()}};
  } catch (const std::exception& error) {
    record_evolution(runtime_, session, "evolution/rejected",
                     {{"proposal_id", proposal.id},
                      {"message", error.what()}});
    throw;
  }
}

arche::CompositionReport Assembly::apply_composition(
    const tokmon::SessionId& session,
    const arche::DesiredComposition& desired) {
  std::lock_guard lock(composition_mutex_);
  if (agent_ && agent_->has_active_runs())
    throw tokmon::Error("snow.composition.busy",
                        "composition changes require a turn boundary");
  const auto required_instance = [&](std::string_view instance) {
    return std::ranges::any_of(desired.plugins, [&](const auto& entry) {
      return entry.instance == instance && !entry.disabled;
    });
  };
  for (const auto* instance : {"session", "storage", "model", "tools",
                               "policy", "agent"}) {
    if (!required_instance(instance))
      throw tokmon::Error(
          "snow.composition.invariant",
          "Snow composition cannot remove a mandatory direct-loop instance",
          {{"instance", instance}});
  }
  record_evolution(runtime_, session, "evolution/commit-prepared",
                   {{"composition", composition_json(desired)}});
  try {
    auto report = reconciler_->apply(runtime_, desired);
    agent_ = runtime_.root_context()->require<Agent>("snow.agent", "^1.0");
    composition_report_ = report;
    const auto lock_path =
        config_.data_root / "plugins" / "composition.lock.json";
    tokmon::write_text_file_atomic(
        lock_path,
        tokmon::Json{{"schema", "org.tokmon.arche.composition-lock/v1"},
                     {"composition", composition_json(desired)},
                     {"epoch", report.epoch_after},
                     {"committed_at", tokmon::iso8601()}}
            .dump(2));
    record_evolution(runtime_, session, "evolution/committed",
                     {{"composition_id", report.composition_id},
                      {"epoch_before", report.epoch_before},
                      {"epoch_after", report.epoch_after}});
    return report;
  } catch (const std::exception& error) {
    record_evolution(runtime_, session, "evolution/rolled-back",
                     {{"composition_id", desired.id},
                      {"message", error.what()}});
    throw;
  }
}

} // namespace snow

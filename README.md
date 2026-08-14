# Tokmon

Tokmon 是一个以 **Arche** 为元框架微内核的原生桌面 Agent。Arche 管理
Context、Fiber、coeffect/provision、可逆 effect、运行时组合事务和插件 ABI；
Snow 提供可回放 Agent runtime；White 提供 Native GUI；Tokmon 只负责产品组合。

## 构建与测试

需要 CMake 3.25+、C++23 编译器以及 `vcpkg.json` 中声明的依赖。

```powershell
cmake --preset default
cmake --build --preset default
ctest --preset default
```

Release 构建使用 `release` preset。安装开发包和可执行程序：

```powershell
cmake --install build/release --prefix dist
```

## 运行

```powershell
snow doctor --workspace C:\work --config-dir-name .snow
snow run --workspace C:\work --message "检查这个仓库"
tokmon --workspace C:\work
```

Snow 默认读取 `<workspace>/.snow/`。Tokmon 将同一个
`config_dir_name` 统一设为 `.tokmon`；第三方 SDK 宿主可设置任意安全的单目录名。
结构化配置只接受严格 UTF-8 JSON，不支持 YAML、TOML、JSON5 或 JSON 注释。
可复制的配置见 `examples/config/`。

Tokmon 输入框命令包括 `/cancel`、`/steer`、`/fork`、`/restart`、
`/diagnostics`、`/inspect`、`/replay R0|R1|R2` 和
`/artifact <sha256> [media-type]`。

## SDK 与 ABI

- C++：链接 `tokmon::snow`、`tokmon::arche`、`tokmon::white`。
- C：包含 `snow/c_api.h`，通过 `snow_host_invoke_v1` 使用 Snow Protocol。
- TypeScript：`sdk/typescript/`。
- Python：`sdk/python/tokmon_snow/`。
- 进程边界：并发 NDJSON JSON-RPC，支持取消、steer、审批、artifact 分块读取、
  诊断和动态组合。

完整设计与安全边界见 [docs/DESIGN.md](docs/DESIGN.md)。
White 声明式 UI 文档、状态绑定与组件扩展见
[white/README.md](white/README.md)。

# Satsuma TestLab

Satsuma TestLab 是一个面向 Windows 和 VMware Workstation 的可恢复测试编排器。Host 将任务清单与已有
Artifact 原子发布到 VMware Shared Folder；每台 Guest 中的 Windows Service 在本机工作目录执行任务，
再把退出码、日志和声明的结果文件写回共享目录。

项目适合可信实验室中的软件安装验证、网络客户端联调、升级回归和多虚拟机测试。它不是恶意样本沙箱，
也不提供租户隔离：任务计划、Artifact、Host 和 Guest 管理员均属于同一信任边界。

## 主要能力

- `echo`、`execute` 与受控 `script` 步骤，支持 CMD、Windows PowerShell 5.1、PowerShell 7。
- 多 VM 顺序启动、可选快照恢复、失败清理和始终执行的 `finally` 步骤。
- Guest inventory 首次上报、缓存自愈和 Host 显式刷新。
- 同一实验室的进程互斥、持久租约、崩溃恢复与人工解锁门禁。
- 统一 Guest 存储根、按运行授权交互用户，以及可确认的本机/共享目录清理。
- Shared Folder 单一事实源，无额外监听端口或管理网络依赖。
- 步骤 claim、租约续期、结果 fencing、崩溃恢复和人工介入门禁。
- 文件取消、运行列表与安全保留策略；失败运行不会阻塞其他运行。
- Agent Windows Service 安装、自更新和 VM 身份迁移。
- SMBIOS UUID 自动发现、Host VM 标识绑定和克隆身份冲突检测。
- Artifact、日志和结果文件容量上限，防止测试耗尽共享盘。
- Windows Debug/Release CI、JSON Schema、便携发行目录和 UTF-8 ZIP 发布包。

## 快速开始

使用发行物只需要 Windows 10/11、VMware Workstation、Guest 内 VMware Tools 和可用的 Shared Folder。
已经取得版本目录或 ZIP 时可直接进入下面的配置步骤，不需要 CMake，也不会在 Host 安装程序。

从源码生成发行物还需要 Visual Studio 2022 C++ 工具链、CMake 3.25+ 和 Git：

```powershell
cmake --preset windows-default
cmake --build --preset windows-release --parallel
ctest --preset windows-release
cmake --build --preset windows-release --target SatsumaPackage
```

发布目标会在根目录 `output` 同时生成可直接使用的版本目录和同名 ZIP，不会向系统安装文件。进入版本目录后，
将 [`lab.template.json`](config/lab.template.json) 复制为 `config/lab.local.json`，并把通用的
[`agent.template.json`](config/agent.template.json) 填写为 `agent.json`。将它与 `bin/SatsumaVM.exe`、
`scripts/install-agent.ps1` 一起放入共享目录的 `satsuma-bootstrap`，再在每台 Guest 的管理员 PowerShell 中
运行安装脚本。Agent 使用 SMBIOS UUID 自动声明硬件身份，不需要为每台 VM 准备不同配置文件。

```powershell
bin\SatsumaHost.exe discover --config config\lab.local.json
bin\SatsumaHost.exe agent rebind --config config\lab.local.json --vm vm_01 --hardware-id <uuid>
bin\SatsumaHost.exe check --config config\lab.local.json --timeout-seconds 180
bin\SatsumaHost.exe lab status --config config\lab.local.json
bin\SatsumaHost.exe orchestrate --config config\lab.local.json --plan examples\multi-vm-task.json --timeout-seconds 900
```

自动化默认使用 `orchestrate`，它会在取得实验室独占租约后启动目标 VM、验证 inventory 和内部 echo、归档
证据并按策略清理。普通 `run` 仍可用于无生命周期任务，但发布后会保持持久租约，必须在报告终态后执行
`runs finalize`。

## 文档

- [用户指南](docs/用户指南.md)：安装、配置、日常命令、更新和排障。
- [首次配置](docs/首次配置.md)：环境信息、配置职责、模板填写和首次验收。
- [架构](docs/架构.md)：组件边界、数据流、可靠性模型和安全假设。
- [文件协议](docs/协议.md)：共享目录布局、版本、Schema 和容量限制。
- [开发指南](docs/开发指南.md)：构建、测试、打包和真实 VMware 验收。
- [AI 操作契约](docs/AI操作契约.md)：自动化工具使用 Satsuma 时的边界。
- [更新日志](更新日志.md)：版本变化和兼容性调整。
- [贡献指南](贡献指南.md)：开发流程和 Review 要求。
- [安全策略](安全策略.md)：安全边界和漏洞报告方式。
- [第三方声明](第三方声明.md)：依赖许可证和商标说明。

## 项目状态

当前版本为 `0.2.1`，只支持 Windows 与 VMware Workstation。真实 VMware 故障注入测试默认关闭，必须在
专用实验 VM 上显式启用并确认。

项目许可证尚未指定。在根目录出现明确的 `LICENSE` 前，源码默认不授予复制、修改或再分发许可。

# Satsuma TestLab

Satsuma TestLab 是专为 AI 代理设计的 Windows 虚拟机自动化测试工具。它让 AI 能够在 VMware Workstation
虚拟机中部署和执行测试、收集完整证据，并自动管理快照与故障恢复。Host 通过 VMware VMCI 网关向 Guest
传输任务与 Artifact；每台 Guest 中的 Windows Service 在本机工作目录执行任务，再通过 VMCI 分块上传退出码、
日志和声明的结果文件。该通道不依赖 Guest 网卡、IP、DNS 或 VPN 状态。

项目适合可信实验室中的软件安装验证、网络客户端联调、升级回归和多虚拟机测试。它不是恶意样本沙箱，
也不提供租户隔离：任务计划、Artifact、Host 和 Guest 管理员均属于同一信任边界。

## 主要能力

- `echo`、`execute` 与受控 `script` 步骤，支持 CMD、Windows PowerShell 5.1、PowerShell 7。
- 多 VM 顺序启动、可选快照恢复、失败清理和始终执行的 `finally` 步骤。
- Guest inventory 首次上报、缓存自愈和 Host 显式刷新。
- 同一实验室的进程互斥、持久租约、崩溃恢复与人工解锁门禁。
- 统一 Guest 存储根、按运行授权交互用户，以及可确认的本机/Host 状态清理。
- VMCI 专用传输，不依赖 VMware Shared Folder 或 Guest 网络栈。
- 步骤 claim、租约续期、结果 fencing、崩溃恢复和人工介入门禁。
- 文件取消、运行列表与安全保留策略；失败运行不会阻塞其他运行。
- Agent Windows Service 安装、自更新和 VM 身份迁移。
- SMBIOS UUID 自动发现、Host VM 标识绑定和克隆身份冲突检测。
- Artifact、日志和结果文件容量上限，防止测试耗尽 Host 或 Guest 磁盘。
- Windows Debug/Release CI、JSON Schema、便携发行目录和 UTF-8 ZIP 发布包。

## 快速开始

使用发行物只需要 Windows 10/11、VMware Workstation，以及 Host/Guest 内可用的 VMware VMCI 驱动。
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
[`agent.template.json`](config/agent.template.json) 填写为 `agent.json`。将 `agent.json`、
`bin/SatsumaVM.exe` 和 `scripts/install-agent.ps1` 复制到 Guest 的同一个本机目录（可用只读 ISO、VMware
控制台或其他一次性安装介质），再在管理员 PowerShell 中运行安装脚本。Agent 使用 SMBIOS UUID 自动声明
硬件身份，不需要为每台 VM 准备不同配置文件。

```powershell
bin\SatsumaHost.exe gateway --config config\lab.local.json
```

网关保持运行，在另一个终端执行：

```powershell
bin\SatsumaHost.exe discover --config config\lab.local.json
bin\SatsumaHost.exe agent rebind --config config\lab.local.json --vm vm_01 --hardware-id <uuid>
bin\SatsumaHost.exe check --config config\lab.local.json --timeout-seconds 180
bin\SatsumaHost.exe lab status --config config\lab.local.json
bin\SatsumaHost.exe orchestrate --config config\lab.local.json --plan examples\multi-vm-task.json --timeout-seconds 900
```

`gateway` 是常驻的 Host 传输进程，应先在独立终端或服务管理器中启动。自动化默认使用 `orchestrate`，它会
在取得实验室独占租约后启动目标 VM、验证 inventory 和内部 echo、归档
证据并按策略清理。普通 `run` 仍可用于无生命周期任务，但发布后会保持持久租约，必须在报告终态后执行
`runs finalize`。

## 让 AI 使用

GitHub Release 发行包会携带与二进制版本配套的 [`satsuma-testlab` Skill](skills/satsuma-testlab/SKILL.md)
和 `AI-START-HERE.md`。将解压目录提供给能够读取本地文件并执行终端命令的 AI 代理，然后让它先读取
`AI-START-HERE.md`，即可进入环境配置、任务生成、执行、取证和恢复流程。

支持 [Agent Skills](https://agentskills.io/) 的客户端可以直接加载发行包中的 `skills/satsuma-testlab/`，也可以
把整个 Skill 目录安装到客户端自己的 Skill 目录。不支持自动发现的 AI 仍可把 `SKILL.md` 当作结构化操作手册
读取。升级 Satsuma 时应同步使用新发行包内的 Skill，避免 AI 按旧命令或旧 Schema 操作新程序。

## 文档

- [用户指南](docs/用户指南.md)：安装、配置、日常命令、更新和排障。
- [首次配置](docs/首次配置.md)：环境信息、配置职责、模板填写和首次验收。
- [架构](docs/架构.md)：组件边界、数据流、可靠性模型和安全假设。
- [VMCI 协议](docs/协议.md)：请求、分块传输、本地镜像、Schema 和容量限制。
- [开发指南](docs/开发指南.md)：构建、测试、打包和真实 VMware 验收。
- [`satsuma-testlab` Skill](skills/satsuma-testlab/SKILL.md)：AI 可直接加载的操作流程、安全边界与按需参考资料。
- [AI 集成说明](docs/AI操作契约.md)：Skill、CLI、Schema 和人类授权之间的职责边界。
- [更新日志](更新日志.md)：版本变化和兼容性调整。
- [贡献指南](贡献指南.md)：开发流程和 Review 要求。
- [安全策略](安全策略.md)：安全边界和漏洞报告方式。
- [第三方声明](第三方声明.md)：依赖许可证和商标说明。

## 项目状态

当前只支持 Windows 与 VMware Workstation。真实 VMware 故障注入测试默认关闭，必须在专用实验 VM 上显式
启用并确认。`master` 分支文档描述正在开发的版本；稳定版本请查看对应 Git Tag 或 GitHub Release，发行包内
的程序、Schema、示例、文档和 Skill 属于同一个版本快照。

项目许可证尚未指定。在根目录出现明确的 `LICENSE` 前，源码默认不授予复制、修改或再分发许可。

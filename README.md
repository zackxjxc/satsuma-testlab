# Satsuma TestLab

简体中文 | [English](README_EN.md)

Satsuma TestLab 是一个面向 AI、运行于 Windows 与 VMware Workstation 环境的虚拟机任务下发器。AI 可以在宿主机上把程序、脚本和测试计划可靠地下发到指定虚拟机，在 Guest 中执行任务，再取回退出码、日志和结果文件；同时还能按测试策略管理虚拟机启动、快照恢复、失败清理和证据归档。

Host 与 Guest 通过 VMware VMCI 通信，不经过 Guest 的网络栈。即使测试过程中启用了 VPN、修改了路由、禁用了网卡，甚至卸载了网络驱动，任务通道仍可继续工作。

## 最快的使用方式

从 [GitHub Releases](https://github.com/zackxjxc/satsuma-testlab/releases) 下载发行包，把 ZIP 文件或解压后的完整目录交给能够读取本地文件并执行终端命令的 AI，然后让它阅读包内的 `AI-START-HERE.md`。AI 会根据随包提供的 Skill 说明环境要求、询问必要的虚拟机信息，并引导你完成配置、任务执行、结果收集和故障恢复。

如果所用 AI 不能直接读取 ZIP，先解压发行包即可；正常使用不需要下载源码，也不需要自行构建项目。

## 适合用它测试什么

- VPN、代理和网络安全客户端：安装驱动、切换隧道、修改 DNS 或路由后，仍需从宿主机持续控制测试并取回证据。
- 安装程序与升级器：验证静默安装、服务注册、驱动更新、重启恢复、升级与回滚，并在失败后恢复测试基线。
- Windows 桌面应用：分别以 SYSTEM 或已登录用户身份执行安装和功能验证，收集日志、退出码与声明的结果文件。
- 多虚拟机联调：按顺序启动多台虚拟机，执行客户端与服务端任务，并统一处理成功、失败和最终清理流程。
- 容易破坏测试环境的回归任务：在隔离快照中重复执行系统配置、故障恢复或兼容性测试，保留可审阅证据。

Satsuma 适合 Host 与 Guest 管理员处于同一信任边界的测试实验室。它不是恶意样本沙箱，也不提供跨租户隔离。

## 人工准备环境，AI 接管测试

Satsuma 的首次部署需要用户和 AI 配合完成，之后日常测试可以由 AI 自主执行。用户负责安装 VMware Workstation、创建测试虚拟机、安装 VMware Tools、选择允许操作的虚拟机与基础快照，并在 Guest 中确认 UAC、安装 SatsumaVM Agent Service。AI 会读取发行包文档，协助发现路径、生成配置、执行绑定和连通性检查，但不会替用户决定虚拟机范围、快照或可能丢失数据的操作。

当环境通过 `check` 并达到 `ready` 状态后，AI 就可以通过宿主机上的 Satsuma CLI 启动和关闭已授权虚拟机、恢复用户指定的快照、下发程序与脚本、等待任务完成、收集日志和结果，并按策略清理或保留失败现场。正常测试过程中通常不需要用户反复进入虚拟机操作。

## 每个步骤都可以选择执行身份

任务计划中的每个 `execute` 或 `script` 步骤都可以单独设置 `run_as`：

| `run_as` | 实际身份 | 适合的任务 |
|---|---|---|
| `system` | Windows `LocalSystem`，具有系统管理员级权限 | 安装程序、Windows Service、驱动、HKLM 和其他需要提权的系统变更 |
| `interactive_user` | 当前登录到虚拟机控制台的用户 | GUI、桌面交互、HKCU、用户配置、普通应用、编译和单元测试 |

同一个任务可以混合两种身份，例如先用 `system` 安装软件，再用 `interactive_user` 验证真实用户场景。`interactive_user` 要求 Guest 中存在已登录的控制台用户；条件不满足时步骤会明确失败，不会静默回退为 SYSTEM。

## 主要能力

- `echo`、`execute` 与受控 `script` 步骤，支持 CMD、Windows PowerShell 5.1、PowerShell 7。
- 多 VM 顺序启动、可选快照恢复、失败清理和始终执行的 `finally` 步骤。
- Guest inventory 首次上报、缓存自愈和 Host 显式刷新。
- 同一实验室的进程互斥、持久租约、崩溃恢复与人工解锁门禁。
- 每个执行或脚本步骤可独立选择 SYSTEM 或已登录用户身份，并在同一任务中组合使用。
- VMCI 专用传输，不依赖 VMware Shared Folder 或 Guest 网络栈。
- 步骤 claim、租约续期、结果 fencing、崩溃恢复和人工介入门禁。
- 文件取消、运行列表与安全保留策略；失败运行不会阻塞其他运行。
- Agent Windows Service 安装、自更新和 VM 身份迁移。
- SMBIOS UUID 自动发现、Host VM 标识绑定和克隆身份冲突检测。
- Artifact、日志和结果文件容量上限，防止测试耗尽 Host 或 Guest 磁盘。
- Windows Debug/Release CI、JSON Schema、便携发行目录和 UTF-8 ZIP 发布包。

## 快速开始

使用发行物只需要 Windows 10/11、VMware Workstation，以及 Host/Guest 内可用的 VMware VMCI 驱动。已经取得版本目录或 ZIP 时可直接进入下面的配置步骤，不需要 CMake，也不会在 Host 安装程序。

从源码生成发行物还需要 Visual Studio 2022 C++ 工具链、CMake 3.25+ 和 Git：

```powershell
cmake --preset windows-default
cmake --build --preset windows-release --parallel
ctest --preset windows-release
cmake --build --preset windows-release --target SatsumaPackage
```

发布目标会在根目录 `output` 同时生成可直接使用的版本目录和同名 ZIP，不会向系统安装文件。进入版本目录后，将 [`lab.template.json`](config/lab.template.json) 复制为 `config/lab.local.json`，并把通用的 [`agent.template.json`](config/agent.template.json) 填写为 `agent.json`。将 `agent.json`、`bin/SatsumaVM.exe` 和 `scripts/install-agent.ps1` 复制到 Guest 的同一个本机目录（可用只读 ISO、VMware 控制台或其他一次性安装介质），再在管理员 PowerShell 中运行安装脚本。Agent 使用 SMBIOS UUID 自动声明硬件身份，不需要为每台 VM 准备不同配置文件。

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

`gateway` 是常驻的 Host 传输进程，应先在独立终端或服务管理器中启动。自动化默认使用 `orchestrate`，它会在取得实验室独占租约后启动目标 VM、验证 inventory 和内部 echo、归档证据并按策略清理。普通 `run` 仍可用于无生命周期任务，但发布后会保持持久租约，必须在报告终态后执行 `runs finalize`。

## AI Skill

发行包会携带与二进制版本配套的 [`satsuma-testlab` Skill](skills/satsuma-testlab/SKILL.md) 和 `AI-START-HERE.md`，让 AI 在不了解本项目的情况下也能找到正确入口，并按需读取首次配置、任务编写和恢复流程。

支持 [Agent Skills](https://agentskills.io/) 的客户端可以直接加载发行包中的 `skills/satsuma-testlab/`，也可以把整个 Skill 目录安装到客户端自己的 Skill 目录。不支持自动发现的 AI 仍可把 `SKILL.md` 当作结构化操作手册读取。升级 Satsuma 时应同步使用新发行包内的 Skill，避免 AI 按旧命令或旧 Schema 操作新程序。

## 文档

- [用户指南](docs/用户指南.md)：安装、配置、日常命令、更新和排障。
- [首次配置](docs/首次配置.md)：环境信息、配置职责、模板填写和首次验收。
- [架构](docs/架构.md)：组件边界、数据流、可靠性模型和安全假设。
- [VMCI 协议](docs/协议.md)：请求、分块传输、本地镜像、Schema 和容量限制。
- [开发指南](docs/开发指南.md)：构建、测试、打包和真实 VMware 验收。
- [`satsuma-testlab` Skill](skills/satsuma-testlab/SKILL.md)：AI 可直接加载的操作流程、安全边界与按需参考资料。
- [AI 集成说明](docs/AI操作契约.md)：Skill、CLI、Schema 和人类授权之间的职责边界。
- [更新日志](CHANGELOG.md)：版本变化和兼容性调整。
- [贡献指南](CONTRIBUTING.md)：开发流程和 Review 要求。
- [社区行为准则](CODE_OF_CONDUCT.md)：参与项目时共同遵守的协作边界。
- [安全策略](SECURITY.md)：安全边界和漏洞报告方式。
- [第三方声明](THIRD_PARTY_NOTICES.md)：依赖许可证和商标说明。

## 项目状态

当前只支持 Windows 与 VMware Workstation。真实 VMware 故障注入测试默认关闭，必须在专用实验 VM 上显式启用并确认。`master` 分支文档描述正在开发的版本；稳定版本请查看对应 Git Tag 或 GitHub Release，发行包内的程序、Schema、示例、文档和 Skill 属于同一个版本快照。

项目采用 [Apache License 2.0](LICENSE) 开源；第三方组件仍适用各自的许可证。

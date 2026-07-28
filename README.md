# Satsuma TestLab

Satsuma TestLab 是一个面向 Windows 和 VMware Workstation 的可恢复测试编排器。Host 将任务清单与已有
Artifact 原子发布到 VMware Shared Folder；每台 Guest 中的 Windows Service 在本机工作目录执行任务，
再把退出码、日志和声明的结果文件写回共享目录。

项目适合可信实验室中的软件安装验证、网络客户端联调、升级回归和多虚拟机测试。它不是恶意样本沙箱，
也不提供租户隔离：任务计划、Artifact、Host 和 Guest 管理员均属于同一信任边界。

## 主要能力

- `echo` 与进程树受控的 `execute` 步骤，支持 `system` 和 `interactive_user` 身份。
- 多 VM 顺序启动、可选快照恢复、失败清理和始终执行的 `finally` 步骤。
- Shared Folder 单一事实源，无额外监听端口或管理网络依赖。
- 步骤 claim、租约续期、结果 fencing、崩溃恢复和人工介入门禁。
- 文件取消、运行列表与安全保留策略；失败运行不会阻塞其他运行。
- Agent Windows Service 安装、自更新和 VM 身份迁移。
- Artifact、日志和结果文件容量上限，防止测试耗尽共享盘。
- Windows Debug/Release CI、JSON Schema、CMake 安装和 ZIP 发布包。

## 快速开始

要求 Windows 10/11、VMware Workstation、Guest 内 VMware Tools 和可用的 Shared Folder。源码构建还需要
Visual Studio 2022 C++ 工具链、CMake 3.25+ 和 Git。

```powershell
cmake --preset windows-default
cmake --build --preset windows-release --parallel
ctest --preset windows-release
```

将 [`lab.json`](lab.json) 复制为被 Git 忽略的 `lab.local.json`，按本机路径修改；将
[`agent-client.json`](examples/agent-client.json)（复制后命名为 `agent.json`）、`SatsumaVM.exe` 和
[`install-agent.ps1`](scripts/install-agent.ps1) 放入共享目录的 `satsuma-bootstrap`，然后在 Guest 管理员
PowerShell 中运行安装脚本。

```powershell
SatsumaHost check --config lab.local.json --timeout-seconds 180
SatsumaHost run --config lab.local.json --plan examples/hello-vm-task.json
SatsumaHost report --config lab.local.json --run <run-id> --wait-seconds 300
```

`run` 会输出 `run_id`。报告的 `status` 为 `succeeded` 才表示业务成功；`pending`、`failed` 和
`manual_intervention_required` 都需要继续处理。

## 文档

- [用户指南](docs/用户指南.md)：安装、配置、日常命令、更新和排障。
- [架构](docs/架构.md)：组件边界、数据流、可靠性模型和安全假设。
- [文件协议](docs/协议.md)：共享目录布局、版本、Schema 和容量限制。
- [开发指南](docs/开发指南.md)：构建、测试、打包和真实 VMware 验收。
- [AI 操作契约](docs/AI操作契约.md)：自动化工具使用 Satsuma 时的边界。
- [更新日志](更新日志.md)：版本变化和兼容性调整。
- [贡献指南](贡献指南.md)：开发流程和 Review 要求。
- [安全策略](安全策略.md)：安全边界和漏洞报告方式。
- [第三方声明](第三方声明.md)：依赖许可证和商标说明。

## 项目状态

当前版本为 `0.1.0`，只支持 Windows 与 VMware Workstation。真实 VMware 故障注入测试默认关闭，必须在
专用实验 VM 上显式启用并确认。

项目许可证尚未指定。在根目录出现明确的 `LICENSE` 前，源码默认不授予复制、修改或再分发许可。

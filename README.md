# Satsuma TestLab

Satsuma 是可信本机环境中的 Windows/VMware 小任务下发工具。Host 把已有 Artifact 和任务计划发布到
专用 Shared Folder，Guest Agent 在 VM 本地目录执行，并返回退出码、日志和声明的结果文件。

业务 Artifact 只在 Guest 内执行。Satsuma 不安装或修改 Host 服务、驱动、防火墙、网络、注册表和其他
系统配置；Host 只运行 Satsuma 自身程序、构建与测试，写入仓库构建目录、配置的共享/归档目录和临时
vmrun 输出，并控制配置中已授权的 VM。

- 用户安装、虚拟机准备、配置文件和首次验收：[SatsumaReadme.md](docs/SatsumaReadme.md)
- AI 环境引导、主动检测和任务执行契约：[SatsumaSkill.md](docs/SatsumaSkill.md)
- Host 配置模板：[lab.json](lab.json)
- VM Agent 配置模板：[agent-client.json](examples/agent-client.json)
- Guest 安装脚本：[install-agent.ps1](scripts/install-agent.ps1)
- 独立示例软件：[demo_app.cpp](examples/demo_app.cpp)

首次使用时把 `lab.json` 复制为已忽略的 `lab.local.json`，再按用户指南配置 VMware 和 Agent。Guest 能
访问 Shared Folder 后只需执行一次安装脚本；脚本会安装并维护 LocalSystem Windows Service。
普通虚拟机可以使用 NAT、DHCP 和任意名称；示例路径和 IP 必须按实际环境修改后使用。

最短使用流程：

1. 使用 `SatsumaHost check` 确认目标 VM 的文件任务通道为 `ready`。
2. 使用 `SatsumaHost run` 下发有限超时的前台 `echo` 或 `execute` 任务。
3. 使用 `SatsumaHost report --wait-seconds` 等待并读取完整结果。
4. 遇到未知平台错误时，可对授权 VM 重启或恢复快照一次；仍失败则停止并人工排查。

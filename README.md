# Satsuma TestLab

Satsuma 是面向 AI 自动化测试的 Windows/VMware 双端执行工具。

- 用户安装、虚拟机准备、配置文件和首次验收：[SatsumaReadme.md](docs/SatsumaReadme.md)
- AI 环境引导、主动检测和任务执行契约：[SatsumaSkill.md](docs/SatsumaSkill.md)
- Host 配置模板：[lab.json](lab.json)
- VM Agent 配置模板：[agent-client.json](examples/agent-client.json)
- Guest 安装脚本：[install-agent.ps1](scripts/install-agent.ps1)
- 独立示例软件：[demo_app.cpp](examples/demo_app.cpp)

首次使用时把 `lab.json` 复制为已忽略的 `lab.local.json`，再按用户指南配置 VMware 和 Agent。
普通虚拟机可以使用 NAT、DHCP 和任意名称；示例路径和 IP 必须按实际环境修改后使用。

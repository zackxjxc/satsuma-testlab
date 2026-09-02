---
name: satsuma-testlab
description: 使用 Satsuma TestLab 配置和操作可信的 Windows VMware 测试虚拟机，编写并校验任务计划，通过 VMCI 执行测试、收集证据并安全恢复。用户要求 AI 运行、诊断或管理基于 Satsuma 的虚拟机测试流程时使用。
metadata:
  author: zackxjxc
  version: "0.3.2"
  satsuma-version: "0.3.2"
---

# Satsuma TestLab

将 Satsuma 作为可信 Windows VMware 虚拟机测试环境的控制平面。所有操作均通过 `bin/SatsumaHost.exe` 完成；不要直接写入协议状态、任务认领、结果或生命周期文件。

本流程要求 Windows 10/11、支持 VMCI 的 VMware Workstation、本地终端访问权限，以及版本匹配的 Satsuma TestLab 发行目录。

## 定位并核对发行包

找到用户提供或批准的发行目录，其中必须包含 `bin/SatsumaHost.exe`、`config/`、`schemas/` 和 `examples/`。发行包通常在 `skills/satsuma-testlab/` 中附带本 Skill，但安装后的 Skill 也可能位于其他位置；不要根据 Skill 的安装路径推断可执行文件的位置。

操作测试环境前，运行：

```powershell
bin\SatsumaHost.exe --version
bin\SatsumaVM.exe --version
```

两个程序都必须报告 `0.3.2`，并与 `metadata.satsuma-version` 一致。如果版本不同，应停止操作，并请用户改用该发行版本附带的 Skill。以发行包内 CLI 的 `--help`、JSON Schema 和模板为权威接口。

## 遵守授权边界

- 只控制用户批准的 `config/lab.local.json` 中列出的虚拟机。
- 将 Satsuma 视为可信测试环境工具，而不是恶意软件沙箱或租户隔离边界。
- 不要猜测 VMX 路径、虚拟机 ID、快照名、VMCI 端点、硬件身份、凭据或缺失的任务字段。
- 如果超出用户已说明的任务范围，且操作会恢复快照、强制停止虚拟机、丢弃 Guest 状态或执行破坏性故障注入测试，应先解释影响并取得授权。
- 不要上传或泄露用户未要求提供的产物、完整日志、凭据或本地路径。
- 同一种未知基础设施故障再次发生时，只执行一次已获授权的恢复尝试；随后停止并保留证据供人工检查。

## 选择所需流程

- 测试环境配置缺失或不完整时，阅读[首次配置](references/setup.md)。
- 创建或修改任务计划前，阅读[任务编写](references/task-authoring.md)。
- 执行、等待、取消、恢复或报告任务前，阅读[执行、结果与恢复](references/operations.md)。

如果用户只需要范围明确的只读检查，不要加载无关参考文档。

## 标准操作流程

1. 理解用户期望的测试结果、所需证据、允许操作的虚拟机集合，以及请求是否授权生命周期变更。
2. 定位版本匹配的发行包和配置；改变测试环境前先检查 `lab status`。
3. 启动或确认 Host VMCI 网关，并在发布业务任务前要求 `check` 成功。
4. 根据 `schemas/task.schema.json` 校验任务，并通过 inventory 确认可执行程序和脚本能力。
5. 完整测试优先使用 `orchestrate`，让同一个租约覆盖虚拟机启动、就绪检查、任务执行、证据归档和清理。
6. 读取 JSON 结果，核对每台目标虚拟机的最终状态和租约清理情况；报告应以证据为依据，不能只看进程退出码。

只有明确不需要生命周期管理时才使用普通 `run`。该命令发布任务后会保留租约；获得终态结果并完成外部清理后，使用 `runs finalize`，并确认所有相关虚拟机均已关闭。

## 完成报告

报告所用配置文件和任务文件、`run_id`、顶层状态、成功与失败步骤数、证据或归档位置、Guest 和 Host 的清理结果、每台目标虚拟机的最终状态、租约状态，以及任何需要人工处理的事项。

# Satsuma AI 操作契约

本文定义自动化助手使用 Satsuma TestLab 时的最小安全流程。它不是可安装的 Codex Skill 清单；实际命令和
Schema 以当前发布包为准。

## 适用条件

仅在以下条件全部满足时使用 Satsuma：

- 用户明确授权控制 `lab.json` 中列出的实验 VM。
- Artifact 与任务来自可信来源，Shared Folder 不跨租户共享。
- 涉及快照恢复、强制停止或真实故障注入时，用户已理解会丢失 Guest 未保存状态。
- Host、Guest 和任务处于同一可信管理边界；不把 Satsuma 当成恶意代码沙箱。

自动化助手不得扩大 VM 范围，不得猜测 VMX、快照、共享目录或 Agent 身份，也不得绕过 Host CLI 直接伪造
claim、结果或生命周期状态。

## 标准流程

1. 读取任务目标、`lab.local.json` 和任务计划，但不输出配置中可能存在的敏感本机路径之外的信息。
2. 使用 `SatsumaHost check` 对目标 VM 做有限超时检查。
3. 普通任务使用 `run`；包含 `lifecycle` 的任务使用 `orchestrate`。
4. 从 Host JSON 输出读取 `run_id`，不要从目录名猜测。
5. 使用带有限 `--wait-seconds` 的 `report` 等待，并同时判断退出码与顶层 `status`。
6. 保留失败证据；只有在确认运行完整后才使用 `runs prune`。

```powershell
SatsumaHost check --config lab.local.json --vm client --timeout-seconds 180
SatsumaHost run --config lab.local.json --plan task.json
SatsumaHost report --config lab.local.json --run <run-id> --wait-seconds 300
```

## 任务生成规则

- 输出必须符合 `schemas/task.schema.json`，未知字段视为错误。
- `run_id`、VM ID 和步骤 ID 只使用字母、数字、下划线和连字符，最长 128 字符。
- 每个 `execute.program` 必须匹配同一 VM 的 Artifact `shared_destination`。
- Artifact `source` 使用 Host 绝对路径；目标只放在 `artifacts/` 下。
- 所有步骤给出有限 `timeout_seconds`。不要用无限等待模拟服务常驻。
- 默认 `run_as=system`；只有业务明确需要活动用户配置文件或桌面资源时才用 `interactive_user`。
- 默认 `retry_safe=false`。只有重复执行不会造成额外副作用时才设为 `true`。
- 只收集任务声明需要的相对文件，不递归打包整个用户目录、临时目录或磁盘。

## 生命周期规则

多 VM 任务必须显式声明每台 VM 的 `on_success` 和 `on_failure`。推荐在专用测试 VM 上使用：

- 执行前需要固定基线时设置 `restore_before`。
- 成功后无保留价值时使用 `stop`。
- 失败后需要恢复干净环境时使用带快照名的 `restore`。
- 需要人工观察现场时使用 `leave_running`，并在结果中明确说明 VM 仍在运行。

不要把唯一用户快照用作自动恢复目标。Satsuma 只允许创建/删除符合 `ai_prefix` 的快照；基础快照不属于
自动化工具所有。

## 结果判定

| 状态/退出码 | 自动化动作 |
|---|---|
| `succeeded` / 0 | 汇总步骤与证据路径，任务完成 |
| `pending` / 0 | 继续有限等待；不能宣称成功 |
| `failed` / 1 | 报告失败步骤、退出码和错误，保留运行目录 |
| 等待超时 / 3 | 查询 `runs list`，根据用户目标决定继续等待或取消 |
| `manual_intervention_required` / 5 | 停止自动重试，报告 claim 恢复证据并请求人工判断 |

非零被测程序退出码属于业务失败，不应通过重复运行、修改 `execution.json` 或删除 claim 来隐藏。

## 取消与恢复

用户要求停止任务时使用：

```powershell
SatsumaHost runs cancel --config lab.local.json --run <run-id> --reason "user requested stop"
```

取消后继续读取报告，直到收齐失败结果或出现人工门禁。不要同时删除运行目录。若 Shared Folder 暂时不可用，
Agent 会按 `reconnect_interval_ms` 重试；Host 侧应先恢复文件通道，再决定是否恢复快照。

发生未知 VMware/Guest 状态时，自动化最多执行一次用户已授权的明确恢复动作。相同故障再次出现时停止，
输出 Host 检查、运行报告和生命周期状态，不进行循环重启或连续快照恢复。

## 禁止事项

- 不在 Host 直接运行业务 Artifact。
- 不把未登记的 Guest 绝对路径作为 `program`。
- 不直接写 `runs/<run-id>/task.json`、claim、`execution.json` 或 `lifecycle.json`。
- 不删除 pending、invalid、人工门禁或仍需取证的运行。
- 不在未明确授权时启用真实 VMware crash/fault recovery CMake 目标。
- 不记录、回传或上传 Artifact 内容、用户数据和完整日志，除非用户明确要求。

## 交付摘要

自动化任务结束时至少说明：使用的配置与任务文件、`run_id`、顶层状态、失败/成功步骤数、证据位置、是否
执行了 VM 恢复/停止、是否仍需人工介入。不得只用命令退出码替代业务结果说明。

# Satsuma AI 操作契约

本文定义自动化助手使用 Satsuma TestLab 时的最小安全流程。它不是可安装的 Codex Skill 清单；实际命令和
Schema 以当前发布包为准。

## 适用条件

仅在以下条件全部满足时使用 Satsuma：

- 用户明确授权控制 `config/lab.local.json` 中列出的实验 VM。
- Artifact 与任务来自可信来源，Shared Folder 不跨租户共享。
- 涉及快照恢复、强制停止或真实故障注入时，用户已理解会丢失 Guest 未保存状态。
- Host、Guest 和任务处于同一可信管理边界；不把 Satsuma 当成恶意代码沙箱。

自动化助手不得扩大 VM 范围，不得猜测 VMX、快照、共享目录或 Agent 身份，也不得绕过 Host CLI 直接伪造
claim、结果或生命周期状态。

## 未配置环境

`config/lab.local.json` 不存在时，先完整读取 [首次配置](首次配置.md)、`config/lab.template.json`、
`config/agent.template.json` 和对应 Schema。项目发行物定义所需字段和目录结构；自动化助手只负责读取本机
可确认信息、列出缺失项并生成配置，用户负责选择和确认 VM、基础快照、Shared Folder 以及管理员操作。

自动化助手必须先做只读发现，不得猜测 VMX、快照名、Guest 共享路径或 VM 身份。确认后生成：

- 一份 `config/lab.local.json`；
- 一份可供所有 VM 使用的 `config/agent.json`；
- 仍需用户进入 VMware GUI、Guest 或 UAC 完成的操作清单。

生成配置后先执行 Schema 校验和 `SatsumaHost check`。环境未达到 `ready` 前，不发布业务任务，不自行扩大
VM 范围，也不循环执行恢复或重启。

## 标准流程

1. 读取任务目标、`config/lab.local.json`、只读 `lab status` 和任务计划，不输出无关的敏感本机路径。
2. 默认使用 `orchestrate` 取得独占租约；它会在启动目标 VM 后验证 inventory 并执行内部 diagnostic echo。
3. 只有 check 返回 `ready` 后，才按 inventory 选择 EXE、CMD、Windows PowerShell 5.1 或 PowerShell 7。
4. 明确 `restore_before`、成功/失败 VM 动作和 `cleanup`，不得猜测快照或默认删除失败现场。
5. 从 Host JSON 输出读取 `run_id`，不要从目录名猜测。
6. Host 返回终态后报告业务状态、归档位置、清理结果、VM 状态和租约是否释放。

```powershell
bin\SatsumaHost.exe lab status --config config\lab.local.json
bin\SatsumaHost.exe orchestrate --config config\lab.local.json --plan task.json --timeout-seconds 900
```

普通 `run` 仅用于确实不需要生命周期编排的兼容工作流。它同样自动执行目标 VM check，但发布后持久租约不会
随 Host 进程退出；收齐终态、完成外部清理后必须执行 `runs finalize`。

## 任务生成规则

- 输出必须符合 `schemas/task.schema.json`，未知字段视为错误。
- `run_id`、VM ID 和步骤 ID 只使用字母、数字、下划线和连字符，最长 128 字符。
- 每个 `execute.program` 必须匹配同一 VM 的 Artifact `shared_destination`。
- 每个 `script.script` 必须匹配同一 VM 的脚本 Artifact；只允许 `cmd`、`windows_powershell` 和 `pwsh`。
- Artifact `source` 使用 Host 绝对路径；目标只放在 `artifacts/` 下。
- 所有步骤给出有限 `timeout_seconds`。不要用无限等待模拟服务常驻。
- 默认 `run_as=system`；只有业务明确需要活动用户配置文件或桌面资源时才用 `interactive_user`。
- 默认 `retry_safe=false`。只有重复执行不会造成额外副作用时才设为 `true`。
- 只收集任务声明需要的相对文件，不递归打包整个用户目录、临时目录或磁盘。
- Windows PowerShell 5.1 非 ASCII 脚本使用 UTF-8 BOM；PowerShell 7 使用 UTF-8 无 BOM；CMD 使用 UTF-8
  无 BOM + CRLF，并显式保留目标命令退出码。

## 日志适配

- 优先通过现有命令行参数或测试配置修改日志路径，不改生产默认值。
- 必须临时改源码时，使用测试分支、独立工作树或可审阅补丁，不混入正式交付代码。
- spdlog 文件优先写到被测 EXE 同目录，并在 `collect_files` 中声明对应的 Guest 工作根相对路径。
- 同时启用 console sink 和 file sink，关键错误及时 flush；stdout/stderr 是文件日志的补充。
- 不硬编码 Shared Folder。Agent 不扫描目录、不支持 glob，也不自动采集 OutputDebugString、ETW 或 Event Log。
- 被测 EXE 崩溃但 Agent 存活时可收集已刷新日志；Agent、VM 或文件通道失效时不得声称 Guest 文件已归档。

## 生命周期规则

多 VM 任务必须显式声明每台 VM 的 `on_success` 和 `on_failure`。推荐在专用测试 VM 上使用：

- 执行前需要固定基线时设置 `restore_before`。
- 成功后无保留价值时使用 `stop`。
- 失败后需要恢复干净环境时使用带快照名的 `restore`。
- 需要人工观察现场时使用 `leave_running`，并在结果中明确说明 VM 仍在运行。

顶层 `cleanup` 只控制 Guest 工作目录和共享运行目录。建议成功时 Guest `delete`、失败时 `retain`；共享运行
只有在 Host 归档校验成功后才能使用 `archive_then_delete`。`manual_intervention_required` 固定保留 Guest 和
共享证据；`RECOVERY_FAILED` 或 cleanup failure 不得自动解锁实验室。

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
| `RECOVERY_FAILED` / 4 | 报告清理/恢复失败，保留租约和证据，等待人工处理 |

非零被测程序退出码属于业务失败，不应通过重复运行、修改 `execution.json` 或删除 claim 来隐藏。

## 取消与恢复

用户要求停止任务时使用：

```powershell
bin\SatsumaHost.exe runs cancel --config config\lab.local.json --run <run-id> --reason "user requested stop"
```

取消后继续读取报告，直到收齐失败结果或出现人工门禁。不要同时删除运行目录。若 Shared Folder 暂时不可用，
Agent 会按 `reconnect_interval_ms` 重试；Host 侧应先恢复文件通道，再决定是否恢复快照。

发生未知 VMware/Guest 状态时，自动化最多执行一次用户已授权的明确恢复动作。相同故障再次出现时停止，
输出 Host 检查、运行报告和生命周期状态，不进行循环重启或连续快照恢复。

Host 崩溃后先读取 `lab status`。仅当持久状态明确允许同一 `run_id` 恢复时使用 `lab recover`；不得把 Host
进程退出视为实验室空闲。`lab unlock --force true` 只能在人已确认原 Host 进程死亡、Guest/外部系统状态和
证据保留情况后执行。

## 禁止事项

- 不在 Host 直接运行业务 Artifact。
- 不把未登记的 Guest 绝对路径作为 `program`。
- 不直接写 `runs/<run-id>/task.json`、claim、`execution.json` 或 `lifecycle.json`。
- 不删除 pending、invalid、人工门禁或仍需取证的运行。
- 不在未明确授权时启用真实 VMware crash/fault recovery CMake 目标。
- 不记录、回传或上传 Artifact 内容、用户数据和完整日志，除非用户明确要求。

## 交付摘要

自动化任务结束时至少说明：使用的配置与任务文件、`run_id`、顶层状态、失败/成功步骤数、Host 归档位置、
Guest/共享目录清理动作、VM 最终状态、租约是否释放以及是否仍需人工介入。不得只用命令退出码替代业务结果。

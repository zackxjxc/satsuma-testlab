# Satsuma AI 操作契约

本文定义自动化助手使用 Satsuma TestLab 时的最小安全流程。实际命令和 Schema 以当前发布包为准。

## 适用条件

仅在以下条件全部满足时使用 Satsuma：

- 用户明确授权控制 `config/lab.local.json` 中列出的实验 VM。
- Artifact 与任务来自可信来源，VMCI 网关不跨租户共享。
- 涉及快照恢复、强制停止或真实故障注入时，用户已理解会丢失 Guest 未保存状态。
- Host、Guest 和任务处于同一可信管理边界；不把 Satsuma 当成恶意代码沙箱。

自动化助手不得扩大 VM 范围，不得猜测 VMX、快照、VMCI endpoint 或 Agent 身份，也不得绕过 Host CLI 直接伪造
claim、结果或生命周期状态。

## 未配置环境

`config/lab.local.json` 不存在时，先完整读取 [首次配置](首次配置.md)、`config/lab.template.json`、
`config/agent.template.json` 和对应 Schema。项目发行物定义所需字段和目录结构；自动化助手只负责读取本机
可确认信息、列出缺失项并生成配置，用户负责选择和确认 VM、基础快照、Host 状态根以及管理员操作。

自动化助手必须先做只读发现，不得猜测 VMX、快照名、Guest 存储路径或 VM 身份。确认后生成：

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
6. Host 返回终态后确认配置内本次使用的目标 VM 均已关闭电源，再报告业务状态、归档位置、清理结果、VM
   状态和租约是否释放；关机失败时不得宣称任务已经完成。

```powershell
bin\SatsumaHost.exe lab status --config config\lab.local.json
bin\SatsumaHost.exe orchestrate --config config\lab.local.json --plan task.json --timeout-seconds 900 --boot-wait-seconds 120
```

普通 `run` 仅用于确实不需要生命周期编排的兼容工作流。它同样自动执行目标 VM check，但发布后持久租约不会
随 Host 进程退出；收齐终态、完成外部清理后必须执行 `runs finalize`，随后关闭并核对本次使用的全部 VM。

## 任务编写、运行身份与日志

任务字段、运行身份选择、脚本编码和日志收集规则已经迁入
[`skills/satsuma-testlab/references/task-authoring.md`](../skills/satsuma-testlab/references/task-authoring.md)。AI 应加载
同发行版本的 Skill，不在本文维护第二套任务规则。

## 生命周期规则

多 VM 任务必须显式声明每台 VM 的 `on_success` 和 `on_failure`，并保证两条路径最终关闭 VM 电源：

- 执行前需要固定基线时设置 `restore_before`。
- 成功后使用 `stop`。
- 失败后需要恢复干净环境时使用带快照名的 `restore`。
- 失败后不恢复快照时使用 `stop`；AI 不得生成 `leave_running`。需要分析现场时保留 Host 状态和归档证据，
  而不是保持虚拟机开机。

顶层 `cleanup` 只控制 Guest 工作目录和 Host 运行目录。建议成功时 Guest `delete`、失败时 `retain`；Host 运行
只有在 Host 归档校验成功后才能使用 `archive_then_delete`。`manual_intervention_required` 固定保留 Guest 和
状态证据，但仍应尽力关闭 VM；`RECOVERY_FAILED` 或 cleanup failure 不得自动解锁实验室。

不要把唯一用户快照用作自动恢复目标。Satsuma 只允许创建/删除符合 `ai_prefix` 的快照；基础快照不属于
自动化工具所有。

## 结果判定

| 状态/退出码 | 自动化动作 |
|---|---|
| `succeeded` / 0 | 汇总步骤与证据路径，任务完成 |
| `pending` / 0 | 继续有限等待；不能宣称成功 |
| `failed` / 1 | 报告失败步骤、退出码和错误，保留运行目录 |
| 等待超时 / 3 | 查询 `runs list`，根据用户目标决定继续等待或取消 |
| `manual_intervention_required` / 5 | 停止自动重试，确认 VM 已关机，报告 claim 恢复证据并请求人工判断 |
| `RECOVERY_FAILED` / 4 | 确认 VM 已关机，报告清理/恢复失败，保留租约和证据，等待人工处理 |

非零被测程序退出码属于业务失败，不应通过重复运行、修改 `execution.json` 或删除 claim 来隐藏。

## 取消与恢复

用户要求停止任务时使用：

```powershell
bin\SatsumaHost.exe runs cancel --config config\lab.local.json --run <run-id> --reason "user requested stop"
```

取消后继续读取报告，直到收齐失败结果或出现人工门禁。不要同时删除运行目录。若 VMCI 网关暂时不可用，
Agent 会按 `reconnect_interval_ms` 重试；Host 侧应先恢复网关，再决定是否恢复快照。

发生未知 VMware/Guest 状态时，自动化最多执行一次用户已授权的明确恢复动作。相同故障再次出现时停止，
输出 Host 检查、运行报告和生命周期状态，不进行循环重启或连续快照恢复。

Host 崩溃后先读取 `lab status`。仅当持久状态明确允许同一 `run_id` 恢复时使用 `lab recover`；不得把 Host
进程退出视为实验室空闲。`lab unlock --force true` 只能在人已确认原 Host 进程死亡、Guest/外部系统状态和
证据保留情况后执行。

恢复快照前必须把 Guest 本地存储、Host 状态根和 Host 归档视为三个独立恢复域，并遵守以下门禁：

1. 先保存报告、生命周期、租约和错误证据，确认原 Host 进程已退出或完成同一 `run_id` 的恢复。
2. 在整理 Host 状态前硬停止目标 VM，避免旧 Agent 与网关继续发布协议数据。
3. 只归档和移走目标 VM 已明确放弃的 `updates`、旧 presence、inventory 和请求；不得清空状态根，不得删除
   pending、人工门禁、未归档运行、claim 或其他 VM 的状态。
4. 恢复并启动 VM 后，必须观察到恢复后生成的新 boot/session/presence；旧文件存在不能作为上线证据。
5. 完整 `check` 通过后才能继续任务。失败时保留现场并停止自动重试。

Host 不需要随 VM 重启宿主操作系统。需要的是确认旧 Host PID 已死亡、持久租约已恢复或人工解锁，然后启动
新的 Host CLI 进程；禁止两个 Host 并发接管同一实验室。

## 禁止事项

- 不在 Host 直接运行业务 Artifact。
- 不把未登记的 Guest 绝对路径作为 `program`。
- 不直接写 `runs/<run-id>/task.json`、claim、`execution.json` 或 `lifecycle.json`。
- 不删除 pending、invalid、人工门禁或仍需取证的运行。
- 不在未明确授权时启用真实 VMware crash/fault recovery CMake 目标。
- 不记录、回传或上传 Artifact 内容、用户数据和完整日志，除非用户明确要求。

## 交付摘要

自动化任务结束时至少说明：使用的配置与任务文件、`run_id`、顶层状态、失败/成功步骤数、Host 归档位置、
Guest/Host 状态清理动作、VM 最终状态、租约是否释放以及是否仍需人工介入。不得只用命令退出码替代业务结果。

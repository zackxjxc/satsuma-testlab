# Satsuma AI 使用契约

本文件供调用 Satsuma 的 AI 阅读。Satsuma 只负责隔离 VM 中的部署、执行、证据收集和显式 VMware
操作；被测程序是否符合业务预期，始终由调用方 AI 判断。

Satsuma 面向可信、单用户 Windows Host 上的可丢弃 VMware VM，以及通常数分钟内结束的前台小任务。
业务 Artifact 只能在 Guest 执行。AI 不得要求 Satsuma 修改 Host 服务、驱动、防火墙、网络、注册表或
其他系统配置。

## 1. 每次接入的读取顺序

1. 找到包含 `lab.json`、`schemas/` 和 `SatsumaHost.exe` 构建产物的 Satsuma 仓库。
2. 完整读取 [SatsumaReadme.md](SatsumaReadme.md)、`lab.json`、`schemas/lab.schema.json`、
   `schemas/agent.schema.json` 和 `schemas/task.schema.json`。
3. 如果存在被 Git 忽略的 `lab.local.json`，将它作为本机真实配置读取，`lab.json` 只作为模板。
4. 读取目标项目的测试目标、构建产物和已有任务文件；不要猜测应该执行哪个程序或参数。
5. 把仓库模板中的盘符、IP、VMX 和快照名视为示例，必须通过本机只读检查或用户确认后才能使用。

## 2. 环境尚未配置时

先做只读发现，再给用户生成一份“本机 Satsuma 环境配置清单”。不要只回复“请准备 VMware”，也
不要要求用户重复提供能够从本机读取的信息。

只读发现至少覆盖：

- `SatsumaHost.exe`、`SatsumaVM.exe` 和 `vmrun.exe` 是否存在及其绝对路径。
- `lab.local.json`（如果存在）当前值、Host 共享目录、归档目录和每个 VMX 是否存在。
- 配置中每台 VM 的 ID、可选管理 IP、基础快照和 Agent 版本。
- 哪些事项只能进入 Guest 或 VMware GUI 后由用户确认。

配置清单必须按以下结构输出：

1. 已自动确认的本机值。
2. Host 上需要用户完成的操作。
3. 每台 VM 内需要用户完成的操作。
4. `lab.local.json` 和每台 VM `agent.json` 应填写的确切值或待确认占位项。
5. 需要管理员权限、VMware GUI 或重启的步骤。
6. 用户完成后由 AI 执行的验收命令和通过标准。

如果用户要求把清单保存为文件，使用 Markdown，并明确区分“已确认”“待用户操作”“尚未验证”。
不得虚构 VM 内状态、共享目录可写性、快照存在性或管理员操作成功。当前项目永不修改 Host 防火墙、
网络、服务、驱动、注册表或系统配置，也不扩大 VMware Shared Folder 范围。Agent Windows Service
只由 Guest 安装脚本和 `SatsumaVM --install-service` 维护，不要求用户进入服务管理器手工配置。

Guest 已能访问 Shared Folder 时，只要求用户执行一次 `scripts/install-agent.ps1` 并确认 Windows UAC。
脚本只接受本地固定磁盘中的专用 `Satsuma` 安装根，完成 ACL 收紧、暂存哈希校验、Service 所有权预检、
旧实例停止、失败回滚和 LocalSystem Windows Service 注册。脚本成功只表示 SCM 已进入 `RUNNING` 并返回
有效 PID；共享目录 presence 和真实任务通道仍必须由后续 `check` 验证。AI 不应要求用户保持前台终端、
每次开机手工启动 Agent，或绕过安装脚本直接使用 `sc.exe` 接管同名 Service。

`--watch` 文件循环和生产 `--service` 只使用共享文件通道，不启动、不调用也不等待 RPC。`--rpc-once` 仅用于
显式诊断；不要把 `SatsumaHost serve` 或 TCP 37100 当作业务任务的前置条件。

## 3. 配置一致性

正式检测前核对以下对应关系：

| Host `lab.local.json` | VM `agent.json` | 要求 |
|---|---|---|
| `lab_id` | `lab_id` | 完全一致 |
| `vms[].id` | `vm_id` | 当前 VM 必须唯一匹配 |
| `vms[].agent_version` | `agent_version` | 完全一致 |
| `host.listen` | `host` | 完全一致 |
| `shared_folder.guest_root` | `shared_root` | 指向同一个 VMware Shared Folder |
| `shared_folder.host_root` | 无 | Host 侧真实、可写且不包含敏感数据 |
| `vms[].snapshots.base` | 无 | 必须是用户手工创建的只读基础快照 |

JSON 中 Windows 反斜杠需要转义。不得把密码、Token、源码目录、用户目录或凭据写入共享目录和任务。
所有 Host 命令都必须显式提供 `--config`。`vmrun`、Host 共享/归档根目录、VMX、Agent Shared Folder 和
本地工作根目录必须使用绝对路径，不能依赖当前目录。

## 4. 主动检测是业务任务的前置门禁

确认目标 VM 已启动后执行；安装完成的 Agent 会由 Windows Service 自动运行：

```text
SatsumaHost.exe check --config lab.local.json --vm <vm-id> --timeout-seconds 180
```

需要全部 VM 时省略 `--vm`。冷启动后的首次检测使用 180–240 秒；只有 Agent 已上线时才可缩短到 30 秒。
必须同时读取进程退出码和 stdout JSON：

| 退出码 | 顶层状态 | AI 行为 |
|---|---|---|
| 0 | `ready` | 允许继续当前目标 VM 的业务任务 |
| 3 | `degraded` | 停止业务任务，逐项解释 `checks[]` 中的失败 |
| 1 | `failed` | 停止业务任务，检查 `agents[]`、运行目录和日志 |
| 1 | 无有效 JSON | 配置或参数解析失败，读取 stderr 后修正 |
| 2 | 无有效 JSON | CLI 只输出用法，修正命令结构后重试 |

`check` 会报告共享/归档容量和 VMware Tools 状态，并写入无害 `echo` 任务，但不会启动 VM、启动 Agent、
改动网络或恢复快照。Shared Folder 检查失败时不会发布任务，`run_id` 为 `null`，目标 Agent 状态为
`skipped`。不得因为 VMX、`vmrun` 或共享目录存在就自行宣称环境可用；只有本轮报告为 `ready` 才能通过
门禁。Agent 探针成功且唯一失败项为 VMware Tools 时，Host 会最多再复检 30 秒；AI 应读取
`initial_environment` 和 `environment_recheck_attempts`，不得把初始 `installed` 隐藏为无条件成功。

## 5. 生成任务文件

任务步骤只支持 `echo` 和前台 `execute`，以 `schemas/task.schema.json` 和现有 C++ 校验为准。一台或多台
VM 的生命周期计划可以使用 `lifecycle.vms[].restore_before`、`on_success`、`on_failure` 和
`lifecycle.finally`；必须在计划顶层显式提供唯一 `run_id` 并通过 `orchestrate` 执行，普通 `run` 会明确
拒绝。后台任务、步骤依赖、业务就绪屏障和任意 Host 命令均不在支持范围，AI 不得生成，也不得直接
生成或修改 claim 内部字段。

文件控制 mailbox、Host stop/cancel API、单调 sequence 和 ACK 不在支持范围。AI 不得手工向共享目录
写入自定义控制文件，也不得把文件存在解释为控制成功。

生成任务时遵守：

- 每个 Artifact 的 `source` 是 Host 上已经存在的绝对文件路径。
- `shared_destination` 必须是 `artifacts/` 下的相对路径。
- `execute.program` 必须与同一 VM 已登记 Artifact 的 `shared_destination` 完全对应。
- `arguments` 的每个元素保持独立，不拼接 shell 命令。
- `collect_files` 只使用 VM 本地运行根目录下的相对路径，同一步骤不得声明 Windows 等价的重复路径。
- 只生成 Schema 声明的字段；Host 会拒绝未知字段，不得依赖拼写错误回落为默认值。
- 每个步骤设置有限的 `timeout_seconds`，通常不超过 300 秒；超出可信前台小任务范围的程序不得下发。
- `echo` 默认可安全重试；`execute` 默认不可重试，只有确认幂等时才设置 `retry_safe: true`。
- 被测程序优先输出明确退出码、UTF-8 日志和 JSON 总结，不把关键结果只留在 GUI。
- `lifecycle.vms` 至少包含一台 VM、不得重复，并覆盖全部 Artifact、主步骤和 `finally` 步骤引用的 VM。
- `lifecycle.vms` 的声明顺序决定恢复、启动和逐台 Agent Ready 检测顺序，清理按逆序执行；不要依赖
  `lab.json` 中的 VM 排列。
- 主任务只会在全部 Agent Ready 后发布，但其中的多 VM 步骤没有依赖或服务就绪语义；需要 Gateway
  业务服务先 Ready 的计划仍不可生成。

## 6. 执行和取证

1. 使用 `SatsumaHost.exe run --config lab.local.json --plan <task.json>` 物化任务。
2. 保存 Host 返回的唯一 `run_id`，不得复用或手工覆盖旧运行目录。
3. 使用 `SatsumaHost.exe report --config lab.local.json --run <run-id> --wait-seconds <1-86400>` 有限等待。
4. 等待超时时读取退出码 3 和当前汇总；需要即时只读状态时省略 `--wait-seconds`。
5. 先读取 `execution.json`、`stdout.log`、`stderr.log` 和声明收集的文件，再修改目标项目。
6. 区分平台失败和业务失败：部署、超时、路径、Agent 与 VMware 属于平台证据；业务断言由 AI 分析。

生命周期计划改用：

```text
SatsumaHost.exe orchestrate --config lab.local.json --plan <task.json> --timeout-seconds <1-86400>
```

计划必须提前固定唯一 `run_id`。只有 Host 进程对已有生命周期归档断点续跑时，才复用同一文件的原始
字节、`run_id` 和相同的有序 VM 集合；计划 SHA-256 或 VM 顺序变化都会拒绝复用归档。VM 重启或快照
恢复后的业务重试属于新运行，必须生成新 `run_id`。

退出码 0 表示 `COMPLETED`，退出码 1 表示业务或执行 `FAILED`，退出码 4 表示 `RECOVERY_FAILED`，
退出码 5 表示 `MANUAL_INTERVENTION_REQUIRED`。
必须读取 `archive_root/runs/<run-id>/lifecycle.json` 和 `evidence/`，不能仅凭主步骤退出码判断恢复结果。

运行证据位于 `shared_folder.host_root/runs/<run_id>/`。不要直接修改已发布的 `task.json`、claim、
`execution.json` 或最终日志；需要重试时生成新运行。

普通运行可使用 `report --wait-seconds <1-86400>` 有限等待。退出码 0 表示报告完整，3 表示等待超时，
5 表示过期危险 claim 需要人工处理；未指定时保持即时汇总行为。顶层 `status` 为 `pending`、
`succeeded`、`failed` 或 `manual_intervention_required`。退出码 0 不表示业务步骤成功，AI 仍须检查
`status`、`failed_steps` 和 `executions[]`。

## 7. VMware 与恢复边界

- VM 生命周期和快照只通过 `SatsumaHost.exe vm/snapshot` 命令操作，不直接调用 `vmrun`。
- 用户基础快照只读，禁止覆盖或删除；AI 只管理带 `ai_prefix` 的派生快照。
- 用户已把配置中的 VM 声明为可丢弃测试机并授权生命周期操作后，AI 可以在该范围内启动、关闭和恢复，
  不要为每次相同操作重复要求用户确认；未知 VM、扩大网络范围或删除基础快照不在该授权内。
- 单 VM 或多 VM 生命周期计划可自动恢复配置中的基础快照或 AI 所有权快照；普通任务仍需显式执行 VM
  命令。
- 多 VM 编排按 `lifecycle.vms` 顺序恢复、启动和检测全部 Agent，主任务完成后按逆序执行 teardown；单台
  清理失败时继续处理其余 VM，最终统一报告 `RECOVERY_FAILED`。
- AI 快照创建或删除命令报错后，Host 会重新列举快照并以目标实际出现或消失为准；返回
  `reconciled=true` 时仍需保留元数据中的原始操作错误作为证据，无法对账时停止自动测试。
- 已存在生命周期归档时，只有同一 `run_id`、相同有序 VM 集合和计划 SHA-256 才允许复用；旧单 VM
  schema v1 归档继续兼容，新的多 VM 归档使用 schema v2。`executing` 和 `collecting_evidence` 可继续
  处理，已有终态幂等返回，其他非终态必须转入人工门禁。
- 真实崩溃恢复只使用默认关闭的 `SatsumaRealVmwareCrashRecovery` 目标，并要求专用 VM、唯一 Agent
  `vm_id` 和精确确认串。它仅供维护者手工回归，不属于日常使用或项目完成条件；普通构建和 `ctest`
  不得触发 Host/Agent 强杀。
- 真实 Shared Folder 瞬断只使用默认关闭的 `SatsumaRealVmwareFaultRecovery` 目标；必须先通过主动检测，
  使用精确确认串，并确认没有另一台 Agent 复用同一 `vm_id`。它同样仅供维护者手工回归，普通构建和
  `ctest` 不得改变 runtime Shared Folder 状态。
- 配置或任务格式错误由 AI 修正输入；程序非零退出和超时保留为任务结果。
- 其他 Host、VMware、Shared Folder、Agent 或 Guest 异常统一视为未知平台错误，但保留底层错误和证据。
- 硬重启按 `vm stop --mode hard` → `vm start` → `check --timeout-seconds 180` 执行。
- 快照恢复按 `vm stop --mode hard` → `vm restore` → `vm start` → `check --timeout-seconds 180` 执行。
- 环境重新为 `ready` 后以新 `run_id` 重试业务任务一次；非幂等任务无法确认干净状态时不重试。
- 重试或 VM 恢复仍失败时停止并等待用户，不无限重试，也不自动修复 Host 系统。
- 被测程序只能在隔离 VM 中执行；Host 只运行 Satsuma 自身程序、构建和自动化测试。

## 8. 对用户的最终反馈

每次运行结论先报告：目标 VM 集合、逐台检测状态、`run_id`、步骤退出码/超时、证据路径和未验证项。未实际
执行的 VMware、管理员或 Guest 操作必须明确标为待完成，不能用构建通过或模拟测试代替真实环境验收。

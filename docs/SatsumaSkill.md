# Satsuma AI 使用契约

本文件供调用 Satsuma 的 AI 阅读。Satsuma 只负责隔离 VM 中的部署、执行、证据收集和显式 VMware
操作；被测程序是否符合业务预期，始终由调用方 AI 判断。

## 1. 每次接入的读取顺序

1. 找到包含 `lab.json`、`schemas/` 和 `SatsumaHost.exe` 构建产物的 Satsuma 仓库。
2. 完整读取 [SatsumaReadme.md](SatsumaReadme.md)、`lab.json`、`schemas/lab.schema.json`、
   `schemas/agent.schema.json` 和 `schemas/task.schema.json`。
3. 读取目标项目的测试目标、构建产物和已有任务文件；不要猜测应该执行哪个程序或参数。
4. 把仓库模板中的盘符、IP、VMX 和快照名视为示例，必须通过本机只读检查或用户确认后才能使用。

## 2. 环境尚未配置时

先做只读发现，再给用户生成一份“本机 Satsuma 环境配置清单”。不要只回复“请准备 VMware”，也
不要要求用户重复提供能够从本机读取的信息。

只读发现至少覆盖：

- `SatsumaHost.exe`、`SatsumaVM.exe` 和 `vmrun.exe` 是否存在及其绝对路径。
- `lab.json` 当前值、Host 共享目录、归档目录和每个 VMX 是否存在。
- Host 当前 IPv4 地址，哪些值可能适合作为 Host-only 管理地址。
- 配置中每台 VM 的 ID、管理 IP、基础快照和 Agent 版本。
- 哪些事项只能进入 Guest 或 VMware GUI 后由用户确认。

配置清单必须按以下结构输出：

1. 已自动确认的本机值。
2. Host 上需要用户完成的操作。
3. 每台 VM 内需要用户完成的操作。
4. `lab.json` 和每台 VM `agent.json` 应填写的确切值或待确认占位项。
5. 需要管理员权限、VMware GUI 或重启的步骤。
6. 用户完成后由 AI 执行的验收命令和通过标准。

如果用户要求把清单保存为文件，使用 Markdown，并明确区分“已确认”“待用户操作”“尚未验证”。
不得虚构 VM 内状态、共享目录可写性、快照存在性或管理员操作成功。未经明确授权，不修改防火墙、
Host/VM 网络、VMware Shared Folder、计划任务或快照。

## 3. 配置一致性

正式检测前核对以下对应关系：

| Host `lab.json` | VM `agent.json` | 要求 |
|---|---|---|
| `lab_id` | `lab_id` | 完全一致 |
| `vms[].id` | `vm_id` | 当前 VM 必须唯一匹配 |
| `vms[].agent_version` | `agent_version` | 完全一致 |
| `host.listen` | `host` | 完全一致 |
| `shared_folder.guest_root` | `shared_root` | 指向同一个 VMware Shared Folder |
| `shared_folder.host_root` | 无 | Host 侧真实、可写且不包含敏感数据 |
| `vms[].snapshots.base` | 无 | 必须是用户手工创建的只读基础快照 |

JSON 中 Windows 反斜杠需要转义。不得把密码、Token、源码目录、用户目录或凭据写入共享目录和任务。

## 4. 主动检测是业务任务的前置门禁

确认目标 VM 已启动且 `SatsumaVM.exe --watch` 正在运行后执行：

```text
SatsumaHost.exe check --config lab.json --vm <vm-id> --timeout-seconds 30
```

需要全部 VM 时省略 `--vm`。必须同时读取进程退出码和 stdout JSON：

| 退出码 | 顶层状态 | AI 行为 |
|---|---|---|
| 0 | `ready` | 允许继续当前目标 VM 的业务任务 |
| 3 | `degraded` | 停止业务任务，逐项解释 `checks[]` 中的失败 |
| 1 | `failed` | 停止业务任务，检查 `agents[]`、运行目录和日志 |
| 1 | 无有效 JSON | 配置或参数解析失败，读取 stderr 后修正 |
| 2 | 无有效 JSON | CLI 只输出用法，修正命令结构后重试 |

`check` 会写入无害 `echo` 任务，但不会启动 VM、启动 Agent、改动网络或恢复快照。不得因为 VMX、
`vmrun` 或共享目录存在就自行宣称环境可用；只有本轮报告为 `ready` 才能通过门禁。

## 5. 生成任务文件

当前任务只支持 `echo` 和前台 `execute`，以 `schemas/task.schema.json` 和现有 C++ 校验为准。不得生成
尚未实现的 `background`、任务内快照、`finally`、自动恢复或任意 Host 命令字段。

生成任务时遵守：

- 每个 Artifact 的 `source` 是 Host 上已经存在的绝对文件路径。
- `shared_destination` 必须是 `artifacts/` 下的相对路径。
- `execute.program` 必须与同一 VM 已登记 Artifact 的 `shared_destination` 完全对应。
- `arguments` 的每个元素保持独立，不拼接 shell 命令。
- `collect_files` 只使用 VM 本地运行根目录下的相对路径。
- 每个步骤设置有限的 `timeout_seconds`；危险程序必须有可验证的外部状态或机器可读结果。
- 被测程序优先输出明确退出码、UTF-8 日志和 JSON 总结，不把关键结果只留在 GUI。

## 6. 执行和取证

1. 使用 `SatsumaHost.exe run --config lab.json --plan <task.json>` 物化任务。
2. 保存 Host 返回的唯一 `run_id`，不得复用或手工覆盖旧运行目录。
3. 等待 Agent 执行；`report` 尚未提供等待参数，需要有限间隔轮询，禁止无限等待。
4. 使用 `SatsumaHost.exe report --config lab.json --run <run-id>` 获取汇总。
5. 先读取 `execution.json`、`stdout.log`、`stderr.log` 和声明收集的文件，再修改目标项目。
6. 区分平台失败和业务失败：部署、超时、路径、Agent 与 VMware 属于平台证据；业务断言由 AI 分析。

运行证据位于 `shared_folder.host_root/runs/<run_id>/`。不要直接修改已发布的 `task.json`、claim、
`execution.json` 或最终日志；需要重试时生成新运行。

## 7. VMware 与恢复边界

- VM 生命周期和快照只通过 `SatsumaHost.exe vm/snapshot` 命令操作，不直接调用 `vmrun`。
- 用户基础快照只读，禁止覆盖或删除；AI 只管理带 `ai_prefix` 的派生快照。
- 当前 JSON 任务不会自动恢复快照。需要恢复时，先停止业务任务，再显式关闭、恢复和启动 VM。
- 任一带外操作失败时停止自动测试，保留错误输出，并要求用户处理 VMware 环境。
- 被测程序只能在隔离 VM 中执行，不在 Host 添加运行被测 exe 的旁路命令。

## 8. 对用户的最终反馈

每次运行结论先报告：目标 VM、检测状态、`run_id`、步骤退出码/超时、证据路径和未验证项。未实际
执行的 VMware、管理员或 Guest 操作必须明确标为待完成，不能用构建通过或模拟测试代替真实环境验收。

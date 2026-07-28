# Satsuma TestLab 虚拟机测试工具项目启动方案

> [!IMPORTANT]
> 本文是立项阶段的历史设计记录，仅用于解释早期决策，不是当前操作指南或未完成工作清单。文中的旧命令、
> RPC 主通道、网络方案和任务结构不得作为当前使用或后续开发依据。当前行为以
> [SatsumaReadme.md](SatsumaReadme.md)、[SatsumaSkill.md](SatsumaSkill.md) 和
> [项目当前状态与后续计划.md](项目当前状态与后续计划.md) 为准。

已有远程库地址https://gitee.com/zackxjxc/satsuma-testlab.git


## 1. 一句话定义

Satsuma 是一个运行在宿主机和测试虚拟机之间的实验控制工具。它不理解被测项目的业务逻辑，
不负责判断 VPN、DNS 或其他程序是否“设计正确”；它只提供一条可靠、可恢复、可重复的路径：

```text
宿主机 AI
  -> 控制虚拟机
  -> 传入被测文件
  -> 要求虚拟机执行命令
  -> 取回日志和结果文件
  -> 必要时关闭、重启或恢复快照
```

被测项目的 AI 是 Satsuma 的上层用户。它决定编译什么、执行什么、如何生成测试报告、如何解释
报告。Satsuma 只保证执行过程和环境边界。

本文档是独立项目的启动方案，不假设任何特定 VPN 仓库、源码布局或既有测试工具存在。

## 2. 产品交付物

首版完成后，用户应得到：

```text
SatsumaHost.exe       宿主机控制端
SatsumaVM.exe         虚拟机执行端
SatsumaReadme.md      用户环境搭建和命令说明
SatsumaSkill.md       供 AI 读取的最小使用契约
lab.json              用户填写的虚拟机配置模板
示例任务文件          一个可复用的 JSON 任务示例
两台已保存快照的 VM    用户按文档手工准备
```

`SatsumaHost.exe` 和 `SatsumaVM.exe` 来自同一个 CMake 工程，共享一个 `SatsumaCore` 静态库。
静态库只在编译期复用协议、JSON、日志和错误处理代码，不产生需要随程序部署的公共 DLL；最终仍是
两个自包含 exe。首版不采用运行时猜测角色的单一 exe，分成两个目标更容易理解、打包和诊断。

## 3. 明确责任边界

### 3.1 Satsuma 负责

- 读取虚拟机配置和任务文件。
- 调用 VMware Workstation 的控制接口。
- 启动、关闭、重启虚拟机。
- 恢复指定快照。
- 保护用户基础快照，并管理 AI 派生快照及其元数据。
- 管理 Host/VM 共享目录中的任务、文件和结果。
- 通过 RPC 向 VM 通知新任务，并接收实时心跳和状态。
- 在 VM 中启动进程、等待完成、超时终止整个进程树。
- 收集共享目录中的 stdout、stderr、退出码、耗时和声明的结果文件。
- 保存运行记录，报告通信、部署、执行和恢复错误。
- 在 Guest 网络失效时使用带外通道恢复环境。

### 3.2 Satsuma 不负责

- 判断被测程序的业务结果是否正确。
- 替 AI 编写测试程序或测试断言。
- 自动猜测应该执行哪个 exe、传哪些参数。
- 解析所有项目自定义日志格式。
- 在宿主机上运行被测项目的高风险程序。
- 修改宿主机的路由、DNS、防火墙或网卡。
- 把某个 VPN 项目的源码、路径或协议写死在平台核心。

宿主机可以运行编译器、Satsuma 自身、纯逻辑单元测试和其他明确列入白名单的低风险工具；涉及
管理员权限、驱动、虚拟网卡、路由、DNS、防火墙、服务或系统重启的被测程序，应在 VM 中运行。

## 4. 使用场景

AI 在宿主机完成目标项目的修改和构建后，判断某个测试需要隔离环境：

```text
目标项目 AI
  -> 生成 target-gateway.exe、target-client.exe 和测试脚本
  -> 在宿主机完成构建
  -> 读取 SatsumaSkill.md
  -> 启动或连接 SatsumaHost.exe
  -> 把产物部署到一个或多个 VM
  -> 让 SatsumaVM.exe 执行命令
  -> 取回 log.txt、result.json 和系统状态
  -> AI 分析失败原因并修改目标项目
  -> 恢复 VM 快照后重新测试
```

Satsuma 不需要知道这些 exe 是 VPN、DNS、驱动安装程序还是普通服务。对它们统一视为“带参数
运行并产生结果的实验程序”。

## 5. 总体架构

```text
+---------------------------- Host machine -----------------------------+
|                                                                        |
|  AI / Codex                                                            |
|       |                                                                |
|       v                                                                |
|  SatsumaHost.exe                                                      |
|    |       |        |                                                  |
|    |       |        +-- Run records / immutable archive                 |
|    |       +----------- Shared folder task/result store                 |
|    |       +----------- coro_rpc command and heartbeat server           |
|    +------------------- VMware provider (vmrun)                         |
+------------------------|-----------------------------------------------+
                         |
              VMware control channel (out of band)
                         |
        +----------------+----------------+
        |                                 |
 +------+------+                    +-----+-------+
 | Client VM   |                    | Gateway VM  |
 | SatsumaVM   |---- management ----| SatsumaVM   |
 | test target |                    | test target |
 +------+------+                    +-----+-------+
        |                                 |
        +-------- isolated experiment network ----------------------------+

 Host:  D:\\vm-share
 Guest: \\\\vmware-host\\Shared Folders\\vm-share
```

默认是一台 Host 和两台 VM。AI 可以在任务中只使用一台 VM，也可以扩展到三台或更多 VM；Satsuma
Host 不限制 VM 的业务角色，只识别配置中的稳定 ID。

## 6. 两个可执行文件

### 6.1 `SatsumaHost.exe`

运行位置：宿主机。

主要能力：

```text
SatsumaHost.exe serve --config lab.json
SatsumaHost.exe vm start --id client
SatsumaHost.exe vm restore --id gateway --snapshot clean
SatsumaHost.exe snapshot create-ai --vm client --name network-ready
SatsumaHost.exe snapshot list --vm client
SatsumaHost.exe file push --vm client --file target.exe
SatsumaHost.exe exec --vm client --program target.exe --arg ...
SatsumaHost.exe run --plan experiment.json
SatsumaHost.exe report --run <run-id>
```

它向 AI 提供本机 CLI 和 JSON 任务入口。运行中的 Host 可以另外开放仅限本机的控制接口，但 AI
不需要理解 Host/VM 的内部 RPC。逐条命令和任务文件最终都调用同一套 Controller API。

### 6.2 `SatsumaVM.exe`

运行位置：每一台测试 VM 内。首版作为管理员权限控制台程序运行，不注册 Windows Service。

主要能力：

- 启动后读取 VM ID、Host 地址、共享目录映射和认证信息。
- 作为 `coro_rpc` client 主动连接 Host，并持续重试。
- 轮询共享目录中的任务清单，并通过 RPC 接收即时任务通知。
- 校验共享目录中的 Artifact hash。
- 以指定运行身份启动被测程序或脚本。
- 使用 Job Object 管理完整进程树。
- 捕获 stdout、stderr、退出码和超时状态。
- 把日志、结果文件和状态心跳定期刷入共享目录。
- Host RPC 不可达时继续执行已经领取的任务，并保留可见的磁盘状态。

`SatsumaVM.exe` 不控制 VMware 快照。任务清单只使用相对于共享根目录的路径，由两端根据配置
分别解析为 Host 路径和 Guest UNC 路径。

### 6.3 一个 exe 是否更好

不推荐首版只编译一个运行时自我判断角色的 exe：

- Host 需要访问 VMware 和本地 Artifact，VM 不需要。
- VM 需要管理员权限，Host 通常不应以相同权限运行。
- 两个角色的配置错误容易互相污染。
- 日志和安装问题不容易判断属于哪一侧。

因此首版采用两个 exe + 一个公共静态库。静态链接后不需要额外部署共享库。一个 exe 的统一启动器
可以作为后续便利功能，而不是核心架构。

## 7. 通信设计

Satsuma 使用三条互补通道，不要求一条通道承担全部职责：

| 通道 | 主要职责 | Guest 普通网络中断后 |
|---|---|---|
| VMware Shared Folder | Artifact、任务清单、日志和结果文件 | 只要 Guest OS 和 VMware Tools 仍运行就可用 |
| `coro_rpc` | 新任务通知、命令控制、实时心跳和快速状态 | 可能中断 |
| VMware `vmrun` | VM 电源、快照、Guest 状态和最终恢复 | 不依赖 Guest 普通网络 |

### 7.1 共享目录是持久数据通道

用户在 VMware 中配置一个专用共享目录：

```text
Host root:  D:\\vm-share
Guest root: \\\\vmware-host\\Shared Folders\\vm-share
```

两端配置只保存自己的绝对根路径，任务清单只使用相对路径。例如：

```text
相对路径: runs/20260722154611/client/target.exe
Host:     D:\\vm-share\\runs\\20260722154611\\client\\target.exe
Guest:    \\\\vmware-host\\Shared Folders\\vm-share\\runs\\20260722154611\\client\\target.exe
```

每个运行目录采用固定结构：

```text
runs/<run_id>/
├── task.json
├── artifacts/
│   ├── client/
│   └── gateway/
├── state/
│   ├── client-agent.json
│   └── gateway-agent.json
└── results/
    ├── client/
    │   ├── execution.json
    │   ├── stdout.log.partial
    │   ├── stderr.log.partial
    │   └── files/
    └── gateway/
```

AI 可以直接把 Artifact 写入 Host 侧运行目录，也可以调用 `SatsumaHost.exe file push`。直接写入前
应先让 Host 分配 `run_id`；写完后仍由 Host 生成 manifest、计算 hash 并确认文件完整，VM 不执行
没有登记在 manifest 中的文件。

可靠性规则：

- Host 先写 `task.json.tmp`，完成并刷新后原子改名为 `task.json`。
- SatsumaVM 领取任务后写入包含 `job_id` 的 claim 文件，防止重复执行。
- SatsumaVM 校验 Artifact hash 后复制到 VM 本地工作目录再执行，不直接从 UNC 路径启动 exe。
- Agent 状态和执行结果同样采用临时文件 + 原子改名。
- stdout/stderr 在本地捕获，同时按固定间隔追加或镜像到共享目录的 `.partial` 文件。
- 被测程序可以直接把额外报告写到任务指定的共享结果目录。
- Host 发现最终结果后立即复制到共享目录之外的只读归档区。
- 每个运行使用全新的 `run_id`，不能复用旧目录。

即使被测程序崩溃、RPC 断开或 Guest 随后死机，Host 仍可读取最后一次已经刷入宿主机磁盘的
任务状态和部分日志。完全死机之后不可能继续产生新日志，但已有证据不会依赖 Guest 再次上传。

### 7.2 `coro_rpc` 是控制和实时状态通道

Host 和 VM 都是同一仓库、同一版本体系下的 C++20 程序，AI 又不直接调用这条内部通道，因此
类型化 RPC 比自行维护 HTTP route、JSON 请求解析和错误映射更合适。RPC 的收益是接口类型明确、
异步调用自然、超时和连接管理集中；它的代价是 Host/VM 版本耦合和二进制消息不便人工查看。

可观察性由共享目录中的 JSON manifest 和状态文件承担，所以不需要为了 `curl` 调试而强行保留
HTTP。以后若引入非 C++ Guest Agent，可以在 Host 外层新增 HTTP 适配器，不改变内部任务模型。

SatsumaVM 主动连接 Host：

```text
SatsumaVM --RPC connect/heartbeat/poll--> SatsumaHost
SatsumaHost --RPC response directive----> SatsumaVM
```

Host 不需要主动连接 VM。VM 作为 RPC client 调用 Host 的注册、心跳和长轮询方法，Host 在响应中
返回任务引用。最小 RPC 接口：

```text
register_agent(AgentHello) -> SessionInfo
heartbeat(AgentStatus) -> HostDirective
poll_task(PollRequest) -> Optional<TaskReference>
report_job(JobStatus) -> Ack
```

`TaskReference` 只引用共享目录中的 manifest：

```json
{
  "type": "run_manifest",
  "run_id": "20260722154611",
  "manifest": "runs/20260722154611/task.json"
}
```

无论 AI 使用逐步命令还是批量任务，Host 都先把请求物化为一个任务清单；逐步命令只是只含一个
step 的小任务。任务清单是事实来源，RPC 只是即时控制通道。Host 尚未启动或 RPC 暂时中断时，
SatsumaVM 继续重试连接，并可以轮询分配给自己的新任务清单。

RPC DTO 必须包含 `protocol_version`、`lab_id`、`vm_id`、`session_id`、`boot_id` 和 `request_id`。
Host/VM 版本不兼容时应明确拒绝连接，不尝试错误反序列化。由于快照可能保存旧版 SatsumaVM，
`lab.json` 还应记录快照对应的 Agent 版本，`doctor` 在测试前检查版本匹配。

首版选用 `yalantinglibs` 中的 `coro_rpc` 和 `struct_pack`，并固定到经过验证的正式 tag，不跟随
`main`。不选择 `rpclib`，因为它的最新正式版本较旧，官方 README 仍明确说明正在寻找维护者。

选型核对日期为 2026-07-22：`yalantinglibs` 的正式版本已到 `0.6.1`，仓库仍活跃并列出
Windows/MSVC CI；`rpclib` 最新正式版本为 `v2.3.0`。施工前仍需重新检查正式版本和 Windows 构建。

- `yalantinglibs`：<https://github.com/alibaba/yalantinglibs>
- `rpclib`：<https://github.com/rpclib/rpclib>

### 7.3 `vmrun` 是 VM 生命周期和最终恢复通道

Host 通过 `vmrun.exe` 和 VMware Tools 完成：

- 启动、停止和重启 VM。
- 查询 VM 是否仍处于 powered-on 状态。
- 恢复快照。
- 在 Agent 未运行时执行 Bootstrap 或紧急命令。
- 在共享目录不可用时尝试取回 Guest 本地文件。

首版不实现 VMware 私有 SDK。所有 `vmrun` 调用使用结构化参数和 `CreateProcessW`，禁止通过
`cmd /c` 拼接未校验字符串。

### 7.4 不采用 FTP

FTP 会增加额外服务、端口、认证和清理逻辑，仍然依赖 Guest 网络。共享目录已经覆盖文件和残留
日志场景，因此首版不引入 FTP。

## 8. 虚拟机环境和快照

### 8.1 用户手工准备

Satsuma 项目不强制自动创建 VM。用户需要：

1. 安装 VMware Workstation 和 VMware Tools。
2. 创建一台 Windows 基础 VM。
3. 以管理员权限启动 `SatsumaVM.exe`，或创建“使用最高权限运行”的启动计划任务。
4. 配置管理网络，使 VM 能访问 Host 的 Satsuma RPC 端口。
5. 配置专用 VMware Shared Folder，并验证 Host/Guest 两侧路径都可读写。
6. 从基础 VM 克隆出 Client、Gateway 等实例。
7. 为每台 VM 设置固定 `vm_id` 和管理 IP。
8. 为实验数据建立隔离的自定义 vmnet，不使用桥接到生产网络。
9. 删除遗留测试进程、适配器、路由和临时文件。
10. 创建名为 `clean` 的用户基础快照并填写 `lab.json`。

### 8.2 快照分层和所有权

Satsuma 区分两类快照：

| 类型 | 创建者 | 用途 | AI 权限 |
|---|---|---|---|
| 用户基础快照 | 用户 | 已安装 SatsumaVM、管理网络和通用工具的可信基线 | 只读，不允许覆盖或删除 |
| AI 派生快照 | AI 通过 SatsumaHost | 某个项目或测试所需的精细环境 | 允许创建、恢复和按配额删除 |

复杂测试默认要求在开始前恢复任务指定的快照。AI 可以先恢复用户基础快照，安装依赖、配置测试
网络并验证环境，然后通过 SatsumaHost 创建自己的派生快照。派生快照建议命名为：

```text
satsuma-ai-<purpose>-<timestamp>
```

Host 为每个 AI 快照记录 VM ID、父快照、创建任务、Agent 版本、环境说明和创建时间。AI 不能直接
调用任意 `vmrun snapshot` 字符串，也不能覆盖用户快照。Host 应设置每台 VM 的 AI 快照数量上限
和可选 TTL，避免无限增长。

任务可以声明：

```json
{
  "snapshot": {
    "restore_before": "satsuma-ai-network-ready-20260722",
    "after": "restore",
    "required": true
  }
}
```

`required=true` 时，恢复失败必须中止测试，不能在未知状态上继续执行。简单、无状态的测试可以明确
设置 `required=false`，但复杂或危险测试默认必须恢复。

### 8.3 快照应保存什么状态

推荐首版快照保存在 `SatsumaVM.exe` 已经以管理员权限运行的状态，这样恢复后 Agent 会继续重试
连接 Host。Agent 配置和共享目录映射必须已经验证，快照中不能包含未完成任务。

恢复运行态快照时，旧 RPC connection 可能已经失效。SatsumaVM 必须检测连接失败，生成新的
`session_id`，重新连接 Host，并根据 Host 的 run 状态决定是否接受任务。`job_id` 防止快照中的旧
内存状态重复执行已经完成的任务。

如果用户更偏好关机快照，则需要用计划任务、启动项或 `vmrun runProgramInGuest` 在每次开机后以
管理员权限启动 SatsumaVM。首版不要求注册 Windows Service。

创建 AI 派生快照前，SatsumaHost 必须确认没有运行中的 job，要求 Agent 刷新状态文件，并将共享
运行目录归档。快照不是替代 teardown，而是下一轮测试的已知起点。

### 8.4 `lab.json` 示例

```json
{
  "schema_version": 1,
  "lab_id": "local-windows-lab",
  "provider": {
    "type": "vmware_workstation",
    "vmrun": "C:\\Program Files\\VMware\\VMware Workstation\\vmrun.exe"
  },
  "host": {
    "listen": "192.168.250.1:37100",
    "archive_root": "D:\\Satsuma\\archive"
  },
  "shared_folder": {
    "host_root": "D:\\vm-share",
    "guest_root": "\\\\vmware-host\\Shared Folders\\vm-share"
  },
  "vms": [
    {
      "id": "client",
      "role": "client",
      "vmx": "D:\\VM\\Client\\Client.vmx",
      "agent_version": "0.1.0",
      "snapshots": {
        "base": "clean",
        "ai_prefix": "satsuma-ai-",
        "max_ai_snapshots": 8
      },
      "management_ip": "192.168.250.11"
    },
    {
      "id": "gateway",
      "role": "gateway",
      "vmx": "D:\\VM\\Gateway\\Gateway.vmx",
      "agent_version": "0.1.0",
      "snapshots": {
        "base": "clean",
        "ai_prefix": "satsuma-ai-",
        "max_ai_snapshots": 8
      },
      "management_ip": "192.168.250.12"
    }
  ]
}
```

密码和 token 不写入任务文件或 Git。首版可以使用环境变量引用，后续使用 Windows Credential
Manager。

## 9. 两种测试使用方式

两种方式不是二选一，而是同一套 API 的两个入口。

### 9.1 交互式逐步模式

适合 AI 调试未知问题：

```text
AI -> Host: 恢复 client 和 gateway 的任务指定快照
AI -> Host: 启动两台 VM
AI -> Host: 等待两个 SatsumaVM 在线
AI -> Host: 创建 run 目录并放入 target-client.exe
AI -> Host: 创建 run 目录并放入 target-gateway.exe
AI -> Host: 在 gateway 执行 target-gateway.exe
AI -> Host: 在 client 执行 target-client.exe
AI -> Host: 请求 client 执行 ping 或测试脚本
AI -> Host: 归档共享目录中的 log.txt 和 result.json
AI -> Host: 停止 job，收集状态，恢复快照
```

优点是每一步都可以观察和调整；缺点是 AI 需要维护当前 `run_id`、job 状态和清理顺序。Host
必须把每一步写入运行记录，不能依赖 AI 自己记忆。

### 9.2 JSON 任务模式

适合重复实验、回归测试和自动重试：

```json
{
  "schema_version": 1,
  "name": "two-vm-network-experiment",
  "snapshot": {
    "restore_before": "satsuma-ai-network-ready-20260722",
    "after": "restore",
    "required": true
  },
  "run_directory": "runs/20260722154611",
  "artifacts": [
    {
      "source": "D:\\Build\\target-client.exe",
      "sha256": "...",
      "vm": "client",
      "shared_destination": "artifacts/client/target-client.exe"
    },
    {
      "source": "D:\\Build\\target-gateway.exe",
      "sha256": "...",
      "vm": "gateway",
      "shared_destination": "artifacts/gateway/target-gateway.exe"
    }
  ],
  "steps": [
    {
      "id": "gateway",
      "vm": "gateway",
      "type": "execute",
      "program": "artifacts/gateway/target-gateway.exe",
      "arguments": [],
      "mode": "background",
      "timeout_seconds": 120
    },
    {
      "id": "client",
      "vm": "client",
      "type": "execute",
      "program": "artifacts/client/target-client.exe",
      "arguments": [],
      "timeout_seconds": 120,
      "collect_files": [
        "results/client/log.txt",
        "results/client/result.json"
      ]
    }
  ],
  "finally": {
    "stop_jobs": true,
    "collect_state": true,
    "restore_snapshot": true
  }
}
```

Host 自动模式必须保证 `finally` 始终执行。任务文件描述执行步骤，不描述 AI 对结果的最终解释。

## 10. 测试结果和 AI 生成报告

Satsuma 不要求被测程序理解 Satsuma 协议。最小结果由 Satsuma 自动生成：

```json
{
  "run_id": "...",
  "vm_id": "client",
  "job_id": "...",
  "status": "exited",
  "exit_code": 0,
  "timed_out": false,
  "duration_ms": 1234,
  "stdout": "stdout.log",
  "stderr": "stderr.log",
  "files": [
    {"path": "log.txt", "sha256": "..."},
    {"path": "result.json", "sha256": "..."}
  ]
}
```

AI 可以为自己的测试程序设计更详细的报告，例如：

```text
target.exe
  -> log.txt
  -> result.json
  -> packets.pcap
  -> counters.json
```

然后在任务中声明 `collect_files`。SatsumaVM 负责把文件放入共享结果目录，SatsumaHost 负责
命名、hash 校验并复制到共享目录之外的归档区；AI 负责读取这些文件并分析内容。

为了让不同项目更容易被 AI 使用，`SatsumaSkill.md` 应建议被测程序优先输出：

- 明确的退出码。
- UTF-8 文本日志。
- 一个机器可读的 JSON 总结。
- 每个子测试的名称、状态、实际值和错误信息。
- 不把关键结果只写到控制台或 GUI。

这是一条建议，不是 Satsuma 对项目源码的强制协议。

## 11. 运行记录和恢复

共享目录保存运行中的证据，Host 归档区保存完成或失败后的不可变副本：

```text
archive/<run_id>/
├── task.json
├── host-run.json
├── vmrun.log
├── client/
│   ├── agent-state.json
│   ├── execution.json
│   ├── stdout.log
│   ├── stderr.log
│   └── collected-files/
└── gateway/
    ├── agent-state.json
    ├── execution.json
    ├── stdout.log
    └── stderr.log
```

Host 维护以下状态：

```text
DEFINED -> RESETTING -> BOOTING -> AGENTS_READY -> DEPLOYING
        -> RUNNING -> COLLECTING -> FINISHED
        -> STOPPING -> RESTORING -> RECOVERED
```

任何失败都必须进入 `STOPPING` 和 `RESTORING`，除非 VMware 本身失去控制；这时报告状态为
`RECOVERY_FAILED`，不能打印“测试完成”。

Host 使用三组信号区分故障，而不是只看 RPC 超时：

| RPC 心跳 | 共享状态文件 | VMware 状态 | 判断 |
|---|---|---|---|
| 正常 | 更新 | powered on | Agent 正常 |
| 中断 | 更新 | powered on | 普通网络/RPC 故障，Agent 仍运行 |
| 中断 | 停止更新 | Guest Tools 可响应 | SatsumaVM 崩溃或卡住 |
| 中断 | 停止更新 | powered on 但 Tools 无响应 | Guest OS 可能卡死 |
| 中断 | 停止更新 | powered off | VM 已关机或崩溃 |

状态文件包含 Agent 心跳和当前 job 状态，Host 同时记录最后更新时间。基于以上判断，Host 按以下
顺序处理。该判断是基于多项证据的故障分类，不承诺在 Guest 完全卡死时知道最后一条指令执行到了
哪一行：

1. 等待有限时间，不无限阻塞。
2. 先归档共享目录中已经落盘的部分日志和状态。
3. 用 `vmrun` 查询 VM、VMware Tools 和 Guest 进程状态。
4. 根据任务超时策略终止 VM 内任务或直接关闭 VM。
5. 恢复 clean 快照。
6. 重新检查 VM 状态并记录恢复结果。

## 12. 安全和权限边界

测试工具允许 AI 让 VM 执行管理员操作，但仍要保护宿主机：

- SatsumaHost 不提供在 Host 上启动被测 exe 的命令；上层 AI 在 Satsuma 之外的行为不属于本工具职责。
- 被测程序只在隔离 VM 中运行。
- 默认使用自定义隔离 vmnet，不使用桥接。
- 只共享专用的 `vm-share`，不共享源码、个人目录、凭据、浏览器数据和 SSH key。
- Artifact 只能来自登记的路径，并在部署前校验 SHA-256。
- SatsumaVM 只接受配置中的 Host 地址和认证 token。
- 每个任务有独立 ID、工作目录、超时和进程组。
- Host 校验所有相对路径都留在共享根目录内，拒绝 `..`、绝对路径和越界重解析点。
- Host 及时把结果复制到 Guest 不可见的归档目录，防止后续任务篡改证据。
- `vmrun` 命令由 Host 生成，不接受 AI 直接传入 shell 字符串。

Satsuma 的目标是保护宿主机并恢复实验环境，不是把 AI 生成的程序变成可信程序。共享目录使 Guest
能够修改 `vm-share` 内的文件，因此该目录只能存放可丢弃的 Artifact 和测试结果。对于真正不可信
的样本，应使用专门实验宿主机，并改用更严格的单向传输或 `vmrun` 取证模式。

## 13. C++/CMake 项目结构

```text
satsuma/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── src/
│   ├── core/                # SatsumaCore 静态库：JSON、协议、日志、hash
│   ├── host/                # Controller、coro_rpc server、任务执行器
│   ├── vm/                  # 管理员控制台、coro_rpc client、Job Object
│   ├── vmware/              # vmrun Provider
│   └── cli/                 # SatsumaHost.exe 命令行入口
├── tests/
│   ├── protocol_tests.cpp
│   ├── task_state_tests.cpp
│   ├── fake_host_vm_tests.cpp
│   └── vmware_provider_tests.cpp
├── schemas/
│   ├── lab.schema.json
│   ├── task.schema.json
│   └── result.schema.json
├── docs/
│   ├── SatsumaReadme.md
│   └── SatsumaSkill.md
└── examples/
    └── hello-vm-task.json
```

目标：

```text
SatsumaHost.exe
SatsumaVM.exe
```

CMake 目标关系：

```text
SatsumaCore (STATIC)
  ├── SatsumaHost.exe
  └── SatsumaVM.exe
```

建议使用 C++20、CMake 3.25+、Job Object、yalantinglibs `coro_rpc`/`struct_pack`、
nlohmann/json、spdlog 和 GoogleTest。`SatsumaCore` 静态链接进两个 exe，不作为第三个运行时文件
交付。首版不需要数据库、FTP、Web UI，也不需要将 AI SDK 嵌入二进制。

## 14. `SatsumaReadme.md` 和 `SatsumaSkill.md`

### 14.1 面向用户的 Readme

`SatsumaReadme.md` 只说明：

- VMware Workstation 版本和 `vmrun.exe` 路径。
- 如何创建管理网和隔离实验网。
- 如何以管理员权限启动 SatsumaVM，以及如何创建可选的最高权限计划任务。
- 如何配置 Host/Guest 共享目录路径。
- 如何保存用户基础快照、创建 AI 派生快照并设置配额。
- 如何填写 `lab.json`。
- 如何启动 Host、检查 Agent、运行示例任务。
- 如何处理 `RECOVERY_FAILED`。

### 14.2 面向 AI 的 Skill

`SatsumaSkill.md` 应尽量短，建议固定包含：

```text
1. 先读取 lab.json 和当前任务的 schema。
2. 只在任务中使用共享根目录下的相对路径。
3. 部署前记录 Artifact hash，读取 lab.json 中的 Host/Guest 路径映射。
4. 优先使用交互模式诊断，稳定后使用任务模式复现。
5. 每个运行都必须有 run_id。
6. 测试程序应输出明确退出码和机器可读结果文件。
7. 失败后先读取 summary.json 和原始日志，再修改目标项目。
8. 复杂测试前恢复指定快照，不覆盖或删除用户基础快照。
9. 只把 Satsuma 的通信、执行和恢复结果当作平台结论；业务正确性由 AI 分析。
10. 出现 RECOVERY_FAILED 时停止自动修改，要求用户处理 VM 环境。
```

AI 可以选择逐步命令或生成 JSON 任务，但两者都必须通过 `SatsumaHost.exe`，不能直接操作 VM
文件、`vmrun` 或宿主机网络。

## 15. MVP 实现顺序

### Phase 0: 手工环境

- 用户创建两台 Windows VM。
- 安装 VMware Tools，以管理员权限启动 SatsumaVM。
- 配置专用共享目录并写入 `lab.json`。
- 配置管理网络。
- 保存用户基础快照。
- 手工验证 Agent 能连接 Host。

### Phase 1: 最小双端通道

- 实现 `SatsumaHost.exe serve`。
- 使用固定版本的 `coro_rpc` 实现连接、心跳、轮询和重连。
- 实现一个无害 `echo` 任务。
- 实现共享目录任务清单、执行、stdout/stderr 刷盘和 Host 归档。
- 实现 `run_id`、request_id 和 hash 校验。

验收：两台 VM 重复执行 20 次 hello 任务，结果一致，Host 能识别任意一台 VM 断线。

### Phase 2: VMware 带外能力

- 封装 `vmrun list/start/stop/revertToSnapshot`。
- 实现 Host 启动、停止和恢复快照。
- 实现用户基础快照保护和 AI 派生快照的创建、配额与元数据。
- Guest Agent 断网时仍能恢复 VM。
- 失败任务强制关闭完整进程树。

验收：在 VM 禁用管理网卡后，Host 仍能停止、恢复快照并完成下一轮任务。

### Phase 3: 两种使用模式

- 完成交互式 CLI。
- 完成 JSON 任务解析和 finally 处理。
- 统一运行记录和报告目录。
- 提供 `SatsumaReadme.md`、`SatsumaSkill.md` 和 schema。

验收：同一个实验可以先逐步调试，再由 JSON 任务无交互重复执行。

### Phase 4: 被测项目接入

- 不修改 Satsuma 核心，新增一个外部 suite 或 task 文件。
- 由目标项目 AI 自己提供 exe、参数和报告文件。
- 用两台 VM 验证复杂或危险测试。

验收：至少一个不属于 Satsuma 的系统程序能够通过同一套 Host/VM 流程完成部署、执行、报告和
快照恢复。

## 16. 方案判断

你的两种使用设想都成立，最佳实现不是二选一：

- 交互式模式是底层原语，适合 AI 调试和发现未知问题。
- JSON 任务模式是交互式 API 的可复现封装，适合回归和自动循环。

通信方面，共享目录负责 Artifact、任务清单和结果证据；`coro_rpc` 负责即时控制和心跳；`vmrun`
负责 VM 生命周期和最终恢复。FTP 不值得引入。

最终的 Satsuma 产品应该保持很小：

```text
两个 exe
一份用户 Readme
一份 AI Skill
一套配置 schema
一套可恢复的 VM 环境
```

它不需要知道目标项目是什么，也不需要替 AI 解释测试结果。只要“部署、执行、取回、恢复”这条
链路可靠，任何需要隔离系统环境的 AI 编程项目都可以把 Satsuma 作为上层工具使用。

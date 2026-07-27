# Satsuma TestLab

Satsuma 在 Windows 宿主机与 VMware 测试虚拟机之间物化任务、部署 Artifact、执行程序并保存证据。
它只报告通信、执行和恢复状态，不判断被测程序的业务结果是否正确。

## 当前已实现增量

版本 `0.1.0` 已提供：

- `SatsumaHost.exe run`：校验任务和 VM ID，计算 Artifact SHA-256，并原子发布运行目录。
- `SatsumaVM.exe --watch`：仅通过共享文件轮询任务、独占领取步骤，并将 Artifact 复制到 VM 本地目录。
- Windows Job Object 进程树管理、超时终止、stdout/stderr 持续落盘和结果文件收集。
- `SatsumaHost.exe report`：汇总当前运行的机器可读结果。
- `SatsumaHost.exe orchestrate`：按单 VM 生命周期策略恢复、启动、检测、执行、归档并清理。
- claim schema v3 主动续租：只自动回收显式 `retry_safe` 的过期步骤，其他步骤保留人工门禁。
- `SatsumaHost.exe check`：检查 Host/VMware 环境，发送无害任务并确认 Agent 执行通道。
- `SatsumaVM` Windows Service：LocalSystem 延迟自动启动、受控停止和失败恢复。
- 独立 Agent 自更新：候选校验、本机切换、简单回滚、presence 确认和成功后完整暂存清理。
- 文件协议 v2 多身份执行：`system` 与活动控制台 `interactive_user`，结果记录请求身份和 Session。
- `vmrun` 运行状态、启动、软/硬关闭、快照恢复、列表、创建和删除封装。
- Host VM 生命周期命令，以及带前缀、配额、所有权保护和元数据的 AI 快照命令。
- 路径越界、绝对任务路径和重解析点校验。

JSON 任务可以通过 `lifecycle` 声明单 VM 的执行前恢复、成功/失败清理策略和 `finally` 步骤。
`orchestrate` 原子保存每次阶段迁移，并将主任务和 `finally` 证据复制到 Guest 不可见的归档目录；
恢复失败以退出码 4 和 `RECOVERY_FAILED` 单独报告。普通 `run` 会拒绝生命周期计划，避免静默忽略策略。
生命周期计划必须显式提供唯一 `run_id`；Host 使用同一 `run_id` 和字节完全相同的计划重启时，可继续
等待已发布主任务或完成证据归档。其他非终态阶段会进入人工门禁，避免重复执行不确定的 VM 副作用。
当前尚不支持多 VM 生命周期。

## 构建

要求 Windows、CMake 3.25+ 和带“使用 C++ 的桌面开发”组件的 Visual Studio。真实崩溃恢复目标另外
要求 PowerShell 7。CMake 会优先使用包管理器提供的 `nlohmann-json`，未找到时下载固定的
`v3.12.0` tag。

```text
cmake --preset windows-default
cmake --build --preset windows-debug
ctest --preset windows-debug
```

生成文件位于 `build/windows-default/bin/Debug/`。运行时只需部署 `SatsumaHost.exe` 或
`SatsumaVM.exe`，`SatsumaCore` 是静态库。MSVC 构建使用静态 Runtime（Release `/MT`、Debug `/MTd`），
Guest 不需要安装与构建机工具集完全匹配的 Visual C++ Redistributable。

### 真实 VMware 手工验收

真实生命周期验收目标默认不存在，避免普通构建或 `ctest` 意外重置 VM。用户准备好 VMware 环境后，
使用专用测试 VM 和可丢弃状态显式配置：

```text
cmake --preset windows-default ^
  -DSATSUMA_ENABLE_REAL_VMWARE_SMOKE=ON ^
  -DSATSUMA_REAL_LAB_CONFIG=D:/Satsuma/lab.json ^
  -DSATSUMA_REAL_VM_ID=client ^
  -DSATSUMA_REAL_SNAPSHOT=clean ^
  -DSATSUMA_REAL_VMWARE_CONFIRM=I_UNDERSTAND_VM_WILL_BE_RESET
cmake --build --preset windows-debug --target SatsumaRealVmwareSmoke
```

该目标会依次查询快照、硬关闭 VM、恢复指定快照并重新启动。任一步失败都会立即停止，且不会由普通
Debug/Release 构建或测试自动触发。

### 真实 Host/Agent 崩溃恢复验收

崩溃恢复目标同样默认不存在，也不注册为普通 `ctest`。它要求 PowerShell 7、已经运行且通过 `check` 的
专用 VM，以及明确的强杀确认串。驱动只终止自己启动的 Host PID；Guest 内 Fixture 会在父进程路径二次
确认是 `SatsumaVM.exe`、一次性标记写穿后终止该父 Agent，由 SCM 按现有失败策略重启 Service。

```text
cmake --preset windows-default ^
  -DSATSUMA_ENABLE_REAL_VMWARE_CRASH_RECOVERY=ON ^
  -DSATSUMA_REAL_LAB_CONFIG=E:/Work/satsuma-testlab/lab.local.json ^
  -DSATSUMA_REAL_VM_ID=client ^
  -DSATSUMA_REAL_CRASH_SCENARIO=All ^
  -DSATSUMA_REAL_CRASH_TIMEOUT_SECONDS=480 ^
  -DSATSUMA_REAL_CRASH_CONFIRM=I_UNDERSTAND_HOST_AND_AGENT_WILL_BE_FORCE_TERMINATED
cmake --build --preset windows-release --target SatsumaRealVmwareCrashRecovery
```

`All` 依次覆盖 Host 在 `executing` 强杀后续跑、Agent 崩溃后的安全 attempt 2，以及不可安全重试步骤的
人工门禁；也可单独选择 `HostCrash`、`AgentRetrySafe` 或 `AgentUnsafe`。每轮先后执行 `check`，所有等待
都有截止时间。机器可读摘要和 Host stdout/stderr 保存到
`archive_root/validation/real-crash-*/summary.json`，失败运行、claim、partial 日志和归档不会自动删除。
该目标使用 `leave_running`，不会恢复或删除快照；测试前必须保证没有另一台 Agent 使用相同 `vm_id`。

### 真实 Shared Folder 瞬断恢复验收

故障恢复目标默认不存在，也不注册为普通 `ctest`。它要求正在运行并通过 `check` 的专用 VM、PowerShell
7 和独立确认串。驱动发布一个 90 秒 retry-safe Fixture，观察 schema v3 claim 和首次续租后，通过
`vmrun disableSharedFolders <vmx> runtime` 关闭 35 秒，再在 `finally` 中执行对应 enable。它不修改 VM
电源、快照、网络设备或防火墙。

```text
cmake --preset windows-default ^
  -DSATSUMA_ENABLE_REAL_VMWARE_FAULT_RECOVERY=ON ^
  -DSATSUMA_REAL_LAB_CONFIG=E:/work/satsuma-testlab/lab.local.json ^
  -DSATSUMA_REAL_VM_ID=client ^
  -DSATSUMA_REAL_SHARED_OUTAGE_SECONDS=35 ^
  -DSATSUMA_REAL_FAULT_CONFIRM=I_UNDERSTAND_SHARED_FOLDER_WILL_BE_TEMPORARILY_DISABLED
cmake --build --preset windows-release --target SatsumaRealVmwareFaultRecovery
```

通过条件包括：outage 内 presence 与续租文件停止变化；恢复后同一 job、boot 和 `attempt 1` 继续续租；
只发布一份成功规范结果且没有归档旧 claim；最终 `check` 返回 `ready`。摘要和 Host stdout/stderr 保存到
`archive_root/validation/real-fault-*/summary.json`。失败证据不会自动删除；enable 失败时驱动立即停止并
报告，不能循环重试或继续其他故障注入。

## 环境准备

1. 安装 VMware Workstation 和 VMware Tools。
2. 普通测试可直接使用 NAT 和 DHCP；危险网络测试再配置独立的 Host-only/LAN Segment。
3. 只共享专用的 `vm-share`，不要共享源码根目录、个人目录或凭据。
4. 复制根目录模板为 `lab.local.json`，再填写本机路径。
5. 为每台 VM 复制并填写一份 `agent.json`，然后运行 `install-agent.ps1` 并确认 UAC。
6. 清理 VM 中的遗留进程和网络状态，保存名为 `clean` 的用户基础快照。

以下路径和 IP 只是示例。仓库跟踪 `lab.json` 模板，本机真实值写入已忽略的 `lab.local.json`，不要把
VMX、快照、盘符或当前 DHCP 地址提交到仓库。

### 1. 准备 Host 目录和程序

建议为共享数据、归档数据和虚拟机文件使用三个互不包含的目录：

| 用途 | 示例 | 是否允许 Guest 访问 |
|---|---|---|
| Satsuma 仓库 | `E:\work\satsuma-testlab` | 否 |
| 共享任务目录 | `D:\vm-share` | 是，只共享此目录 |
| Host 归档目录 | `D:\Satsuma\archive` | 否 |
| VMX 目录 | `D:\VM\Client\Client.vmx` | 否 |

可以在 Host PowerShell 中创建尚不存在的数据目录：

```powershell
New-Item -ItemType Directory -Force -Path 'D:\vm-share'
New-Item -ItemType Directory -Force -Path 'D:\Satsuma\archive'
```

构建 Release 版本并确认两个程序存在：

```powershell
cmake --preset windows-default
cmake --build --preset windows-release
Get-Item 'build/windows-default/bin/Release/SatsumaHost.exe'
Get-Item 'build/windows-default/bin/Release/SatsumaVM.exe'
```

### 2. 配置 VMware 网络

最简环境可以直接使用 VMware 默认 NAT 和 DHCP。虚拟机名称、Windows 计算机名、网卡名称和固定
Guest IP 都不是文件任务通道的运行条件；Host 不依赖 Guest IP 反向连接。

只有被测程序会修改路由、DNS、防火墙或虚拟适配器时，才建议使用两张隔离网卡：

| 网卡 | 推荐模式 | 用途 |
|---|---|---|
| 管理网卡 | Host-only 自定义 VMnet | 可选 RPC 诊断、环境诊断 |
| 实验网卡 | 另一条 Host-only/LAN Segment | 被测程序通信，可由测试破坏 |

不要把危险测试使用的实验网卡桥接到办公网、家庭主网或生产网络。`host.listen` 仍需填写 Guest 可达的
Host VMware 虚拟网卡地址和端口，供显式 RPC 诊断使用；Guest 保持 DHCP 即可。

在 Host 上用只读命令确认地址确实绑定在本机网卡上：

```powershell
Get-NetIPAddress -AddressFamily IPv4 | Select-Object InterfaceAlias, IPAddress
```

`--watch` 文件循环和生产 Windows Service 不启动、调用或等待 RPC，不需要开放 TCP 37100。只有显式运行
`SatsumaHost serve` 与 `SatsumaVM --rpc-once` 做诊断时才需要对应端口；防火墙修改需要管理员权限，
应由用户确认作用域后手工完成，且不得把端口开放到公共网络。

### 3. 配置 VMware Shared Folder

对每台 VM 打开 VMware 设置，启用 Shared Folders，并添加同一个 Host 目录：

```text
Host path:  D:\vm-share
Share name: vm-share
Guest path: \\vmware-host\Shared Folders\vm-share
```

选择始终启用，并确认 Guest 已安装、运行 VMware Tools。在 Host 创建探针文件：

```powershell
Set-Content -LiteralPath 'D:\vm-share\host-probe.txt' -Value 'host-ok' -Encoding UTF8
```

随后在每台 VM 的 PowerShell 中确认能读取，并写入该 VM 自己的探针：

```powershell
Get-Content -LiteralPath '\\vmware-host\Shared Folders\vm-share\host-probe.txt' -Encoding UTF8
Set-Content -LiteralPath '\\vmware-host\Shared Folders\vm-share\client-probe.txt' `
    -Value 'client-ok' -Encoding UTF8
```

最后回到 Host 确认 `client-probe.txt` 可读。Gateway VM 使用不同文件名，避免互相覆盖。共享目录
只能放可丢弃的任务、Artifact 和结果，不能共享仓库、用户目录、密钥或浏览器数据。

VMware Shared Folder 可能在 Guest 进程退出后短暂保留文件句柄或 lease，使 Host 的覆盖、改名或删除
暂时返回占用或拒绝访问。每次任务必须使用新的 `run_id` 目录，协议状态使用不可变文件发布；清理只做
有限重试，失败时保留隔离目录供后续回收，不能无限重试或阻塞后续运行。

### 4. 填写 Host 的 `lab.local.json`

Host 固定读取用户传给 `--config` 的 JSON。先复制仓库模板，保留模板本身不变：

```powershell
Copy-Item -LiteralPath 'lab.json' -Destination 'lab.local.json'
```

只修改 `lab.local.json` 中的实际值；格式约束见
[lab.schema.json](../schemas/lab.schema.json)。

| 字段 | 填写规则 |
|---|---|
| `schema_version` | 固定为 `1` |
| `lab_id` | 本实验室稳定 ID，只能使用字母、数字、下划线和连字符 |
| `provider.type` | 固定为 `vmware_workstation` |
| `provider.vmrun` | Host 上 `vmrun.exe` 的绝对路径 |
| `host.listen` | Guest 可达的 Host VMware 网卡地址和端口 |
| `host.archive_root` | Guest 不可见、Host 可写的绝对目录 |
| `shared_folder.host_root` | VMware 实际共享的 Host 绝对目录 |
| `shared_folder.guest_root` | 同一共享在 Guest 中的 UNC 根路径 |
| `vms[].id` | 任务和 Agent 使用的稳定 VM ID，每台唯一 |
| `vms[].role` | 给用户和 AI 阅读的角色名 |
| `vms[].vmx` | 对应 `.vmx` 文件的 Host 绝对路径 |
| `vms[].agent_version` | 基础快照内 Agent 版本，当前填 `0.1.0` |
| `vms[].snapshots.base` | 用户创建且不允许 AI 删除的基础快照名 |
| `vms[].snapshots.ai_prefix` | AI 派生快照前缀，如 `satsuma-ai-` |
| `vms[].snapshots.max_ai_snapshots` | 单台 VM 可保留的 AI 快照数量，范围 1–64 |
| `vms[].management_ip` | 可选元数据；使用 DHCP 时可以省略 |

JSON 内 Windows 路径的反斜杠必须写成 `\\`。`vmrun`、VMX、共享目录和归档目录都必须使用
当前电脑上的真实路径；不要把密码、Token 或其他凭据写入配置文件。

### 5. 部署并填写每台 VM 的 `agent.json`

以 [agent-client.json](../examples/agent-client.json) 为模板生成当前 VM 专用的 `agent.json`；格式约束见
[agent.schema.json](../schemas/agent.schema.json)。在 Host 共享目录中准备部署文件：

```powershell
New-Item -ItemType Directory -Force -Path 'D:\vm-share\satsuma-bootstrap'
Copy-Item 'build/windows-default/bin/Release/SatsumaVM.exe' 'D:\vm-share\satsuma-bootstrap/'
Copy-Item 'scripts/install-agent.ps1' 'D:\vm-share\satsuma-bootstrap/'
Copy-Item 'agent.json' 'D:\vm-share\satsuma-bootstrap/'
```

| 字段 | 填写规则 |
|---|---|
| `schema_version` | 固定为 `1` |
| `protocol_version` | 固定为 `2`；v1 只用于旧配置自更新迁移，不允许启动当前 Agent |
| `lab_id` | 必须与 Host `lab.json` 完全一致 |
| `vm_id` | 必须等于 Host `vms[].id` 中当前 VM 的 ID |
| `agent_version` | 必须与该 VM 的 `vms[].agent_version` 一致 |
| `host` | 必须与 `lab.json` 的 `host.listen` 一致 |
| `shared_root` | 当前 VM 实测可读写的 Shared Folder UNC 路径 |
| `local_work_root` | VM 内本地执行目录，不要设为共享目录 |
| `poll_interval_ms` | 文件任务轮询间隔，范围 100–60000 |
| `reconnect_interval_ms` | Shared Folder 不可用后的重试间隔，范围 100–60000 |
| `rpc_timeout_ms` | RPC 诊断超时，范围 100–300000；Service 文件主线不使用 |

多台 VM 必须分别保存自己的配置，至少修改 `vm_id`；单台任意 VM 可以继续使用逻辑 ID `client`，
它不要求 VMware 显示名称也叫 Client。在 Guest 的任意 PowerShell 中执行一行安装命令：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File '\\vmware-host\Shared Folders\vm-share\satsuma-bootstrap\install-agent.ps1'
```

共享目录中的候选二进制如果使用唯一文件名，不要覆盖旧文件；通过参数明确选择本次候选：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
    -File '\\vmware-host\Shared Folders\vm-share\satsuma-bootstrap\install-agent.ps1' `
    -AgentFileName 'SatsumaVM-service-<timestamp>.exe'
```

脚本在需要时自动显示一次 UAC 提示，然后在本地固定磁盘的 `C:\Satsuma` 中创建 `bin` 和 `work`。
它先把共享目录候选复制为固定的 `SatsumaVM.new.exe`，核对 SHA-256 并执行配置预检，再有限等待当前
Service 停止、停止并注销旧计划任务，最后切换文件并注册 Service。安装使用一个管理员全局 Mutex 防止
两个脚本同时切换文件；失败时只尝试恢复本次 `.bak` 文件和旧 Service，复杂故障直接恢复 Guest 快照。

安装成功后，`SatsumaVM.exe --install-service` 创建或更新服务名 `SatsumaVM`、显示名 `SatsumaVM Agent`
的 Windows Service。Service 使用 LocalSystem、延迟自动启动和 5/15/60 秒失败重启策略；SCM 命令固定为
`"C:\Satsuma\bin\SatsumaVM.exe" --config C:\Satsuma\agent.json --service`。安装只等待 SCM 进入
`RUNNING` 并返回有效 PID，不把共享目录 presence 当成 SCM 注册事务。`--remove-service` 会有限等待停止，
删除后等待 SCM 对象消失；Service 不存在时幂等返回。安装成功后脚本删除 `.bak`、`.new` 和配置暂存文件，
PowerShell 可以关闭，不需要保留前台窗口。

`--watch` 是不注册系统启动项的纯文件前台模式，适合诊断；`--service` 只应由 SCM 启动；`--rpc-once`
仅保留为非生产 RPC 诊断入口。`--once`、`--watch`、`--rpc-once` 和 `--validate-config` 都不会修改 Service。
Agent 就绪后会在共享目录发布 `agents/<vm-id>.json`。被测 Artifact 始终复制到 Guest 本地工作目录执行，
不直接从 UNC 路径运行。

#### 任务运行身份

任务计划中的 `execute.run_as` 可省略；Host 会将省略值解释为 `system`，并在发布的文件协议 v2
`task.json` 中显式写出身份。可用值为：

| 值 | 生产行为 |
|---|---|
| `system` | 由 LocalSystem Service 在 `agent.json.local_work_root/<run-id>` 中执行 |
| `interactive_user` | 在当前活动控制台用户 Token 下执行，工作目录为该用户 `%LOCALAPPDATA%\SatsumaTestLab\<lab-id>\runs\<run-id>` |

`echo` 不接受 `run_as`。`interactive_user` 只允许生产 LocalSystem Service 通过 `WTSQueryUserToken` 和
`CreateProcessAsUserW` 启动；无活动控制台 Session 时，该步骤以
`No active interactive user session is available` 失败，Agent 继续处理后续步骤，绝不回落为 SYSTEM。
启动前和恢复挂起 helper 前都会核对 Session、用户 SID 和登录 `AuthenticationId`。Artifact 在用户
impersonation 下部署，helper、目标及其子进程仍受同一个 Job Object、超时和 Service stop/cancel 约束。

`execution.json` 始终记录 `run_as`；交互身份成功取得后还记录 `interactive_session_id`。前台
`--once`/`--watch` 只用于诊断，不提供生产 SYSTEM 身份保证，非 LocalSystem 进程会拒绝交互用户步骤。

#### Agent 自更新

Host 通过独立更新通道发布候选，不把自更新伪装成普通 `execute` 步骤：

```powershell
SatsumaHost.exe agent update `
    --config lab.local.json `
    --vm client `
    --binary E:\release\SatsumaVM.exe `
    --version 0.1.0 `
    --timeout-seconds 180
```

Host 在 `updates/<vm-id>/<update-id>` 的隐藏暂存目录中复制候选，计算大小和 SHA-256、写入不可变
`update.json`，再整体改名发布。Agent 校验清单后复制为本机 `C:\Satsuma\bin\SatsumaVM.new.exe`，由该
候选以独立、无 Job Object 的 `--apply-update` 模式有限停止 Service、切换文件并等待新 PID 发布匹配
版本和 update ID 的 presence。旧 v1 `agent.json` 会在成功切换时原子升级到文件协议 v2；失败回滚会
恢复旧配置。

成功结果写回前，助手必须删除 `SatsumaVM.bak.exe`、确认 `SatsumaVM.new.exe` 不存在，并删除本机配置
备份、manifest 和状态文件；Host 读取成功结果后删除整个共享更新目录。哈希、停服、改名、启动或
presence 任一步失败都会写回明确结果；文件已经切换时先恢复旧 EXE、旧配置和旧 presence。只有自动恢复
也失败时才保留备份和状态证据，复杂恢复使用 Guest 快照。该流程已通过本机文件切换和注入式
Service/presence 测试，并已在真实 LocalSystem Service 中完成 Host 驱动更新、presence 与暂存清理验收。

### 6. 创建用户基础快照

在创建 `clean` 快照前，确认以下条件：

- VMware Tools、共享目录、管理网和 Agent 配置均已验证。
- VM 内没有正在执行的 Satsuma 任务或被测程序。
- 临时 Artifact、路由、DNS、虚拟适配器和测试进程已经清理。
- `clean` 不以 `satsuma-ai-` 开头，并与 `lab.json` 中的基础快照名完全一致。

推荐在安装脚本验收成功后关闭 VM，再创建关机快照。恢复快照并启动 VM 后，SCM 会为 Windows Service
创建全新的 Agent 进程，不会复用旧进程内存。开机时 Shared Folder 暂时未就绪不会导致 Agent 退出，
Service 会继续轮询文件通道。

用户基础快照属于只读基线。AI 只能通过 `SatsumaHost snapshot create-ai/delete-ai` 管理带配置
前缀的派生快照，不能覆盖或删除 `clean`。

### 7. 首次启动和验收顺序

1. 在 Host 启动目标 VM：

   ```powershell
   & 'build/windows-default/bin/Release/SatsumaHost.exe' vm start `
       --id client --config 'lab.local.json'
   ```

2. 等待 Windows Service 自动启动 `SatsumaVM.exe --service` 并持续发布 presence。延迟自动启动在当前
   Windows 11 Guest 冷启动中实测约需 120–140 秒。
3. 在 Host 终端执行主动检测：

   ```powershell
   & 'build/windows-default/bin/Release/SatsumaHost.exe' check `
       --config 'lab.local.json' --vm client --timeout-seconds 180
   $LASTEXITCODE
   ```

4. 只有退出码为 0 且 JSON 顶层 `status` 为 `ready`，才运行真实测试。两台 VM 都准备完成后，省略
   `--vm` 可以一次检测 `lab.json` 中的全部 VM。

5. 运行无害示例并读取 Host 返回的 `run_id`：

   ```powershell
   & 'build/windows-default/bin/Release/SatsumaHost.exe' run `
       --config 'lab.local.json' --plan 'examples/hello-vm-task.json'
   ```

6. Agent 完成任务后查询报告：

   ```powershell
   & 'build/windows-default/bin/Release/SatsumaHost.exe' report `
       --config 'lab.local.json' --run '<run-id>'
   ```

### 8. 运行独立示例软件

`SatsumaDemoApp.exe` 不链接 `SatsumaCore`，用于模拟普通软件读取输入、处理数据、输出日志并生成结果。
CMake 会为每个构建配置生成包含真实 Artifact 绝对路径的任务文件：

```powershell
cmake --build --preset windows-release --target SatsumaDemoApp
& 'build/windows-default/bin/Release/SatsumaHost.exe' run `
    --config 'lab.local.json' `
    --plan 'build/windows-default/examples/demo-app-task-Release.json'
```

任务会收集 `output/demo-result.json` 和 `output/transformed.txt`。使用 Host 返回的 `run_id` 查询报告，
并核对退出码、stdout、stderr、结果内容和 SHA-256 后，才算通过真实 Artifact 执行验收。

## AI 主动检测模式

正式发布测试任务前，AI 应主动检查目标 VM 的完整自动化通道：

```text
SatsumaHost.exe check --config lab.local.json --vm client --timeout-seconds 180
```

`check` 会验证共享目录和归档目录的容量与原子写入、`vmrun` 控制通道、VMX、基础快照和 VMware Tools
状态，然后发布唯一的 `echo` 任务并等待 Agent 返回结果。它不会启动、关闭、恢复虚拟机或改动快照。
未指定 `--vm` 时会检查配置中的全部 VM；`--timeout-seconds` 接受 1–300 秒，默认 30 秒。Agent 已上线时
默认值通常足够；VM 冷启动后首次检查建议使用 180–240 秒，以覆盖 Windows 延迟自动启动和 VMware
Tools 就绪时间。

报告为机器可读 JSON，调用方必须同时检查退出码和顶层 `status`：

- 退出码 0、`ready`：环境检查和全部 Agent 探针均通过，可以继续。
- 退出码 3、`degraded`：Agent 探针通过，但 VMware 或环境检查存在异常，应停止并处理报告中的失败项。
- 退出码 1、`failed`：Agent 超时、执行失败或结果不匹配，自动化通道当前不可用。

Shared Folder 检查失败时无法安全发布 echo。此时 `check` 仍返回完整 JSON，`run_id` 为 `null`，目标
`agents[]` 标记为 `skipped`，不会退化成只有 stderr 的异常。结果读取期间的瞬时文件错误会重试到本次
截止时间；持续失败会在对应 Agent 项中保留最后一次读取错误。

`check` 不会替用户启动 VM 或 Agent。常见失败项可按下表定位：

| 报告位置 | 常见原因 | 用户处理 |
|---|---|---|
| `checks/shared_folder` | Host 共享目录不存在或不可写 | 核对 `host_root`、目录权限和磁盘状态 |
| `checks/archive` | Host 归档目录不存在或不可写 | 创建目录并核对 `archive_root` |
| `checks/vmrun` | `vmrun.exe` 路径错误 | 核对 VMware 安装目录和 `provider.vmrun` |
| `checks/vmware_control` | VMware 控制命令失败或超时 | 启动 VMware 服务，手工确认 `vmrun list` 可用 |
| `checks/vmx` | VMX 路径错误或文件已移动 | 在 VMware 中找到实际 `.vmx` 并更新配置 |
| `checks/snapshots` | 基础快照不存在 | 手工创建快照或修正 `snapshots.base` |
| `checks/vmware_tools` | VM 未运行、Tools 未启动或 Guest 响应异常 | 启动 VM 并确认 `vmrun checkToolsState` 返回 `running` |
| `agents[].status=skipped` | Shared Folder 检查失败，未发布探针 | 先修复共享目录，再重新执行完整 `check` |
| `agents[].status=timeout` | VM 未启动、Agent 未运行或 Guest 看不到共享目录 | 启动 VM/Agent，重新验证 Shared Folder |
| `agents[].status=failed` | Agent 结果、退出码或 stdout 不符合探针 | 查看该 `run_id` 下的 `execution.json` 和日志 |

## 运行文件通道示例

带生命周期策略的单 VM 计划使用独立编排入口：

```text
SatsumaHost.exe orchestrate --config lab.local.json --plan task.json --timeout-seconds 300
```

`lifecycle.vms` 当前必须且只能包含一台 VM。每台策略必须同时定义 `on_success` 和 `on_failure`，动作可为
`leave_running`、`stop` 或带 `snapshot` 的 `restore`；可选的 `restore_before` 在启动 VM 前恢复快照，
`lifecycle.finally` 中的步骤在主任务完成或失败后单独发布。自动恢复只接受配置中的用户基础快照或
AI 前缀快照。计划顶层必须显式填写唯一 `run_id`，并在恢复时保持计划原始字节不变。状态保存在
`archive_root/runs/<run-id>/lifecycle.json`，证据保存在同目录的 `evidence/`。

`echo` 默认 `retry_safe=true`，`execute` 默认 `false`；只有调用方确认步骤可幂等重放时，才应为
`execute` 显式设置 `retry_safe=true`。Agent 使用 120 秒固定 claim 租约，并每 30 秒发布一份带连续序号的
不可变续租 sidecar；读取方会校验 owner、序号和时间单调性。租约到期后，只有显式声明可安全重放的
步骤才允许递增 attempt；`boot_id` 用于取证，不是接管前置条件，旧 owner 仍受结果 fencing 限制。旧格式、
损坏或不可安全重试的 claim 会生成 `claim-recovery.json`，`orchestrate` 随即跳过 `finally` 和恢复动作，
以退出码 5、`MANUAL_INTERVENTION_REQUIRED` 停止。

在 Host 上物化任务：

```text
SatsumaHost.exe run --config lab.local.json --plan examples/hello-vm-task.json
```

Windows Service 正常运行时无需手工启动 Agent。仅在前台诊断时，可领取一次任务或持续轮询：

```text
SatsumaVM.exe --config agent-client.json --once
SatsumaVM.exe --config agent-client.json --watch
```

Host 根据 `run` 输出的 `run_id` 查看报告：

```text
SatsumaHost.exe report --config lab.local.json --run <run-id>
SatsumaHost.exe report --config lab.local.json --run <run-id> --wait-seconds 300
```

有限等待在完成时返回 0，等待超时返回 3，人工门禁返回 5；不带 `--wait-seconds` 时仍为即时只读汇总。

Host 使用本机配置执行 VM 和快照操作：

```text
SatsumaHost.exe vm start --id client --config lab.local.json
SatsumaHost.exe vm stop --id client --mode hard --config lab.local.json
SatsumaHost.exe vm restore --id client --snapshot clean --config lab.local.json
SatsumaHost.exe snapshot list --vm client --config lab.local.json
SatsumaHost.exe snapshot create-ai --vm client --name network-ready --config lab.local.json
SatsumaHost.exe snapshot delete-ai --vm client --snapshot <snapshot-name> --config lab.local.json
```

基础快照是只读所有权，`delete-ai` 只接受配置中 AI 前缀开头且确实存在的快照。创建和删除记录保存在
`host.archive_root/snapshots/<vm-id>/`。

每次运行使用 `shared_folder.host_root/runs/<run_id>/`，不要复用旧的 `run_id`。`execution.json`、
最终日志和收集文件发布前均经过完整写入或原子改名；执行期间可读取 `.partial` 日志。不要在 Guest
刚执行过文件后立即依赖 Host 覆盖或删除同一路径，新的任务和 Artifact 应使用新的运行目录或唯一文件名。

## 权限与故障边界

Host 通常不需要管理员权限。生产 Agent 由 LocalSystem Windows Service 运行，只有这种承载方式才提供
`run_as=system` 与 `run_as=interactive_user` 的生产身份保证。前台诊断可使用管理员权限约束和清理普通
测试进程，但不能把前台调用方身份解释为 SYSTEM。
任务中的 `program` 必须对应已登记的 Artifact，任务路径必须相对运行根目录。遇到 Artifact hash 不一致、
路径越界或声明的结果文件缺失时，Agent 会生成失败结果，不会继续猜测。

VM 卡死时可使用 Host 的硬关闭、恢复快照和重新启动命令完成带外恢复。单 VM 生命周期计划也可由
`orchestrate` 自动串联这些步骤；返回 `RECOVERY_FAILED` 时必须停止后续测试并保留归档，不能把它
降级解释为普通业务失败。已存在的生命周期归档不会被覆盖，当前需人工判断后再决定如何处理未完成运行。

# Satsuma TestLab

Satsuma 在 Windows 宿主机与 VMware 测试虚拟机之间物化任务、部署 Artifact、执行程序并保存证据。
它只报告通信、执行和恢复状态，不判断被测程序的业务结果是否正确。

## 当前已实现增量

版本 `0.1.0` 已提供：

- `SatsumaHost.exe run`：校验任务和 VM ID，计算 Artifact SHA-256，并原子发布运行目录。
- `SatsumaVM.exe`：轮询任务、独占领取步骤、校验并复制 Artifact 到 VM 本地目录。
- Windows Job Object 进程树管理、超时终止、stdout/stderr 持续落盘和结果文件收集。
- `SatsumaHost.exe report`：汇总当前运行的机器可读结果。
- `SatsumaHost.exe check`：检查 Host/VMware 环境，发送无害任务并确认 Agent 执行通道。
- `coro_rpc` Agent 注册、心跳、任务轮询、状态上报和断线重连。
- `vmrun` 运行状态、启动、软/硬关闭、快照恢复、列表、创建和删除封装。
- Host VM 生命周期命令，以及带前缀、配额、所有权保护和元数据的 AI 快照命令。
- 路径越界、绝对任务路径和重解析点校验。

JSON 任务的自动快照恢复和失败后自动恢复仍属于后续增量；当前版本通过显式 Host 命令提供 VMware
带外操作，不会把尚未接入任务状态机的恢复流程报告为已完成。

## 构建

要求 Windows、CMake 3.25+ 和带“使用 C++ 的桌面开发”组件的 Visual Studio。CMake 会优先使用
包管理器提供的 `nlohmann-json`，未找到时下载固定的 `v3.12.0` tag。

```text
cmake --preset windows-default
cmake --build --preset windows-debug
ctest --preset windows-debug
```

生成文件位于 `build/windows-default/bin/Debug/`。运行时只需部署 `SatsumaHost.exe` 或
`SatsumaVM.exe`，`SatsumaCore` 是静态库。

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

## 环境准备

1. 安装 VMware Workstation 和 VMware Tools。
2. 普通测试可直接使用 NAT 和 DHCP；危险网络测试再配置独立的 Host-only/LAN Segment。
3. 只共享专用的 `vm-share`，不要共享源码根目录、个人目录或凭据。
4. 复制根目录模板为 `lab.local.json`，再填写本机路径。
5. 为每台 VM 复制并填写一份 `agent.json`，然后以管理员权限启动 `SatsumaVM.exe`。
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
Guest IP 都不是运行条件；Agent 主动连接 `host.listen`，Host 不依赖 Guest IP 反向连接。

只有被测程序会修改路由、DNS、防火墙或虚拟适配器时，才建议使用两张隔离网卡：

| 网卡 | 推荐模式 | 用途 |
|---|---|---|
| 管理网卡 | Host-only 自定义 VMnet | Agent 连接 Host RPC、环境诊断 |
| 实验网卡 | 另一条 Host-only/LAN Segment | 被测程序通信，可由测试破坏 |

不要把危险测试使用的实验网卡桥接到办公网、家庭主网或生产网络。无论使用 NAT 还是 Host-only，
`host.listen` 都填写 Host 对应 VMware 虚拟网卡的地址和端口；Guest 保持 DHCP 即可。

在 Host 上用只读命令确认地址确实绑定在本机网卡上：

```powershell
Get-NetIPAddress -AddressFamily IPv4 | Select-Object InterfaceAlias, IPAddress
```

如果使用 RPC 实时状态，需要在 Host 防火墙中允许对应 VMware NAT 或 Host-only 网段入站访问 TCP
37100。防火墙修改需要管理员权限，应由用户确认作用域后手工完成；不要把端口开放到公共网络。
共享文件任务在 RPC 暂时不可用时仍可执行，Agent 会记录连接失败并继续重连。

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
| `protocol_version` | 固定为 `1` |
| `lab_id` | 必须与 Host `lab.json` 完全一致 |
| `vm_id` | 必须等于 Host `vms[].id` 中当前 VM 的 ID |
| `agent_version` | 必须与该 VM 的 `vms[].agent_version` 一致 |
| `host` | 必须与 `lab.json` 的 `host.listen` 一致 |
| `shared_root` | 当前 VM 实测可读写的 Shared Folder UNC 路径 |
| `local_work_root` | VM 内本地执行目录，不要设为共享目录 |
| `poll_interval_ms` | 文件任务轮询间隔，范围 100–60000 |
| `reconnect_interval_ms` | RPC 断线重连间隔，范围 100–60000 |
| `rpc_timeout_ms` | 单次 RPC 超时，范围 100–300000 |

多台 VM 必须分别保存自己的配置，至少修改 `vm_id`；单台任意 VM 可以继续使用逻辑 ID `client`，
它不要求 VMware 显示名称也叫 Client。在 Guest 的管理员 PowerShell 中执行一行安装命令：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File '\\vmware-host\Shared Folders\vm-share\satsuma-bootstrap\install-agent.ps1'
```

脚本会创建 `C:\Satsuma\bin` 和 `C:\Satsuma\work`、复制并校验 Agent，然后以前台 `--watch`
模式运行。被测 Artifact 始终复制到 Guest 本地工作目录执行，不直接从 UNC 路径运行。

确认验收通过后，可由用户在 Windows“任务计划程序”中创建“计算机启动时”任务，勾选“使用最高
权限运行”，程序填写 `SatsumaVM.exe` 的绝对路径，参数填写
`--config C:\Satsuma\agent.json --watch`，起始目录填写 `C:\Satsuma\bin`。首版不安装服务，
也不会替用户创建管理员计划任务。

### 6. 创建用户基础快照

在创建 `clean` 快照前，确认以下条件：

- VMware Tools、共享目录、管理网和 Agent 配置均已验证。
- VM 内没有正在执行的 Satsuma 任务或被测程序。
- 临时 Artifact、路由、DNS、虚拟适配器和测试进程已经清理。
- `clean` 不以 `satsuma-ai-` 开头，并与 `lab.json` 中的基础快照名完全一致。

推荐创建关机快照，并使用上面的最高权限计划任务在开机后启动 Agent。这样恢复快照后不会复用旧
进程内存和 RPC 会话。如果不配置计划任务，每次恢复和启动 VM 后都需要用户手工启动 Agent。

用户基础快照属于只读基线。AI 只能通过 `SatsumaHost snapshot create-ai/delete-ai` 管理带配置
前缀的派生快照，不能覆盖或删除 `clean`。

### 7. 首次启动和验收顺序

1. 在 Host 启动目标 VM：

   ```powershell
   & 'build/windows-default/bin/Release/SatsumaHost.exe' vm start `
       --id client --config 'lab.local.json'
   ```

2. 确认 VM 中的 `SatsumaVM.exe --watch` 已运行。
3. 如需 RPC 实时状态，在一个 Host 终端持续运行：

   ```powershell
   & 'build/windows-default/bin/Release/SatsumaHost.exe' serve --config 'lab.local.json'
   ```

4. 在另一个 Host 终端执行主动检测：

   ```powershell
   & 'build/windows-default/bin/Release/SatsumaHost.exe' check `
       --config 'lab.local.json' --vm client --timeout-seconds 30
   $LASTEXITCODE
   ```

5. 只有退出码为 0 且 JSON 顶层 `status` 为 `ready`，才运行真实测试。两台 VM 都准备完成后，省略
   `--vm` 可以一次检测 `lab.json` 中的全部 VM。

6. 运行无害示例并读取 Host 返回的 `run_id`：

   ```powershell
   & 'build/windows-default/bin/Release/SatsumaHost.exe' run `
       --config 'lab.local.json' --plan 'examples/hello-vm-task.json'
   ```

7. Agent 完成任务后查询报告：

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
SatsumaHost.exe check --config lab.local.json --vm client --timeout-seconds 30
```

`check` 会验证共享目录和归档目录的原子写入、`vmrun` 控制通道、VMX 和基础快照，然后发布唯一的
`echo` 任务并等待 Agent 返回结果。它不会启动、关闭、恢复虚拟机或改动快照。未指定 `--vm` 时会检查
配置中的全部 VM；`--timeout-seconds` 接受 1–300 秒，默认 30 秒。

报告为机器可读 JSON，调用方必须同时检查退出码和顶层 `status`：

- 退出码 0、`ready`：环境检查和全部 Agent 探针均通过，可以继续。
- 退出码 3、`degraded`：Agent 探针通过，但 VMware 或环境检查存在异常，应停止并处理报告中的失败项。
- 退出码 1、`failed`：Agent 超时、执行失败或结果不匹配，自动化通道当前不可用。

`check` 不会替用户启动 VM 或 Agent。常见失败项可按下表定位：

| 报告位置 | 常见原因 | 用户处理 |
|---|---|---|
| `checks/shared_folder` | Host 共享目录不存在或不可写 | 核对 `host_root`、目录权限和磁盘状态 |
| `checks/archive` | Host 归档目录不存在或不可写 | 创建目录并核对 `archive_root` |
| `checks/vmrun` | `vmrun.exe` 路径错误 | 核对 VMware 安装目录和 `provider.vmrun` |
| `checks/vmware_control` | VMware 控制命令失败或超时 | 启动 VMware 服务，手工确认 `vmrun list` 可用 |
| `checks/vmx` | VMX 路径错误或文件已移动 | 在 VMware 中找到实际 `.vmx` 并更新配置 |
| `checks/snapshots` | 基础快照不存在 | 手工创建快照或修正 `snapshots.base` |
| `agents[].status=timeout` | VM 未启动、Agent 未运行或 Guest 看不到共享目录 | 启动 VM/Agent，重新验证 Shared Folder |
| `agents[].status=failed` | Agent 结果、退出码或 stdout 不符合探针 | 查看该 `run_id` 下的 `execution.json` 和日志 |

## 运行文件通道示例

在 Host 上物化任务：

```text
SatsumaHost.exe run --config lab.local.json --plan examples/hello-vm-task.json
```

在 Client VM 中领取一次任务，或持续轮询：

```text
SatsumaVM.exe --config agent-client.json --once
SatsumaVM.exe --config agent-client.json --watch
```

Host 根据 `run` 输出的 `run_id` 查看报告：

```text
SatsumaHost.exe report --config lab.local.json --run <run-id>
```

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
最终日志和收集文件发布前均经过完整写入或原子改名；执行期间可读取 `.partial` 日志。

## 权限与故障边界

Host 通常不需要管理员权限。VM Agent 应以管理员权限运行，才能约束和清理需要高权限的被测进程。
任务中的 `program` 必须对应已登记的 Artifact，任务路径必须相对运行根目录。遇到 Artifact hash 不一致、
路径越界或声明的结果文件缺失时，Agent 会生成失败结果，不会继续猜测。

VM 卡死时可使用 Host 的硬关闭、恢复快照和重新启动命令完成带外恢复。当前 JSON 任务尚未自动串联
这三步；任一命令失败时 Host 返回非零退出码，调用方必须停止后续测试并保留错误输出。

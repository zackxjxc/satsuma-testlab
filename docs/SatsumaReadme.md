# Satsuma TestLab

Satsuma 在 Windows 宿主机与 VMware 测试虚拟机之间物化任务、部署 Artifact、执行程序并保存证据。
它只报告通信、执行和恢复状态，不判断被测程序的业务结果是否正确。

## 当前已实现增量

版本 `0.1.0` 已提供：

- `SatsumaHost.exe run`：校验任务和 VM ID，计算 Artifact SHA-256，并原子发布运行目录。
- `SatsumaVM.exe`：轮询任务、独占领取步骤、校验并复制 Artifact 到 VM 本地目录。
- Windows Job Object 进程树管理、超时终止、stdout/stderr 持续落盘和结果文件收集。
- `SatsumaHost.exe report`：汇总当前运行的机器可读结果。
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
2. 配置管理网络和独立实验网络；不要桥接到生产网络。
3. 只共享专用的 `vm-share`，不要共享源码根目录、个人目录或凭据。
4. 根据本机路径填写根目录的 `lab.json`。
5. 在 VM 中复制并填写 `examples/agent-client.json`，然后以管理员权限启动 `SatsumaVM.exe`。
6. 清理 VM 中的遗留进程和网络状态，保存名为 `clean` 的用户基础快照。

## 运行文件通道示例

在 Host 上物化任务：

```text
SatsumaHost.exe run --config lab.json --plan examples/hello-vm-task.json
```

在 Client VM 中领取一次任务，或持续轮询：

```text
SatsumaVM.exe --config agent-client.json --once
SatsumaVM.exe --config agent-client.json --watch
```

Host 根据 `run` 输出的 `run_id` 查看报告：

```text
SatsumaHost.exe report --config lab.json --run <run-id>
```

Host 在当前目录读取 `lab.json` 时，可以执行 VM 和快照操作：

```text
SatsumaHost.exe vm start --id client
SatsumaHost.exe vm stop --id client --mode hard
SatsumaHost.exe vm restore --id client --snapshot clean
SatsumaHost.exe snapshot list --vm client
SatsumaHost.exe snapshot create-ai --vm client --name network-ready
SatsumaHost.exe snapshot delete-ai --vm client --snapshot satsuma-ai-network-ready-20260723120000
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

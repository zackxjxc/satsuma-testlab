# Satsuma TestLab

Satsuma 在 Windows 宿主机与 VMware 测试虚拟机之间物化任务、部署 Artifact、执行程序并保存证据。
它只报告通信、执行和恢复状态，不判断被测程序的业务结果是否正确。

## 当前首个增量

版本 `0.1.0` 已提供：

- `SatsumaHost.exe run`：校验任务和 VM ID，计算 Artifact SHA-256，并原子发布运行目录。
- `SatsumaVM.exe`：轮询任务、独占领取步骤、校验并复制 Artifact 到 VM 本地目录。
- Windows Job Object 进程树管理、超时终止、stdout/stderr 持续落盘和结果文件收集。
- `SatsumaHost.exe report`：汇总当前运行的机器可读结果。
- 路径越界、绝对任务路径和重解析点校验。

`coro_rpc` 实时心跳、`vmrun` 生命周期管理、快照保护和归档区复制属于后续增量，当前版本不会假装
这些能力已经可用。

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

每次运行使用 `shared_folder.host_root/runs/<run_id>/`，不要复用旧的 `run_id`。`execution.json`、
最终日志和收集文件发布前均经过完整写入或原子改名；执行期间可读取 `.partial` 日志。

## 权限与故障边界

Host 通常不需要管理员权限。VM Agent 应以管理员权限运行，才能约束和清理需要高权限的被测进程。
任务中的 `program` 必须对应已登记的 Artifact，任务路径必须相对运行根目录。遇到 Artifact hash 不一致、
路径越界或声明的结果文件缺失时，Agent 会生成失败结果，不会继续猜测。

当前版本尚未实现 VMware 带外恢复，因此 VM 卡死时需要用户手工恢复 `clean` 快照。加入 `vmrun` 后，
恢复失败会使用 `RECOVERY_FAILED` 明确停止自动流程。

# 执行、结果与恢复

开始、等待、取消、恢复或报告测试前，阅读本参考文档。

## 执行前检查与运行

改变环境前先检查持久化测试状态：

```powershell
SatsumaHost\SatsumaHost.exe lab status --config config\lab.local.json
```

必须确认 VMCI 网关可用，并完成一次结果为 `status: ready` 的完整 `check`。优先使用单条有时间上限的编排命令：

```powershell
SatsumaHost\SatsumaHost.exe orchestrate --config config\lab.local.json --plan task.json --timeout-seconds 900 --boot-wait-seconds 120
```

从 Host 的 JSON 输出中读取 `run_id`，绝不能根据目录名推断。不要通过编辑传输状态根目录下的文件改变任务结果。

## 解读结果

| 结果 | 操作 |
|---|---|
| `succeeded`，退出码 0 | 报告成功前，核对步骤结果、归档、清理、虚拟机最终状态及租约释放情况 |
| `pending`，退出码 0 | 继续进行有时间上限的等待；不要报告成功 |
| `failed`，退出码 1 | 报告失败步骤、程序退出码及保留的证据 |
| 等待超时，退出码 3 | 检查 `runs list`；根据用户已有授权决定继续等待或取消，无法判断时询问用户 |
| `RECOVERY_FAILED`，退出码 4 | 保留租约和证据，在可能的情况下确认虚拟机已关闭，并要求人工处理 |
| `manual_intervention_required`，退出码 5 | 停止自动重试，保留证据，核对虚拟机电源状态，并请求用户决定 |

被测程序的非零退出码属于业务失败。不要通过编辑状态或重试不安全步骤来掩盖失败。

## 取消任务

用户要求停止任务时，应使用 CLI，并持续读取报告，直到任务进入终态或需要人工干预的状态：

```powershell
SatsumaHost\SatsumaHost.exe runs cancel --config config\lab.local.json --run <run-id> --reason "user requested stop"
```

取消期间不要删除任务目录。

## 恢复边界

Host 崩溃后，先检查 `lab status`。只有持久化状态明确允许恢复时，才能针对同一个 `run_id` 使用 `lab recover`。执行 `lab unlock --force true` 前，必须由人工确认原 Host 进程已经退出、外部及 Guest 状态已经核对，并且证据已得到保留。

恢复快照前，应将 Guest 存储、Host 传输状态和 Host 归档视为三个独立的恢复域：

1. 保留报告、生命周期状态、租约数据和相关错误。
2. 确认旧 Host 进程已经退出，或者恢复同一任务。
3. 整理被遗弃的 Host 协议状态前，先强制关闭目标虚拟机。
4. 任何归档或删除操作都必须限定在受影响的虚拟机和任务范围内；绝不能清空整个状态根目录。
5. 只恢复用户批准的快照，按需启动虚拟机，并等待新的启动、会话和在线状态。
6. 重新执行一次完整 `check`；同一种未知故障再次出现时停止恢复。

绝不能让两个 Host 控制器同时操作同一个测试环境。通常不需要重启宿主机；应改为明确处理进程、租约、Guest 和证据状态。

# 首次配置

当 `config/lab.local.json` 缺失、不完整或校验失败，或者需要安装或重新绑定 Guest Agent 时，阅读本参考文档。

## 写入前先发现环境

阅读匹配发行版本中的 `docs/首次配置.md`、`config/lab.template.json`、`config/agent.template.json`、`schemas/lab.schema.json` 和 `schemas/agent.schema.json`。在不改变虚拟机状态的前提下检查 Host，并收集：

- `vmrun.exe` 的绝对路径；
- Host 专用归档根目录和 VMCI 传输状态根目录；
- 测试环境 ID 和 VMCI 端口；
- 每台已授权虚拟机的稳定 ID 和 VMX 绝对路径；
- 用户选择的基础快照，以及 AI 管理的快照前缀和配额；
- 预期的 Agent 版本。

不要根据磁盘上发现或 VMware 列出的虚拟机推断授权边界。用户尚未选择的值应保持未确定状态；绝不能带着 `REQUIRED` 或 `replace_with` 等模板标记继续执行。

## 创建并校验配置

复制模板，不要直接修改模板本身：

```powershell
Copy-Item config\lab.template.json config\lab.local.json
Copy-Item config\agent.template.json config\agent.json
```

所有 Guest 最初可以共用同一份 `agent.json`。其中的 `lab_id`、Agent 版本和 VMCI 端口必须与 Host 配置一致。除非用户有明确要求，否则不要手动选择 Guest 存储根目录；安装程序会选择受支持的本地固定磁盘，并拒绝桌面、临时目录、网络位置和可移动磁盘。

准备好 `bin/SatsumaVM.exe`、`scripts/install-agent.ps1` 和 `agent.json`，以便传入每台 Guest。安装 Windows 服务及确认 UAC 属于管理员操作。如果 AI 在当前环境中无法完成这些操作，应给出准确的人工接管步骤，不能声称配置已经完成。

## 绑定并验收环境

在独立的 Host 进程中启动网关：

```powershell
bin\SatsumaHost.exe gateway --config config\lab.local.json
```

目标虚拟机在线后，使用 `discover` 获取其硬件 UUID，将其绑定到用户选定的虚拟机 ID，然后执行完整检查：

```powershell
bin\SatsumaHost.exe discover --config config\lab.local.json
bin\SatsumaHost.exe agent rebind --config config\lab.local.json --vm <vm-id> --hardware-id <uuid>
bin\SatsumaHost.exe check --config config\lab.local.json --vm <vm-id> --timeout-seconds 180
```

只有 `status: ready` 才表示环境可以承载业务任务；虚拟机仅仅处于开机状态并不够。Agent 安装、身份绑定和完整检查全部成功后，正常关闭 Guest，再由用户确认创建或替换基础快照。在 Agent 安装前创建的快照不能作为有效的自动化基线。

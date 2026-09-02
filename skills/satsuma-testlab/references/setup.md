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

## 未初始化 Guest 的人工交接

当 Guest 尚未安装 Agent，且 Host 与 Guest 之间还没有可用的 Satsuma 通道时，不要把启用 Guest Account
控制通道、VMware Shared Folders、网络共享或其他长期集成机制作为初始化前提。首次引导应采用一次性的人工
文件交接：AI 先在 Host 上完成所有能够完成的准备工作，再由用户使用自己熟悉的方式把文件复制进 VM 并执行。

AI 应创建一个内容完整、可以直接交付的目录，至少包含 `SatsumaVM.exe`、`install-agent.ps1` 和已经填写且完成
基本校验的 `agent.json`；如当前初始化还需要其他脚本、配置或辅助文件，也应一并准备好。不要让用户进入 Guest
后再手工拼接命令、编辑 JSON 或从多个位置寻找依赖。

准备完成后，向用户提供一段可核对的人工操作说明，其中必须明确：

- Host 上待复制目录的准确路径和目录内文件；
- Guest 中建议放置的本机目录，以及需要管理员权限的准确执行命令；
- 用于判断成功的关键输出或状态，例如 Agent 版本、Service 名称、`Running` 状态和 `Auto` 启动类型；
- 如果结果不一致，应停止在哪一步，并让用户原样返回错误信息；
- 用户确认成功后，AI 将继续执行的 Host 侧发现、绑定和 `check` 步骤。

发出说明后等待用户确认，不要把“文件已准备好”当成“Guest 已初始化”，也不要在用户尚未回报执行结果时继续
进行依赖 Agent 在线的操作。用户已有便捷复制方式时直接沿用；只有用户明确要求时，才协助配置额外的传输机制。

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

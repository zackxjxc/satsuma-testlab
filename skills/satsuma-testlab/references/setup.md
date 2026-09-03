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

已明确授权 VM 路径和共同基础快照时，优先调用：

```powershell
SatsumaHost\SatsumaHost.exe init --config config\lab.local.json --vmx <VMX或单VM目录> --vmx <另一VMX或目录> --base-snapshot <已确认快照>
```

`init` 自动查找 vmrun、读取 VMX 硬件 UUID、生成中性 VM ID，并用发行包内 Agent 二进制生成预期哈希。
默认端口为 `42510`，已有 Agent 不同则显式传 `--vmci-port`。实验室 ID 自动生成并在接入后下发；
仅兼容旧式固定实验室 Agent 时需要显式传匹配的 `--lab-id`。
不要把 Agent 自报哈希写成预期哈希来消除不匹配。初始化不会启动 Host 或 VM，输出 `initialized`
仅说明配置已生成，不代表在线验收通过。配置已存在时会拒绝覆盖。

交付给消费者时，提供完整发行包、本机 JSON 和环境说明，关闭验证用 Host 网关并完成旧运行的正常清理。
消费者负责启动自己的 Host 网关；不要要求复制旧 `state`、`archive` 或租约作为运行前提。

需要手工配置不同快照或其他高级字段时：

复制模板，不要直接修改模板本身：

```powershell
Copy-Item config\lab.template.json config\lab.local.json
```

Guest 无需外部 JSON。安装程序自动生成本机配置，默认 CID `2`、端口 `42510`，省略实验室/VM 身份。
特殊部署才提供可选 `agent.json`；不要把旧实验室配置混入通用安装目录。安装器自动选择本地固定磁盘。

每台 Guest 只需复制发行包的 `SatsumaGuestAgent-Install` 目录，其中已并列放置 `SatsumaVM.exe` 和 `install-agent.ps1`，无需 AI 预处理。
确认 UAC 属于管理员操作；AI 无法完成时应给出准确的人工接管步骤，不能声称安装已经完成。

## 未初始化 Guest 的人工交接

当 Guest 尚未安装 Agent，且 Host 与 Guest 之间还没有可用的 Satsuma 通道时，不要把启用 Guest Account
控制通道、VMware Shared Folders、网络共享或其他长期集成机制作为初始化前提。首次引导应采用一次性的人工
文件交接：AI 先在 Host 上完成所有能够完成的准备工作，再由用户使用自己熟悉的方式把文件复制进 VM 并执行。

若用户要求 AI 准备交接目录，只需放入 `SatsumaVM.exe` 和 `install-agent.ps1`。用户也可自行复制这两个文件。
不要要求用户进入 Guest 后再编辑 JSON；没有 BAT 入口。

准备完成后，向用户提供一段可核对的人工操作说明，其中必须明确：

- Host 上待复制目录的准确路径和目录内文件；
- Guest 中建议放置的本机目录，执行 `./install-agent.ps1` 或右键“使用 PowerShell 运行”，接受 UAC；
- 用于判断成功的关键输出或状态，例如 Agent 版本、Service 名称、`Running` 状态和 `Auto` 启动类型；
- 如果结果不一致，应停止在哪一步，并让用户原样返回错误信息；
- 用户确认成功后，AI 将继续执行的 Host 侧发现、绑定和 `check` 步骤。

发出说明后等待用户确认，不要把“文件已准备好”当成“Guest 已初始化”，也不要在用户尚未回报执行结果时继续
进行依赖 Agent 在线的操作。用户已有便捷复制方式时直接沿用；只有用户明确要求时，才协助配置额外的传输机制。

PS1 默认保留成功/失败窗口。自动化调用加脚本参数 `-NonInteractive`，以免等待键盘输入。
若启动前被执行策略拦截，给出 `powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\install-agent.ps1`。
PS1 无法在尚未执行时解除策略拦截；不要承诺仅靠脚本自身解决，也不要修改长期执行策略。

## 绑定并验收环境

在独立的 Host 进程中启动网关：

```powershell
SatsumaHost\SatsumaHost.exe gateway --config config\lab.local.json
```

通用 Agent 先通过 `enroll` 接收实验室及中性 VM ID；Host 未登记的硬件被拒绝。使用 `discover` 和 `check`
确认在线可用。手工 Host 模板也必须预先填写硬件 UUID。旧式固定实验室 Agent 才使用下面的手动绑定流程：

```powershell
SatsumaHost\SatsumaHost.exe discover --config config\lab.local.json
SatsumaHost\SatsumaHost.exe agent rebind --config config\lab.local.json --vm <vm-id> --hardware-id <uuid>
SatsumaHost\SatsumaHost.exe check --config config\lab.local.json --vm <vm-id> --timeout-seconds 180
```

只有 `status: ready` 才表示环境可以承载业务任务。通用冷快照可在 Service 安装及版本检查通过后、Host 尚未启动时
制作，但必须标为“已安装、待在线验收”；消费者首次使用仍须执行完整 `check`。安装前的快照不包含 Agent 服务。

# AI 集成说明

本文面向维护者和集成者，说明 Satsuma TestLab 如何向 AI 代理公开稳定、可审阅的操作界面。真正供 AI 加载的
操作入口是 [`skills/satsuma-testlab/SKILL.md`](../skills/satsuma-testlab/SKILL.md)；不要在本文和 Skill 中
分别维护两套命令流程。

## 集成层次

| 层次 | 权威来源 | 作用 |
|---|---|---|
| 产品行为 | `SatsumaHost.exe --help`、`SatsumaVM.exe --help` | 当前二进制实际支持的命令与参数 |
| 数据结构 | `schemas/*.schema.json` | 配置、任务、状态和结果允许的字段 |
| 通信语义 | [`协议.md`](协议.md) | VMCI 请求、传输状态和一致性约束 |
| 人类流程 | [`用户指南.md`](用户指南.md)、[`首次配置.md`](首次配置.md) | 安装、配置、使用和排障说明 |
| AI 流程 | `skills/satsuma-testlab/` | AI 的决策顺序、安全边界和按需参考资料 |
| 发行入口 | `AI-START-HERE.md` | 让尚未安装 Skill 的 AI 找到同包 Skill |

CLI 和 Schema 是程序接口的事实来源；Skill 负责告诉 AI 何时读取这些接口、如何选择工作流以及何时停止，
而不是复制完整字段清单。

## 为什么随发行物提供 Skill

AI 使用流程会随 CLI、Schema 和恢复语义演进。发行包同时携带二进制、文档、示例和 Skill，可以保证 AI 读取
的是与程序匹配的操作规则。用户也可以把整个 Skill 目录安装到支持 Agent Skills 的客户端，但升级程序时必须
同步替换已安装的 Skill。

源码中的 Skill metadata 声明其适配的 Satsuma 版本。CMake 配置阶段校验该版本与 `project(... VERSION ...)`
一致；发行包入口文档中的显示版本则由 CMake 从同一个项目版本生成。长期文档不重复写死当前产品版本，稳定
文档快照由 Git Tag 和 GitHub Release 确定。

## 授权与安全模型

加载 Skill 不等于获得无限制操作授权。AI 仍必须遵守用户任务的范围，只控制用户确认并写入
`config/lab.local.json` 的实验 VM。下列信息不得靠扫描结果自行决定：

- 哪些 VM 属于本次任务；
- 哪个快照可以恢复；
- 是否允许强制停止、丢弃 Guest 状态或运行真实故障注入；
- Artifact 是否可信以及是否允许以 SYSTEM 身份运行；
- 哪些日志、文件和本机路径可以向外部披露。

Satsuma 是可信实验室工具，不是恶意样本沙箱或跨租户隔离边界。Skill 应让 AI 通过 Host CLI 工作，不允许直接
伪造或修改 claim、结果、租约和生命周期状态。

## 可移植性

`skills/satsuma-testlab/` 采用开放的 Agent Skills 目录结构：必需入口为 `SKILL.md`，较长流程放入
`references/` 并按需读取。支持该格式的客户端可以自动发现或安装；其他能够读取本地文件的 AI 也可以把它
作为普通 Markdown 操作手册使用。

Skill 不携带二进制、不下载程序，也不保存环境配置、凭据或 VM 身份。它通过用户提供的发行目录定位程序，
因此同一份源码仍可被不同 AI 客户端使用。

## 维护规则

修改公开行为时按职责更新：

- CLI 参数变化：修改 CLI、帮助输出、测试以及引用该命令的 Skill reference。
- 任务或结果字段变化：先修改 Schema 和协议实现，再更新示例及相关 Skill 决策。
- 生命周期或恢复规则变化：修改实现、端到端测试、用户指南和 `references/operations.md`。
- 首次安装变化：修改 EXE 安装入口、首次配置文档和 `references/setup.md`。
- 产品版本变化：修改 CMake 项目版本和 Skill metadata；发行入口由构建生成。

发布检查必须确认发行 ZIP 包含 `AI-START-HERE.md`、完整 Skill 目录和全部引用文件，并验证 Host、Agent、Skill
及发行包名称属于同一个产品版本。

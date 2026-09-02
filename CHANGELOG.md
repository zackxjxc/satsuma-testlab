# 更新日志

本文件记录项目的重要变化。首个稳定版本发布后，项目将遵循语义化版本规则。

## 0.3.2 - 2026-09-02

### 新增

- Host 与 Agent 的 `--version --json` 输出 GitHub Actions Build Number、Run Attempt 和 Git Commit；Agent
  presence 同时发布构建身份和二进制 SHA-256，Lab 可选择锁定精确 Agent 哈希。
- `plan validate` 和 `orchestrate --dry-run` 在改变 VM 前静态检查任务、VM 引用、Artifact 和生命周期要求。
- `orchestrate --output jsonl` 输出阶段变化、心跳和最终结果；`orchestrate`/`report` 支持原子
  `--report-out` 文件。
- `report` 和 `runs list` 可以直接查询已完成并校验的独立归档，不再依赖 Host 活跃运行目录。

### 变更

- 文档明确同 VM 串行、跨 VM 并发、Windows Job 完整进程树、Lab 单写租约和快照所有权语义。
- Agent 更新成功后提示已有基础快照仍可能恢复旧 Agent，并给出冷快照升级流程。
- 写租约冲突错误增加占用命令、PID、租约路径和安全重试建议。

### 修复

- 步骤超时时保留进程树超时作为主错误，将收集文件缺失作为次生诊断追加。
- 恢复失败时继续保留实验室租约，避免不确定外部状态被误判为可以安全重试。

## 0.3.1 - 2026-09-02

### 新增

- 完整英文 README，并在中英文首页提供双向语言入口。
- GitHub 标准社区文件、Apache License 2.0 和社区行为准则。
- 标签驱动的 GitHub Release：自动验证版本、编译测试、生成便携 ZIP 与 SHA-256 校验文件并发布。

### 修复

- 缩短 Windows CI 多虚拟机测试路径，避免传统路径长度限制导致 Agent 无法创建日志。
- Host 清理运行状态时等待整组目录稳定消失，防止迟到写入重新产生已归档状态。
- 重写 Claim 续租测试的时序断言并扩大慢速 CI 的执行预算，消除 runner 调度停顿造成的伪失败。
- 等待交互任务所属 Job 的完整进程树退出，避免子进程终止尚未收敛时提前返回。
- SYSTEM 任务同样等待所属 Job 的完整进程树退出，确保取消、超时和正常完成后不残留子进程。
- 为假 `vmrun` 的正常路径保留合理启动预算，避免高负载环境把进程启动延迟误判为对账失败。
- 发行校验值改由 .NET SHA-256 实现生成，兼容不提供 `Get-FileHash` 的 Windows PowerShell 环境。
- 更新失败终态集成测试只读取已原子发布的目录，避免把 Host 隐藏暂存目录误判为可消费更新。

## 0.3.0 - 2026-09-01

### 新增

- 基于 libzmq 原生 `vmci://` 的 Host 常驻网关、Guest 本地镜像、1 MiB 分块传输与 SHA-256 校验。
- Host 权威 claim/续租/结果 fencing RPC，以及不经过 Guest 网络栈的取消查询。
- Host 运行列表、文件取消和已完成运行保留命令。
- 按运行隔离的 Agent 故障处理和诊断错误文件。
- Artifact、参数、收集文件和进程输出容量限制。
- 取消、Presence、Claim、运行清单、生命周期和报告的 JSON Schema。
- Windows Debug/Release CI、CMake 安装规则和 ZIP 打包。
- 公开的架构、用户、协议、开发、安全和贡献文档。
- 面向 AI 代理的可安装 `satsuma-testlab` Skill、按需参考资料和发行包 AI 入口。

### 变更

- `transport.state_root` 保存 Host 权威状态，VMCI 是唯一生产通信通道。
- Agent 安装介质改为同目录本地输入，配置新增 VMCI CID、端口、请求超时和本地 `mirror_root`。
- Host 与 VM 生产代码拆分为可供测试复用的库和轻量可执行程序入口。
- 配置解析器会拒绝未知字段和已移除字段，不再静默忽略。
- 报告命令使用不同退出码区分业务失败、超时和人工介入。
- Host 归档成功后通过显式请求清理 Guest 本地工作目录，并等待 Agent 回执。
- 任务 schema 收敛为 v3，VMCI 运行清单升级为 v4；删除无效的 request ID、echo 超时、重复超时布尔值和过期身份字段。
- 运行清单、结果、claim、更新和生命周期解析器在运行时拒绝未知 JSON 字段。
- JSON 契约校验收敛为公共实现，本地 CTest 直接验证 Draft 2020-12 Schema 与公开示例。
- Host CLI、生命周期编排和 Agent 步骤执行按职责拆分，协议、退出码和持久化状态保持不变。
- 生产与测试 VM 库共享同一份 CMake 源文件清单和严格警告配置。
- Host 和 Agent 可执行文件写入由 CMake 项目版本生成的 Windows 版本资源。
- GitHub README 与发行包 README 分离；发行入口从 CMake 项目版本生成，并校验 Skill 适配版本。
- 发行包使用独立运行组件，排除依赖库的开发头文件、静态库和 CMake 元数据。
- 缩短 Host 原子发布暂存目录名，避免深层实验室路径触发 Windows 传统 260 字符路径限制。

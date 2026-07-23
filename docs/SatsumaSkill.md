# Satsuma 最小使用契约

1. 先读取 `lab.json`、`schemas/task.schema.json` 和当前任务文件。
2. 任务内只使用共享运行根目录下的相对路径；拒绝绝对路径、`..` 和重解析点。
3. 每个 Artifact 必须登记 Host 绝对源路径、目标 VM 和 `artifacts/` 下的相对目标；可预填 SHA-256。
4. 先运行 `SatsumaHost.exe run` 获取唯一 `run_id`，不要复用旧运行目录。
5. 被测程序应返回明确退出码，并优先生成 UTF-8 日志和机器可读 JSON 结果。
6. 失败后先读取 `execution.json`、`stdout.log`、`stderr.log` 和已收集文件，再修改目标项目。
7. Satsuma 的结论只覆盖部署、执行、超时和证据收集；业务正确性由调用它的 AI 判断。
8. 被测程序只能在隔离 VM 中执行，不向 Host 添加运行被测 exe 的旁路命令。
9. 当前 `0.1.0` 只提供共享文件通道；在 `vmrun` 恢复能力完成前，VM 卡死必须交由用户恢复快照。
10. 后续出现 `RECOVERY_FAILED` 时停止自动修改，要求用户处理 VMware 环境。

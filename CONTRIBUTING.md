# 贡献指南

## 提交变更前

会改变 VMCI/本地状态协议、VM 生命周期语义、Service 权限、支持的 VMware 产品或兼容性承诺的变更，应先通过 Issue
或聚焦的设计说明进行讨论。小型修复和文档纠正可以直接提交 Pull Request。

只能贡献自己有权提交的代码和文档。项目许可证尚未确定；在根目录加入明确的 `LICENSE` 前，不应认为提交
内容会自动授予下游复制、修改或再分发权利。

## 开发流程

1. 创建范围明确的分支，不要在差异中混入无关格式调整或生成文件。
2. 遵守 `.editorconfig` 和 `.gitattributes`。现有文件使用 UTF-8 和 LF，明确要求 BOM 的 PowerShell
   脚本除外。
3. 根据行为风险补充相应测试。协议变更必须同步更新 C++ 校验、JSON Schema、示例和 `docs/协议.md`。
4. 请求 Review 前完成 Debug 和 Release 构建及测试。
5. 在 Pull Request 中说明行为、兼容性影响、验证结果，以及尚未执行的真实 VMware 场景。

```powershell
cmake --preset windows-default
cmake --build --preset windows-debug --parallel
ctest --preset windows-debug
cmake --build --preset windows-release --parallel
ctest --preset windows-release
git diff --check
```

## Review 标准

变更应保持 Host `transport.state_root` 作为唯一任务事实源，显式限定 VM 范围，使用原子协议写入，并为取消和超时设置
有限边界。新增依赖必须有明确的维护收益、精确版本，并在 `THIRD_PARTY_NOTICES.md` 中登记。

不得提交真实 VM 凭据、VMX 内容、本机绝对路径、VMCI 传输证据、私有 Artifact 或包含用户数据的测试
日志。真实 VMware 测试必须保持显式启用，并且不得在普通 CI 中运行。

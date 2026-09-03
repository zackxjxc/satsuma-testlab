# Satsuma TestLab

[简体中文](README.md) | English

Satsuma TestLab is a Windows virtual machine task runner designed for AI agents and VMware Workstation test environments. An AI agent can use the Host CLI to deploy programs, scripts, and test plans to selected virtual machines, execute them inside the Guest, and retrieve exit codes, logs, and declared result files. It can also manage VM startup, snapshot recovery, failure cleanup, and evidence archiving according to explicit test policies.

Host and Guest communicate through VMware VMCI instead of the Guest network stack. The task channel remains available when a test enables a VPN, changes routes, disables network adapters, or even removes a network driver.

## Fastest way to get started

Download a package from [GitHub Releases](https://github.com/zackxjxc/satsuma-testlab/releases), then give the ZIP file or the complete extracted directory to an AI agent that can read local files and run terminal commands. Ask it to read `AI-START-HERE.md`. The bundled Skill will explain the environment requirements, collect the necessary VM information, and guide the AI through configuration, task execution, result collection, and recovery.

If the AI cannot inspect a ZIP file directly, extract it first. Normal use does not require the source repository or a local build toolchain.

## What it is suitable for

- VPN, proxy, and endpoint networking software whose tests modify drivers, tunnels, DNS, or routes while Host control must remain available.
- Installers and updaters that need silent installation, Windows Service or driver validation, restart recovery, upgrade, and rollback testing.
- Windows desktop applications that require a mixture of LocalSystem and signed-in interactive-user execution.
- Multi-VM scenarios that start machines in a defined order, run client and server tasks, and apply unified success, failure, and final cleanup policies.
- Destructive regression tests that should run repeatedly from isolated snapshots while preserving reviewable evidence.

Satsuma is intended for test labs where Host and Guest administrators share the same trust boundary. It is not a malware sandbox and does not provide tenant isolation.

## Human-prepared environment, AI-operated testing

Initial deployment is a collaborative step between the user and the AI. The user installs VMware Workstation, creates the test VMs, installs VMware Tools, chooses the authorized VMs and base snapshots, approves UAC prompts, and installs the SatsumaVM Agent Service inside each Guest. The AI can inspect the release documentation, help locate paths, generate configuration, perform identity binding, and validate connectivity, but it must not choose destructive VM or snapshot operations on the user's behalf.

After `check` reports a `ready` environment, the AI can independently start and stop authorized VMs, restore user-approved snapshots, deploy programs and scripts, wait for completion, collect logs and result files, and retain or clean up failure state according to the task policy. Routine testing normally requires no repeated manual interaction inside the Guest.

## Per-step execution identity

Every `execute` or `script` step can choose its own `run_as` identity:

| `run_as` | Effective identity | Typical use |
|---|---|---|
| `system` | Windows `LocalSystem`, with system-administrator privileges | Installers, Windows Services, drivers, HKLM, and other privileged system changes |
| `interactive_user` | The user currently signed in to the VM console | GUI and desktop interaction, HKCU, user configuration, ordinary applications, builds, and unit tests |

A single task can combine both identities—for example, install software as `system`, then validate the real user experience as `interactive_user`. Interactive-user execution requires a signed-in console user; otherwise the step fails explicitly and never falls back silently to LocalSystem.

## Key capabilities

- `echo`, `execute`, and controlled `script` steps for CMD, Windows PowerShell 5.1, and PowerShell 7.
- Ordered multi-VM startup, optional snapshot restoration, failure cleanup, and always-run `finally` steps.
- Initial Guest inventory publication, cache repair, and explicit Host refresh.
- Per-lab process exclusion, persistent leases, crash recovery, and guarded manual unlock.
- Independent LocalSystem or interactive-user execution identity for each executable or script step.
- VMCI-only production transport with no dependency on VMware Shared Folders or the Guest network stack.
- Step claims, lease renewal, result fencing, crash recovery, and manual-intervention gates.
- File-based cancellation, run listing, and safe retention policies without blocking unrelated runs.
- Agent Windows Service installation, self-update, and VM identity migration.
- SMBIOS UUID discovery, explicit Host binding, and cloned-identity conflict detection.
- Limits for artifacts, logs, result files, and collected data to protect Host and Guest storage.
- Windows Debug/Release CI, JSON Schema validation, portable release directories, and UTF-8 ZIP packages.

## Quick start

Using a release package requires Windows 10 or 11, VMware Workstation, and working VMware VMCI drivers on the Host and Guest. It does not install files on the Host and does not require CMake.

Building from source additionally requires the Visual Studio 2022 C++ toolchain, CMake 3.25 or later, and Git:

```powershell
cmake --preset windows-default
cmake --build --preset windows-release --parallel
ctest --preset windows-release
cmake --build --preset windows-release --target SatsumaPackage
```

The package target creates both a portable version directory and a ZIP file under `output`. The Host CLI is in `SatsumaHost`; use `SatsumaHost/SatsumaHost.exe init` to create `config/lab.local.json`. Copy the complete `SatsumaGuestAgent-Install` directory to a Guest using a read-only ISO, the VMware console, or another one-time installation medium, then run its `install-agent.ps1`. The installer creates an unbound local configuration and the Agent enrolls with the Host using its SMBIOS UUID.

Start the Host gateway in a dedicated terminal:

```powershell
SatsumaHost\SatsumaHost.exe gateway --config config\lab.local.json
```

Use another Host terminal for discovery, binding, validation, and orchestration:

```powershell
SatsumaHost\SatsumaHost.exe discover --config config\lab.local.json
SatsumaHost\SatsumaHost.exe agent rebind --config config\lab.local.json --vm vm_01 --hardware-id <uuid>
SatsumaHost\SatsumaHost.exe check --config config\lab.local.json --timeout-seconds 180
SatsumaHost\SatsumaHost.exe lab status --config config\lab.local.json
SatsumaHost\SatsumaHost.exe orchestrate --config config\lab.local.json --plan examples\multi-vm-task.json --timeout-seconds 900
```

`gateway` is the long-running Host transport process. Automation should normally use `orchestrate`, which acquires the exclusive lab lease, starts the selected VMs, validates inventory and the internal echo diagnostic, archives evidence, and applies cleanup policies. The lower-level `run` command remains available for tasks without lifecycle policies, but its persistent lease must be finalized with `runs finalize` after the report reaches a terminal state.

## AI Skill

Every release includes the version-matched [`satsuma-testlab` Skill](skills/satsuma-testlab/SKILL.md) and `AI-START-HERE.md`. Clients that support [Agent Skills](https://agentskills.io/) can load `skills/satsuma-testlab/` directly or install it into their own Skill directory. Other AI agents can read `SKILL.md` as a structured operations manual.

When upgrading Satsuma, use the Skill shipped with the new release. Do not operate a new binary with commands or schemas from an older Skill.

## Documentation

The detailed documentation is currently maintained in Chinese:

- [User guide](docs/用户指南.md): installation, configuration, commands, updates, and troubleshooting.
- [Initial setup](docs/首次配置.md): environment inputs, configuration responsibilities, templates, and acceptance checks.
- [Architecture](docs/架构.md): component boundaries, data flow, reliability model, and security assumptions.
- [VMCI protocol](docs/协议.md): requests, chunked transport, local mirrors, schemas, and limits.
- [Development guide](docs/开发指南.md): build, test, package, and real VMware validation.
- [`satsuma-testlab` Skill](skills/satsuma-testlab/SKILL.md): AI-operable workflows and safety boundaries.
- [AI integration contract](docs/AI操作契约.md): responsibilities of the Skill, CLI, schemas, and human authorization.
- [Changelog](CHANGELOG.md), [contribution guide](CONTRIBUTING.md), [code of conduct](CODE_OF_CONDUCT.md), [security policy](SECURITY.md), and [third-party notices](THIRD_PARTY_NOTICES.md).

## Project status

Satsuma currently supports Windows and VMware Workstation only. Destructive real-VMware fault-injection tests are disabled by default and require explicit opt-in on dedicated test VMs. Documentation on `master` describes the development version; stable documentation belongs to the corresponding Git tag or GitHub Release.

The project is licensed under the [Apache License 2.0](LICENSE). Third-party components remain subject to their respective licenses.

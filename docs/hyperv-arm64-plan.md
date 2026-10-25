# Remaining ARM64 Hyper-V work

Objective: implement all relevant Hyper-V extensions for the Windows ARM64 VM.
This inventory preserves the broad goal; implemented interfaces alone do not
establish completion, Windows consumption or a performance benefit.

| Family | Evidence now | Remaining work |
| --- | --- | --- |
| Discovery, identity, synthetic-register transport | Portable/native/EFI and Windows calls | Maintain compatibility when adding privileges |
| Reference counter and clock page | Native two-VP reads/rebasing; Windows page registration | Direct Windows page-use evidence and performance comparison |
| SynIC, SINTs, events, synthetic timers | Native delivery and Windows counters | Cover additional advertised modes in actual guest use |
| SMP IPIs, context synchronization, address-space flushes | Native execution; Windows IPIs/context calls | Windows flush-path observation and acceleration measurements |
| Crash reporting | Validated register/message model | Controlled actual-guest crash evidence on a disposable disk |
| VMBus, GPADLs, rings, channel lifecycle | Windows channels and typed publication/consumption audits | Preserve teardown/backpressure behavior as services grow |
| Heartbeat, timesync, shutdown/reboot | Actual Windows responses, clock correction, poweroff and reset | Keep bounded regression coverage; hibernation is not verified |
| KVP/Data Exchange | Codec, IC state machine, rings and actual Windows KVP 5 intrinsic record | Native set/get/delete verification, broader enumeration and IP exchange |
| VP runtime, assist-page and scheduling interfaces | Not implemented as complete facilities | Determine ARM64 contract/backend support, implement relevant paths and observe use |
| Guest file copy, backup/VSS, memory/CPU hotplug | Not implemented | Assess applicability to this VM, dependencies and implement applicable services |
| Synthetic network/storage | Existing VM uses working VirtIO devices | Resolve applicability of Hyper-V synthetic alternatives rather than claiming VirtIO implements them |
| Migration/reenlightenment, nested virtualization, VSM/VTL | No capability claimed | Assess against backend architecture; unsupported features must stay unadvertised |

Immediate sequence: expand KVP operations and batch remaining applicable
interfaces, then run combined Windows validation and measured on/off comparisons.
Decisions that a family does not apply need concrete architectural reasons;
this table does not silently remove them from the objective.

Guest tests use 4 GiB, explicit startup/shutdown budgets and cleanup. Leave the
VM stopped between tests. Portable protocol work does not need a running VM.

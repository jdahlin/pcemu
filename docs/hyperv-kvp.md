# KVP / Data Exchange implementation

The next integration channel is KVP. Microsoft's
[Data Exchange documentation](https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/integration-services-data-exchange)
describes guest/host metadata and custom key/value pairs exchanged without
network connectivity. The Windows service is `vmickvpexchange`.

Wire definitions were cross-checked against
[Microsoft OpenVMM's KVP protocol](https://github.com/microsoft/openvmm/blob/main/vm/devices/hyperv_ic_protocol/src/kvp.rs)
and [host implementation](https://github.com/microsoft/openvmm/blob/main/vm/devices/hyperv_ic/src/kvp.rs),
plus [Linux's guest ABI](https://github.com/torvalds/linux/blob/master/include/uapi/linux/hyperv.h).
The interface GUID is `a9a0f4e7-5a45-4d96-b827-8a841e8c03e6`; the distinct
Windows-compatible instance GUID is `242ff919-07db-4180-9c2e-b86cb68c8c55`.

`ArmHyperVKvpCodec` encodes the fixed 2,580-byte standard KVP payload, including
the alignment after its two-byte operation/pool header. It supports get, set,
delete and enumerate across the four externally exchanged pools. Values support
UTF-16 strings/expandable strings, DWORD and QWORD. Encoding a mutation does not
queue or send it; the codec has no service, network or disk side effects.

Enumeration decoding requires a matching operation, pool and index, validates
byte counts against 512-byte key and 2,048-byte value limits, checks terminated
UTF-16 including surrogate pairs, and rejects unsupported types and malformed
integer lengths. The caller must validate IC status, transaction and negotiated
versions before decoding a payload.

The diskless codec suite passes 2,611 checks under AddressSanitizer/UBSan,
including every truncated payload length, malformed strings and size/type
boundaries. This is a payload component, not an offered KVP service. Remaining
work includes negotiation, IC transactions/timeouts, channel integration, bounded
host queries and actual Windows responses. IP configuration exchange and KVP
5.0's binary address format are separate unimplemented operations.

The Windows test VM remains stopped during this work. Future guest validations
should use bounded runs and shut the guest down after collecting their evidence.

## Host IC transaction model

`ArmHyperVKvp` provides the standard KVP offer (channel 4, connection 19),
framework 1.0/3.0 and KVP 4.0 negotiation. It does not offer unsupported KVP 5
or older service layouts. Opening initiates negotiation only; enumeration is
an explicit host request. No SET, DELETE or IP configuration is transmitted.

One request may be outstanding, and its bounded result must be drained before
another request is accepted. Repeated packet construction under backpressure
does not change state or count a request; `sent` marks actual publication.
The thirty-second timeout starts at publication and stops the session until
channel reopen. Requests are never retried automatically.

Receive validation covers the VMBus/pipe envelope, transaction ID, flags,
framework/service versions and reserved fields before decoding a successful
record. Guest errors and end-of-pool remain distinct status-bearing results.
Malformed/duplicate/late replies never produce a record. Reports expose bounded
state/counters, not key/value contents.

The diskless suite passes 5,439 checks under AddressSanitizer/UBSan, including
all truncated negotiation/query reply lengths, wrong transaction/version/pool/
index, explicit guest failure, end-of-pool, timeout and reopen. Channel
registration/service dispatch and actual Windows queries remain unimplemented;
this model alone does not add a default VM device. The Windows VM stays stopped.

## VMBus integration

`ArmHyperV::enableKvp()` now registers the real offer, event connection and
packet handler. The ARM platform includes KVP as its fourth VMBus IC. Host
callers explicitly use `enumerateKvp(pool,index)` and drain `takeKvpResult()`;
opening the channel sends negotiation only. The service loop publishes a
request only when its ring has room, counts only successful publication and
uses the common deadline scheduling. Ready/idle KVP adds no periodic polling.
Close clears pending results and removes its service deadline.

The full ring/GPADL/SignalEvent test passes 41 checks under ASan/UBSan,
including a full outbound ring, actual reply decoding, unread-result protection,
end-of-pool, timeout and close. Timesync/shutdown regressions pass 32/38 checks,
the core Hyper-V suite passes 2,170 checks, and the isolated native runner
compiles. Windows channel negotiation and returned guest values remain separate
verification steps.

## First bounded Windows negotiation run

The native Windows test used four GiB and an explicit startup/shutdown budget.
All four channels opened, but KVP rejected one version reply and did not become
ready. No KVP enumeration or guest mutation was requested. The test was stopped
after this protocol mismatch; Hyper-V poweroff completed with runner exit zero.
Evidence is retained under `build/windows11-hyperv/kvp-negotiation/`. An earlier
supervisor attempt sent SIGUSR1 before runner initialization and terminated it;
the corrected supervisor waits for initial statistics before requesting capture.
That failed launch is not guest protocol evidence. No VM is left running.

The KVP model now records bounded numeric negotiation fields (body length,
counts, reserved word and selected versions) for the next investigation. It
does not log KVP key/value contents or relax validation. The cause of the actual
reply rejection is not yet established; these added fields need a new native
build/run before claiming compatibility.

The bounded diagnostic run returned an eight-byte negotiation body with no
selected-version entries, rather than a malformed KVP record. It again reached
Hyper-V poweroff with exit zero (`kvp-diagnostic/result.json`). This motivates
offering both standard KVP 3.0 and 4.0, as in the Microsoft OpenVMM host, instead
of offering only 4.0. Their standard enumeration payload is shared; IP exchange
remains unimplemented. The selected service version is now retained and used
in subsequent query headers and reply validation. Diagnostic counts/reserved
fields are also captured for short eight-byte negotiation bodies.

All four framework/service combinations (framework 1/3, KVP 3/4) pass the
expanded 10,909-check suite, including ASan/UBSan. Actual Windows selection of
one of these versions still requires the next bounded run.

The 3/4-only run also returned no selected version and shut down with exit
zero. Inspection of the installed, WOF-decoded `icsvc.dll` with its matching
public PDB establishes the guest's actual contract: the KVP singleton at RVA
`0x4c440` has one version entry (count at +0x30), pointing at RVA `0x4c110`,
whose value is `05 00 00 00` (5.0). The endpoint loads these fields in its
initialization path. This supersedes the earlier compatibility hypothesis.

The host now offers standard KVP 3/4/5 and retains whichever is selected. It
still never sends IP operations, including version 5's binary IP format. The
expanded suite passes 16,361 checks under ASan/UBSan. A new opt-in runner flag,
`--hyperv-kvp-probe`, queues exactly one intrinsic-pool enumeration at index zero
once the channel is ready. It logs the status and UTF-16-escaped result. It
requires VMBus, does not mutate guest registry data, and does not run by default.
The platform exposes query/result forwarding between stopped-VP slices.

The remaining family-level work is tracked in [the scope inventory](hyperv-arm64-plan.md).

## Actual Windows KVP result

The combined bounded probe using commit `92f9ce8` succeeds. Windows selects
framework 3.0 and KVP 5.0, then returns a nonempty `FullyQualifiedDomainName`
record for intrinsic pool Auto, index zero. The host validates and drains the
record: one negotiation, one request, one response, one record, status zero,
zero rejected responses and zero timeouts. This confirms the version-table
diagnosis and real guest data exchange through the VMBus rings.

Evidence is `build/windows11-hyperv/kvp-query/`: `observation.json`, final
`hyperv.json`, `host.log`, `result.json` and `audit.json`. The private returned
value is retained only in ignored `guest-record.json` (mode 0600). Launch
metadata hashes that record, host log and counters. The normal Windows audit
and explicit poweroff gate pass; the supervisor records exit zero after
56.9 seconds and no running VM. The existing auditor does not yet expose a
separate typed KVP-record gate.

This verifies one intrinsic query, not every KVP operation. Remaining KVP work
is broader host query APIs, multiple indexes/pools, get/set/delete execution
and IP exchange, with appropriate verification of each implemented operation.

## Explicit standard operations

The KVP model and host/platform APIs now support get, set and delete in addition
to enumeration. Calls remain explicit, with one pending transaction and one
unread result. SET/DELETE are restricted to host-owned External/AutoExternal
pools; guest-owned intrinsic and Guest pools stay read-only from these APIs.
Invalid keys/values leave the ready state unchanged. No default boot path
queues a mutation.

GET responses validate the exact requested key and typed value. Successful
SET/DELETE responses require the expected operation, pool and echoed key;
`mutationAcknowledgments` counts protocol acknowledgments only. A returned
status or acknowledgment does not establish persisted registry state. Guest
errors remain results, timeouts never trigger automatic mutation retries, and
close/reopen discard pending payload/result state.

Validation: 16,439 state-machine checks and 7,778 codec checks pass under
AddressSanitizer/UBSan. The full GPADL/ring/SignalEvent test now passes 57 checks,
including set/get/delete through public host APIs, and the ARM platform forwarding
header passes native syntax validation. Actual Windows mutation/readback/delete
cleanup still requires a single bounded disposable-key test on the private disk.
The VM remains stopped while this diskless implementation is completed.

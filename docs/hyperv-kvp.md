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

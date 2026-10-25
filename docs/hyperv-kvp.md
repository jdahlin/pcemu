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

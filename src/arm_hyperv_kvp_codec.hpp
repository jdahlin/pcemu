#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace pc {
// Standard Key-Value Pair (KVP) payload, inside an Integration Component (IC)
// message; IP-address configuration messages are outside this codec.
// Microsoft OpenVMM KvpHeader/Value/KvpMessage, pinned primary wire reference:
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L29
// Standard (non-IP) KVP payload, inside an IC message. No guest mutations are
// performed here: encoding a SET/DELETE does not queue or transmit anything.
class ArmHyperVKvpCodec {
  public:
    // Wire operations 0..3 query, replace/create, remove or enumerate a key.
    // IP-address operations 4/5 are deliberately unsupported here.
    // https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L38
    enum class Operation : uint8_t { Get = 0, Set = 1, Delete = 2, Enumerate = 3 };
    // Separate guest-service collections, selected by wire ID. Availability and
    // contents belong to the guest service; this codec applies no write permissions.
    // https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L58
    enum class Pool : uint8_t { External = 0, Guest = 1, Auto = 2, AutoExternal = 3 };
    // Windows registry-compatible value type IDs, not their byte widths. Strings
    // are UTF-16LE; DWORD/QWORD are unsigned little-endian 32/64-bit integers.
    // ExpandString preserves raw text; the codec does not expand environment variables.
    // https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L96
    enum class Type : uint32_t { String = 1, ExpandString = 2, Dword = 4, Qword = 11 };
    struct Value {
        Type type = Type::String;
        std::u16string text;
        uint64_t number = 0;
    };
    struct Record {
        std::u16string key;
        Value value;
    };
    // Standard padded payload is 2580 bytes, with 512-byte key and 2048-byte
    // value arrays. Text lengths count bytes including a two-byte NUL terminator.
    // https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L76
    // https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L146
    static constexpr size_t PayloadBytes = 2580, MaximumKeyBytes = 512, MaximumValueBytes = 2048;
    // Encode one zero-padded standard payload; throw for invalid operation/key/type/size.
    static std::vector<uint8_t> encode_request(Operation operation, Pool pool, uint32_t index = 0,
                                               const std::u16string &key = {},
                                               const Value *value = nullptr);
    // Caller must first verify the IC transaction/status/version. Rejects
    // malformed lengths, UTF-16, operation/pool/index and unsupported types.
    // Decode a successful GET payload only when its pool and exact key match the request.
    static std::optional<Record> decode_get_response(std::span<const uint8_t> payload, Pool pool,
                                                     const std::u16string &key);
    // Validate a successful SET/DELETE payload by its operation/pool and echoed key.
    static bool mutation_acknowledged(std::span<const uint8_t> payload, Operation operation,
                                      Pool pool, const std::u16string &key);
    // Decode a successful enumeration payload only for the requested pool/index.
    static std::optional<Record> decode_enumeration(std::span<const uint8_t> payload, Pool pool,
                                                    uint32_t index);
};
} // namespace pc

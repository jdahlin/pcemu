// Key-Value Pair (KVP) exchange lets a host query a guest-maintained collection
// of named values or explicitly create/replace/delete supported entries. A pool
// selector chooses the collection; a key is UTF-16 text; values are string,
// expandable string, unsigned 32-bit or unsigned 64-bit data. This file encodes
// only the standard payload inside an Integration Component (IC) message, not
// transport, negotiation, authorization or IP-address configuration operations.
//
// The fixed 2580-byte body starts with operation/pool and alignment padding.
// GET/SET carry Value at +4; ENUMERATE adds an index then Value at +8; DELETE
// carries a byte length and key. Value holds type, byte lengths, a 512-byte key
// array and a 2048-byte value array. String lengths include a two-byte NUL and
// are byte counts, not character counts. Supplementary Unicode characters use
// two UTF-16 code units. All integer/code-unit bytes are little-endian.
//
// IMPLEMENTATION NOTE: Strictly validate exact payload size, lengths, surrogate
// pairing, matching operation/pool/key/index and supported value types. Reject
// embedded NULs and isolated surrogate halves. Payload padding is zero on encode;
// the header alignment bytes must be zero on decode. SET/DELETE acknowledgment
// checks echoed identity, not persistence/readback. The host IC session enforces
// mutation pool policy and surrounding status/version/transaction checks. Encoding
// a request here has no guest-side effect and does not queue or send anything.
// Microsoft OpenVMM primary wire definitions (pinned): KvpHeader, Value,
// KvpMessage, MessageGetSet/Delete/Enumerate; Unicode 17.0, 3.8 / 3.9.2.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L29
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L83
// https://unicode.org/versions/Unicode17.0.0/core-spec/chapter-3/#G2630

#include "arm_hyperv_kvp_codec.hpp"
#include "arm_hyperv_ic_protocol.hpp"
#include <stdexcept>
namespace pc {
namespace {
using hyperv_ic::read_little_endian;
using hyperv_ic::write_little_endian;
// Standard payload has an operation/pool byte pair, two alignment bytes, then
// an operation-specific body. GET/SET start Value at +4; ENUMERATE adds an
// index u32 at +4 and starts Value at +8; DELETE has size u32 at +4 then key.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L29
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L113
struct PayloadField {
    enum : size_t {
        Operation = 0,
        Pool = 1,
        Reserved = 2,
        Body = 4,
        EnumerationIndex = 4,
        EnumerationValue = 8,
        DeleteKeyBytes = 4,
        DeleteKey = 8,
        GetSetType = 4,
        GetSetKeyBytes = 8,
        GetSetValueBytes = 12,
        GetSetKey = 16,
        GetSetValue = 528
    };
};
// Relative to Value, not the outer payload: type, key bytes, value bytes, key,
// then value. These offsets account for the 512-byte fixed key storage.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L83
struct ValueField {
    enum : size_t {
        Type = 0,
        KeyBytes = 4,
        ValueBytes = 8,
        Key = 12,
        Data = Key + ArmHyperVKvpCodec::MaximumKeyBytes
    };
};
constexpr size_t Utf16CodeUnitBytes = 2;
// UTF-16 supplementary characters require one leading D800h..DBFFh code unit
// followed by one trailing DC00h..DFFFh. Isolated halves are invalid Unicode.
// Unicode 17.0, 3.8 Surrogates, D71-D75 / 3.9.2 UTF-16.
// https://unicode.org/versions/Unicode17.0.0/core-spec/chapter-3/#G2630
constexpr uint16_t LeadingSurrogateFirst = 0xd800, LeadingSurrogateLast = 0xdbff,
                   TrailingSurrogateFirst = 0xdc00, TrailingSurrogateLast = 0xdfff;
// Validate UTF-16 surrogate pairing and forbid embedded NULs within the declared text.
// Empty strings are permitted here; key callers separately require a nonempty key.
// https://unicode.org/versions/Unicode17.0.0/core-spec/chapter-3/#G2630
bool valid_utf16_string(const std::u16string &s) {
    for (size_t i = 0; i < s.size(); ++i) {
        auto c = uint16_t(s[i]);
        if (!c)
            return false;
        if (c >= LeadingSurrogateFirst && c <= LeadingSurrogateLast) {
            if (++i == s.size() || s[i] < TrailingSurrogateFirst || s[i] > TrailingSurrogateLast)
                return false;
        } else if (c >= TrailingSurrogateFirst && c <= TrailingSurrogateLast)
            return false;
    }
    return true;
}
// Encode valid text in little-endian code units, leaving its NUL in zero-initialized storage.
// Require room for that terminator and throw before writing malformed/oversized text.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L83
void encode_utf16_string(std::span<uint8_t> b, size_t at, const std::u16string &s, size_t maximum) {
    if (s.size() >= maximum / Utf16CodeUnitBytes || !valid_utf16_string(s))
        throw std::invalid_argument("Invalid KVP UTF-16 string");
    for (size_t i = 0; i < s.size(); ++i)
        write_little_endian(b, at + Utf16CodeUnitBytes * i, s[i], Utf16CodeUnitBytes);
}
// Decode a bounded byte length including NUL, rejecting odd lengths, missing terminators or invalid
// UTF-16. Caller has validated the full fixed-size payload and the target array offset.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L83
std::optional<std::u16string> decode_utf16_string(std::span<const uint8_t> b, size_t at,
                                                  size_t size, size_t maximum) {
    if (size < Utf16CodeUnitBytes || size > maximum || (size & 1) ||
        read_little_endian(b, at + size - Utf16CodeUnitBytes, Utf16CodeUnitBytes))
        return {};
    std::u16string s;
    for (size_t i = 0; i < size - Utf16CodeUnitBytes; i += Utf16CodeUnitBytes)
        s += char16_t(read_little_endian(b, at + i, Utf16CodeUnitBytes));
    if (!valid_utf16_string(s))
        return {};
    return s;
}
} // namespace
// Encode a zero-padded GET/SET/DELETE/ENUMERATE body with operation-specific alignment.
// Validate parameter combinations, key/value bounds, type and DWORD range; this never transmits.
// IMPLEMENTATION NOTE: Pool write permissions belong to the host session, not this wire codec.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L113
std::vector<uint8_t> ArmHyperVKvpCodec::encode_request(Operation op, Pool pool, uint32_t index,
                                                       const std::u16string &key,
                                                       const Value *value) {
    if (unsigned(op) > unsigned(Operation::Enumerate) ||
        unsigned(pool) > unsigned(Pool::AutoExternal) ||
        (op != Operation::Enumerate && key.empty()) ||
        (op == Operation::Enumerate && (!key.empty() || value)) ||
        (op != Operation::Set && value) || (op == Operation::Set && !value))
        throw std::invalid_argument("Invalid KVP request");
    std::vector<uint8_t> b(PayloadBytes);
    b[PayloadField::Operation] = uint8_t(op);
    b[PayloadField::Pool] = uint8_t(pool);
    if (op == Operation::Enumerate) {
        write_little_endian(b, PayloadField::EnumerationIndex, index, 4);
        return b;
    }
    if (op == Operation::Delete) {
        encode_utf16_string(b, PayloadField::DeleteKey, key, MaximumKeyBytes);
        write_little_endian(b, PayloadField::DeleteKeyBytes, (key.size() + 1) * Utf16CodeUnitBytes,
                            4);
        return b;
    }
    encode_utf16_string(b, PayloadField::GetSetKey, key, MaximumKeyBytes);
    write_little_endian(b, PayloadField::GetSetKeyBytes, (key.size() + 1) * Utf16CodeUnitBytes, 4);
    if (op == Operation::Get)
        return b;
    write_little_endian(b, PayloadField::GetSetType, uint32_t(value->type), 4);
    switch (value->type) {
    case Type::String:
    case Type::ExpandString:
        encode_utf16_string(b, PayloadField::GetSetValue, value->text, MaximumValueBytes);
        write_little_endian(b, PayloadField::GetSetValueBytes,
                            (value->text.size() + 1) * Utf16CodeUnitBytes, 4);
        break;
    case Type::Dword:
        if (value->number > UINT32_MAX)
            throw std::invalid_argument("KVP DWORD overflow");
        write_little_endian(b, PayloadField::GetSetValueBytes, 4, 4);
        write_little_endian(b, PayloadField::GetSetValue, value->number, 4);
        break;
    case Type::Qword:
        write_little_endian(b, PayloadField::GetSetValueBytes, 8, 4);
        write_little_endian(b, PayloadField::GetSetValue, value->number, 8);
        break;
    default:
        throw std::invalid_argument("Unsupported KVP value type");
    }
    return b;
}
namespace {
// Decode Value at a validated payload offset, checking nonempty key and exact typed value lengths.
// Strings remain raw UTF-16, including REG_EXPAND_SZ; no environment expansion occurs here.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L83
std::optional<ArmHyperVKvpCodec::Record> decode_value_record(std::span<const uint8_t> b,
                                                             size_t at) {
    using C = ArmHyperVKvpCodec;
    auto key = decode_utf16_string(b, at + ValueField::Key,
                                   size_t(read_little_endian(b, at + ValueField::KeyBytes, 4)),
                                   C::MaximumKeyBytes);
    if (!key || key->empty())
        return {};
    C::Record r;
    r.key = std::move(*key);
    r.value.type = C::Type(read_little_endian(b, at + ValueField::Type, 4));
    auto size = read_little_endian(b, at + ValueField::ValueBytes, 4);
    switch (r.value.type) {
    case C::Type::String:
    case C::Type::ExpandString: {
        auto value =
            decode_utf16_string(b, at + ValueField::Data, size_t(size), C::MaximumValueBytes);
        if (!value)
            return {};
        r.value.text = std::move(*value);
        break;
    }
    case C::Type::Dword:
        if (size != 4)
            return {};
        r.value.number = read_little_endian(b, at + ValueField::Data, 4);
        break;
    case C::Type::Qword:
        if (size != 8)
            return {};
        r.value.number = read_little_endian(b, at + ValueField::Data, 8);
        break;
    default:
        return {};
    }
    return r;
}
// Require a full standard payload, matching operation/pool and zero two-byte alignment padding.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L146
bool valid_payload_header(std::span<const uint8_t> b, ArmHyperVKvpCodec::Operation op,
                          ArmHyperVKvpCodec::Pool pool) {
    return b.size() == ArmHyperVKvpCodec::PayloadBytes &&
           unsigned(pool) <= unsigned(ArmHyperVKvpCodec::Pool::AutoExternal) &&
           b[PayloadField::Operation] == uint8_t(op) && b[PayloadField::Pool] == uint8_t(pool) &&
           !read_little_endian(b, PayloadField::Reserved, Utf16CodeUnitBytes);
}
} // namespace
// Decode a successful enumeration body only for the expected pool/index; malformed values return
// none. Caller must already validate the surrounding IC version/transaction/success status.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L136
std::optional<ArmHyperVKvpCodec::Record>
ArmHyperVKvpCodec::decode_enumeration(std::span<const uint8_t> b, Pool pool, uint32_t index) {
    if (!valid_payload_header(b, Operation::Enumerate, pool) ||
        read_little_endian(b, PayloadField::EnumerationIndex, 4) != index)
        return {};
    return decode_value_record(b, PayloadField::EnumerationValue);
}
// Decode a successful GET body and require the exact requested UTF-16 key/pool.
// Caller must already validate the surrounding IC version/transaction/success status.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L113
std::optional<ArmHyperVKvpCodec::Record>
ArmHyperVKvpCodec::decode_get_response(std::span<const uint8_t> b, Pool pool,
                                       const std::u16string &key) {
    if (!valid_payload_header(b, Operation::Get, pool))
        return {};
    auto r = decode_value_record(b, PayloadField::Body);
    if (!r || r->key != key)
        return {};
    return r;
}
// Validate SET/DELETE acknowledgment by operation, pool and echoed nonempty key.
// Does not read back/compare a stored value or itself perform a guest mutation.
// https://github.com/microsoft/openvmm/blob/76420ea20a67ef65c80a62f1fc93de3b8f1eabaa/vm/devices/hyperv_ic_protocol/src/kvp.rs#L121
bool ArmHyperVKvpCodec::mutation_acknowledged(std::span<const uint8_t> b, Operation op, Pool pool,
                                              const std::u16string &key) {
    if ((op != Operation::Set && op != Operation::Delete) || !valid_payload_header(b, op, pool))
        return false;
    auto sizeAt =
             op == Operation::Set ? PayloadField::GetSetKeyBytes : PayloadField::DeleteKeyBytes,
         keyAt = op == Operation::Set ? PayloadField::GetSetKey : PayloadField::DeleteKey;
    auto returned =
        decode_utf16_string(b, keyAt, size_t(read_little_endian(b, sizeAt, 4)), MaximumKeyBytes);
    return returned && !returned->empty() && *returned == key;
}
} // namespace pc

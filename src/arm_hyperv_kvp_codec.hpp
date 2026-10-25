#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace pc {
// Standard (non-IP) KVP payload, inside an IC message. No guest mutations are
// performed here: encoding a SET/DELETE does not queue or transmit anything.
class ArmHyperVKvpCodec {
  public:
    enum class Operation : uint8_t { Get=0, Set=1, Delete=2, Enumerate=3 };
    enum class Pool : uint8_t { External=0, Guest=1, Auto=2, AutoExternal=3 };
    enum class Type : uint32_t { String=1, ExpandString=2, Dword=4, Qword=11 };
    struct Value { Type type=Type::String; std::u16string text; uint64_t number=0; };
    struct Record { std::u16string key; Value value; };
    static constexpr size_t payloadBytes=2580, maxKeyBytes=512, maxValueBytes=2048;
    static std::vector<uint8_t> request(Operation operation,Pool pool,uint32_t index=0,
                                       const std::u16string &key={},const Value *value=nullptr);
    // Caller must first verify the IC transaction/status/version. Rejects
    // malformed lengths, UTF-16, operation/pool/index and unsupported types.
    static std::optional<Record> enumeration(std::span<const uint8_t> payload,Pool pool,uint32_t index);
};
}

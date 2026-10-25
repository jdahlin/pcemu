#pragma once
#include "arm_synic.hpp"
#include "arm_vmbus.hpp"
#include "arm_hyperv_timers.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <utility>
#include <map>
#include <span>
#include <string>
namespace pc {
// ARM64 TLFS discovery and synthetic registers. No x86 CPUID/MSR emulation.
// Called only with all vCPUs stopped, just like the other ARM device models.
class ArmHyperV {
  public:
    using Registers = std::array<uint64_t, 18>;
    // Clock returns monotonic host time in 100 ns units; injectable for rate tests.
    using Clock = std::function<uint64_t()>;
    ArmHyperV(bool enabled, std::span<uint8_t> ram, uint64_t base, unsigned cpus, Clock clock = {});
    // False leaves the call to PSCI/the platform's unsupported-call fallback.
    bool call(unsigned immediate, Registers &registers, unsigned vp);
    using ContextSynchronizer = std::function<bool(uint64_t)>;
    void setContextSynchronizer(ContextSynchronizer synchronize) { contextSynchronizer = std::move(synchronize); }
    void setTranslationFlusher(ContextSynchronizer flush) { translationFlusher = std::move(flush); }
    using SoftwareInterruptSender = std::function<bool(uint64_t, unsigned)>;
    void setSoftwareInterruptSender(SoftwareInterruptSender send) { interruptSender = std::move(send); }
    void service();
    std::optional<uint64_t> serviceDelay100ns() const;
    bool timersActive() const { return enabled && timers.nextDeadline().has_value(); }
    void setPrivateInterruptSender(SoftwareInterruptSender send);
    void advertiseSynic(bool advertise);
    // Owns fixed VMBus bootstrap (4) and control (1) message connections.
    void enableVmbus();
    bool offerVmbusChannel(const ArmVmbus::Offer &offer, ArmVmbus::PacketHandler handler,
                          std::function<void()> closed = {});
    // Message payloads are borrowed only for the synchronous handler call.
    using MessageHandler = std::function<uint16_t(unsigned, uint32_t, std::span<const uint8_t>)>;
    using EventHandler = std::function<uint16_t(unsigned, uint16_t)>;
    // Trusted host setup only. IDs are partition-local, unscoped 24-bit IDs;
    // duplicate bindings and an unbounded number of endpoints are rejected.
    bool bindMessageConnection(uint32_t id, MessageHandler handler);
    bool bindEventConnection(uint32_t id, unsigned flagCount, EventHandler handler);
    bool removeConnection(uint32_t id);
    ArmSynic &synicController() { return synic; }
    std::string report() const;
    // One bounded last-event snapshot, drained by the platform after each HVC.
    std::string takeCrashReport();
  private:
    struct Counts {
        uint64_t reads = 0, writes = 0, failures = 0;
        // One last write per bounded SynIC/timer register counter, no payloads.
        std::array<uint64_t, 2> lastWrite{};
        uint64_t lastWritePadding = 0;
        uint32_t lastWriteNamePadding = 0;
        unsigned lastWriteVp = 0;
        bool lastWriteFast = false;
        uint16_t lastWriteStatus = 0;
    };
    bool enabled;
    bool synicAdvertised = false;
    ArmSynic synic;
    ArmVmbus vmbus;
    bool vmbusEnabled = false;
    std::map<uint32_t, uint32_t> vmbusEventPorts;
    ArmHyperVTimers timers;
    SoftwareInterruptSender privateInterruptSender;
    struct Connection { MessageHandler message; EventHandler event; unsigned flagCount = 0; };
    std::map<uint32_t, Connection> connections;
    uint64_t postedMessages = 0, signaledEvents = 0;
    struct PostSnapshot {
        bool observed = false, packetObserved = false;
        uint64_t control = 0, address = 0;
        unsigned vp = 0;
        uint16_t status = 0;
        uint32_t connectionId = 0, reserved = 0, type = 0, size = 0;
        std::string vmbusPrefixHex;
    } lastPost;
    bool allowPostMessages = false, allowSignalEvents = false;
    SoftwareInterruptSender interruptSender;
    uint64_t ipiRequests = 0, ipiTargetMask = 0;
    unsigned ipiVector = 0;
    std::array<uint64_t, 8> ipiTargetRequests{};
    std::span<uint8_t> ram;
    uint64_t base, guestId = 0, partitionInfo = 0;
    unsigned cpus;
    Clock clock;
    ContextSynchronizer contextSynchronizer, translationFlusher;
    uint64_t tlbFlushRequests = 0, tlbFlushTargetMask = 0, tlbFlushAddressSpace = 0, tlbFlushFlags = 0;
    uint64_t contextSyncRequests = 0, contextSyncTargetMask = 0;
    uint64_t clockOrigin, referenceTime = 0;
    std::array<uint64_t, 8> tlbiControl{}, syntheticVectors{};
    std::array<std::array<uint64_t, 5>, 8> crashParameters{};
    std::array<uint64_t, 5> notifiedParameters{};
    uint64_t crashNotifications = 0, crashControl = 0;
    unsigned crashVp = 0;
    std::string crashMessageHex;
    bool crashPending = false;
    std::string crashReport() const;
    uint64_t uidQueries = 0, versionQueries = 0, smcccCalls = 0, legacyCalls = 0;
    uint64_t disabledCalls = 0, failures = 0, omittedCounters = 0;
    uint64_t lastControl = 0, lastResult = 0;
    uint32_t lastRegister = 0;
    std::map<uint32_t, Counts> registers;
    std::map<uint16_t, uint64_t> calls, callFailures;
    uint64_t timeNow() const;
    uint64_t invoke(Registers &r, bool smccc, unsigned vp);
    uint16_t readRegister(uint32_t name, unsigned vp, std::array<uint64_t, 2> &value);
    uint16_t writeRegister(uint32_t name, unsigned vp, std::array<uint64_t, 2> value);
    Counts *count(uint32_t name);
};
} // namespace pc

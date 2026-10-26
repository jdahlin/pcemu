#pragma once
#include "arm_hyperv_heartbeat.hpp"
#include "arm_hyperv_kvp.hpp"
#include "arm_hyperv_reference_clock.hpp"
#include "arm_hyperv_shutdown.hpp"
#include "arm_hyperv_timers.hpp"
#include "arm_hyperv_timesync.hpp"
#include "arm_synic.hpp"
#include "arm_vmbus.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <utility>
namespace pc {
// ARM64 Microsoft TLFS subset: discovery, VP registers and host service routing.
// https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface
// Called only with all vCPUs stopped, just like the other ARM device models.
class ArmHyperV {
  public:
    using Registers = std::array<uint64_t, 18>;
    // Clock returns monotonic host time in 100 ns units; injectable for rate tests.
    using Clock = std::function<uint64_t()>;
    // Construct initial register state, clocks and timer delivery routing; reject unsupported VP
    // counts.
    ArmHyperV(bool enabled, std::span<uint8_t> ram, uint64_t base, unsigned cpus, Clock clock = {});
    // False leaves the call to PSCI/the platform's unsupported-call fallback.
    // Recognize SMCCC discovery/HVC calls and legacy HVC #1, update bounded diagnostics, and return
    // X0 status.
    bool handle_hypercall(unsigned immediate, Registers &registers, unsigned vp);
    using ContextSynchronizer = std::function<bool(uint64_t)>;
    // Install the stopped-VP native execution-context synchronization callback.
    // TLFS Hypercall Interface.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface
    void set_context_synchronizer(ContextSynchronizer synchronize) {
        contextSynchronizer = std::move(synchronize);
    }
    // Install native TLB (translation lookaside buffer) invalidation for requested VPs.
    // TLFS Hypercall Interface.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface
    void set_translation_flusher(ContextSynchronizer flush) {
        translationFlusher = std::move(flush);
    }
    using SoftwareInterruptSender = std::function<bool(uint64_t, unsigned)>;
    // Install native GIC software-interrupt delivery for cluster IPI requests.
    // TLFS Hypercall Interface.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface
    void set_software_interrupt_sender(SoftwareInterruptSender send) {
        interruptSender = std::move(send);
    }
    struct CounterSample {
        uint64_t value, frequency;
    };
    using CounterProvider = std::function<CounterSample()>;
    // Install once before guest identity/discovery. No provider means no feature.
    // Install a host counter faster than 10 MHz before guest identity; reject repeated/late setup.
    bool enable_reference_clock(CounterProvider provider);
    // Invalidate before a host counter discontinuity, then republish any registered reference page.
    void rebase_reference_clock(bool before);
    // Retry queued messages and integration packets, then service channels and due synthetic
    // timers.
    void service();
    // Return the next host wakeup delay, or none for idle state; queued work uses a one-millisecond
    // floor.
    std::optional<uint64_t> next_service_delay_100ns() const;
    // Report whether an enabled interface currently has any armed synthetic timer.
    // TLFS Timers.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/timers#synthetic-timers
    bool timers_active() const {
        return enabled && timers.nextDeadline().has_value();
    }
    // Install native private-interrupt delivery for SynIC/timers; an advertised backend cannot be
    // removed.
    void set_private_interrupt_sender(SoftwareInterruptSender send);
    // Expose SynIC features only with a real interrupt backend; VMBus prevents later withdrawal.
    void advertise_synic(bool advertise);
    // Owns fixed VMBus bootstrap (4) and control (1) message connections.
    // Bind bootstrap/control message connections and native SynIC event/message delivery once.
    void enable_vmbus();
    // Bind a channel event connection and inventory offer atomically; reject collisions/setup
    // failure.
    bool offer_vmbus_channel(const ArmVmbus::Offer &offer, ArmVmbus::PacketHandler handler,
                             std::function<void()> closed = {}, std::function<void()> opened = {});
    // Registers a real host initiated heartbeat IC in the VMBus inventory.
    // Offer a heartbeat IC once, wiring open/close/response events to its session.
    void enable_heartbeat();
    // UTC clock returns FILETIME; reference time remains the monotonic Clock.
    // Offer time synchronization once; default host UTC is converted to FILETIME separately from
    // monotonic time.
    void enable_time_sync(Clock utcClock = {});
    // Offer shutdown negotiation once; offering the channel does not request a power action.
    void enable_shutdown();
    // Offer key/value pair exchange once; no host query or mutation is issued by setup.
    void enable_kvp();
    // Queue a trusted host pool/index enumeration only while the enabled KVP session accepts work.
    bool enumerate_kvp_value(ArmHyperVKvpCodec::Pool pool, uint32_t index);
    // Queue a trusted host key lookup; return false when unavailable or busy.
    bool get_kvp_value(ArmHyperVKvpCodec::Pool pool, const std::u16string &key);
    // Queue an explicit trusted host mutation under the KVP session pool policy.
    bool set_kvp_value(ArmHyperVKvpCodec::Pool pool, const std::u16string &key,
                       const ArmHyperVKvpCodec::Value &value);
    // Queue an explicit trusted host key deletion under the KVP session pool policy.
    bool delete_kvp_value(ArmHyperVKvpCodec::Pool pool, const std::u16string &key);
    // Transfer the completed KVP result to its host consumer once.
    std::optional<ArmHyperVKvp::Result> take_kvp_result();
    // Trusted host request, with all VPs stopped. Acceptance is not poweroff.
    // Queue an explicit host power-action request; success means queued, not actual guest shutdown.
    bool request_shutdown(ArmHyperVShutdown::Action action, bool force = false,
                          uint32_t timeoutSeconds = 0);
    // Message payloads are borrowed only for the synchronous handler call.
    using MessageHandler = std::function<uint16_t(unsigned, uint32_t, std::span<const uint8_t>)>;
    using EventHandler = std::function<uint16_t(unsigned, uint16_t)>;
    // Trusted host setup only. IDs are partition-local, unscoped 24-bit IDs;
    // duplicate bindings and an unbounded number of endpoints are rejected.
    // Bind one bounded, unscoped message connection to a synchronous borrowed-payload callback.
    bool bind_message_connection(uint32_t id, MessageHandler handler);
    // Bind one bounded event connection with an explicit accepted flag range.
    bool bind_event_connection(uint32_t id, unsigned flagCount, EventHandler handler);
    // Remove a host binding except active VMBus-owned control and channel connections.
    bool remove_connection(uint32_t id);
    // Expose the owned SynIC for trusted stopped-VP platform setup/tests.
    // TLFS Hypercall Interface.
    // https://learn.microsoft.com/en-us/virtualization/hyper-v-on-windows/tlfs/hypercall-interface
    ArmSynic &synic_controller() {
        return synic;
    }
    // Serialize host counters, registration state and enabled subdevice reports without changing
    // guest state.
    std::string report() const;
    // One bounded last-event snapshot, drained by the platform after each HVC.
    // Drain one pending crash notification; repeated drains return an empty string.
    std::string take_crash_report();

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
    bool vmbusEnabled = false, heartbeatEnabled = false;
    ArmHyperVHeartbeat heartbeat;
    bool timesyncEnabled = false;
    ArmHyperVTimeSync timesync;
    Clock utcClock;
    bool shutdownEnabled = false;
    ArmHyperVShutdown shutdown;
    bool kvpEnabled = false;
    ArmHyperVKvp kvp;
    std::map<uint32_t, uint32_t> vmbusEventPorts;
    ArmHyperVReferenceClock referenceClock;
    CounterProvider counterProvider;
    // Pair a host architectural counter/frequency with current partition reference time.
    ArmHyperVReferenceClock::Sample reference_sample() const;
    ArmHyperVTimers timers;
    SoftwareInterruptSender privateInterruptSender;
    struct Connection {
        MessageHandler message;
        EventHandler event;
        unsigned flagCount = 0;
    };
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
    uint64_t tlbFlushRequests = 0, tlbFlushTargetMask = 0, tlbFlushAddressSpace = 0,
             tlbFlushFlags = 0;
    uint64_t contextSyncRequests = 0, contextSyncTargetMask = 0;
    uint64_t clockOrigin, referenceTime = 0;
    std::array<uint64_t, 8> tlbiControl{}, syntheticVectors{};
    std::array<std::array<uint64_t, 5>, 8> crashParameters{};
    std::array<uint64_t, 5> notifiedParameters{};
    uint64_t crashNotifications = 0, crashControl = 0;
    unsigned crashVp = 0;
    std::string crashMessageHex;
    bool crashPending = false;
    // Serialize the last immutable crash snapshot as bounded host JSON; do not reread guest RAM.
    std::string crash_report() const;
    uint64_t uidQueries = 0, versionQueries = 0, smcccCalls = 0, legacyCalls = 0;
    uint64_t disabledCalls = 0, failures = 0, omittedCounters = 0;
    uint64_t lastControl = 0, lastResult = 0;
    uint32_t lastRegister = 0;
    std::map<uint32_t, Counts> registers;
    std::map<uint16_t, uint64_t> calls, callFailures;
    // Sample elapsed monotonic 100 ns time without moving behind the last reference-register read.
    uint64_t reference_time_100ns() const;
    // Validate control/transport/target fields, dispatch an operation and report status or
    // completed repetitions.
    uint64_t invoke_hypercall(Registers &r, bool smccc, unsigned vp);
    // Read one named VP register, route SynIC/timers, and expose only supported discovery features.
    uint16_t read_vp_register(uint32_t name, unsigned vp, std::array<uint64_t, 2> &value);
    // Validate then update one named VP register or route it to the owning subdevice.
    uint16_t write_vp_register(uint32_t name, unsigned vp, std::array<uint64_t, 2> value);
    // Find/create a bounded host register-activity counter; count omissions without retaining
    // payloads.
    Counts *register_counters(uint32_t name);
};
} // namespace pc

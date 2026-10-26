#include "arm_hyperv.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <utility>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace pc {
namespace {
constexpr uint32_t uid = 0x8600ff01, function = 0x46000001;
constexpr uint16_t invalidCode = 2, invalidInput = 3, invalidAlignment = 4,
                   invalidParameter = 5, accessDenied = 6, invalidVp = 14;
constexpr uint64_t privileges = (1ull << 1) | (1ull << 5) | (1ull << 6) | (1ull << 49) | (1ull << 51) | (1ull << 52);
uint64_t get(std::span<const uint8_t> b, size_t at, size_t size) {
    uint64_t v = 0;
    for (size_t i = 0; i < size; ++i) v |= uint64_t(b[at + i]) << (8 * i);
    return v;
}
void put(std::span<uint8_t> b, size_t at, uint64_t v, size_t size) {
    for (size_t i = 0; i < size; ++i) b[at + i] = uint8_t(v >> (8 * i));
}
}
ArmHyperV::ArmHyperV(bool on, std::span<uint8_t> memory, uint64_t start, unsigned count, Clock source)
    : enabled(on), synic(memory, start, count), vmbus(memory, start, count), referenceClock(memory, start), timers(count), ram(memory), base(start), cpus(count), clock(std::move(source)) {
    if (!clock) clock = [] {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) / 100;
    };
    clockOrigin = clock();
    timers.setDelivery([this](unsigned vp, unsigned timer, uint64_t config, uint64_t expiration, uint64_t now) {
        if (config & 0x1000)
            return privateInterruptSender && privateInterruptSender(1ull << vp, unsigned((config >> 4) & 31));
        std::array<uint8_t, 24> payload{};
        put(payload, 0, timer, 4); put(payload, 8, expiration, 8); put(payload, 16, now, 8);
        return synic.try_post_message(vp, unsigned((config >> 16) & 15), 0x80000010, 0, payload) == 0;
    });
    if (!cpus || cpus > 8) throw std::runtime_error("Invalid Hyper-V VP count");
}
bool ArmHyperV::enableReferenceClock(CounterProvider provider) {
    if (!enabled || guestId || counterProvider || !provider || provider().frequency<=10000000) return false;
    counterProvider=std::move(provider);return true;
}
ArmHyperVReferenceClock::Sample ArmHyperV::referenceSample() const {
    auto sample=counterProvider();return {sample.value,timeNow(),sample.frequency};
}
void ArmHyperV::rebaseReferenceClock(bool before) {
    if (!counterProvider) return;
    if (before) referenceClock.invalidate_reference_page();
    else if (referenceClock.registration()&1) referenceClock.update_reference_mapping(referenceSample());
}
uint64_t ArmHyperV::timeNow() const {
    auto now = clock();
    return std::max(referenceTime, now >= clockOrigin ? now - clockOrigin : 0);
}
void ArmHyperV::service() {
    if (enabled) {
        synic.service_message_queues(); // A false injection keeps the queued message for retry.
        if (heartbeatEnabled) {
            auto now=timeNow();
            auto packet=heartbeat.packet(now);
            if(!packet.empty() && !vmbus.sendChannelPacket(ArmHyperVHeartbeat::channelId,packet))
                heartbeat.sent(now);
        }
        if (timesyncEnabled) {
            auto now=timeNow();
            auto due=timesync.delay(now);
            if(due && !*due) {
                auto packet=timesync.packet(now,utcClock());
                if(!packet.empty() && !vmbus.sendChannelPacket(ArmHyperVTimeSync::channelId,packet))
                    timesync.sent(now);
            }
        }
        if (shutdownEnabled) {
            auto now=timeNow();
            auto packet=shutdown.packet(now);
            if(!packet.empty() && !vmbus.sendChannelPacket(ArmHyperVShutdown::channelId,packet))
                shutdown.sent(now);
        }
        if (kvpEnabled) {
            auto now=timeNow();auto packet=kvp.packet(now);
            if(!packet.empty() && !vmbus.sendChannelPacket(ArmHyperVKvp::channelId,packet))kvp.sent(now);
        }
        if (vmbusEnabled) vmbus.service();
        timers.service(timeNow());
    }
}
std::optional<uint64_t> ArmHyperV::serviceDelay100ns() const {
    if (!enabled) return {};
    const auto now = timeNow();
    auto delay = timers.nextDelay(now, false);
    if (heartbeatEnabled) {
        auto next=heartbeat.delay(now);
        if(next) delay=std::min(delay.value_or(UINT64_MAX), std::max(*next,uint64_t(10000)));
    }
    if (timesyncEnabled) {
        auto next=timesync.delay(now);
        if(next) delay=std::min(delay.value_or(UINT64_MAX),std::max(*next,uint64_t(10000)));
    }
    if (shutdownEnabled) {
        auto next=shutdown.delay(now);
        if(next) delay=std::min(delay.value_or(UINT64_MAX),std::max(*next,uint64_t(10000)));
    }
    if (synic.needs_service() || (vmbusEnabled && vmbus.needsService())) return std::min(delay.value_or(10000), uint64_t(10000));
    if (kvpEnabled) {
        auto next=kvp.delay(now);
        if(next) delay=std::min(delay.value_or(UINT64_MAX),std::max(*next,uint64_t(10000)));
    }
    return delay;
}
void ArmHyperV::setPrivateInterruptSender(SoftwareInterruptSender send) {
    if (synicAdvertised && !send) throw std::runtime_error("Cannot remove an advertised SynIC interrupt backend");
    privateInterruptSender = std::move(send);
    synic.set_interrupt_sender(privateInterruptSender);
}
void ArmHyperV::advertiseSynic(bool advertise) {
    if (advertise && (!enabled || !privateInterruptSender))
        throw std::runtime_error("SynIC advertisement requires enabled Hyper-V and a private interrupt backend");
    if (!advertise && vmbusEnabled) throw std::runtime_error("Cannot disable SynIC with VMBus attached");
    synicAdvertised = advertise;
}
void ArmHyperV::enableVmbus() {
    if (vmbusEnabled) return;
    if (!enabled || !synicAdvertised || !privateInterruptSender)
        throw std::runtime_error("VMBus requires advertised SynIC with an interrupt backend");
    if (connections.contains(1) || connections.contains(4) || connections.size() > 62)
        throw std::runtime_error("VMBus control connections unavailable");
    vmbus.setSender([this](unsigned vp, unsigned sint, std::span<const uint8_t> payload) {
        return synic.post_message(vp, sint, 1, 0, payload);
    });
    bindMessageConnection(4, [this](unsigned, uint32_t type, std::span<const uint8_t> payload) {
        return vmbus.receive(type, payload, true);
    });
    bindMessageConnection(1, [this](unsigned, uint32_t type, std::span<const uint8_t> payload) {
        return vmbus.receive(type, payload, false);
    });
    vmbus.setEventSender([this](unsigned vp, unsigned sint, unsigned flag) {
        bool newly = false;
        return synic.signal_event(vp, sint, flag, newly);
    });
    // Grant the implemented event API. No channel event ports exist until an
    // actual channel is offered; unknown connections still fail, never succeed.
    allowSignalEvents = true;
    vmbusEnabled = true;
}
bool ArmHyperV::offerVmbusChannel(const ArmVmbus::Offer &offer, ArmVmbus::PacketHandler handler,
                                 std::function<void()> closed, std::function<void()> opened) {
    if (!vmbusEnabled || connections.contains(offer.connection)) return false;
    if (!bindEventConnection(offer.connection, 1, [this, id = offer.channel](unsigned, uint16_t) {
            return vmbus.signalChannel(id);
        })) return false;
    if (!vmbus.addChannel(offer, std::move(handler), std::move(closed), std::move(opened))) {
        connections.erase(offer.connection); return false;
    }
    vmbusEventPorts.emplace(offer.connection, offer.channel);
    return true;
}
void ArmHyperV::enableHeartbeat() {
    if(heartbeatEnabled) return;
    if(!offerVmbusChannel(ArmHyperVHeartbeat::offer(),
        [this](std::span<const uint8_t> packet) {
            heartbeat.receive(packet,timeNow()); return std::vector<uint8_t>{};
        }, [this] {heartbeat.closed();}, [this] {heartbeat.opened(timeNow());}))
        throw std::runtime_error("Heartbeat requires VMBus and an available channel/connection");
    heartbeatEnabled=true;
}
void ArmHyperV::enableTimeSync(Clock source) {
    if(timesyncEnabled) return;
    if(!source) source=[] {
        using Ticks=std::chrono::duration<int64_t,std::ratio<1,10000000>>;
        return ArmHyperVTimeSync::utcFromUnix100ns(std::chrono::duration_cast<Ticks>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    };
    if(!offerVmbusChannel(ArmHyperVTimeSync::offer(),
        [this](std::span<const uint8_t> packet) {
            timesync.receive(packet,timeNow()); return std::vector<uint8_t>{};
        },[this]{timesync.closed();},[this]{timesync.opened(timeNow());}))
        throw std::runtime_error("Timesync requires VMBus and an available channel/connection");
    utcClock=std::move(source);timesyncEnabled=true;
}
void ArmHyperV::enableShutdown() {
    if(shutdownEnabled) return;
    if(!offerVmbusChannel(ArmHyperVShutdown::offer(),
        [this](std::span<const uint8_t> packet) {
            shutdown.receive(packet,timeNow()); return std::vector<uint8_t>{};
        },[this]{shutdown.closed();},[this]{shutdown.opened(timeNow());}))
        throw std::runtime_error("Shutdown requires VMBus and an available channel/connection");
    shutdownEnabled=true;
}
void ArmHyperV::enableKvp() {
    if(kvpEnabled) return;
    if(!offerVmbusChannel(ArmHyperVKvp::offer(),
        [this](std::span<const uint8_t> packet) {
            kvp.receive(packet,timeNow());return std::vector<uint8_t>{};
        },[this]{kvp.closed();},[this]{kvp.opened(timeNow());}))
        throw std::runtime_error("KVP requires VMBus and an available channel/connection");
    kvpEnabled=true;
}
bool ArmHyperV::enumerateKvp(ArmHyperVKvpCodec::Pool pool,uint32_t index) {
    return enabled && kvpEnabled && kvp.enumerate(pool,index);
}
bool ArmHyperV::getKvp(ArmHyperVKvpCodec::Pool pool,const std::u16string &key) {
    return enabled && kvpEnabled && kvp.get(pool,key);
}
bool ArmHyperV::setKvp(ArmHyperVKvpCodec::Pool pool,const std::u16string &key,const ArmHyperVKvpCodec::Value &value) {
    return enabled && kvpEnabled && kvp.set(pool,key,value);
}
bool ArmHyperV::deleteKvp(ArmHyperVKvpCodec::Pool pool,const std::u16string &key) {
    return enabled && kvpEnabled && kvp.erase(pool,key);
}
std::optional<ArmHyperVKvp::Result> ArmHyperV::takeKvpResult() {
    return kvpEnabled ? kvp.takeResult() : std::nullopt;
}
bool ArmHyperV::requestShutdown(ArmHyperVShutdown::Action action,bool force,uint32_t seconds) {
    return enabled && shutdownEnabled && shutdown.request(action,force,seconds);
}
bool ArmHyperV::bindMessageConnection(uint32_t id, MessageHandler handler) {
    if (id > 0xffffff || !handler || connections.contains(id) || connections.size() >= 64) return false;
    connections.emplace(id, Connection{std::move(handler), {}, 0});
    allowPostMessages = true; return true;
}
bool ArmHyperV::bindEventConnection(uint32_t id, unsigned flagCount, EventHandler handler) {
    if (id > 0xffffff || !handler || !flagCount || flagCount > 65536 ||
        connections.contains(id) || connections.size() >= 64) return false;
    connections.emplace(id, Connection{{}, std::move(handler), flagCount});
    allowSignalEvents = true; return true;
}
bool ArmHyperV::removeConnection(uint32_t id) {
    if (vmbusEnabled && (id == 1 || id == 4 || vmbusEventPorts.contains(id))) return false;
    return connections.erase(id) != 0;
}
ArmHyperV::Counts *ArmHyperV::count(uint32_t name) {
    auto it = registers.find(name);
    if (it != registers.end()) return &it->second;
    if (registers.size() == 128) { ++omittedCounters; return nullptr; }
    return &registers[name];
}
uint16_t ArmHyperV::readRegister(uint32_t name, unsigned vp, std::array<uint64_t, 2> &value) {
    value = {};
    if (name >= 0xb0000 && name <= 0xb0007) {
        if (!guestId) return accessDenied;
        return timers.read(name, vp, value[0]);
    }
    if (name >= 0xa0000 && name <= 0xa0015) {
        if (!guestId) return accessDenied;
        return synic.read_vp_register(name, vp, value[0]);
    }
    if (name >= 0x210 && name <= 0x214) {
        value[0] = crashParameters[vp][name - 0x210]; return 0;
    }
    if (name == 0x215) { value[0] = (1ull << 63) | (1ull << 62); return 0; }
    switch (name) {
    case 0x100: value[0] = 1ull | (1ull << 48); return 0; // PCEmu interface version 1.0, build 1
    case 0x200:
        // WinHv requires the complete reference-counter/SynIC/timer/native
        // interrupt-controller/hypercall/VP-index privilege set. On ARM64,
        // interrupt-controller access is the architectural virtual GIC, not
        // the x64-only synthetic EOI/ICR/TPR register interface.
        value[0] = privileges | (counterProvider ? 1ull << 9 : 0) | (synicAdvertised ? 4 : 0) | (vmbusEnabled ? 0x18 : 0) | (allowPostMessages ? 1ull << 36 : 0) | (allowSignalEvents ? 1ull << 37 : 0);
        value[1] = 1ull << 8; return 0; // ARM64 bit 72: crash registers
    case 0x201: value[0] = 0xffffffff00000000ull | (interruptSender ? 4 : 0); return 0; // Recommend IPIs only with a real interrupt backend
    case 0x202: value[0] = cpus | (uint64_t(cpus) << 32); return 0;
    case 0x203: value[0] = 2; return 0; // Stage-2 translation, no synthetic interrupt remapping
    case 0x204: case 0x205: case 0x206: case 0x207: case 0x208: case 0x209: return 0;
    case 0x90002: value[0] = guestId; return 0;
    case 0x90004: {
        // Shared by all VPs, including early reads before GuestOsId. Dispatch
        // is serialized with all vCPUs stopped. Clamp equal-resolution samples
        // to maintain the TLFS strictly increasing access order.
        auto now = clock();
        auto elapsed = now >= clockOrigin ? now - clockOrigin : 0;
        referenceTime = std::max(referenceTime + 1, elapsed);
        value[0] = referenceTime; return 0;
    }
    case 0x90017:
        if (!counterProvider) return invalidParameter;
        if (!guestId) return accessDenied;
        value[0]=referenceClock.registration();return 0;
    case 0x90006: value[0] = 0x31237648; return 0; // ARM64 interface version: Hv#1
    case 0x90003:
        if (!guestId) return accessDenied;
        value[0] = vp; return 0;
    case 0x90015:
        if (!guestId) return accessDenied;
        value[0] = partitionInfo; return 0;
    case 0x90016:
        if (!guestId) return accessDenied;
        value[0] = tlbiControl[vp]; return 0;
    case 0x40400: value[0] = syntheticVectors[vp]; return 0;
    case 0x40401: return 0; // No synthetic exception has been injected.
    default: return invalidParameter;
    }
}
uint16_t ArmHyperV::writeRegister(uint32_t name, unsigned vp, std::array<uint64_t, 2> value) {
    if (name==0x90017) {
        if (!counterProvider) return invalidParameter;
        if (!guestId) return accessDenied;
        if (value[1]) return invalidParameter;
        return referenceClock.configure_reference_page(value[0], (value[0]&1) ? referenceSample() : ArmHyperVReferenceClock::Sample{}) ? 0 : invalidParameter;
    }
    if (name >= 0xb0000 && name <= 0xb0007) {
        if (!guestId) return accessDenied;
        if (value[1]) return invalidParameter;
        return timers.write(name, vp, value[0], timeNow());
    }
    if (name >= 0xa0000 && name <= 0xa0015) {
        if (!guestId) return accessDenied;
        if (value[1]) return invalidParameter;
        return synic.write_vp_register(name, vp, value[0]);
    }
    if (name >= 0x210 && name <= 0x214) {
        if (value[1]) return invalidParameter;
        crashParameters[vp][name - 0x210] = value[0]; return 0;
    }
    if (name == 0x215) {
        constexpr uint64_t notify = 1ull << 63, message = 1ull << 62;
        if (value[1] || (value[0] & ((1ull << 58) - 1)) ||
            (value[0] && !(value[0] & notify))) return invalidParameter;
        if (!value[0]) return 0;
        const auto &parameters = crashParameters[vp];
        std::string hex;
        if (value[0] & message) {
            auto address = parameters[3], size = parameters[4];
            if (size > 4096 || (size && (address < base || address - base > ram.size() ||
                size > ram.size() - (address - base)))) return invalidParameter;
            constexpr char digits[] = "0123456789abcdef";
            hex.reserve(size_t(size) * 2);
            for (size_t i = 0; i < size; ++i) {
                auto byte = ram[size_t(address - base) + i];
                hex += digits[byte >> 4]; hex += digits[byte & 15];
            }
        }
        // Copy before returning to the guest: later register/RAM writes must
        // not change already notified diagnostics. No guest shutdown is implied.
        notifiedParameters = parameters;
        crashControl = value[0]; crashVp = vp;
        crashMessageHex = std::move(hex);
        ++crashNotifications; crashPending = true;
        return 0;
    }
    if (name == 0x40400) {
        if (value[1] || (value[0] & 0x7ff)) return invalidParameter;
        // A guest VA, separate from the architectural VBAR_EL1. Optional
        // synthetic interrupt/exception producers remain disabled.
        syntheticVectors[vp] = value[0];
        return 0;
    }
    if (name == 0x90016) {
        if (!guestId) return accessDenied;
        if (value[1] || (value[0] & ~uint64_t(1))) return invalidParameter;
        // Per-VP TLBI opt-in. Architectural TLBI still executes through HVF;
        // this backend does not queue additional asynchronous flush actions.
        tlbiControl[vp] = value[0];
        return 0;
    }
    if (name == 0x90015) {
        if (!guestId) return accessDenied;
        if (value[1] || (value[0] & 0xffe)) return invalidParameter;
        if (value[0] & 1) {
            uint64_t address = value[0] & ~uint64_t(4095);
            if (address < base || address - base > ram.size() ||
                4096 > ram.size() - (address - base)) return invalidParameter;
            // Windows supplies this shared page. No asynchronous address-space
            // flushes are queued by this backend: its pending-flush word is zero.
            // All remaining fields are reserved/zero in this initial interface.
            std::fill_n(ram.begin() + size_t(address - base), 4096, uint8_t(0));
        }
        partitionInfo = value[0];
        return 0;
    }
    if (name != 0x90002 || value[1]) return invalidParameter;
    if (vp != 0) return accessDenied;
    guestId = value[0];
    return 0;
}
bool ArmHyperV::call(unsigned immediate, Registers &r, unsigned vp) {
    if (immediate > 1) return false;
    if (!immediate && uint32_t(r[0]) == uid) {
        ++uidQueries;
        if (!enabled) return false;
        r[0] = 0x4d32ba58; r[1] = 0xcd244764; r[2] = 0x8eef6c75; r[3] = 0x16597024;
        return true;
    }
    if (!immediate && (uint32_t(r[0]) == 0x80000000 || uint32_t(r[0]) == 0x80000001)) {
        ++versionQueries;
        if (!enabled) return false;
        if (uint32_t(r[0]) == 0x80000000) r[0] = 0x10002; // SMCCC 1.2
        else r[0] = (uint32_t(r[1]) == uid || uint32_t(r[1]) == function ||
                     uint32_t(r[1]) == 0x80000000 || uint32_t(r[1]) == 0x80000001) ? 0 : uint64_t(-1);
        return true;
    }
    if (!immediate && uint32_t(r[0]) != function) return false;
    if (immediate) ++legacyCalls; else ++smcccCalls;
    lastControl = r[immediate ? 0 : 1];
    auto code = uint16_t(lastControl);
    if (calls.contains(code) || calls.size() < 128) ++calls[code]; else ++omittedCounters;
    if (!enabled) { ++disabledCalls; lastResult = uint64_t(-1); return false; }
    if (code == 0x5c) {
        lastPost = {};
        lastPost.observed = true; lastPost.control = lastControl;
        lastPost.address = (lastControl & 0x10000) ? 0 : r[immediate ? 1 : 2];
        lastPost.vp = vp;
    }
    lastResult = invoke(r, !immediate, vp);
    if (code == 0x5c) lastPost.status = uint16_t(lastResult);
    if (uint16_t(lastResult)) {
        ++failures;
        if (calls.contains(code)) ++callFailures[code];
    }
    r[0] = lastResult;
    return true;
}
uint64_t ArmHyperV::invoke(Registers &r, bool smccc, unsigned vp) {
    auto control = r[smccc ? 1 : 0];
    auto code = uint16_t(control);
    if (code == 0x5c || code == 0x5d) {
        if (!guestId || (code == 0x5c ? !allowPostMessages : !allowSignalEvents)) return accessDenied;
        if (vp >= cpus || (control & ~uint64_t(0x1ffff))) return invalidInput;
        const bool message = code == 0x5c;
        const bool fast = control & 0x10000;
        const size_t size = message ? 256 : 8;
        const unsigned in = smccc ? 2 : 1;
        std::array<uint8_t, 256> bytes{};
        if (fast) {
            // The fixed PostMessage packet is larger than ARM's 128-byte
            // fast input area. SignalEvent fits in one register pair.
            if (message) return invalidInput;
            put(bytes, 0, r[in], 8);
        } else {
            const auto address = r[in];
            if (address & 7) return invalidAlignment;
            if (size > 4096 - (address & 4095) || address < base ||
                address - base > ram.size() || size > ram.size() - (address - base)) return invalidInput;
            std::copy_n(ram.begin() + size_t(address - base), size, bytes.begin());
        }
        const auto id = uint32_t(get(bytes, 0, 4));
        if (message) {
            const auto type = uint32_t(get(bytes, 8, 4));
            const auto length = get(bytes, 12, 4);
            lastPost.packetObserved = true; lastPost.connectionId = id;
            lastPost.reserved = uint32_t(get(bytes, 4, 4));
            lastPost.type = type; lastPost.size = uint32_t(length);
            // Only the bounded VMBus control prefix, never generic payloads.
            if (vmbusEnabled && ((id & 0xffffff) == 1 || (id & 0xffffff) == 4) && type == 1 && length <= 240) {
                constexpr char hex[] = "0123456789abcdef";
                for (size_t i = 0; i < std::min<size_t>(size_t(length), 56); ++i) {
                    auto b = bytes[16 + i];
                    lastPost.vmbusPrefixHex += hex[b >> 4]; lastPost.vmbusPrefixHex += hex[b & 15];
                }
            }
            // WinHv's reusable PostMessage buffer leaves this alignment word
            // untouched, just like register-association padding.
            if (!type || (type & 0x80000000) || length > 240) return invalidParameter;
        } else if (get(bytes, 6, 2)) return invalidParameter;
        auto found = connections.find(id);
        if (found == connections.end()) return 0x12; // InvalidConnectionId, including unsupported scopes.
        // Copy callbacks so an endpoint can safely remove its own binding.
        auto endpoint = found->second;
        uint16_t status;
        if (message) {
            if (!endpoint.message) return 0x11; // InvalidPortId: wrong endpoint type.
            status = endpoint.message(vp, uint32_t(get(bytes, 8, 4)),
                std::span<const uint8_t>(bytes).subspan(16, size_t(get(bytes, 12, 4))));
            if (!status) ++postedMessages;
        } else {
            if (!endpoint.event) return 0x11;
            const auto flag = uint16_t(get(bytes, 4, 2));
            if (flag >= endpoint.flagCount) return invalidParameter;
            status = endpoint.event(vp, flag);
            if (!status) ++signaledEvents;
        }
        return status;
    }
    if (code == 0xd6) {
        // ARM enlightened TLB drain: Windows supplies no input or output
        // parameters, after consulting its partition-info pending-flush word.
        if (!guestId) return accessDenied;
        if ((control & ~uint64_t(0x1ffff)) || vp >= cpus) return invalidInput;
        if (!translationFlusher) return invalidCode;
        const uint64_t targets = 1ull << vp;
        if (!translationFlusher(targets)) return 0x71;
        ++tlbFlushRequests; tlbFlushTargetMask = targets;
        tlbFlushAddressSpace = 0; tlbFlushFlags = 0;
        return 0;
    }
    if (code == 0x2 || code == 0x13 || code == 0x19 || code == 0x1a || code == 0xb || code == 0x15) {
        if (!guestId) return accessDenied;
        const bool flush = code == 0x2 || code == 0x13;
        const bool ipi = code == 0xb || code == 0x15;
        const bool extended = code == 0x13 || code == 0x1a || code == 0x15;
        const uint64_t allowed = extended ? 0x7ffffff : 0x1ffff;
        if ((control & ~allowed) || vp >= cpus) return invalidInput;
        size_t banks = extended ? (control >> 17) & 0x3ff : 0;
        if (banks > 64) return invalidInput;
        size_t prefix = flush ? 16 : 8;
        size_t size = prefix + (extended ? 16 + banks * 8 : 8);
        unsigned in = smccc ? 2 : 1;
        std::vector<uint8_t> input(size);
        if (control & 0x10000) {
            if (size > 128) return invalidInput;
            for (size_t i = 0; i < size; ++i) input[i] = uint8_t(r[in + i / 8] >> ((i % 8) * 8));
        } else {
            auto address = r[in];
            if ((address & 7) || size > 4096 - (address & 4095) || address < base ||
                address - base > ram.size() || size > ram.size() - (address - base)) return invalidAlignment;
            std::copy_n(ram.begin() + size_t(address - base), size, input.begin());
        }
        auto flags = get(input, prefix - 8, 8);
        if (flush && (flags & ~uint64_t(7))) return invalidParameter;
        // ARM64 IPI vectors are GIC SGI IDs. Accept current VTL0 or explicit
        // VTL0 (UseTargetVtl bit), with all remaining flag/reserved bytes zero.
        if (ipi && ((flags & ~0x1fffffffffull) || uint32_t(flags) >= 16 ||
            ((flags & (0x10ull << 32)) && (flags & (0xfull << 32))))) return invalidParameter;
        const bool allProcessors = flush && (flags & 1);
        uint64_t targets = 0;
        const uint64_t valid = (1ull << cpus) - 1;
        if (!extended) {
            targets = get(input, prefix, 8);
            if (!flush && !ipi && (flags > 1 || (flags && targets))) return invalidParameter;
            if (allProcessors || (!flush && !ipi && flags)) targets = valid;
        } else {
            // HV_VP_SET follows the operation's fixed prefix. Only its bank
            // contents belong to the variable input header.
            if (!flush && !ipi && flags) return invalidParameter;
            auto format = get(input, prefix, 8), validBanks = get(input, prefix + 8, 8);
            if (banks != size_t(std::popcount(validBanks))) return invalidInput;
            if (format == 1) {
                if (validBanks) return invalidParameter;
                targets = valid;
            } else if (!format) {
                size_t at = prefix + 16;
                for (unsigned bank = 0; bank < 64; ++bank) if (validBanks & (1ull << bank)) {
                    auto bits = get(input, at, 8); at += 8;
                    // Empty banks are legal even beyond the configured VP range.
                    if (bank && bits && !allProcessors) return invalidVp;
                    if (!bank) targets = bits;
                }
            } else return invalidParameter;
        }
        if (allProcessors) targets = valid; // The supplied processor mask is ignored.
        if (targets & ~valid) return invalidVp;
        if (ipi) {
            if (!interruptSender) return invalidCode;
            if (!interruptSender(targets, uint32_t(flags))) return 0x71;
            ++ipiRequests; ipiTargetMask = targets; ipiVector = uint32_t(flags);
            for (unsigned cpu = 0; cpu < cpus; ++cpu) if (targets & (1ull << cpu)) ++ipiTargetRequests[cpu];
            return 0;
        }
        if (flush) {
            if (!translationFlusher) return invalidCode;
            // The native backend conservatively flushes all ASIDs/global
            // mappings, which also covers any requested address-space subset.
            if (!translationFlusher(targets)) return 0x71;
            ++tlbFlushRequests; tlbFlushTargetMask = targets;
            tlbFlushAddressSpace = get(input, 0, 8); tlbFlushFlags = flags;
            return 0;
        }
        if (!contextSynchronizer) return invalidCode;
        if (!contextSynchronizer(targets)) return 0x71; // HV_STATUS_OPERATION_FAILED
        ++contextSyncRequests; contextSyncTargetMask = targets;
        return 0;
    }
    if (code == 0x8001) {
        if (!guestId) return accessDenied;
        if ((control & ~uint64_t(0x1ffff)) || vp >= cpus) return invalidInput;
        if (control & 0x10000) {
            // No input; one 16-byte output pair. Only its low 8 bytes are
            // defined. Clear the padding as for GetVpRegisters fast output.
            unsigned out = smccc ? 2 : 15;
            r[out] = r[out + 1] = 0;
        } else {
            auto address = r[smccc ? 3 : 2];
            if ((address & 7) || address < base || address - base > ram.size() ||
                8 > ram.size() - (address - base)) return invalidAlignment;
            // No 0x8002+ service is currently provided. A zero capability
            // bitmap is the truthful supported-set response, not a stub action.
            std::fill_n(ram.begin() + size_t(address - base), 8, uint8_t(0));
        }
        return 0;
    }
    if (code != 0x50 && code != 0x51) return guestId ? invalidCode : accessDenied;
    // Only Fast, Code, RepCount and RepStart are supported. No nested/variable headers.
    constexpr uint64_t allowed = 0x0fff0fff0001ffffull;
    if (control & ~allowed) return invalidInput;
    size_t reps = (control >> 32) & 0xfff, first = (control >> 48) & 0xfff;
    if (!reps || first >= reps || vp >= cpus) return invalidInput;
    bool fast = control & 0x10000, write = code == 0x51;
    size_t inputSize = 16 + reps * (write ? 32 : 4), outputSize = write ? 0 : reps * 16;
    if (inputSize > 4096 || outputSize > 4096) return invalidInput;
    std::vector<uint8_t> input(inputSize);
    std::span<uint8_t> output;
    unsigned inputStart = smccc ? 2 : 1, outputStart = 0;
    auto memory = [&](uint64_t address, size_t size) -> std::span<uint8_t> {
        if ((address & 7) || size > 4096 - (address & 4095) || address < base ||
            address - base > ram.size() || size > ram.size() - (address - base)) return {};
        return ram.subspan(size_t(address - base), size);
    };
    if (fast) {
        auto inputPairs = (inputSize + 15) / 16, outputPairs = outputSize / 16;
        if (inputPairs + outputPairs > 8) return invalidInput;
        for (size_t i = 0; i < inputSize; ++i) input[i] = uint8_t(r[inputStart + i / 8] >> ((i % 8) * 8));
        // OpenVMM ARM64 ABI: SMCCC output follows 16-byte input pairs;
        // legacy HVC #1 output occupies the final pairs ending at X16.
        outputStart = unsigned(smccc ? inputStart + inputPairs * 2 : 17 - outputPairs * 2);
    } else {
        auto in = memory(r[inputStart], inputSize);
        if (in.empty()) return invalidAlignment;
        std::copy(in.begin(), in.end(), input.begin());
        if (outputSize) {
            auto a = r[inputStart], b = r[inputStart + 1];
            output = memory(b, outputSize);
            if (output.empty() || (a < b + outputSize && b < a + inputSize)) return invalidAlignment;
        }
    }
    if (get(input, 0, 8) != uint64_t(-1)) return accessDenied;
    auto target = get(input, 8, 4);
    if (target != 0xfffffffe && target != vp) return invalidVp;
    if (get(input, 12, 4)) return invalidParameter; // VTL0 only, reserved bytes zero
    for (size_t i = first; i < reps; ++i) {
        size_t at = 16 + i * (write ? 32 : 4);
        auto name = uint32_t(get(input, at, 4)); lastRegister = name;
        auto counters = count(name);
        if (counters) { if (write) ++counters->writes; else ++counters->reads; }
        std::array<uint64_t, 2> value{};
        uint16_t status;
        if (write) {
            const bool synthetic = (name >= 0xa0000 && name <= 0xa0015) || (name >= 0xb0000 && name <= 0xb0007);
            if (counters && synthetic) {
                counters->lastWrite = {get(input, at + 16, 8), get(input, at + 24, 8)};
                counters->lastWriteVp = vp;
                counters->lastWriteNamePadding = uint32_t(get(input, at + 4, 4));
                counters->lastWritePadding = get(input, at + 8, 8);
                counters->lastWriteFast = fast;
            }
            // Windows WinHv reuses register-association buffers without
            // initializing the 12 alignment bytes. Like OpenVMM, consume only
            // the name and value; header/VTL and register bits stay validated.
            status = writeRegister(name, vp, {get(input, at + 16, 8), get(input, at + 24, 8)});
            if (counters && synthetic) counters->lastWriteStatus = status;
        } else status = readRegister(name, vp, value);
        if (status) { if (counters) ++counters->failures; return (uint64_t(i) << 32) | status; }
        if (!write) {
            if (fast) { r[outputStart + i * 2] = value[0]; r[outputStart + i * 2 + 1] = value[1]; }
            else { put(output, i * 16, value[0], 8); put(output, i * 16 + 8, value[1], 8); }
        }
    }
    return uint64_t(reps) << 32;
}
std::string ArmHyperV::crashReport() const {
    std::ostringstream s;
    s << "{\"notifications\":" << crashNotifications << ",\"vp\":" << crashVp
      << ",\"control\":" << crashControl << ",\"parameters\":[";
    for (unsigned i = 0; i < 5; ++i) { if (i) s << ','; s << notifiedParameters[i]; }
    s << "],\"messageHex\":\"" << crashMessageHex << "\"}";
    return s.str();
}
std::string ArmHyperV::takeCrashReport() {
    if (!crashPending) return {};
    crashPending = false;
    return crashReport();
}
std::string ArmHyperV::report() const {
    std::ostringstream s;
    s << "{\"schema\":1,\"mode\":\"" << (enabled ? (vmbusEnabled ? "vmbus" : (synicAdvertised ? "synic" : "basic")) : "off") << "\",\"guestOsId\":" << guestId
      << ",\"partitionInfoPage\":" << partitionInfo
      << ",\"referenceTime100ns\":" << referenceTime
      << ",\"referenceTscPage\":" << referenceClock.registration()
      << ",\"referenceTscSequence\":" << referenceClock.sequence()
      << ",\"syntheticTimerDeliveries\":" << timers.delivered()
      << ",\"postedMessages\":" << postedMessages << ",\"signaledEvents\":" << signaledEvents
      << ",\"lastMessagePost\":{\"observed\":" << (lastPost.observed ? "true" : "false")
      << ",\"packetObserved\":" << (lastPost.packetObserved ? "true" : "false")
      << ",\"control\":" << lastPost.control << ",\"address\":" << lastPost.address
      << ",\"vp\":" << lastPost.vp << ",\"status\":" << lastPost.status
      << ",\"connectionId\":" << lastPost.connectionId << ",\"reserved\":" << lastPost.reserved
      << ",\"messageType\":" << lastPost.type << ",\"payloadBytes\":" << lastPost.size
      << ",\"vmbusPrefixHex\":\"" << lastPost.vmbusPrefixHex << "\"}"
      << ",\"ipiRequests\":" << ipiRequests << ",\"ipiTargetMask\":" << ipiTargetMask
      << ",\"ipiVector\":" << ipiVector
      << ",\"crash\":" << crashReport()
      << ",\"tlbFlushRequests\":" << tlbFlushRequests
      << ",\"tlbFlushTargetMask\":" << tlbFlushTargetMask
      << ",\"tlbFlushAddressSpace\":" << tlbFlushAddressSpace
      << ",\"tlbFlushFlags\":" << tlbFlushFlags
      << ",\"contextSyncRequests\":" << contextSyncRequests
      << ",\"contextSyncTargetMask\":" << contextSyncTargetMask
      << ",\"uidQueries\":" << uidQueries << ",\"smcccVersionQueries\":" << versionQueries
      << ",\"smcccCalls\":" << smcccCalls << ",\"legacyCalls\":" << legacyCalls
      << ",\"disabledCalls\":" << disabledCalls << ",\"failures\":" << failures
      << ",\"lastControl\":" << lastControl << ",\"lastResult\":" << lastResult
      << ",\"lastRegister\":" << lastRegister << ",\"omittedCounters\":" << omittedCounters;
    const auto &activity = timers.activity();
    s << ",\"timerActivity\":{\"configWrites\":" << activity.configWrites
      << ",\"countWrites\":" << activity.countWrites
      << ",\"repeatedConfigWrites\":" << activity.repeatedConfigWrites
      << ",\"repeatedCountWrites\":" << activity.repeatedCountWrites
      << ",\"zeroCountWrites\":" << activity.zeroCountWrites
      << ",\"autoEnableWrites\":" << activity.autoEnableWrites
      << ",\"periodicRearms\":" << activity.periodicRearms
      << ",\"unchangedArmedDeadline\":" << activity.unchangedArmedDeadline
      << ",\"serviceCalls\":" << timers.serviceCalls()
      << ",\"serviceTimerVisits\":" << timers.serviceTimerVisits()
      << ",\"deadlineQueries\":" << timers.deadlineQueries()
      << ",\"deadlineTimerVisits\":" << timers.deadlineTimerVisits()
      << ",\"deadlineRebuilds\":" << timers.deadlineRebuilds()
      << ",\"serviceFastReturns\":" << timers.serviceFastReturns()
      << ",\"deliveryRetries\":" << timers.deliveryRetries()
      << ",\"configFlags\":[";
    for (unsigned i = 0; i < activity.configFlags.size(); ++i) {
        if (i) s << ',';
        s << activity.configFlags[i];
    }
    s << "]},\"calls\":[";
    bool comma = false;
    for (auto [code, n] : calls) { if (comma) s << ','; comma = true; s << "{\"code\":" << code << ",\"count\":" << n
          << ",\"failures\":" << (callFailures.contains(code) ? callFailures.at(code) : 0) << '}'; }
    s << "],\"ipiTargetRequests\":[";
    for (unsigned cpu = 0; cpu < cpus; ++cpu) { if (cpu) s << ','; s << ipiTargetRequests[cpu]; }
    s << "],\"registers\":["; comma = false;
    for (auto [name, n] : registers) {
        if (comma) s << ','; comma = true;
        s << "{\"name\":" << name << ",\"reads\":" << n.reads << ",\"writes\":" << n.writes << ",\"failures\":" << n.failures;
        if (n.writes && ((name >= 0xa0000 && name <= 0xa0015) || (name >= 0xb0000 && name <= 0xb0007)))
            s << ",\"lastWriteLow\":" << n.lastWrite[0] << ",\"lastWriteHigh\":" << n.lastWrite[1]
              << ",\"lastWriteVp\":" << n.lastWriteVp << ",\"lastWriteStatus\":" << n.lastWriteStatus
              << ",\"lastWriteNamePadding\":" << n.lastWriteNamePadding << ",\"lastWritePadding\":" << n.lastWritePadding
              << ",\"lastWriteFast\":" << (n.lastWriteFast ? "true" : "false");
        s << '}';
    }
    s << "],\"vps\":[";
    for (unsigned vp = 0; vp < cpus; ++vp) {
        if (vp) s << ',';
        s << "{\"index\":" << vp << ",\"tlbiControl\":" << tlbiControl[vp]
          << ",\"syntheticVector\":" << syntheticVectors[vp] << '}';
    }
    s << ']';
    if (synicAdvertised) s << ",\"synic\":" << synic.report();
    if (vmbusEnabled) s << ",\"vmbus\":" << vmbus.report();
    if (kvpEnabled) s << ",\"kvp\":" << kvp.report();
    if (shutdownEnabled) s << ",\"shutdown\":" << shutdown.report();
    if (timesyncEnabled) s << ",\"timesync\":" << timesync.report();
    if (heartbeatEnabled) s << ",\"heartbeat\":" << heartbeat.report();
    return s.str() + "}\n";
}
} // namespace pc

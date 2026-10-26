#pragma once
#include "arm_virt.hpp"
#include "state_archive.hpp"
namespace pc {
// Host-only explicit state inventory. Device callbacks/RAM spans retain the
// bindings made by the fresh machine constructor; live queues and registers
// retain their architectural meaning. These fields are not a hardware wire ABI.
template<class T> void ArmSnapshotAccess::fields(StateArchive &a, T &v) {
    if constexpr (std::is_same_v<T,VirtioBlock>) {
        a(v.featureSelect, v.driverSelect, v.queueSelect, v.queueSize, v.ready, v.status, v.interruptStatus, v.driverFeatures, v.descriptors, v.available, v.used, v.lastAvailable, v.usedIndex, v.failure, v.requests, v.bytesRead, v.bytesWritten);
    }
    else if constexpr (std::is_same_v<T,VirtioPciDevice>) {
        a(v.queueCount, v.config, v.bar, v.barProbe, v.irqLevel, v.command, v.msixControl, v.configVector, v.queueVectors, v.vectorTable, v.pendingBits, v.messageInterrupts);
    }
    else if constexpr (std::is_same_v<T,VirtioNet>) {
        a(v.queues, v.featureSelect, v.driverSelect, v.queueSelect, v.status, v.isr, v.driverFeatures, v.signaledQueue, v.failure, v.outgoing, v.transmitted, v.received, v.resetCount);
    }
    else if constexpr (std::is_same_v<T,VirtioRng>) {
        a(v.featureSelect, v.driverSelect, v.queueSelect, v.queueSize, v.ready, v.status, v.interruptStatus, v.driverFeatures, v.descriptors, v.available, v.used, v.lastAvailable, v.usedIndex, v.failure);
    }
    else if constexpr (std::is_same_v<T,VirtioDevice>) {
        a(v.featureSelect, v.driverSelect, v.queueSelect, v.status, v.interruptStatus, v.generation, v.driverFeatures, v.lastInterruptQueue, v.queues, v.failure, v.completions, v.resetCount);
    }
    else if constexpr (std::is_same_v<T,VirtioSerial>) {
        a(static_cast<VirtioDevice &>(v));
        a(v.ports, v.deviceReady, v.controls);
    }
    else if constexpr (std::is_same_v<T,VirtioSerial::Port>) {
        a(v.name, v.hostOpen, v.guestOpen, v.ready, v.incoming, v.outgoing);
    }
    else if constexpr (std::is_same_v<T,VirtioBalloon>) {
        a(static_cast<VirtioDevice &>(v));
        a(v.maximumPages, v.target, v.actual, v.inflatedCount, v.inflated, v.discarded, v.wantStats, v.stats);
    }
    else if constexpr (std::is_same_v<T,VirtioGpu>) {
        a(static_cast<VirtioDevice &>(v));
        a(v.resources, v.allocated, v.lastFrame, v.lastWidth, v.lastHeight, v.scanoutResource, v.cursorResource, v.cursorX, v.cursorY, v.hotX, v.hotY, v.scanout, v.events, v.generation);
        if (a.loading) { v.displayedGeneration = 0; v.displayedComposite = true; }
    }
    else if constexpr (std::is_same_v<T,VirtioGpu::Resource>) {
        a(v.width, v.height, v.format, v.data, v.backing, v.backingBytes);
    }
    else if constexpr (std::is_same_v<T,UsbXhci>) {
        a(v.bar, v.pciCommand, v.usbcmd, v.status, v.iman, v.erstSize, v.config, v.imod, v.crcr, v.dcbaa, v.erst, v.erdp, v.eventBase, v.eventIndex, v.eventCount, v.outstanding, v.eventCycle, v.probe, v.commandRing, v.slots, v.ports, v.pending, v.keys, v.buttons, v.tabletX, v.tabletY, v.failure, v.keyInputs, v.pointerInputs);
    }
    else if constexpr (std::is_same_v<T,UsbXhci::Slot>) {
        a(v.enabled, v.port, v.endpoints, v.active, v.control, v.actual, v.shortPacket, v.controlIn, v.configuration, v.protocol, v.idle);
    }
    else if constexpr (std::is_same_v<T,ArmFwCfg>) {
        a(v.items, v.directory, v.cursor, v.dmaAddress, v.selected, v.cpus);
    }
    else if constexpr (std::is_same_v<T,ArmFwCfg::Item>) {
        a(v.name, v.bytes, v.writable);
    }
    else if constexpr (std::is_same_v<T,ArmNorFlash>) {
        a(v.pending, v.data, v.visible, v.buffer, v.statusLength, v.statusAddress, v.blockAddress, v.remaining, v.dirtyStart, v.dirtyEnd);
    }
    else if constexpr (std::is_same_v<T,ArmPl011>) {
        a(v.receive, v.transcript, v.control, v.integer_divisor, v.fractional_divisor, v.line_control, v.interrupt_mask, v.fifo_levels);
    }
    else if constexpr (std::is_same_v<T,ArmOsLock>) {
        a(v.locked, v.doubleLocked);
    }
    else if constexpr (std::is_same_v<T,ArmSynic>) {
        a(v.state);
    }
    else if constexpr (std::is_same_v<T,ArmSynic::Vp>) {
        a(v.control, v.events, v.messages, v.sint, v.queue);
    }
    else if constexpr (std::is_same_v<T,ArmHyperVTimers>) {
        a(v.lastSample, v.sampled, v.timers, v.deliveries, v.writes, v.services, v.visits, v.retries, v.fastReturns);
        if (a.loading) { v.deadlinesDirty = true; v.earliest.reset(); v.earliestNonLazy.reset(); }
    }
    else if constexpr (std::is_same_v<T,ArmHyperVReferenceClock>) {
        a(v.registered, v.generation, v.valid);
    }
    else if constexpr (std::is_same_v<T,ArmHyperVHeartbeat>) {
        a(v.state, v.responsive, v.expired, v.unresponsive, v.framework, v.version, v.applicationState, v.since, v.sequence, v.probes, v.responses, v.rejected, v.timeouts, v.negotiations);
    }
    else if constexpr (std::is_same_v<T,ArmHyperVTimeSync>) {
        a(v.state, v.framework, v.transaction, v.initial, v.since, v.negotiations, v.samples, v.acks, v.rejected, v.timeouts);
    }
    else if constexpr (std::is_same_v<T,ArmHyperVShutdown>) {
        a(v.state, v.framework, v.transaction, v.flags, v.guestTimeout, v.lastStatus, v.since, v.negotiations, v.requests, v.accepted, v.denied, v.rejected, v.timeouts);
    }
    else if constexpr (std::is_same_v<T,ArmHyperVKvp>) {
        a(v.state, v.framework, v.service, v.transaction, v.pool, v.index, v.lastStatus, v.versionReserved, v.versionBodySize, v.versionFrameworkCount, v.versionServiceCount, v.versionFramework, v.versionService, v.since, v.negotiations, v.requests, v.responses, v.records, v.ends, v.rejected, v.timeouts, v.operation, v.queryBody, v.queryKey, v.mutationAcks, v.result);
    }
    else if constexpr (std::is_same_v<T,ArmHyperVKvp::Result>) {
        a(v.pool, v.index, v.status, v.record, v.operation);
    }
    else if constexpr (std::is_same_v<T,ArmHyperVKvpCodec::Record>) {
        a(v.key, v.value);
    }
    else if constexpr (std::is_same_v<T,ArmHyperVKvpCodec::Value>) {
        a(v.type, v.text, v.number);
    }
    else if constexpr (std::is_same_v<T,ArmGpadl>) {
        a(v.nextGeneration, v.descriptors);
    }
    else if constexpr (std::is_same_v<T,ArmGpadl::Descriptor>) {
        a(v.channel, v.expectedBytes, v.expectedRanges, v.encoded, v.ranges, v.bytes, v.ready, v.generation);
    }
    else if constexpr (std::is_same_v<T,ArmGpadl::Range>) {
        a(v.offset, v.bytes, v.pages);
    }
    else if constexpr (std::is_same_v<T,ArmVmbus::Reply>) {
        a(v.vp, v.sint, v.payload);
    }
    else if constexpr (std::is_same_v<T,ArmHyperV::PostSnapshot>) {
        a(v.observed, v.packetObserved, v.control, v.address, v.vp, v.status, v.connectionId, v.reserved, v.type, v.size, v.vmbusPrefixHex);
    }
    else if constexpr (std::is_same_v<T,ArmHyperV>) {
        a(v.enabled, v.synicAdvertised, v.synic, v.vmbus, v.vmbusEnabled, v.heartbeatEnabled, v.heartbeat, v.timesyncEnabled, v.timesync, v.shutdownEnabled, v.shutdown, v.kvpEnabled, v.kvp, v.vmbusEventPorts, v.referenceClock, v.timers, v.postedMessages, v.signaledEvents, v.lastPost, v.allowPostMessages, v.allowSignalEvents, v.ipiRequests, v.ipiTargetMask, v.ipiVector, v.ipiTargetRequests, v.guestId, v.partitionInfo, v.referenceTime, v.tlbiControl, v.syntheticVectors, v.crashParameters, v.notifiedParameters, v.crashNotifications, v.crashControl, v.crashVp, v.crashMessageHex, v.crashPending, v.uidQueries, v.versionQueries, v.smcccCalls, v.legacyCalls, v.disabledCalls, v.failures, v.omittedCounters, v.lastControl, v.lastResult, v.lastRegister, v.registers, v.calls, v.callFailures, v.tlbFlushRequests, v.tlbFlushTargetMask, v.tlbFlushAddressSpace, v.tlbFlushFlags, v.contextSyncRequests, v.contextSyncTargetMask);
    }
    else if constexpr (std::is_same_v<T,IntelHda>) {
        a(v.registers, v.bar, v.pci_command, v.bar_probe, v.bar_upper_probe, v.wall_ticks, v.frame_fraction, v.codec_commands, v.playback_frames, v.output_frames, v.nonzero_samples, v.descriptor_index, v.descriptor_offset, v.response_count, v.converter_stream_channel, v.pin_control, v.converter_format, v.power_state);
    }
    else if constexpr (std::is_same_v<T,ArmVirtMachine::Config>) {
        a(v.cpus, v.memoryBytes, v.firmware, v.dtb, v.variableTemplate, v.variables, v.disk, v.diskOverlay, v.extraDisk, v.extraDiskOverlay, v.network, v.intelHda, v.virtioMsix, v.virtioSerial, v.spiceAgent, v.virtioGpu, v.virtioBalloon, v.balloonLegacyPfn64, v.balloonTargetMB, v.guestAgentSocket, v.hyperv, v.hypervSynic, v.sshPort, v.networkCapture, v.hypervVmbus, v.virtioTransport, v.diskController, v.acpiDirectory);
    }
    else if constexpr (std::is_same_v<T,ArmPl031>) {
        int64_t counter = v.clock_adjustment + v.host_seconds();
        a(counter, v.previous_counter, v.match_register, v.load_register, v.interrupt_enabled, v.raw_interrupt);
        if (a.loading) v.clock_adjustment = counter - v.host_seconds();
    }
    else if constexpr (std::is_same_v<T,ArmVmbus>) {
        a(v.replies, v.targetVp, v.targetSint, v.gpadls, v.connected, v.version,
          v.contacts, v.offers, v.unloads, v.delivered, v.rejected, v.deliveredTypes,
          v.channelOffers, v.opens, v.closes, v.packetsIn, v.packetsOut, v.faults);
        uint64_t count = v.channels.size(); a(count);
        if (count != v.channels.size()) throw std::runtime_error("Snapshot VMBus channel inventory mismatch");
        for (auto &[id,c] : v.channels) {
            uint32_t savedId = id; a(savedId);
            if (savedId != id) throw std::runtime_error("Snapshot VMBus channel mismatch");
            a(c.offer, c.offered, c.work, c.notification, c.ringGpadl, c.vp, c.pending);
            auto ring = [&](std::unique_ptr<ArmVmbusRing> &r) {
                bool exists = bool(r); a(exists);
                if (!exists) { if (a.loading) r.reset(); return; }
                uint32_t channel = 0, gpadl = 0, capacity = 0; uint64_t offset = 0, lease = 0;
                if (!a.loading) { channel = r->channel; gpadl = r->id; capacity = r->capacity; offset = r->offset; lease = r->lease; }
                a(channel, gpadl, capacity, offset, lease);
                if (a.loading) {
                    if (channel != id || gpadl != c.ringGpadl || capacity > 64*1024*1024)
                        throw std::runtime_error("Invalid snapshot VMBus ring");
                    r = std::make_unique<ArmVmbusRing>(v.gpadls, channel, gpadl, offset, capacity + 4096);
                    if (r->lease != lease) throw std::runtime_error("Snapshot GPADL generation mismatch");
                }
            };
            ring(c.rx); ring(c.tx);
        }
    }
    else static_assert(std::is_same_v<T,void>, "Missing explicit snapshot state inventory");
}
// Host checkpoint access stays in this adapter; peripheral headers never name
// the concrete board/execution type or take a dependency on its implementation.
template<class T> uint64_t ArmSnapshotAccess::sampleReference(T &v) {
    return v.reference_time_100ns();
}
// Read the serialized partition time before rebasing its new host clock origin.
template<class T> uint64_t ArmSnapshotAccess::savedReference(T &v) {
    return v.referenceTime;
}
// Preserve guest reference time while optionally replacing the host origin.
template<class T> void ArmSnapshotAccess::setReference(T &v,uint64_t time,bool rebase) {
    v.referenceTime=time;
    if (rebase) v.clockOrigin=v.clock()-time;
}
// Force the first restored framebuffer to be published to the frontend.
template<class T> void ArmSnapshotAccess::invalidateFramebuffer(T &v) {
    v.previousDescriptor.clear(); v.previousPixels.clear();
}

}

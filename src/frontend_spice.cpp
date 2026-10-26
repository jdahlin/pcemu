#include "frontend_spice.hpp"
#include "host_keyboard.hpp"
#include "virtio_serial.hpp"
#include <algorithm>
#include <array>
#include <deque>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
#ifdef PCEMU_HAVE_SPICE
#include <glib-unix.h>
#include <spice.h>
#include <spice/enums.h>

struct SpiceTimer {
    SpiceTimerFunc function;
    void *opaque;
    GSource *source = nullptr;
};
struct SpiceWatch {
    int fd;
    SpiceWatchFunc function;
    void *opaque;
    GSource *source = nullptr;
};
namespace pc {
struct SpiceFrontend::Impl {
    static inline Impl *active = nullptr;
    GMainContext *context = nullptr;
    SpiceServer *server = nullptr;
    SpiceCoreInterface core{};
    QXLInterface displayInterface{};
    SpiceKbdInterface keyboardInterface{};
    SpiceTabletInterface tabletInterface{};
    SpiceCharDeviceInterface agentInterface{};
    template <class T> struct Instance {
        T base{};
        Impl *owner;
    };
    Instance<QXLInstance> display{{}, this};
    Instance<SpiceKbdInstance> keyboard{{}, this};
    Instance<SpiceTabletInstance> tablet{{}, this};
    Instance<SpiceCharDeviceInstance> agent{{}, this};
    template <class T> static Impl &self(T *p) {
        return *reinterpret_cast<Instance<T> *>(p)->owner;
    }
    struct Frame {
        QXLDrawable draw{};
        QXLImage image{};
        QXLCursorCmd cursor{};
        std::vector<uint8_t> cursorData;
        std::vector<uint32_t> pixels;
    };
    std::mutex frameMutex;
    std::set<Frame *> frames;
    std::deque<Frame *> commands, cursorCommands;
    PointerShape lastCursor;
    bool cursorPending = false;
    std::vector<uint32_t> surface, pending;
    unsigned width = 800, height = 600, pendingWidth = 0, pendingHeight = 0;
    uint64_t imageId = 0;
    std::vector<FrontendEvent> events;
    std::array<uint16_t, 512> scanToHid{};
    bool extended = false, agentEnabled = false, agentAttached = false;
    unsigned pauseBytes = 0;
    uint8_t buttons = 0;
    VirtioSerial *serial = nullptr;
    static constexpr size_t eventLimit = 4096;
    void enqueue(FrontendEvent e) {
        if (events.size() < eventLimit)
            events.push_back(e);
        else {
            events.clear();
            events.push_back({FrontendEvent::Kind::FocusLost});
        }
    }
    static void timerCancel(SpiceTimer *t) {
        if (t->source) {
            g_source_destroy(t->source);
            g_source_unref(t->source);
            t->source = nullptr;
        }
    }
    static void timerStart(SpiceTimer *t, uint32_t ms) {
        timerCancel(t);
        t->source = g_timeout_source_new(ms);
        g_source_set_callback(
            t->source,
            [](void *data) -> gboolean {
                auto *timer = static_cast<SpiceTimer *>(data);
                auto fn = timer->function;
                auto opaque = timer->opaque;
                auto *source = timer->source;
                timer->source = nullptr;
                g_source_unref(source);
                fn(opaque);
                return G_SOURCE_REMOVE;
            },
            t, nullptr);
        g_source_attach(t->source, active->context);
    }
    static void watchMask(SpiceWatch *w, int mask) {
        if (w->source) {
            g_source_destroy(w->source);
            g_source_unref(w->source);
            w->source = nullptr;
        }
        if (!mask)
            return;
        GIOCondition condition = GIOCondition(G_IO_ERR | G_IO_HUP);
        if (mask & SPICE_WATCH_EVENT_READ)
            condition = GIOCondition(condition | G_IO_IN);
        if (mask & SPICE_WATCH_EVENT_WRITE)
            condition = GIOCondition(condition | G_IO_OUT);
        w->source = g_unix_fd_source_new(w->fd, condition);
        g_source_set_callback(
            w->source, G_SOURCE_FUNC(+[](int fd, GIOCondition condition, void *data) -> gboolean {
                auto *watch = static_cast<SpiceWatch *>(data);
                int events = 0;
                if (condition & (G_IO_IN | G_IO_ERR | G_IO_HUP))
                    events |= SPICE_WATCH_EVENT_READ;
                if (condition & G_IO_OUT)
                    events |= SPICE_WATCH_EVENT_WRITE;
                watch->function(fd, events, watch->opaque);
                return G_SOURCE_CONTINUE;
            }),
            w, nullptr);
        g_source_attach(w->source, active->context);
    }
    void setButtons(uint32_t state) {
        // SPICE L/M/R bits differ from the frontend's L/R/M mask.
        uint8_t next = (state & 1) | ((state & 2) << 1) | ((state & 4) >> 1);
        for (uint8_t bit : {1, 2, 4})
            if ((next ^ buttons) & bit) {
                FrontendEvent e{FrontendEvent::Kind::Button};
                e.button = bit;
                e.released = !(next & bit);
                enqueue(e);
            }
        buttons = next;
    }
    void key(uint8_t byte) {
        if (pauseBytes) {
            --pauseBytes;
            return;
        }
        if (byte == 0xe1) {
            enqueue({FrontendEvent::Kind::Key, 72, false});
            enqueue({FrontendEvent::Kind::Key, 72, true});
            pauseBytes = 5;
            extended = false;
            return;
        }
        if (byte == 0xe0) {
            extended = true;
            return;
        }
        auto usage = scanToHid[(extended ? 256 : 0) + (byte & 127)];
        extended = false;
        if (usage)
            enqueue({FrontendEvent::Kind::Key, usage, bool(byte & 128)});
    }
    void clearQueued() {
        std::lock_guard lock(frameMutex);
        for (auto *frame : commands) {
            frames.erase(frame);
            delete frame;
        }
        commands.clear();
        for (auto *frame : cursorCommands) {
            frames.erase(frame);
            delete frame;
        }
        cursorCommands.clear();
    }
    void primary(unsigned w, unsigned h) {
        surface.assign(size_t(w) * h, 0);
        width = w;
        height = h;
        QXLDevSurfaceCreate p{};
        p.width = w;
        p.height = h;
        p.stride = -int(w * 4);
        p.format = SPICE_SURFACE_FMT_32_xRGB;
        p.mouse_mode = true;
        p.mem = uintptr_t(surface.data());
        spice_qxl_create_primary_surface(&display.base, 0, &p);
    }
    void submit() {
        if (pending.empty())
            return;
        {
            std::lock_guard lock(frameMutex);
            if (frames.size() >= 3)
                return;
        }
        if (pendingWidth != width || pendingHeight != height) {
            clearQueued();
            spice_qxl_destroy_primary_surface(&display.base, 0);
            primary(pendingWidth, pendingHeight);
            cursorPending = true;
        }
        auto frame = std::make_unique<Frame>();
        frame->pixels.swap(pending);
        auto &d = frame->draw;
        auto &i = frame->image;
        d.release_info.id = uintptr_t(frame.get());
        d.bbox.right = width;
        d.bbox.bottom = height;
        d.effect = QXL_EFFECT_OPAQUE;
        d.type = QXL_DRAW_COPY;
        d.surfaces_dest[0] = d.surfaces_dest[1] = d.surfaces_dest[2] = -1;
        d.u.copy.rop_descriptor = SPICE_ROPD_OP_PUT;
        d.u.copy.src_bitmap = uintptr_t(&i);
        d.u.copy.src_area.right = width;
        d.u.copy.src_area.bottom = height;
        i.descriptor.id = ++imageId;
        i.descriptor.type = SPICE_IMAGE_TYPE_BITMAP;
        i.descriptor.width = i.bitmap.x = width;
        i.descriptor.height = i.bitmap.y = height;
        i.bitmap.flags = QXL_BITMAP_DIRECT | QXL_BITMAP_TOP_DOWN;
        i.bitmap.stride = width * 4;
        i.bitmap.format = SPICE_BITMAP_FMT_32BIT;
        i.bitmap.data = uintptr_t(frame->pixels.data());
        {
            std::lock_guard lock(frameMutex);
            frames.insert(frame.get());
            commands.push_back(frame.release());
        }
        spice_qxl_wakeup(&display.base);
        QXLRect area{0, 0, int32_t(height), int32_t(width)};
        spice_qxl_update_area(&display.base, 0, &area, nullptr, 0, true);
    }
    Impl(unsigned port, const std::string &passwordFile) {
        if (active)
            throw std::runtime_error("Only one SPICE server is supported per process");
        if (!port || port > 65535)
            throw std::runtime_error("Invalid SPICE port");
        std::string password;
        if (!passwordFile.empty()) {
            std::ifstream file(passwordFile, std::ios::binary);
            if (!file)
                throw std::runtime_error("Cannot read SPICE password file");
            std::array<char, 257> bytes{};
            file.read(bytes.data(), bytes.size());
            password.assign(bytes.data(), size_t(file.gcount()));
            if (!password.empty() && password.back() == '\n')
                password.pop_back();
            if (password.empty() || password.size() > 255 ||
                password.find_first_of("\r\n") != std::string::npos ||
                password.find('\0') != std::string::npos)
                throw std::runtime_error("SPICE password must contain 1..255 bytes on one line");
        }
        active = this;
        context = g_main_context_new();
        try {
            core.base = {SPICE_INTERFACE_CORE, "PCEmu event loop", SPICE_INTERFACE_CORE_MAJOR,
                         SPICE_INTERFACE_CORE_MINOR};
            core.timer_add = [](SpiceTimerFunc f, void *p) { return new SpiceTimer{f, p}; };
            core.timer_start = timerStart;
            core.timer_cancel = timerCancel;
            core.timer_remove = [](SpiceTimer *t) {
                timerCancel(t);
                delete t;
            };
            core.watch_add = [](int fd, int mask, SpiceWatchFunc f, void *p) {
                auto *w = new SpiceWatch{fd, f, p};
                watchMask(w, mask);
                return w;
            };
            core.watch_update_mask = watchMask;
            core.watch_remove = [](SpiceWatch *w) {
                watchMask(w, 0);
                delete w;
            };
            core.channel_event = [](int event, SpiceChannelEventInfo *info) {
                if (info->type == SPICE_CHANNEL_MAIN && event == SPICE_CHANNEL_EVENT_DISCONNECTED) {
                    active->enqueue({FrontendEvent::Kind::FocusLost});
                    active->buttons = 0;
                    active->extended = false;
                    active->pauseBytes = 0;
                }
            };
            server = spice_server_new();
            spice_server_set_addr(server, "127.0.0.1", SPICE_ADDR_FLAG_IPV4_ONLY);
            if (spice_server_set_port(server, int(port)))
                throw std::runtime_error("Invalid SPICE listen port");
            if (password.empty())
                spice_server_set_noauth(server);
            else
                spice_server_set_ticket(server, password.c_str(), 0, 0, 0);
            spice_server_set_name(server, "PCEmu Windows 11");
            spice_server_set_streaming_video(server, SPICE_STREAM_VIDEO_OFF);
            spice_server_set_image_compression(server, SPICE_IMAGE_COMPRESSION_OFF);
            spice_server_set_agent_mouse(server, false);
            spice_server_set_agent_copypaste(server, true);
            spice_server_set_agent_file_xfer(server, false);
            if (spice_server_init(server, &core))
                throw std::runtime_error("Cannot start loopback SPICE server");
            displayInterface.base = {SPICE_INTERFACE_QXL, "PCEmu software display",
                                     SPICE_INTERFACE_QXL_MAJOR, SPICE_INTERFACE_QXL_MINOR};
            displayInterface.attached_worker = [](QXLInstance *) {};
            displayInterface.set_compression_level = [](QXLInstance *, int) {};
            displayInterface.get_init_info = [](QXLInstance *, QXLDevInitInfo *i) {
                *i = {};
                i->num_memslots = i->num_memslots_groups = 1;
                i->memslot_gen_bits = i->memslot_id_bits = 1;
                i->n_surfaces = 1;
            };
            displayInterface.get_command = [](QXLInstance *q, QXLCommandExt *cmd) {
                auto &s = self(q);
                std::lock_guard lock(s.frameMutex);
                if (s.commands.empty())
                    return 0;
                *cmd = {};
                cmd->cmd.type = QXL_CMD_DRAW;
                cmd->cmd.data = uintptr_t(&s.commands.front()->draw);
                s.commands.pop_front();
                return 1;
            };
            displayInterface.req_cmd_notification = [](QXLInstance *q) {
                auto &s = self(q);
                std::lock_guard lock(s.frameMutex);
                return int(s.commands.empty());
            };
            displayInterface.release_resource = [](QXLInstance *q, QXLReleaseInfoExt r) {
                auto &s = self(q);
                auto *f = reinterpret_cast<Frame *>(uintptr_t(r.info->id));
                std::lock_guard lock(s.frameMutex);
                s.frames.erase(f);
                delete f;
            };
            displayInterface.get_cursor_command = [](QXLInstance *q, QXLCommandExt *cmd) {
                auto &s = self(q);
                std::lock_guard lock(s.frameMutex);
                if (s.cursorCommands.empty())
                    return 0;
                *cmd = {};
                cmd->cmd.type = QXL_CMD_CURSOR;
                cmd->cmd.data = uintptr_t(&s.cursorCommands.front()->cursor);
                s.cursorCommands.pop_front();
                return 1;
            };
            displayInterface.req_cursor_notification = [](QXLInstance *q) {
                auto &s = self(q);
                std::lock_guard lock(s.frameMutex);
                return int(s.cursorCommands.empty());
            };
            displayInterface.notify_update = [](QXLInstance *, uint32_t) {};
            displayInterface.flush_resources = [](QXLInstance *) { return 0; };
            displayInterface.async_complete = [](QXLInstance *, uint64_t) {};
            displayInterface.update_area_complete = [](QXLInstance *, uint32_t, QXLRect *,
                                                       uint32_t) {};
            display.base.base.sif = &displayInterface.base;
            if (spice_server_add_interface(server, &display.base.base))
                throw std::runtime_error("Cannot register SPICE display");
            QXLDevMemSlot slot{};
            slot.virt_end = UINTPTR_MAX;
            slot.qxl_ram_size = UINT32_MAX;
            spice_qxl_add_memslot(&display.base, &slot);
            primary(width, height);
            spice_qxl_set_device_info(&display.base, "pci/0000/07.0", 0, 1);
            keyboardInterface.base = {SPICE_INTERFACE_KEYBOARD, "PCEmu USB keyboard",
                                      SPICE_INTERFACE_KEYBOARD_MAJOR,
                                      SPICE_INTERFACE_KEYBOARD_MINOR};
            keyboardInterface.push_scan_freg = [](SpiceKbdInstance *k, uint8_t b) {
                self(k).key(b);
            };
            keyboardInterface.get_leds = [](SpiceKbdInstance *) -> uint8_t { return 0; };
            keyboard.base.base.sif = &keyboardInterface.base;
            for (uint16_t usage = 4; usage < 256; ++usage) {
                std::vector<uint8_t> bytes;
                HostKeyboard encoder([&](uint8_t b) { bytes.push_back(b); });
                encoder.event(usage, false);
                if (bytes.size() == 1)
                    scanToHid[bytes[0]] = usage;
                else if (bytes.size() == 2 && bytes[0] == 0xe0)
                    scanToHid[256 + bytes[1]] = usage;
            }
            scanToHid[256 + 0x5b] = 227;
            scanToHid[256 + 0x5c] = 231;
            scanToHid[256 + 0x5d] = 101;
            scanToHid[256 + 0x37] = 70; // Print Screen; ignore its fake Shift fragments.
            tabletInterface.base = {SPICE_INTERFACE_TABLET, "PCEmu USB tablet",
                                    SPICE_INTERFACE_TABLET_MAJOR, SPICE_INTERFACE_TABLET_MINOR};
            tabletInterface.set_logical_size = [](SpiceTabletInstance *, int, int) {};
            tabletInterface.position = [](SpiceTabletInstance *t, int x, int y, uint32_t buttons) {
                auto &s = self(t);
                FrontendEvent e{FrontendEvent::Kind::AbsoluteMotion};
                e.x = std::clamp(x, 0, int(s.width) - 1) * 65535 / std::max(1, int(s.width) - 1);
                e.y = std::clamp(y, 0, int(s.height) - 1) * 65535 / std::max(1, int(s.height) - 1);
                s.enqueue(e);
                s.setButtons(buttons);
            };
            tabletInterface.wheel = [](SpiceTabletInstance *t, int wheel, uint32_t buttons) {
                auto &s = self(t);
                s.setButtons(buttons);
                FrontendEvent e{FrontendEvent::Kind::Wheel};
                e.y = -wheel;
                s.enqueue(e);
            };
            tabletInterface.buttons = [](SpiceTabletInstance *t, uint32_t b) {
                self(t).setButtons(b);
            };
            tablet.base.base.sif = &tabletInterface.base;
            agentInterface.base = {SPICE_INTERFACE_CHAR_DEVICE, "PCEmu SPICE agent",
                                   SPICE_INTERFACE_CHAR_DEVICE_MAJOR,
                                   SPICE_INTERFACE_CHAR_DEVICE_MINOR};
            agentInterface.state = [](SpiceCharDeviceInstance *a, int connected) {
                auto &s = self(a);
                s.agentEnabled = connected;
                if (s.serial)
                    s.serial->set_host_port_open(connected, 1);
            };
            agentInterface.write = [](SpiceCharDeviceInstance *a, const uint8_t *data, int count) {
                auto &s = self(a);
                return s.serial && count > 0 ? int(s.serial->receive_from_host({data, size_t(count)}, 1)) : 0;
            };
            agentInterface.read = [](SpiceCharDeviceInstance *a, uint8_t *data, int count) {
                auto &s = self(a);
                if (!s.serial || count <= 0)
                    return 0;
                auto bytes = s.serial->take_guest_output(size_t(count), 1);
                std::copy(bytes.begin(), bytes.end(), data);
                return int(bytes.size());
            };
            agent.base.base.sif = &agentInterface.base;
            agent.base.subtype = "vdagent";
            if (spice_server_add_interface(server, &keyboard.base.base) ||
                spice_server_add_interface(server, &tablet.base.base))
                throw std::runtime_error("Cannot register SPICE input");
            spice_server_vm_start(server);
        } catch (...) {
            cleanup();
            throw;
        }
    }
    void submitCursor() {
        if (!cursorPending)
            return;
        {
            std::lock_guard lock(frameMutex);
            if (frames.size() >= 6)
                return;
        }
        auto frame = std::make_unique<Frame>();
        frame->cursor.release_info.id = uintptr_t(frame.get());
        if (!lastCursor.visible)
            frame->cursor.type = QXL_CURSOR_HIDE;
        else {
            frame->cursor.type = QXL_CURSOR_SET;
            frame->cursor.u.set.visible = true;
            size_t size = lastCursor.pixels.size() * 4;
            frame->cursorData.resize(sizeof(QXLCursor) + size);
            auto *shape = reinterpret_cast<QXLCursor *>(frame->cursorData.data());
            shape->header.unique = ++imageId;
            shape->header.type = SPICE_CURSOR_TYPE_ALPHA;
            shape->header.width = lastCursor.width;
            shape->header.height = lastCursor.height;
            shape->header.hot_spot_x = lastCursor.hotX;
            shape->header.hot_spot_y = lastCursor.hotY;
            shape->data_size = shape->chunk.data_size = uint32_t(size);
            std::copy_n(reinterpret_cast<const uint8_t *>(lastCursor.pixels.data()), size,
                        frame->cursorData.data() + sizeof(QXLCursor));
            frame->cursor.u.set.shape = uintptr_t(shape);
        }
        {
            std::lock_guard lock(frameMutex);
            frames.insert(frame.get());
            cursorCommands.push_back(frame.release());
        }
        cursorPending = false;
        spice_qxl_wakeup(&display.base);
    }
    void cleanup() {
        serial = nullptr;
        if (server) {
            spice_server_destroy(server);
            server = nullptr;
        }
        for (auto *frame : frames)
            delete frame;
        frames.clear();
        commands.clear();
        if (context) {
            g_main_context_unref(context);
            context = nullptr;
        }
        active = nullptr;
    }
    ~Impl() {
        cleanup();
    }
    void pump(VirtioSerial *next) {
        bool available = next && next->port_ready(1) && next->guest_port_open(1);
        if (agentAttached && (next != serial || !available)) {
            spice_server_remove_interface(&agent.base.base);
            agentAttached = false;
            if (serial)
                serial->set_host_port_open(false, 1);
        }
        serial = next;
        if (available && !agentAttached) {
            if (spice_server_add_interface(server, &agent.base.base))
                throw std::runtime_error("Cannot register SPICE guest agent");
            agentAttached = true;
        }
        if (serial && agentAttached) {
            serial->set_host_port_open(agentEnabled, 1);
            spice_server_char_device_wakeup(&agent.base);
        }
    }
};
SpiceFrontend::SpiceFrontend(unsigned port, const std::string &passwordFile)
    : impl(std::make_unique<Impl>(port, passwordFile)) {}
SpiceFrontend::~SpiceFrontend() = default;
std::vector<FrontendEvent> SpiceFrontend::poll() {
    for (unsigned i = 0; i < 64 && g_main_context_pending(impl->context); ++i)
        g_main_context_iteration(impl->context, false);
    impl->submit();
    impl->submitCursor();
    std::vector<FrontendEvent> events;
    events.swap(impl->events);
    return events;
}
void SpiceFrontend::present(std::span<const uint32_t> pixels, unsigned width, unsigned height,
                            DisplayShape) {
    if (!width || !height || width > 4096 || height > 4096 ||
        pixels.size() != size_t(width) * height)
        throw std::runtime_error("Invalid SPICE framebuffer");
    impl->pending.assign(pixels.begin(), pixels.end());
    impl->pendingWidth = width;
    impl->pendingHeight = height;
    impl->submit();
}
void SpiceFrontend::setPointerShape(const PointerShape &shape) {
    if (shape.visible && (!shape.width || !shape.height || shape.width > 64 || shape.height > 64 ||
                          shape.hotX >= shape.width || shape.hotY >= shape.height ||
                          shape.pixels.size() != size_t(shape.width) * shape.height))
        throw std::runtime_error("Invalid SPICE cursor shape");
    const auto &old = impl->lastCursor;
    if (old.visible == shape.visible && old.active == shape.active && old.width == shape.width &&
        old.height == shape.height && old.hotX == shape.hotX && old.hotY == shape.hotY &&
        old.pixels == shape.pixels)
        return;
    impl->lastCursor = shape;
    impl->cursorPending = true;
    impl->submitCursor();
}
void SpiceFrontend::pumpAgent(VirtioSerial *serial) {
    impl->pump(serial);
}
} // namespace pc
#else
namespace pc {
struct SpiceFrontend::Impl {};
SpiceFrontend::SpiceFrontend(unsigned, const std::string &) {
    throw std::runtime_error(
        "SPICE support unavailable: install spice-server and reconfigure CMake");
}
SpiceFrontend::~SpiceFrontend() = default;
std::vector<FrontendEvent> SpiceFrontend::poll() {
    return {};
}
void SpiceFrontend::present(std::span<const uint32_t>, unsigned, unsigned, DisplayShape) {}
void SpiceFrontend::pumpAgent(VirtioSerial *) {}
void SpiceFrontend::setPointerShape(const PointerShape &) {}
} // namespace pc
#endif

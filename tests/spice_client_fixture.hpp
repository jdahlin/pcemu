#pragma once
#include <cstring>
#include <spice-client.h>
#include <spice/vd_agent.h>
#include <stdexcept>
#include <string>
struct Client {
    SpiceSession *session = spice_session_new();
    SpiceChannel *display = nullptr, *inputs = nullptr, *main = nullptr;
    std::string hostClipboard, guestClipboard;
    bool receiveClipboard = false;
    unsigned cursorSets = 0, cursorHides = 0;
    bool authError = false;
    int cursorWidth = 0, cursorHeight = 0, hotX = 0, hotY = 0;
    uint32_t cursorPixel = 0;
    Client(unsigned port, const char *password = nullptr) {
        auto number = std::to_string(port);
        g_object_set(session, "host", "127.0.0.1", "port", number.c_str(), "enable-audio", FALSE,
                     nullptr);
        if (password)
            g_object_set(session, "password", password, nullptr);
        g_signal_connect(
            session, "channel-new",
            G_CALLBACK(+[](SpiceSession *, SpiceChannel *channel, void *data) {
                auto &c = *static_cast<Client *>(data);
                g_signal_connect(
                    channel, "channel-event",
                    G_CALLBACK(+[](SpiceChannel *, SpiceChannelEvent event, void *data) {
                        if (event == SPICE_CHANNEL_ERROR_AUTH)
                            static_cast<Client *>(data)->authError = true;
                    }),
                    data);
                if (SPICE_IS_MAIN_CHANNEL(channel)) {
                    c.main = channel;
                    g_signal_connect(
                        channel, "main-clipboard-selection-request",
                        G_CALLBACK(+[](SpiceMainChannel *main, guint selection, guint type,
                                       void *data) -> gboolean {
                            auto &c = *static_cast<Client *>(data);
                            if (selection || type != VD_AGENT_CLIPBOARD_UTF8_TEXT ||
                                c.hostClipboard.empty())
                                return false;
                            spice_main_channel_clipboard_selection_notify(
                                main, selection, type,
                                reinterpret_cast<const guchar *>(c.hostClipboard.data()),
                                c.hostClipboard.size());
                            return true;
                        }),
                        data);
                    g_signal_connect(
                        channel, "main-clipboard-selection-grab",
                        G_CALLBACK(+[](SpiceMainChannel *main, guint selection, gpointer types,
                                       guint count, void *data) -> gboolean {
                            auto &c = *static_cast<Client *>(data);
                            if (!c.receiveClipboard || selection)
                                return false;
                            auto *values = static_cast<guint32 *>(types);
                            for (guint i = 0; i < count; ++i)
                                if (values[i] == VD_AGENT_CLIPBOARD_UTF8_TEXT) {
                                    spice_main_channel_clipboard_selection_request(
                                        main, selection, VD_AGENT_CLIPBOARD_UTF8_TEXT);
                                    return true;
                                }
                            return false;
                        }),
                        data);
                    g_signal_connect(
                        channel, "main-clipboard-selection",
                        G_CALLBACK(+[](SpiceMainChannel *, guint selection, guint type,
                                       gpointer bytes, guint size, void *data) {
                            auto &c = *static_cast<Client *>(data);
                            if (c.receiveClipboard && !selection &&
                                type == VD_AGENT_CLIPBOARD_UTF8_TEXT && size <= 1024 * 1024)
                                c.guestClipboard.assign(static_cast<const char *>(bytes), size);
                        }),
                        data);
                }
                if (SPICE_IS_DISPLAY_CHANNEL(channel))
                    c.display = channel;
                if (SPICE_IS_INPUTS_CHANNEL(channel))
                    c.inputs = channel;
                if (SPICE_IS_CURSOR_CHANNEL(channel)) {
                    g_signal_connect(channel, "cursor-set",
                                     G_CALLBACK(+[](SpiceCursorChannel *, gint w, gint h, gint x,
                                                    gint y, gpointer rgba, void *data) {
                                         auto &c = *static_cast<Client *>(data);
                                         ++c.cursorSets;
                                         c.cursorWidth = w;
                                         c.cursorHeight = h;
                                         c.hotX = x;
                                         c.hotY = y;
                                         std::memcpy(&c.cursorPixel, rgba, 4);
                                     }),
                                     data);
                    g_signal_connect(channel, "cursor-hide",
                                     G_CALLBACK(+[](SpiceCursorChannel *, void *data) {
                                         ++static_cast<Client *>(data)->cursorHides;
                                     }),
                                     data);
                }
                spice_channel_connect(channel);
            }),
            this);
        if (!spice_session_connect(session))
            throw std::runtime_error("SPICE connection failed");
    }
    void disconnect() {
        if (session) {
            spice_session_disconnect(session);
            g_object_unref(session);
            session = nullptr;
        }
    }
    ~Client() {
        disconnect();
    }
    bool pixel(unsigned width, unsigned height, uint32_t wanted) {
        SpiceDisplayPrimary p{};
        if (!display || !spice_display_channel_get_primary(display, 0, &p) ||
            unsigned(p.width) != width || unsigned(p.height) != height)
            return false;
        uint32_t actual;
        std::memcpy(&actual, p.data + (height / 2) * p.stride + (width / 2) * 4, 4);
        return (actual & 0xffffff) == wanted;
    }
};

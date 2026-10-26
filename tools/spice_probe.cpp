// Headless real-client probe for installed guest display, input and cursor.
#include "../tests/spice_client_fixture.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>
static void pump(unsigned ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    do {
        for (unsigned i = 0; i < 64 && g_main_context_pending(nullptr); ++i)
            g_main_context_iteration(nullptr, false);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < end);
}
static void type(SpiceInputsChannel *input, const std::string &text) {
    const std::string letters = "abcdefghijklmnopqrstuvwxyz";
    const unsigned scans[] = {30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
                              49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44};
    const std::string plain = "1234567890-=\t[]\n;'`\\,./ ";
    const std::string shifted = "!@#$%^&*()_+\t{}\n:\"~|<>? ";
    const unsigned other[] = {2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13,
                              15, 26, 27, 28, 39, 40, 41, 43, 51, 52, 53, 57};
    for (unsigned char c : text) {
        bool shift = c >= 'A' && c <= 'Z';
        unsigned scan = 0;
        auto letter = letters.find(char(shift ? c + 32 : c));
        if (letter != std::string::npos)
            scan = scans[letter];
        else {
            auto at = plain.find(char(c));
            if (at == std::string::npos) {
                at = shifted.find(char(c));
                shift = true;
            }
            if (at == std::string::npos)
                throw std::runtime_error("Unsupported text byte");
            scan = other[at];
        }
        if (shift)
            spice_inputs_channel_key_press(input, 42);
        spice_inputs_channel_key_press_and_release(input, scan);
        if (shift)
            spice_inputs_channel_key_release(input, 42);
        pump(35);
    }
}
int main(int argc, char **argv) {
    try {
        if (argc != 3)
            throw std::runtime_error("Usage: spice-probe PORT ACTION_FILE");
        Client client(unsigned(std::stoul(argv[1])));
        for (unsigned i = 0; i < 100 && (!client.inputs || !client.display); ++i)
            pump(100);
        if (!client.inputs || !client.display)
            throw std::runtime_error("SPICE channels unavailable");
        pump(250);
        std::ifstream actions(argv[2]);
        if (!actions)
            throw std::runtime_error("Cannot open action file");
        std::string line;
        while (std::getline(actions, line)) {
            std::istringstream command(line);
            std::string action;
            command >> action;
            auto *input = SPICE_INPUTS_CHANNEL(client.inputs);
            if (action.empty() || action[0] == '#')
                continue;
            if (action == "wait") {
                unsigned ms;
                command >> ms;
                pump(ms);
            } else if (action == "text") {
                std::string text;
                std::getline(command >> std::ws, text);
                type(input, text);
            } else if (action == "type-file") {
                std::string path;
                command >> path;
                std::ifstream file(path, std::ios::binary);
                if (!file)
                    throw std::runtime_error("Cannot read private typing file");
                type(input, std::string(std::istreambuf_iterator<char>(file), {}));
            } else if (action == "key" || action == "chord") {
                std::vector<unsigned> keys;
                unsigned key;
                while (command >> std::hex >> key)
                    keys.push_back(key);
                for (auto key : keys)
                    spice_inputs_channel_key_press(input, key);
                for (auto it = keys.rbegin(); it != keys.rend(); ++it)
                    spice_inputs_channel_key_release(input, *it);
                pump(150);
            } else if (action == "clipboard-put") {
                std::getline(command >> std::ws, client.hostClipboard);
                for (unsigned i = 0; i < 100; ++i) {
                    gboolean connected = false;
                    if (client.main)
                        g_object_get(client.main, "agent-connected", &connected, nullptr);
                    if (connected)
                        break;
                    pump(100);
                }
                if (!client.main)
                    throw std::runtime_error("SPICE main channel missing");
                gboolean connected = false;
                g_object_get(client.main, "agent-connected", &connected, nullptr);
                if (!connected)
                    throw std::runtime_error("SPICE guest agent unavailable");
                guint32 type = VD_AGENT_CLIPBOARD_UTF8_TEXT;
                spice_main_channel_clipboard_selection_grab(SPICE_MAIN_CHANNEL(client.main), 0,
                                                            &type, 1);
                client.receiveClipboard = true;
                pump(1000);
            } else if (action == "clipboard-expect") {
                std::string expected;
                std::getline(command >> std::ws, expected);
                for (unsigned i = 0; i < 100 && client.guestClipboard != expected; ++i)
                    pump(100);
                if (client.guestClipboard != expected)
                    throw std::runtime_error("SPICE guest clipboard mismatch");
                std::cout << "SPICE guest clipboard verified, bytes=" << expected.size() << '\n';
            } else if (action == "move") {
                int x, y;
                command >> x >> y;
                spice_inputs_channel_position(input, x, y, 0, 0);
                pump(200);
            } else if (action == "click") {
                spice_inputs_channel_button_press(input, 1, 1);
                pump(80);
                spice_inputs_channel_button_release(input, 1, 0);
                pump(200);
            } else if (action == "capture") {
                std::string path;
                command >> path;
                SpiceDisplayPrimary p{};
                if (!spice_display_channel_get_primary(client.display, 0, &p))
                    throw std::runtime_error("SPICE display unavailable");
                std::ofstream out(path, std::ios::binary);
                out << "P6\n" << p.width << ' ' << p.height << "\n255\n";
                for (int y = 0; y < p.height; ++y)
                    for (int x = 0; x < p.width; ++x) {
                        const auto *pixel = p.data + y * p.stride + x * 4;
                        out.put(pixel[2]);
                        out.put(pixel[1]);
                        out.put(pixel[0]);
                    }
                if (!out)
                    throw std::runtime_error("Cannot save SPICE display");
                std::cout << "SPICE capture " << p.width << 'x' << p.height
                          << "; cursor sets=" << client.cursorSets
                          << " hides=" << client.cursorHides << " shape=" << client.cursorWidth
                          << 'x' << client.cursorHeight << " hotspot=" << client.hotX << ','
                          << client.hotY << '\n';
            } else
                throw std::runtime_error("Unknown probe action");
        }
        client.disconnect();
        pump(100);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

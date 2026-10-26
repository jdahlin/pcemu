#include "frontend_spice.hpp"
#include "spice_client_fixture.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <spice-client.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
using namespace pc;
static unsigned checks = 0;
static void check(bool good) {
    ++checks;
    if (!good)
        throw std::runtime_error("SPICE check " + std::to_string(checks) + " failed");
}

int main() {
    try {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        check(sock >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(bind(sock, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
        socklen_t length = sizeof(address);
        check(getsockname(sock, reinterpret_cast<sockaddr *>(&address), &length) == 0);
        unsigned port = ntohs(address.sin_port);
        close(sock);
        {
            SpiceFrontend server(port);
            std::vector<FrontendEvent> events;
            auto spin = [&](auto predicate) {
                auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                do {
                    auto incoming = server.poll();
                    events.insert(events.end(), incoming.begin(), incoming.end());
                    for (unsigned i = 0; i < 64 && g_main_context_pending(nullptr); ++i)
                        g_main_context_iteration(nullptr, FALSE);
                    if (predicate())
                        return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                } while (std::chrono::steady_clock::now() < end);
                return false;
            };
            server.present(std::vector<uint32_t>(320 * 200, 0xff12ab34), 320, 200);
            for (unsigned i = 0; i < 100; ++i)
                server.present(std::vector<uint32_t>(320 * 200, 0xff12ab34 + i), 320, 200);
            server.present(std::vector<uint32_t>(320 * 200, 0xff12ab34), 320, 200);
            Client client(port);
            check(spin([&] { return client.pixel(320, 200, 0x12ab34) && client.inputs; }));
            PointerShape cursor;
            cursor.active = cursor.visible = true;
            cursor.width = cursor.height = 16;
            cursor.hotX = 3;
            cursor.hotY = 7;
            cursor.pixels.assign(256, 0xff123456);
            server.setPointerShape(cursor);
            check(spin([&] { return client.cursorSets > 0; }));
            check(client.cursorWidth == 16 && client.cursorHeight == 16 && client.hotX == 3 &&
                  client.hotY == 7);
            check(client.cursorPixel == 0xff563412); // spice-gtk cursor signal exposes RGBA bytes.
            auto *input = SPICE_INPUTS_CHANNEL(client.inputs);
            spice_inputs_channel_key_press(input, 0x1e);
            spice_inputs_channel_key_release(input, 0x1e);
            spice_inputs_channel_key_press(input, 0x11d);
            spice_inputs_channel_key_release(input, 0x11d);
            spice_inputs_channel_key_press(input, 0x15b);
            spice_inputs_channel_key_release(input, 0x15b);
            spice_inputs_channel_position(input, 160, 100, 0, 0);
            spice_inputs_channel_button_press(input, 1, 1);
            spice_inputs_channel_button_release(input, 1, 0);
            check(spin([&] {
                return std::count_if(events.begin(), events.end(), [](auto &e) {
                           return e.kind == FrontendEvent::Kind::Key;
                       }) >= 6;
            }));
            check(std::count_if(events.begin(), events.end(), [](auto &e) {
                      return e.kind == FrontendEvent::Kind::Key && e.key == 4;
                  }) == 2);
            check(std::count_if(events.begin(), events.end(), [](auto &e) {
                      return e.kind == FrontendEvent::Kind::Key && e.key == 228;
                  }) == 2);
            check(std::count_if(events.begin(), events.end(), [](auto &e) {
                      return e.kind == FrontendEvent::Kind::Key && e.key == 227;
                  }) == 2);
            check(spin([&] {
                return std::any_of(events.begin(), events.end(), [](auto &e) {
                    return e.kind == FrontendEvent::Kind::AbsoluteMotion && e.x > 32000 &&
                           e.y > 32000;
                });
            }));
            check(spin([&] {
                return std::count_if(events.begin(), events.end(), [](auto &e) {
                           return e.kind == FrontendEvent::Kind::Button && e.button == 1;
                       }) == 2;
            }));
            cursor.visible = false;
            server.setPointerShape(cursor);
            check(spin([&] { return client.cursorHides > 0; }));
            server.present(std::vector<uint32_t>(640 * 360, 0xffeebb22), 640, 360);
            check(spin([&] { return client.pixel(640, 360, 0xeebb22); }));
            events.clear();
            client.disconnect();
            check(spin([&] {
                return std::any_of(events.begin(), events.end(), [](auto &e) {
                    return e.kind == FrontendEvent::Kind::FocusLost;
                });
            }));
            Client reconnect(port);
            check(spin([&] { return reconnect.pixel(640, 360, 0xeebb22); }));
            reconnect.disconnect();
            spin([] { return true; });
        }
        char name[] = "/tmp/pcemu-spice-password-XXXXXX";
        int fd = mkstemp(name);
        check(fd >= 0);
        check(write(fd, "pcemu\n", 6) == 6);
        close(fd);
        {
            SpiceFrontend secured(port, name);
            secured.present(std::vector<uint32_t>(40 * 30, 0xffabcdef), 40, 30);
            auto spin = [&](auto predicate) {
                auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                do {
                    secured.poll();
                    for (unsigned i = 0; i < 64 && g_main_context_pending(nullptr); ++i)
                        g_main_context_iteration(nullptr, false);
                    if (predicate())
                        return true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                } while (std::chrono::steady_clock::now() < end);
                return false;
            };
            {
                Client bad(port, "incorrect");
                check(spin([&] { return bad.authError; }));
                bad.disconnect();
                spin([] { return true; });
            }
            {
                Client good(port, "pcemu");
                check(spin([&] { return good.pixel(40, 30, 0xabcdef); }));
                good.disconnect();
                spin([] { return true; });
            }
        }
        unlink(name);
        std::cout << checks << " real SPICE client checks passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

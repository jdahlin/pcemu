#pragma once
#include "host_key.hpp"
#include <array>
#include <functional>
namespace pc {
// Host key policy shared by PS/2 and USB. Bare Command maps to the Windows key;
// host shortcuts and focus changes must never leave a modifier held in a guest.
class HostKeyState {
    std::function<void(uint16_t, bool)> emit;
    std::array<bool, 512> held{}, sent{};

  public:
    explicit HostKeyState(std::function<void(uint16_t, bool)> sink) : emit(std::move(sink)) {}
    void event(uint16_t key, bool up) {
        if (key >= held.size())
            return;
        bool command = key == hid::KEY_LGUI || key == hid::KEY_RGUI;
        if (up) {
            if (!held[key])
                return;
            if (command && !sent[key])
                emit(key, false);
            if (sent[key] || command)
                emit(key, true);
            held[key] = sent[key] = false;
            return;
        }
        held[key] = true;
        if (command)
            return;
        for (auto meta : {hid::KEY_LGUI, hid::KEY_RGUI})
            if (held[meta] && !sent[meta]) {
                emit(meta, false);
                sent[meta] = true;
            }
        emit(key, false);
        sent[key] = true;
    }
    void release() {
        for (unsigned i = 0; i < held.size(); ++i) {
            if (sent[i])
                emit(i, true);
            held[i] = sent[i] = false;
        }
    }
    void consumeCommand() {
        for (auto key : {hid::KEY_LGUI, hid::KEY_RGUI}) {
            if (sent[key])
                emit(key, true);
            held[key] = sent[key] = false;
        }
    }
};
} // namespace pc

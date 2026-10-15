// Os controles pelo SDL2 (platform/gamepad.h): o SDL_GameController (Xbox, DualShock/DualSense, os controles Bluetooth
// do Android e os USB do Linux) no layout do XInput, mais o controle virtual do toque no Android (virtual_pad_sdl.h).
//
// Os índices ficam estáveis: um controle novo ocupa o primeiro lugar livre e o que sai libera o seu (os outros não
// andam). O lugar 0 junta o controle virtual: sem controle físico ali, é o virtual (conectado enquanto o lançador mostra
// os botões); com um, os botões dos dois somam e cada analógico é o mais empurrado. A vibração do XInput é um estado
// (até mudar); o SDL_GameControllerRumble tem duração, então ela é renovada a cada leitura enquanto não for zero.

#include "platform/gamepad.h"

#include <SDL.h>

#include <atomic>
#include <cstdlib>
#include <mutex>

#include "platform/virtual_pad_sdl.h"

namespace nf {

namespace {

struct VirtualPad {
    std::mutex lock;
    bool visible = false;
    PadState state;
};
VirtualPad g_virtual;

int16_t stick(SDL_GameController* c, SDL_GameControllerAxis a, bool flip) {
    int v = SDL_GameControllerGetAxis(c, a);
    if (flip) v = -v - 1;  // o SDL tem y para baixo; o XInput, para cima
    if (v < -32768) v = -32768;
    if (v > 32767) v = 32767;
    return int16_t(v);
}

int16_t larger(int16_t a, int16_t b) { return std::abs(int(a)) >= std::abs(int(b)) ? a : b; }

class SdlPads final : public PadSource {
public:
    void poll(PadState out[kMaxPads]) override {
        if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
            for (int i = 0; i < kMaxPads; ++i) out[i] = PadState{};
            merge_virtual(out[0]);
            return;
        }
        SDL_GameControllerUpdate();
        refresh();
        for (int i = 0; i < kMaxPads; ++i) {
            PadState s{};
            SDL_GameController* c = pads_[i];
            if (c != nullptr && SDL_GameControllerGetAttached(c)) {
                s.connected = true;
                auto bit = [&](SDL_GameControllerButton b, uint16_t mask) {
                    if (SDL_GameControllerGetButton(c, b) != 0) s.buttons = uint16_t(s.buttons | mask);
                };
                bit(SDL_CONTROLLER_BUTTON_DPAD_UP, kPadDpadUp);
                bit(SDL_CONTROLLER_BUTTON_DPAD_DOWN, kPadDpadDown);
                bit(SDL_CONTROLLER_BUTTON_DPAD_LEFT, kPadDpadLeft);
                bit(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, kPadDpadRight);
                bit(SDL_CONTROLLER_BUTTON_START, kPadStart);
                bit(SDL_CONTROLLER_BUTTON_BACK, kPadBack);
                bit(SDL_CONTROLLER_BUTTON_LEFTSTICK, kPadLThumb);
                bit(SDL_CONTROLLER_BUTTON_RIGHTSTICK, kPadRThumb);
                bit(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, kPadLB);
                bit(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, kPadRB);
                bit(SDL_CONTROLLER_BUTTON_A, kPadA);
                bit(SDL_CONTROLLER_BUTTON_B, kPadB);
                bit(SDL_CONTROLLER_BUTTON_X, kPadX);
                bit(SDL_CONTROLLER_BUTTON_Y, kPadY);
                s.lt = uint8_t(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
                s.rt = uint8_t(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
                s.lx = stick(c, SDL_CONTROLLER_AXIS_LEFTX, false);
                s.ly = stick(c, SDL_CONTROLLER_AXIS_LEFTY, true);
                s.rx = stick(c, SDL_CONTROLLER_AXIS_RIGHTX, false);
                s.ry = stick(c, SDL_CONTROLLER_AXIS_RIGHTY, true);
                if (rumble_low_[i] != 0 || rumble_high_[i] != 0) {
                    SDL_GameControllerRumble(c, rumble_low_[i], rumble_high_[i], 250);
                }
            }
            out[i] = s;
        }
        merge_virtual(out[0]);
    }

    void vibrate(int pad, uint16_t low, uint16_t high) override {
        if (pad < 0 || pad >= kMaxPads) return;
        rumble_low_[pad] = low;
        rumble_high_[pad] = high;
        if (pads_[pad] != nullptr) SDL_GameControllerRumble(pads_[pad], low, high, low != 0 || high != 0 ? 250 : 0);
    }

private:
    void refresh() {
        // Os que saíram liberam o lugar.
        for (int i = 0; i < kMaxPads; ++i) {
            if (pads_[i] != nullptr && !SDL_GameControllerGetAttached(pads_[i])) {
                SDL_GameControllerClose(pads_[i]);
                pads_[i] = nullptr;
                ids_[i] = -1;
            }
        }
        const int n = SDL_NumJoysticks();
        if (n == last_count_) return;
        last_count_ = n;
        for (int j = 0; j < n; ++j) {
            if (!SDL_IsGameController(j)) continue;
            const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(j);
            bool known = false;
            for (int i = 0; i < kMaxPads; ++i) known = known || ids_[i] == id;
            if (known) continue;
            for (int i = 0; i < kMaxPads; ++i) {
                if (pads_[i] != nullptr) continue;
                pads_[i] = SDL_GameControllerOpen(j);
                ids_[i] = pads_[i] != nullptr ? id : -1;
                break;
            }
        }
    }

    static void merge_virtual(PadState& s) {
        std::lock_guard<std::mutex> hold(g_virtual.lock);
        if (!g_virtual.visible) return;
        const PadState& v = g_virtual.state;
        s.connected = true;
        s.buttons = uint16_t(s.buttons | v.buttons);
        s.lt = s.lt > v.lt ? s.lt : v.lt;
        s.rt = s.rt > v.rt ? s.rt : v.rt;
        s.lx = larger(s.lx, v.lx);
        s.ly = larger(s.ly, v.ly);
        s.rx = larger(s.rx, v.rx);
        s.ry = larger(s.ry, v.ry);
    }

    SDL_GameController* pads_[kMaxPads]{};
    SDL_JoystickID ids_[kMaxPads]{-1, -1, -1, -1};
    uint16_t rumble_low_[kMaxPads]{};
    uint16_t rumble_high_[kMaxPads]{};
    int last_count_ = -1;
};

SdlPads g_pads;

}  // namespace

PadSource& system_pad_source() { return g_pads; }

void set_virtual_pad(bool visible, uint16_t buttons, int16_t lx, int16_t ly, int16_t rx, int16_t ry, uint8_t lt,
                     uint8_t rt) {
    std::lock_guard<std::mutex> hold(g_virtual.lock);
    g_virtual.visible = visible;
    g_virtual.state.connected = visible;
    g_virtual.state.buttons = buttons;
    g_virtual.state.lx = lx;
    g_virtual.state.ly = ly;
    g_virtual.state.rx = rx;
    g_virtual.state.ry = ry;
    g_virtual.state.lt = lt;
    g_virtual.state.rt = rt;
}

}  // namespace nf

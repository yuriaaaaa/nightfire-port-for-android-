// O que o os_posix.cpp deixa para o programa de janela do SDL (NF_SDL_APP; platform/os.h): o caractere de uma tecla no
// layout do usuário (o SDL_GetKeyFromScancode), o estado do teclado do sistema e a caixa de erro
// (SDL_ShowSimpleMessageBox; no Android também o logcat).

#include "platform/os.h"

#include <SDL.h>

#include <cstdio>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include "platform/keys.h"

namespace nf::os {

namespace {

// O scancode do SDL de uma tecla do port (o inverso de window_sdl.cpp, só as que têm caractere).
SDL_Scancode scancode_of(int vk) {
    if (vk >= 'A' && vk <= 'Z') return SDL_Scancode(SDL_SCANCODE_A + (vk - 'A'));
    if (vk >= '1' && vk <= '9') return SDL_Scancode(SDL_SCANCODE_1 + (vk - '1'));
    switch (vk) {
        case '0': return SDL_SCANCODE_0;
        case key::OemMinus: return SDL_SCANCODE_MINUS;
        case key::OemPlus: return SDL_SCANCODE_EQUALS;
        case key::Oem4: return SDL_SCANCODE_LEFTBRACKET;
        case key::Oem6: return SDL_SCANCODE_RIGHTBRACKET;
        case key::Oem5: return SDL_SCANCODE_BACKSLASH;
        case key::Oem1: return SDL_SCANCODE_SEMICOLON;
        case key::Oem7: return SDL_SCANCODE_APOSTROPHE;
        case key::Oem3: return SDL_SCANCODE_GRAVE;
        case key::OemComma: return SDL_SCANCODE_COMMA;
        case key::OemPeriod: return SDL_SCANCODE_PERIOD;
        case key::Oem2: return SDL_SCANCODE_SLASH;
        case key::Oem102: return SDL_SCANCODE_NONUSBACKSLASH;
        case key::AbntC1: return SDL_SCANCODE_INTERNATIONAL1;
        case key::Shift:
        case key::LShift: return SDL_SCANCODE_LSHIFT;
        case key::RShift: return SDL_SCANCODE_RSHIFT;
        case key::Control:
        case key::LControl: return SDL_SCANCODE_LCTRL;
        case key::RControl: return SDL_SCANCODE_RCTRL;
        case key::Menu:
        case key::LMenu: return SDL_SCANCODE_LALT;
        case key::RMenu: return SDL_SCANCODE_RALT;
        default: return SDL_SCANCODE_UNKNOWN;
    }
}

}  // namespace

uint32_t key_char(int key) {
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) return 0;
    const SDL_Scancode sc = scancode_of(key);
    if (sc == SDL_SCANCODE_UNKNOWN) return 0;
    const SDL_Keycode k = SDL_GetKeyFromScancode(sc);
    // Os keycodes imprimíveis do SDL são o próprio caractere (minúsculo para as letras).
    if (k <= 0 || (k & SDLK_SCANCODE_MASK) != 0 || k < 0x20 || k == 0x7f) return 0;
    uint32_t c = uint32_t(k);
    if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    return c;
}

bool key_held(int key) {
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) return false;
    SDL_PumpEvents();
    const SDL_Scancode sc = scancode_of(key);
    if (sc == SDL_SCANCODE_UNKNOWN) return false;
    int n = 0;
    const Uint8* state = SDL_GetKeyboardState(&n);
    if (key == key::Shift) return state[SDL_SCANCODE_LSHIFT] != 0 || state[SDL_SCANCODE_RSHIFT] != 0;
    return state != nullptr && int(sc) < n && state[sc] != 0;
}

void show_error(const std::wstring& title, const std::wstring& text) {
    show_error_narrow(to_utf8(title).c_str(), to_utf8(text).c_str());
}

void show_error_narrow(const char* title, const char* text) {
    std::fprintf(stderr, "%s: %s\n", title, text);
    std::fflush(stderr);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_ERROR, "Nightfire", "%s: %s", title, text);
#endif
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, nullptr);
}

}  // namespace nf::os

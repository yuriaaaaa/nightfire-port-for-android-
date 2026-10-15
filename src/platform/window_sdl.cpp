// A janela do SDL2 (platform/window.h): o Android (a superfície do SDLActivity, sempre em tela cheia na resolução do
// aparelho) e o Linux com janela (a build NF_SDL, para testar o renderizador Vulkan e o laço numa máquina comum).
//
// native_handle() é o SDL_Window* (o renderizador Vulkan cria a superfície com SDL_Vulkan_CreateSurface). O SDL é
// iniciado aqui, na thread do jogo (a pilha da arena: platform/arena.h), que é também a que bombeia os eventos.
//
// Teclas: pelo scancode (a posição, como o Raw Input do Windows) para os códigos do port (platform/keys.h, os números dos
// VK_ do Windows). Mouse: o modo relativo do SDL é a captura (o movimento cru, o cursor escondido); um clique na janela
// captura, como no Windows. No Android não há o que capturar: o mouse fica "capturado" sempre (o olhar vem do toque,
// psiInput_AddTouchLook, e do controle), assim o teclado e o controle de um aparelho Bluetooth valem no jogo.
// system_display_modes (platform/display_modes.h): os modos do monitor da janela pelo SDL.

#include "platform/window.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

#include "platform/display_modes.h"
#include "platform/keys.h"
#include "platform/os.h"

namespace nf {

struct Window::Native {
    SDL_Window* window = nullptr;
    bool down[256]{};
    int windowed_w = 0, windowed_h = 0;
};

struct WindowEvents {
    static int vk_of(SDL_Scancode sc);
    static void key(Window& w, int vk, bool down) {
        if (vk <= 0 || vk >= 256) return;
        if (down && !w.native_->down[vk]) w.pressed_[vk] = true;
        w.native_->down[vk] = down;
    }
    static void sync_size(Window& w) {
        int dw = 0, dh = 0;
        SDL_Vulkan_GetDrawableSize(w.native_->window, &dw, &dh);
        if (dw <= 0 || dh <= 0) SDL_GetWindowSize(w.native_->window, &dw, &dh);
        if (dw != w.width_ || dh != w.height_) {
            w.width_ = dw;
            w.height_ = dh;
            w.resized_ = true;
        }
    }
};

int WindowEvents::vk_of(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0) return '0';
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return key::F1 + (sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_F13 && sc <= SDL_SCANCODE_F24) return key::F1 + 12 + (sc - SDL_SCANCODE_F13);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return key::Numpad0 + 1 + (sc - SDL_SCANCODE_KP_1);
    switch (sc) {
        case SDL_SCANCODE_KP_0: return key::Numpad0;
        case SDL_SCANCODE_RETURN: return key::Return;
        case SDL_SCANCODE_KP_ENTER: return key::Return;
        case SDL_SCANCODE_ESCAPE: return key::Escape;
        case SDL_SCANCODE_BACKSPACE: return key::Back;
        case SDL_SCANCODE_TAB: return key::Tab;
        case SDL_SCANCODE_SPACE: return key::Space;
        case SDL_SCANCODE_MINUS: return key::OemMinus;
        case SDL_SCANCODE_EQUALS: return key::OemPlus;
        case SDL_SCANCODE_LEFTBRACKET: return key::Oem4;
        case SDL_SCANCODE_RIGHTBRACKET: return key::Oem6;
        case SDL_SCANCODE_BACKSLASH: return key::Oem5;
        case SDL_SCANCODE_SEMICOLON: return key::Oem1;
        case SDL_SCANCODE_APOSTROPHE: return key::Oem7;
        case SDL_SCANCODE_GRAVE: return key::Oem3;
        case SDL_SCANCODE_COMMA: return key::OemComma;
        case SDL_SCANCODE_PERIOD: return key::OemPeriod;
        case SDL_SCANCODE_SLASH: return key::Oem2;
        case SDL_SCANCODE_NONUSBACKSLASH: return key::Oem102;
        case SDL_SCANCODE_INTERNATIONAL1: return key::AbntC1;
        case SDL_SCANCODE_CAPSLOCK: return key::Capital;
        case SDL_SCANCODE_INSERT: return key::Insert;
        case SDL_SCANCODE_DELETE: return key::Delete;
        case SDL_SCANCODE_HOME: return key::Home;
        case SDL_SCANCODE_END: return key::End;
        case SDL_SCANCODE_PAGEUP: return key::Prior;
        case SDL_SCANCODE_PAGEDOWN: return key::Next;
        case SDL_SCANCODE_LEFT: return key::Left;
        case SDL_SCANCODE_RIGHT: return key::Right;
        case SDL_SCANCODE_UP: return key::Up;
        case SDL_SCANCODE_DOWN: return key::Down;
        case SDL_SCANCODE_KP_MULTIPLY: return key::Multiply;
        case SDL_SCANCODE_KP_PLUS: return key::Add;
        case SDL_SCANCODE_KP_MINUS: return key::Subtract;
        case SDL_SCANCODE_KP_PERIOD: return key::Decimal;
        case SDL_SCANCODE_KP_DIVIDE: return key::Divide;
        case SDL_SCANCODE_LSHIFT: return key::LShift;
        case SDL_SCANCODE_RSHIFT: return key::RShift;
        case SDL_SCANCODE_LCTRL: return key::LControl;
        case SDL_SCANCODE_RCTRL: return key::RControl;
        case SDL_SCANCODE_LALT: return key::LMenu;
        case SDL_SCANCODE_RALT: return key::RMenu;
        case SDL_SCANCODE_LGUI: return key::LWin;
        case SDL_SCANCODE_RGUI: return key::RWin;
        case SDL_SCANCODE_APPLICATION: return key::Apps;
        default: return 0;
    }
}

namespace {

#if defined(__ANDROID__)
constexpr bool kTouchDevice = true;
#else
constexpr bool kTouchDevice = false;
#endif

void init_sdl() {
    if (SDL_WasInit(SDL_INIT_VIDEO) != 0) return;
    // O toque é do lançador (o controle virtual e o olhar por arrasto, GameActivity.kt): nada de mouse falso.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) != 0) {
        throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
    }
}

}  // namespace

Window::Window(const std::wstring& title, int width, int height, bool fullscreen) : native_(std::make_unique<Native>()) {
    init_sdl();
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (fullscreen || kTouchDevice) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    native_->windowed_w = std::max(320, width);
    native_->windowed_h = std::max(240, height);
    native_->window = SDL_CreateWindow(os::to_utf8(title).c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       native_->windowed_w, native_->windowed_h, flags);
    if (native_->window == nullptr) throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
    handle_ = native_->window;
    fullscreen_ = fullscreen || kTouchDevice;
    WindowEvents::sync_size(*this);
    resized_ = false;
    focused_ = true;
    if (kTouchDevice) captured_ = true;
}

Window::~Window() {
    if (native_->window != nullptr) {
        SDL_SetRelativeMouseMode(SDL_FALSE);
        SDL_DestroyWindow(native_->window);
    }
}

bool Window::pump() {
    if (native_->window == nullptr) return false;
    SDL_Event e;
    bool open = true;
    while (SDL_PollEvent(&e) != 0) {
        switch (e.type) {
            case SDL_QUIT: open = false; break;
            case SDL_APP_TERMINATING: open = false; break;
            case SDL_WINDOWEVENT:
                switch (e.window.event) {
                    case SDL_WINDOWEVENT_SIZE_CHANGED:
                    case SDL_WINDOWEVENT_RESIZED: WindowEvents::sync_size(*this); break;
                    case SDL_WINDOWEVENT_FOCUS_GAINED: focused_ = true; break;
                    case SDL_WINDOWEVENT_FOCUS_LOST:
                        focused_ = false;
                        std::memset(native_->down, 0, sizeof(native_->down));
                        if (!kTouchDevice && captured_) capture(false);
                        break;
                    case SDL_WINDOWEVENT_CLOSE: open = false; break;
                    default: break;
                }
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                const bool down = e.type == SDL_KEYDOWN;
                const int vk = WindowEvents::vk_of(e.key.keysym.scancode);
                // Alt+Enter: a tela cheia sem bordas, como no Windows.
                if (down && vk == key::Return && (e.key.keysym.mod & KMOD_ALT) != 0 && !kTouchDevice) {
                    toggle_fullscreen();
                    break;
                }
                WindowEvents::key(*this, vk, down);
                // Os genéricos (Shift, Ctrl, Alt) seguem os lados, como o GetAsyncKeyState.
                native_->down[key::Shift] = native_->down[key::LShift] || native_->down[key::RShift];
                native_->down[key::Control] = native_->down[key::LControl] || native_->down[key::RControl];
                native_->down[key::Menu] = native_->down[key::LMenu] || native_->down[key::RMenu];
                if (down && (vk == key::LShift || vk == key::RShift)) pressed_[key::Shift] = true;
                if (down && (vk == key::LControl || vk == key::RControl)) pressed_[key::Control] = true;
                if (down && (vk == key::LMenu || vk == key::RMenu)) pressed_[key::Menu] = true;
                break;
            }
            case SDL_MOUSEMOTION:
                if (e.motion.which == SDL_TOUCH_MOUSEID) break;
                if (captured_ && !menu_mode_) {
                    mouse_dx_ += float(e.motion.xrel);
                    mouse_dy_ += float(e.motion.yrel);
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                if (e.button.which == SDL_TOUCH_MOUSEID) break;
                const bool down = e.type == SDL_MOUSEBUTTONDOWN;
                int vk = 0;
                switch (e.button.button) {
                    case SDL_BUTTON_LEFT: vk = key::LButton; break;
                    case SDL_BUTTON_RIGHT: vk = key::RButton; break;
                    case SDL_BUTTON_MIDDLE: vk = key::MButton; break;
                    case SDL_BUTTON_X1: vk = key::XButton1; break;
                    case SDL_BUTTON_X2: vk = key::XButton2; break;
                    default: break;
                }
                // O primeiro clique numa janela solta só captura (não atira), como no Windows.
                if (down && vk == key::LButton && !captured_ && !menu_mode_ && focused_) {
                    capture(true);
                    break;
                }
                WindowEvents::key(*this, vk, down);
                break;
            }
            case SDL_MOUSEWHEEL: {
                float y = e.wheel.preciseY;
                if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) y = -y;
                wheel_ += y;
                break;
            }
            default: break;
        }
    }
    if (!open) {
        SDL_SetRelativeMouseMode(SDL_FALSE);
        SDL_DestroyWindow(native_->window);
        native_->window = nullptr;
        handle_ = nullptr;
    }
    return open;
}

void Window::set_title(const std::wstring& title) {
    if (native_->window != nullptr) SDL_SetWindowTitle(native_->window, os::to_utf8(title).c_str());
}

void Window::toggle_fullscreen() {
    if (native_->window == nullptr || kTouchDevice) return;
    fullscreen_ = !fullscreen_;
    SDL_SetWindowFullscreen(native_->window, fullscreen_ ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if (!fullscreen_) {
        SDL_SetWindowSize(native_->window, native_->windowed_w, native_->windowed_h);
        SDL_SetWindowPosition(native_->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
    WindowEvents::sync_size(*this);
}

void Window::set_client_size(int width, int height) {
    native_->windowed_w = std::max(320, width);
    native_->windowed_h = std::max(240, height);
    if (native_->window == nullptr || fullscreen_) return;
    SDL_SetWindowSize(native_->window, native_->windowed_w, native_->windowed_h);
    SDL_SetWindowPosition(native_->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    WindowEvents::sync_size(*this);
}

void Window::set_topmost() {
    if (native_->window != nullptr) SDL_RaiseWindow(native_->window);
}

void Window::set_menu_mode(bool on) {
    if (on == menu_mode_) return;
    menu_mode_ = on;
    if (kTouchDevice) return;
    if (on) {
        capture_after_menu_ = captured_;
        if (captured_) capture(false);
    } else if (capture_after_menu_ && focused_) {
        capture(true);
    }
}

void Window::set_cursor_visible(bool on) {
    cursor_visible_ = on;
    if (!kTouchDevice && !captured_) SDL_ShowCursor(on || !menu_mode_ ? SDL_ENABLE : SDL_DISABLE);
}

bool Window::cursor_position(float* x, float* y) const {
    if (native_->window == nullptr || !focused_ || kTouchDevice) return false;
    int mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    int ww = 0, wh = 0;
    SDL_GetWindowSize(native_->window, &ww, &wh);
    if (ww <= 0 || wh <= 0 || mx < 0 || my < 0 || mx >= ww || my >= wh) return false;
    // Do tamanho da janela (pontos) para o da área de desenho (pixels).
    *x = float(mx) * float(width_) / float(ww);
    *y = float(my) * float(height_) / float(wh);
    return true;
}

void Window::take_mouse_delta(float& dx, float& dy) {
    dx = mouse_dx_;
    dy = mouse_dy_;
    mouse_dx_ = mouse_dy_ = 0.0f;
}

float Window::take_wheel() {
    const float w = wheel_;
    wheel_ = 0.0f;
    return w;
}

bool Window::key_down(int key) const { return key > 0 && key < 256 && native_->down[key]; }

bool Window::key_pressed(int key) {
    if (key <= 0 || key >= 256) return false;
    const bool p = pressed_[key];
    pressed_[key] = false;
    return p;
}

void Window::capture(bool on) {
    if (kTouchDevice) {
        captured_ = true;
        return;
    }
    captured_ = on;
    SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
    mouse_dx_ = mouse_dy_ = 0.0f;
}

bool system_display_modes(void* native_window, std::vector<DisplayMode>* modes, DisplayMode* desktop) {
    auto* window = static_cast<SDL_Window*>(native_window);
    const int display = window != nullptr ? std::max(0, SDL_GetWindowDisplayIndex(window)) : 0;
    SDL_DisplayMode d{};
    if (SDL_GetDesktopDisplayMode(display, &d) != 0) return false;
    if (desktop != nullptr) {
        desktop->width = d.w;
        desktop->height = d.h;
    }
    if (modes != nullptr) {
        modes->clear();
        const int n = SDL_GetNumDisplayModes(display);
        for (int i = 0; i < n; ++i) {
            SDL_DisplayMode m{};
            if (SDL_GetDisplayMode(display, i, &m) != 0) continue;
            const DisplayMode dm{m.w, m.h};
            if (std::find(modes->begin(), modes->end(), dm) == modes->end()) modes->push_back(dm);
        }
        if (modes->empty()) modes->push_back(DisplayMode{d.w, d.h});
    }
    return true;
}

}  // namespace nf

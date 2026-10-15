#pragma once
// O controle virtual do toque (só do port; Android): o estado que o lançador manda (GameActivity.kt, pela JNI de
// jni_android.cpp) entra no controle 0 de gamepad_sdl.cpp, somado ao controle físico se houver um (os botões juntos, o
// analógico mais empurrado de cada lado).

#include <cstdint>

namespace nf {

// buttons: os bits do XInput (platform/gamepad.h); analógicos -32768..32767 com y para cima; gatilhos 0..255. Qualquer
// thread.
void set_virtual_pad(bool visible, uint16_t buttons, int16_t lx, int16_t ly, int16_t rx, int16_t ry, uint8_t lt,
                     uint8_t rt);

}  // namespace nf

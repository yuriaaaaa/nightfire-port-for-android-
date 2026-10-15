// A ponte JNI do lançador do Android (GameActivity.kt / NativeBridge.kt, android/app): o controle virtual do toque
// (virtual_pad_sdl.h) e o olhar pelo arrasto do lado direito da tela (psiInput_AddTouchLook, platform/psi_input.h). As
// funções são chamadas na thread da interface do Android; as duas do outro lado aceitam qualquer thread.

#include <jni.h>

#include <cstdint>

#include "platform/psi_input.h"
#include "platform/virtual_pad_sdl.h"

extern "C" {

JNIEXPORT void JNICALL Java_org_nightfirepc_port_NativeBridge_nativeSetVirtualPadState(JNIEnv*, jclass, jboolean visible,
                                                                                     jint buttons, jint lx, jint ly,
                                                                                     jint rx, jint ry, jint lt, jint rt) {
    auto clamp16 = [](jint v) { return int16_t(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); };
    auto clamp8 = [](jint v) { return uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v)); };
    nf::set_virtual_pad(visible != JNI_FALSE, uint16_t(buttons), clamp16(lx), clamp16(ly), clamp16(rx), clamp16(ry),
                        clamp8(lt), clamp8(rt));
}

JNIEXPORT void JNICALL Java_org_nightfirepc_port_NativeBridge_nativeAddTouchLook(JNIEnv*, jclass, jfloat dx, jfloat dy) {
    psiInput_AddTouchLook(dx, dy);
}

}  // extern "C"

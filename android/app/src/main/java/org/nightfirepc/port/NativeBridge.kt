package org.nightfirepc.port

/** As funções do jogo que o lançador chama (src/platform/jni_android.cpp). */
object NativeBridge {
    /** O controle de toque: os bits do XInput, analógicos -32768..32767 com y para cima, gatilhos 0..255. */
    @JvmStatic
    external fun nativeSetVirtualPadState(visible: Boolean, buttons: Int, lx: Int, ly: Int, rx: Int, ry: Int, lt: Int, rt: Int)

    /** O olhar pelo toque e pelo giroscópio, em contagens do mouse (a sensibilidade do mouse do jogo vale por cima). */
    @JvmStatic
    external fun nativeAddTouchLook(dx: Float, dy: Float)
}

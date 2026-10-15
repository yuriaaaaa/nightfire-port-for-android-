package org.nightfirepc.port

/**
 * O carregador da libmain.so (src/platform/nfload_android.cpp): abre o jogo no começo de uma reserva de 4 GB de
 * endereços, onde cabem os ponteiros de 32 bits dele e a arena. Se não der, o SDLActivity carrega do jeito normal.
 */
object NativeLoader {
    @JvmStatic
    external fun nativePreload(): Boolean

    fun preload(): Boolean = try {
        System.loadLibrary("nfload")
        nativePreload()
    } catch (e: Throwable) {
        android.util.Log.w("Nightfire", "nfload: $e")
        false
    }
}

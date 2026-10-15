// O carregador da libmain.so no Android (libnfload.so; GameActivity.loadLibraries, android/app): antes do
// System.loadLibrary("main") do SDLActivity, reserva 4 GB de endereços e abre a libmain.so no começo da reserva
// (android_dlopen_ext com ANDROID_DLEXT_RESERVED_ADDRESS). Os ponteiros de 32 bits do jogo são deslocamentos a partir do
// começo da imagem (platform/ptr32.h) e a arena precisa de centenas de MB livres logo acima dela (platform/arena.cpp); no
// Android as bibliotecas são postas de cima para baixo, abaixo das que já estão carregadas, então o espaço acima da
// libmain.so é o das bibliotecas do sistema e do ART, sem garantia de um buraco desse tamanho. O resto da reserva fica
// para a arena (NF_ARENA_RESERVED, lido em os_posix.cpp). O System.loadLibrary("main") que vem depois acha a mesma
// biblioteca já aberta, e o ART chama o JNI_OnLoad dela como sempre.
//
// Também liga o stdout e o stderr ao arquivo de registro da partida (NF_LOG_FILE) antes do dlopen, para que um erro
// nos construtores estáticos do jogo (a arena, por exemplo) fique no registro. Sem nenhuma dependência além da libc,
// da libdl e do liblog (nada do libc++).

#include <android/dlext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr uint64_t kWindow = uint64_t(1) << 32;  // a janela dos ponteiros de 32 bits
constexpr uint64_t kAlign = uint64_t(1) << 20;   // o começo da reserva alinhado a 1 MB (o p_align é 16 KB)

void say(const char* text) {
    __android_log_print(ANDROID_LOG_INFO, "Nightfire", "%s", text);
    dprintf(2, "nfload: %s\n", text);
}

void redirect_output() {
    const char* log = getenv("NF_LOG_FILE");
    if (log == nullptr || *log != '/') return;
    const int fd = open(log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);
}

}  // namespace

extern "C" JNIEXPORT jboolean JNICALL Java_org_nightfirepc_port_NativeLoader_nativePreload(JNIEnv*, jclass) {
    redirect_output();
    const uint64_t size = kWindow + kAlign;
    void* r = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    char msg[256];
    if (r == MAP_FAILED) {
        say("não consegui reservar 4 GB; a libmain.so vai pelo caminho normal");
        return JNI_FALSE;
    }
    const uintptr_t begin = uintptr_t(r);
    const uintptr_t end = begin + size;
    const uintptr_t start = (begin + kAlign - 1) & ~uintptr_t(kAlign - 1);
    // O pedaço antes do alinhamento volta ao sistema; o resto fica (a imagem e a arena).
    if (start > begin) munmap(r, start - begin);
    snprintf(msg, sizeof(msg), "%lx-%lx", static_cast<unsigned long>(start), static_cast<unsigned long>(end));
    setenv("NF_ARENA_RESERVED", msg, 1);

    android_dlextinfo ext = {};
    ext.flags = ANDROID_DLEXT_RESERVED_ADDRESS;
    ext.reserved_addr = reinterpret_cast<void*>(start);
    ext.reserved_size = size_t(end - start);
    void* h = android_dlopen_ext("libmain.so", RTLD_NOW, &ext);
    if (h == nullptr) {
        const char* err = dlerror();
        snprintf(msg, sizeof(msg), "android_dlopen_ext falhou (%s); a libmain.so vai pelo caminho normal",
                 err != nullptr ? err : "?");
        say(msg);
        unsetenv("NF_ARENA_RESERVED");
        munmap(reinterpret_cast<void*>(start), size_t(end - start));
        return JNI_FALSE;
    }
    Dl_info info = {};
    void* sym = dlsym(h, "SDL_main");
    if (sym != nullptr && dladdr(sym, &info) != 0) {
        snprintf(msg, sizeof(msg), "libmain.so em %p, reserva %lx-%lx", info.dli_fbase,
                 static_cast<unsigned long>(start), static_cast<unsigned long>(end));
        say(msg);
    }
    return JNI_TRUE;
}

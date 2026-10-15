// O relatório de falhas do Android (platform/crash_report.h): o sinal, o endereço e a pilha de chamadas no stderr (que o
// lançador manda para Internal storage/Nightfire/logs) e no logcat.
//
// Diferenças do crash_report_posix.cpp:
//  - o backtrace() do execinfo só existe a partir da API 33; aqui a pilha vem do _Unwind_Backtrace do desenrolador do NDK,
//    cada endereço com a biblioteca e o deslocamento dentro dela (o addr2line do NDK lê o libmain.so com eles);
//  - o processo é do ART, que trata SIGSEGV por conta própria (as verificações de nulo e de estouro de pilha do código
//    gerenciado). O sigaction do aplicativo passa pela libsigchain: o tratador do ART decide primeiro e só o que ele não
//    quer chega aqui. Depois do relatório o tratador anterior volta (o do debuggerd, que grava o tombstone) e a instrução
//    roda de novo, em vez do SIG_DFL do POSIX.

#include "platform/crash_report.h"

#include <android/log.h>
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>
#include <unwind.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace nf {

namespace {

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
struct sigaction g_old[5];

const char* signal_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        case SIGFPE: return "SIGFPE";
        case SIGABRT: return "SIGABRT";
        default: return "?";
    }
}

void write_text(const char* s) {
    __android_log_write(ANDROID_LOG_ERROR, "Nightfire", s);
    size_t n = std::strlen(s);
    while (n > 0) {
        const ssize_t w = write(STDERR_FILENO, s, n);
        if (w <= 0) break;
        s += w;
        n -= size_t(w);
    }
}

struct Walk {
    uintptr_t pcs[48];
    int count = 0;
};

_Unwind_Reason_Code walk_one(_Unwind_Context* ctx, void* arg) {
    auto* w = static_cast<Walk*>(arg);
    const uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc != 0 && w->count < 48) w->pcs[w->count++] = pc;
    return w->count < 48 ? _URC_NO_REASON : _URC_END_OF_STACK;
}

void walk() {
    Walk w;
    _Unwind_Backtrace(walk_one, &w);
    for (int i = 0; i < w.count; ++i) {
        Dl_info info{};
        char line[512];
        if (dladdr(reinterpret_cast<void*>(w.pcs[i]), &info) != 0 && info.dli_fbase != nullptr) {
            const char* lib = info.dli_fname != nullptr ? std::strrchr(info.dli_fname, '/') : nullptr;
            lib = lib != nullptr ? lib + 1 : (info.dli_fname != nullptr ? info.dli_fname : "?");
            std::snprintf(line, sizeof(line), "  #%02d pc %08zx %s (%s)\n", i,
                          size_t(w.pcs[i] - reinterpret_cast<uintptr_t>(info.dli_fbase)), lib,
                          info.dli_sname != nullptr ? info.dli_sname : "?");
        } else {
            std::snprintf(line, sizeof(line), "  #%02d pc %p\n", i, reinterpret_cast<void*>(w.pcs[i]));
        }
        write_text(line);
    }
}

void (*volatile g_flush)() = nullptr;
volatile sig_atomic_t g_reporting = 0;

void on_signal(int sig, siginfo_t* info, void* uc) {
    if (g_reporting == 0) {
        g_reporting = 1;
        char line[160];
        std::snprintf(line, sizeof(line), "\nFALHA: sinal %d (%s) em %p\n", sig, signal_name(sig),
                      info != nullptr ? info->si_addr : nullptr);
        write_text(line);
        walk();
        crash_report_flush();
    }
    // O tratador de antes (o do debuggerd): a instrução roda de novo e ele grava o tombstone.
    for (size_t i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); ++i) {
        if (kSignals[i] != sig) continue;
        const struct sigaction& old = g_old[i];
        if ((old.sa_flags & SA_SIGINFO) != 0 && old.sa_sigaction != nullptr) {
            old.sa_sigaction(sig, info, uc);
            return;
        }
        sigaction(sig, &old, nullptr);
        if (old.sa_handler == SIG_IGN) return;
        if (old.sa_handler != SIG_DFL && old.sa_handler != nullptr) {
            old.sa_handler(sig);
            return;
        }
        if (sig == SIGABRT) raise(sig);
        return;
    }
}

alignas(16) char g_main_alt_stack[size_t(64) << 10];

}  // namespace

void install_crash_report() {
    stack_t alt{};
    alt.ss_sp = g_main_alt_stack;
    alt.ss_size = sizeof(g_main_alt_stack);
    sigaltstack(&alt, nullptr);
    struct sigaction sa {};
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof(kSignals) / sizeof(kSignals[0]); ++i) sigaction(kSignals[i], &sa, &g_old[i]);
}

void crash_report_exception(void* system_exception) {
    const auto* info = static_cast<const siginfo_t*>(system_exception);
    char line[160];
    std::snprintf(line, sizeof(line), "\nFALHA: sinal %d (%s) em %p\n", info != nullptr ? info->si_signo : 0,
                  info != nullptr ? signal_name(info->si_signo) : "?", info != nullptr ? info->si_addr : nullptr);
    write_text(line);
    walk();
    crash_report_flush();
}

void crash_report_here(const char* why) {
    std::fflush(stderr);
    write_text("\nFALHA: ");
    write_text(why);
    write_text("\n");
    walk();
    crash_report_flush();
}

void crash_report_set_flush(void (*flush)()) { g_flush = flush; }

void crash_report_flush() {
    std::fflush(stdout);
    std::fflush(stderr);
    void (*flush)() = g_flush;
    if (flush != nullptr) flush();
}

}  // namespace nf

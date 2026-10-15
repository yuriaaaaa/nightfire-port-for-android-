// O ponto de entrada do programa de janela do SDL2 (platform/entry.h): o SDL_main. No Android o SDLActivity o chama na
// thread dele (a "SDLThread") com os argumentos de GameActivity.getArguments(); no Linux é o main.
//
// O jogo roda na pilha da arena (nf::run_main, platform/arena.h: uma thread nova, juntada no fim), que também inicia o
// SDL (window_sdl.cpp) e bombeia os eventos. No Android, antes: o stdout e o stderr vão para o arquivo de registro que o
// lançador escolheu (NF_LOG_FILE: Internal storage/Nightfire/logs/nightfire.log), e a pasta de trabalho passa a ser a
// dos arquivos do jogador (NF_FILES_DIR: nightfire.ini, saves/ e o hd_ui que o lançador copia).

#include "platform/entry.h"

#include <SDL.h>
#include <SDL_main.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "platform/arena.h"
#include "platform/crash_report.h"
#include "platform/os.h"

namespace {

void start_log() {
#if defined(__ANDROID__)
    const char* log = std::getenv("NF_LOG_FILE");
    if (log != nullptr && *log == '/') {
        if (std::freopen(log, "a", stderr) != nullptr) std::setvbuf(stderr, nullptr, _IONBF, 0);
        if (std::freopen(log, "a", stdout) != nullptr) std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
    }
    const char* files = std::getenv("NF_FILES_DIR");
    if (files != nullptr && *files == '/') {
        if (chdir(files) != 0) std::fprintf(stderr, "chdir %s falhou\n", files);
    }
#endif
    char when[64];
    const std::time_t t = std::time(nullptr);
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
#if defined(NF_PORT_BUILD)
    std::fprintf(stderr, "Nightfire PC (SDL) %s, %s\n", NF_PORT_BUILD, when);
#else
    std::fprintf(stderr, "Nightfire PC (SDL), %s\n", when);
#endif
}

}  // namespace

extern "C" int SDL_main(int argc, char* argv[]) {
    start_log();
    nf::install_crash_report();
    std::vector<std::wstring> args;
    for (int i = 0; i < argc; ++i) {
        args.push_back(nf::os::from_utf8(argv[i] != nullptr ? argv[i] : ""));
        std::fprintf(stderr, "argumento %d: %s\n", i, argv[i] != nullptr ? argv[i] : "");
    }
    std::vector<wchar_t*> wargv;
    for (std::wstring& a : args) wargv.push_back(a.data());
    wargv.push_back(nullptr);
    const int n = int(args.size());
    const int code = nf::run_main([&] { return nf_app_main(n, wargv.data()); });
    std::fprintf(stderr, "fim: código %d\n", code);
    std::fflush(nullptr);
    SDL_Quit();
#if defined(__ANDROID__)
    // O SDLActivity termina a Activity quando o SDL_main volta; o processo pode continuar vivo para a próxima partida, e
    // o jogo tem estado estático: sai de vez.
    std::_Exit(code);
#endif
    return code;
}

#if !defined(__ANDROID__)
// No Linux o SDL não troca o main (o SDL_main.h só faz isso nos sistemas que pedem): o main chama o de cima.
#undef main
int main(int argc, char* argv[]) { return SDL_main(argc, argv); }
#endif

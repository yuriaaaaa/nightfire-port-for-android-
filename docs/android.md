# Android

O port no Android (arm64-v8a, Android 8.0 ou mais novo, Vulkan 1.0): o mesmo jogo do PC, com o SDL2 no lugar do Win32
e do AppKit e um renderizador Vulkan no lugar do Direct3D 11 e do Metal, mais um lançador próprio com o controle de
toque. Alvo de teste: Galaxy S25 (Adreno 830), driver Vulkan do sistema.

O APK não traz nada do jogo: o jogador usa a própria cópia (PS2, NTSC-U, SLUS-20579), um `.iso` ou a pasta com os
arquivos do disco. Nenhum APK pronto vai para o repositório ou para os lançamentos públicos (o APK é compilado de
código e de dados do port; o jogo continua vindo do disco do jogador).

## Compilar

```bash
android/build.sh            # android/build/out/Nightfire-android-arm64.apk
android/build.sh --native   # só libmain.so, libSDL2.so e libc++_shared.so em android/app/src/main/jniLibs
```

Precisa do SDK do Android (plataforma 35, build-tools 35, NDK 27.2.12479018), JDK 17, CMake 3.24+ e Ninja no PATH, e
rede para a fonte do SDL2 2.30.12 (baixada em `android/deps`, conferida pelo SHA-256) e para o Gradle (o Maven Central
às vezes responde 429: repetir). O `build.sh` compila o `CMakeLists.txt` da raiz com o NDK (`ANDROID` liga `NF_SDL`;
o jogo é a biblioteca `libmain.so` que o `SDLActivity` carrega, o libc++ compartilhado porque o SDL2 tem partes em
C++) e depois o Gradle. O lado Java do SDL2 vem da fonte baixada (`android-project`), sem cópia no repositório. A
assinatura: `NF_KEYSTORE`, `NF_STORE_PASSWORD`, `NF_KEY_ALIAS`, `NF_KEY_PASSWORD`; sem elas, a chave de depuração.

No Linux a mesma camada SDL2 + Vulkan compila como programa de janela (`-DNF_SDL=ON`, com `libsdl2-dev` e
`libvulkan-dev`), o que permite conferir o renderizador sem aparelho:

```bash
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DNF_SDL=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build/linux --target nightfire nf_render_check
NF_VK_VALIDATION=1 ./build/linux/nf_render_check --out /tmp/render --compare tests/data/render   # lavapipe
xvfb-run -a ./build/linux/nf_render_check --out /tmp/rw --scene rigido --janela                   # a cadeia de troca
```

## Peças

- `src/platform/*_sdl.cpp`: a janela, os eventos, o teclado, o mouse, os controles (`gamepad_sdl.cpp`, no layout do
  XInput, somado ao controle virtual do toque), o áudio (`audio_device_sdl.cpp`) e o resto do sistema (`os_sdl.cpp`);
  `entry_sdl.cpp` é o `SDL_main` (no Android lê `NF_LOG_FILE` e `NF_FILES_DIR` que o lançador põe no ambiente);
  `jni_android.cpp` recebe o controle de toque e o olhar pelo toque; `crash_report_android.cpp` manda a pilha de uma
  falha para o registro.
- `src/render/renderer_vulkan.cpp`: a fachada `Renderer` em Vulkan 1.0, com o `renderer_metal.mm` como especificação
  (os mesmos passos, a mesma ordem de desenho, as opções do PC: escala, MSAA com a névoa por amostra e o alfa para
  cobertura, filtros, VSync). Os shaders são um arquivo GLSL (`renderer_vulkan.glsl`) compilado pelo `glslc` (do NDK)
  uma vez por função de entrada em listas C de SPIR-V. Memória em blocos de 64 MB por tipo, recursos liberados por
  número de envio, três quadros em voo, a rotação do Android tratada na cadeia de troca (transformação identidade).
  `NF_VK_VALIDATION=1` liga a camada de validação e conta os erros. `gpu_adapters_vulkan.cpp` lista os aparelhos.
- `src/render/ui_vector_null.cpp`: ainda não há desenho vetorial (o CoreGraphics do macOS e o Direct2D do Windows não
  existem aqui), então os menus ficam no estilo Original e a camada HD desligada, como no Windows sem Direct2D.
- `android/app`: o lançador em Kotlin (sem AndroidX).
  - `LauncherActivity`: o acesso ao armazenamento ("Acesso a todos os arquivos"), o disco (achado em
    Internal storage/Nightfire, ou escolhido: um `.iso` ou uma pasta, pelo seletor do sistema, convertido em caminho),
    as opções do toque e JOGAR. Na primeira partida escreve um `nightfire.ini` (estilo de botões NightFire, os gatilhos
    atirando e mirando, sem a tela de entrada do PC) e copia a pasta `hd_ui` do APK.
  - `GameActivity` (um `SDLActivity`): chama o jogo com `--disc <disco>`, põe o controle de toque por cima, a mira pelo
    giroscópio (opcional) e o menu de quatro dedos: continuar, a pausa (Start), mostrar ou esconder o toque, o
    giroscópio, as sensibilidades e a opacidade, sair. Um controle físico esconde o toque; tocar a tela o traz de volta.
  - `VirtualPadView`: o controle de toque como um controle do Xbox virtual (os bits do XInput), então vale o estilo
    de botões escolhido no jogo: o analógico esquerdo nasce onde o dedo encosta, o lado direito (e o dedo em ATIRAR
    ou MIRAR) olha como um mouse, os botões ATIRAR, MIRAR, PULAR, AGACHAR, USAR, MODO, o direcional (armas e
    aparelhos), START e VISÃO.

## Pastas no aparelho

| Pasta | O que tem |
|-------|-----------|
| Internal storage/Nightfire | o `.iso` (opcional: pode ficar em qualquer pasta), `nightfire.ini`, `saves/`, `hd_ui/` |
| Internal storage/Nightfire/logs | `nightfire.log` desta partida e as 8 anteriores (`nightfire-<data>.log`) |
| Internal storage/Nightfire/textures | o pacote de texturas HD do PCSX2 (`SLUS-20579/replacements`), opcional |

## Estado

- Feito: a camada SDL2, o renderizador Vulkan, o lançador, o APK. No Linux (lavapipe), `nf_render_check` passa as 30
  cenas não vetoriais dentro dos limites contra as referências do Direct3D 11, com 0 erros de validação, e a janela
  de verdade (Xvfb) dá o mesmo quadro que fora da tela.
- Ainda não testado num aparelho de verdade.
- Falta: o desenho vetorial (os menus Modern e a camada HD; um rasterizador de CPU como o do macOS), a decodificação
  de BC1-BC7 na CPU para pacotes HD em GPUs sem `textureCompressionBC` (os Adreno não têm: hoje esses DDS são
  ignorados), a vibração do controle de toque, o layout editável do toque, gravar e carregar a qualquer momento.

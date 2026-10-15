// O desenho vetorial ausente (só do port): a fábrica de render/ui_vector.h sem backend. É a do Android e do Linux com o
// SDL (docs/android.md) até haver um rasterizador vetorial na CPU sem o CoreGraphics: o renderizador Vulkan recebe nulo e
// o jogo fica com os menus Original e sem a camada HD, como no Windows sem Direct2D.

#include "render/ui_vector.h"

namespace nf {

std::unique_ptr<UiVectorCanvas> create_ui_vector_canvas() { return nullptr; }

}  // namespace nf

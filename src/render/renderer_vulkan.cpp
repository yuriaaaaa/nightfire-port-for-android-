// O renderizador Vulkan: o backend do Android e do Linux com janela (o SDL2) de render/renderer.h (o Renderer::Impl e,
// no fim, os repasses da fachada); docs/android.md. É o renderizador Metal (render/renderer_metal.mm) passado para o
// Vulkan 1.0 quase linha por linha: os mesmos shaders (render/renderer_vulkan.glsl, compilados em SPIR-V pelo glslc na
// build), a mesma resolução do estado do GS, a mesma ordem dos desenhos e as mesmas opções do PC. A especificação continua
// sendo o Direct3D 11 (render/renderer_d3d11.cpp); o teste de conformidade (nf_render_check) compara com as referências
// dele.
//
// O quadro: tudo é desenhado num alvo próprio (target, BGRA8 do tamanho da janela, ou da captura com "offscreen"), que
// guarda o quadro entre as chamadas; present() copia o alvo para a imagem da cadeia de troca (a superfície do SDL) e a
// apresenta. Os comandos de um quadro vão num command buffer só, aberto no primeiro desenho e enviado no present() (ou
// antes, quando save_png precisa ler o alvo). Um render pass fica aberto enquanto os desenhos seguidos usam os mesmos
// anexos (o encoder do Metal); a névoa (que lê a profundidade), a limpeza do Z do meio do quadro e o 2D trocam de pass.
//
// O que o Vulkan pede a mais que o Metal:
// - os layouts das imagens: cada GImage guarda o seu (todos os mipmaps juntos) e as transições são barreiras completas
//   (ALL_COMMANDS para ALL_COMMANDS), feitas fora dos render passes; simples e corretas, não as mais finas;
// - a memória: um alocador próprio em blocos de 64 MB por tipo de memória (o Android limita as alocações), as imagens
//   grandes com memória própria;
// - o tempo de vida: o que sai de uso (uma textura trocada, os pedaços dos dados do quadro, os pools de descritores) só é
//   destruído quando a GPU acaba o command buffer que o usou (Core::retire, pelos números de envio);
// - os descritores: o conjunto 0 é o pedaço dos dados do quadro (uniform buffers dinâmicos), o 1 as duas texturas (em
//   cache por quadro), o 2 os deltas dos morphs de cada malha; as constantes pequenas vão em push constants.
//
// Os recursos: as texturas na memória da GPU, enviadas por um command buffer de envio (cópias de pedaços de passagem e a
// geração dos mipmaps por blits) que sai antes do do quadro; as malhas em buffers visíveis pela CPU (na memória unificada
// do Android é a mesma memória; no PC, a do sistema).
//
// O 2D vetorial (os menus Modern, a camada HD) é a UiVectorCanvas (render/ui_vector.h), como no Metal; no Android ainda
// não há uma (render/ui_vector_null.cpp): o jogo fica com os menus Original.

#include "render/renderer.h"

#include <SDL.h>
#include <SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "platform/display_modes.h"
#include "platform/image_io.h"
#include "platform/os.h"
#include "platform/vu1.h"
#include "render/gpu_adapters.h"
#include "render/gpu_adapters_vulkan.h"
#include "render/remix_d3d9.h"
#include "render/ui_hd.h"
#include "render/ui_vector.h"
#include "ui/modern_ui.h"

namespace nf {

namespace {

// Os shaders em SPIR-V (glslc -mfmt=c: uma lista de palavras entre chaves).
const uint32_t kVsMain[] =
#include "VS_MAIN.inc"
    ;
const uint32_t kVsSkin[] =
#include "VS_SKIN.inc"
    ;
const uint32_t kVsSkinMorph[] =
#include "VS_SKIN_MORPH.inc"
    ;
const uint32_t kVsFull[] =
#include "VS_FULL.inc"
    ;
const uint32_t kVsMovie[] =
#include "VS_MOVIE.inc"
    ;
const uint32_t kVsRect[] =
#include "VS_RECT.inc"
    ;
const uint32_t kPsMain[] =
#include "PS_MAIN.inc"
    ;
const uint32_t kPsA2c[] =
#include "PS_A2C.inc"
    ;
const uint32_t kPsFog[] =
#include "PS_FOG.inc"
    ;
const uint32_t kPsFogMs[] =
#include "PS_FOG_MS.inc"
    ;
const uint32_t kPsBlit[] =
#include "PS_BLIT.inc"
    ;
const uint32_t kPsDown[] =
#include "PS_DOWN.inc"
    ;
const uint32_t kPsBlur[] =
#include "PS_BLUR.inc"
    ;
const uint32_t kPsBlurMix[] =
#include "PS_BLUR_MIX.inc"
    ;
const uint32_t kPsMovie[] =
#include "PS_MOVIE.inc"
    ;
const uint32_t kPsComposite[] =
#include "PS_COMPOSITE.inc"
    ;

enum Module : int {
    kModVsMain, kModVsSkin, kModVsMorph, kModVsFull, kModVsMovie, kModVsRect,
    kModPsMain, kModPsA2c, kModPsFog, kModPsFogMs, kModPsBlit, kModPsDown, kModPsBlur, kModPsBlurMix, kModPsMovie,
    kModPsComposite, kModCount
};

// As constantes dos shaders, no layout dos blocos do GLSL (std140 nos uniform buffers, std430 nas push constants; iguais
// aos structs do Metal e aos constant buffers do D3D11).
struct FrameU {
    float view_proj[16];
    float light_dir[3];
    float ambient;
    float debug_shading;
    float lights_on;
    float pad[2];
};
static_assert(sizeof(FrameU) == 96);

struct ObjectU {
    float world[16];
    float tint[4];
    float light_pos[2][4];
    float light_col[2][4];
    float env_s[4];
    float env_t[4];
    float num_lights;
    float env;
    float premult;
    float sprite;
    float atst;
    float aref;
    float aint;
    float screen;
};
static_assert(sizeof(ObjectU) == 208);

static_assert(kMaxMorphTargets == 22, "o tamanho de w[] no shader acompanha kMaxMorphTargets");
static_assert(kMaxSkinBones == 128, "a paleta do skinning tem até 128 matrizes");
constexpr size_t kBonesBytes = size_t(kMaxSkinBones) * 64;
struct MorphU {
    float w[kMaxMorphTargets][4];
    uint32_t n;
    uint32_t count;
    uint32_t pad[2];
};
static_assert(sizeof(MorphU) == 368);

struct FogU {
    float color[4];
    float a, b;
    float nf, fmn, n;
    float pad[3];
};
static_assert(sizeof(FogU) == 48);

struct DownU {
    float src_size[2];
    float ratio[2];
    float taps;
    float pad[3];
};
static_assert(sizeof(DownU) == 32);

struct BlurU {
    float texel[2];
    float dir[2];
    float amount;
    float pad[3];
};
static_assert(sizeof(BlurU) == 32);

struct MovieU {
    float rect[4];
    float target[2];
    float tex_size[2];
};
static_assert(sizeof(MovieU) == 32);

struct RectU {
    float rect[4];
    float target[2];
    uint32_t pitch;
    uint32_t pad;
};
static_assert(sizeof(RectU) == 32);

constexpr uint32_t kPushBytes = 64;

static_assert(offsetof(Vertex, pos) == offsetof(SkinnedVertex, pos) && offsetof(Vertex, normal) == offsetof(SkinnedVertex, normal) &&
                  offsetof(Vertex, uv) == offsetof(SkinnedVertex, uv) && offsetof(Vertex, color) == offsetof(SkinnedVertex, color),
              "os quatro primeiros campos de Vertex e SkinnedVertex no mesmo lugar");

// Estado efetivo de um desenho: a equação do GS, o teste de alfa e o de Z (resolve_state de renderer_d3d11.cpp).
struct DrawState {
    uint64_t alpha = 0;
    bool abe = true;
    float atst = -1.0f;  // ATST, -1 sem teste
    float aref = 0.0f;
    bool aint = true;
    VkCompareOp zfunc = VK_COMPARE_OP_GREATER_OR_EQUAL;
    bool zwrite = true;
};

// Junta o estado da parte da malha (own), o do desenho (item) e, para o que ninguém escreveu, a regra antiga pelo modo de
// psiSetUpColourBlend: opaco testa alfa em 50% e escreve Z; misturado descarta só alfa < 0.004 e não escreve Z.
DrawState resolve_state(const GsState& own, const DrawItem& item) {
    DrawState s;
    const bool opaque = item.blend == Blend::Opaque;
    s.alpha = (own.has & GsState::kAlpha) ? own.alpha
                                           : ((item.gs.has & GsState::kAlpha) ? item.gs.alpha : gs_alpha_of_blend(item.blend));
    s.abe = own.abe != 0;
    const bool has_test = ((own.has | item.gs.has) & GsState::kTest) != 0;
    if (has_test) {
        const uint64_t test = (own.has & GsState::kTest) ? own.test : item.gs.test;
        if (test & 1) {  // ATE
            s.atst = float((test >> 1) & 7);
            s.aref = float((test >> 4) & 0xff);
        }
        if (((test >> 16) & 1) == 0) {
            s.zfunc = VK_COMPARE_OP_ALWAYS;  // ZTE desligado
        } else {
            switch ((test >> 17) & 3) {
                case 0: s.zfunc = VK_COMPARE_OP_NEVER; break;
                case 1: s.zfunc = VK_COMPARE_OP_ALWAYS; break;
                case 2: s.zfunc = VK_COMPARE_OP_GREATER_OR_EQUAL; break;  // Z invertido, como no GS
                default: s.zfunc = VK_COMPARE_OP_GREATER; break;
            }
        }
    } else {
        s.atst = 5.0f;  // GEQUAL, na escala contínua
        s.aref = opaque ? 64.0f : 0.004f * 128.0f;
        s.aint = false;
    }
    const bool has_zbuf = ((own.has | item.gs.has) & GsState::kZbuf) != 0;
    if (has_zbuf) {
        const uint64_t zbuf = (own.has & GsState::kZbuf) ? own.zbuf : item.gs.zbuf;
        s.zwrite = ((zbuf >> 32) & 1) == 0;
    } else {
        s.zwrite = opaque;
    }
    return s;
}

VkBlendFactor vk_factor(BlendFactor f) {
    switch (f) {
        case BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
        case BlendFactor::One: return VK_BLEND_FACTOR_ONE;
        case BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
        case BlendFactor::InvSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
        case BlendFactor::InvDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case BlendFactor::Fix: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        default: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    }
}

VkBlendOp vk_op(BlendOp op) {
    switch (op) {
        case BlendOp::Subtract: return VK_BLEND_OP_SUBTRACT;
        case BlendOp::RevSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
        default: return VK_BLEND_OP_ADD;
    }
}

bool uses_fix(const GsBlendEq& g) {
    const auto fix = [](BlendFactor f) { return f == BlendFactor::Fix || f == BlendFactor::InvFix; };
    return g.enable && (fix(g.src) || fix(g.dst));
}

// Os programas (os do Metal; os três primeiros com o pixel shader da cobertura como variante).
enum Prog : uint32_t {
    kProgMain = 0,       // vs_main + ps_main (ou ps_a2c)
    kProgSkin = 1,       // vs_skin
    kProgMorph = 2,      // vs_skin_morph
    kProgFog = 3,        // vs_full + ps_fog
    kProgFogMs = 4,      // vs_full + ps_fog_ms
    kProgBlit = 5,       // vs_full + ps_blit
    kProgBlitSample = 6, // vs_full + ps_blit (o texelFetch lê também as texturas comprimidas)
    kProgDown = 7,       // vs_full + ps_down
    kProgBlur = 8,       // vs_full + ps_blur
    kProgBlurMix = 9,    // vs_full + ps_blur_mix
    kProgMovie = 10,     // vs_movie + ps_movie
    kProgComposite = 11, // vs_rect + ps_composite (mistura do alfa pré-multiplicado)
    kProgHistory = 12,   // vs_full + ps_blit com a mistura pela constante (o rastro da tela)
};

constexpr size_t kChunkBytes = size_t(4) << 20;
constexpr size_t kUploadFlushBytes = size_t(64) << 20;
constexpr VkDeviceSize kBlockBytes = VkDeviceSize(64) << 20;
constexpr VkDeviceSize kDedicatedBytes = VkDeviceSize(16) << 20;
constexpr int kTsSlots = 64;

template <class T>
uint64_t hv(T h) {
    uint64_t v = 0;
    std::memcpy(&v, &h, sizeof(h));
    return v;
}

void vk_check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Vulkan: %s falhou (%d)", what, int(r));
        throw std::runtime_error(buf);
    }
}

std::atomic<uint64_t> g_validation_errors{0};

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++g_validation_errors;
        std::fprintf(stderr, "vulkan %s: %s\n", severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "erro" : "aviso",
                     data != nullptr && data->pMessage != nullptr ? data->pMessage : "?");
    }
    return VK_FALSE;
}

// ---------------------------------------------------------------- o dispositivo, a memória e o tempo de vida

struct MemBlock {
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    uint8_t* map = nullptr;
    uint32_t type = 0;
    bool image = false;
    std::vector<std::pair<VkDeviceSize, VkDeviceSize>> free;  // (início, tamanho), em ordem
};

struct Allocation {
    MemBlock* block = nullptr;           // nulo: memória própria (mem)
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize offset = 0, size = 0;
    uint8_t* ptr = nullptr;              // mapeada (as visíveis pela CPU)
};

// O dispositivo e o que vive tanto quanto ele: compartilhado (shared_ptr) pelas imagens e buffers, que podem sair depois
// do Renderer (as referências de keep_texture).
struct Core {
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memprops{};
    VkPhysicalDeviceFeatures features{};  // as ligadas

    std::vector<std::unique_ptr<MemBlock>> blocks;

    // O tempo de vida: o número do último envio (submitted) e do último que a GPU acabou (completed); o que sai de uso vai
    // para pending e, no próximo envio que o cobre, para a cova com o número dele.
    uint64_t submitted = 0, completed = 0;
    std::vector<std::function<void()>> pending;
    std::deque<std::pair<uint64_t, std::function<void()>>> grave;
    bool closed = false;

    std::map<std::array<uint64_t, 4>, VkFramebuffer> framebuffers;

    ~Core() {
        if (device != VK_NULL_HANDLE) vkDeviceWaitIdle(device);
        closed = true;
        for (auto& f : pending) f();
        pending.clear();
        while (!grave.empty()) {
            auto f = std::move(grave.front().second);
            grave.pop_front();
            f();
        }
        for (auto& [k, fb] : framebuffers) vkDestroyFramebuffer(device, fb, nullptr);
        for (auto& b : blocks) vkFreeMemory(device, b->mem, nullptr);
        if (device != VK_NULL_HANDLE) vkDestroyDevice(device, nullptr);
        if (messenger != VK_NULL_HANDLE) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (destroy != nullptr) destroy(instance, messenger, nullptr);
        }
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
    }

    void retire(std::function<void()> fn) {
        if (closed) {
            fn();
            return;
        }
        pending.push_back(std::move(fn));
    }
    void tag_pending(uint64_t serial) {
        for (auto& f : pending) grave.emplace_back(serial, std::move(f));
        pending.clear();
    }
    void run_grave() {
        while (!grave.empty() && grave.front().first <= completed) {
            auto f = std::move(grave.front().second);
            grave.pop_front();
            f();
        }
    }

    int memory_type(uint32_t bits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < memprops.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return int(i);
        }
        return -1;
    }

    Allocation alloc(const VkMemoryRequirements& req, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback, bool image) {
        int type = memory_type(req.memoryTypeBits, want);
        if (type < 0) type = memory_type(req.memoryTypeBits, fallback);
        if (type < 0) return {};
        const bool host = (memprops.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
        Allocation a;
        if (req.size >= kDedicatedBytes) {
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = uint32_t(type);
            if (vkAllocateMemory(device, &ai, nullptr, &a.mem) != VK_SUCCESS) return {};
            a.size = req.size;
            if (host) {
                void* p = nullptr;
                if (vkMapMemory(device, a.mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
                    vkFreeMemory(device, a.mem, nullptr);
                    return {};
                }
                a.ptr = static_cast<uint8_t*>(p);
            }
            return a;
        }
        const VkDeviceSize align = std::max<VkDeviceSize>(req.alignment, 16);
        const auto carve = [&](MemBlock& b) -> bool {
            for (size_t i = 0; i < b.free.size(); ++i) {
                const VkDeviceSize start = b.free[i].first, len = b.free[i].second;
                const VkDeviceSize at = (start + align - 1) / align * align;
                if (at + req.size > start + len) continue;
                const VkDeviceSize tail = start + len - (at + req.size);
                b.free.erase(b.free.begin() + long(i));
                if (tail > 0) b.free.insert(b.free.begin() + long(i), {at + req.size, tail});
                if (at > start) b.free.insert(b.free.begin() + long(i), {start, at - start});
                a.block = &b;
                a.mem = b.mem;
                a.offset = at;
                a.size = req.size;
                a.ptr = b.map != nullptr ? b.map + at : nullptr;
                return true;
            }
            return false;
        };
        for (auto& b : blocks) {
            if (b->type == uint32_t(type) && b->image == image && carve(*b)) return a;
        }
        auto b = std::make_unique<MemBlock>();
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = kBlockBytes;
        ai.memoryTypeIndex = uint32_t(type);
        if (vkAllocateMemory(device, &ai, nullptr, &b->mem) != VK_SUCCESS) return {};
        b->size = kBlockBytes;
        b->type = uint32_t(type);
        b->image = image;
        b->free.push_back({0, kBlockBytes});
        if (host) {
            void* p = nullptr;
            if (vkMapMemory(device, b->mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
                vkFreeMemory(device, b->mem, nullptr);
                return {};
            }
            b->map = static_cast<uint8_t*>(p);
        }
        blocks.push_back(std::move(b));
        if (!carve(*blocks.back())) return {};
        return a;
    }

    void free(const Allocation& a) {
        if (a.mem == VK_NULL_HANDLE) return;
        if (a.block == nullptr) {
            vkFreeMemory(device, a.mem, nullptr);
            return;
        }
        auto& f = a.block->free;
        auto it = std::lower_bound(f.begin(), f.end(), std::make_pair(a.offset, VkDeviceSize(0)));
        it = f.insert(it, {a.offset, a.size});
        // Junta com o vizinho de depois e o de antes.
        if (it + 1 != f.end() && it->first + it->second == (it + 1)->first) {
            it->second += (it + 1)->second;
            f.erase(it + 1);
        }
        if (it != f.begin() && (it - 1)->first + (it - 1)->second == it->first) {
            (it - 1)->second += it->second;
            f.erase(it);
        }
    }

    VkFramebuffer framebuffer(VkRenderPass rp, VkImageView color, VkImageView depth, uint32_t w, uint32_t h) {
        const std::array<uint64_t, 4> key{hv(rp), hv(color), hv(depth), uint64_t(w) << 32 | h};
        auto it = framebuffers.find(key);
        if (it != framebuffers.end()) return it->second;
        VkImageView views[2] = {color, depth};
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = rp;
        fi.attachmentCount = depth != VK_NULL_HANDLE ? 2 : 1;
        fi.pAttachments = views;
        fi.width = w;
        fi.height = h;
        fi.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        vk_check(vkCreateFramebuffer(device, &fi, nullptr, &fb), "vkCreateFramebuffer");
        framebuffers.emplace(key, fb);
        return fb;
    }
    void forget_view(VkImageView v) {
        if (v == VK_NULL_HANDLE) return;
        const uint64_t h = hv(v);
        for (auto it = framebuffers.begin(); it != framebuffers.end();) {
            if (it->first[1] == h || it->first[2] == h) {
                vkDestroyFramebuffer(device, it->second, nullptr);
                it = framebuffers.erase(it);
            } else {
                ++it;
            }
        }
    }
};

// Uma imagem da GPU (o GpuImage da fachada é um GImage*).
struct GImage {
    std::shared_ptr<Core> core;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;      // todos os mipmaps (a leitura)
    VkImageView att_view = VK_NULL_HANDLE;  // o nível 0 (o anexo de um render pass)
    Allocation mem;
    VkFormat format = VK_FORMAT_UNDEFINED;
    int w = 0, h = 0;
    uint32_t mips = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool owned = true;  // false: uma imagem da cadeia de troca

    ~GImage() {
        Core* c = core.get();
        if (c == nullptr) return;
        const VkImageView v = view, av = att_view;
        const VkImage img = image;
        const bool own = owned;
        const Allocation m = mem;
        c->retire([c, v, av, img, own, m] {
            c->forget_view(v);
            if (av != v) c->forget_view(av);
            if (av != VK_NULL_HANDLE && av != v) vkDestroyImageView(c->device, av, nullptr);
            if (v != VK_NULL_HANDLE) vkDestroyImageView(c->device, v, nullptr);
            if (own && img != VK_NULL_HANDLE) vkDestroyImage(c->device, img, nullptr);
            c->free(m);
        });
    }
};

struct GBuffer {
    std::shared_ptr<Core> core;
    VkBuffer buf = VK_NULL_HANDLE;
    Allocation mem;
    VkDeviceSize size = 0;
    uint8_t* ptr() const { return mem.ptr; }

    ~GBuffer() {
        Core* c = core.get();
        if (c == nullptr) return;
        const VkBuffer b = buf;
        const Allocation m = mem;
        c->retire([c, b, m] {
            if (b != VK_NULL_HANDLE) vkDestroyBuffer(c->device, b, nullptr);
            c->free(m);
        });
    }
};

// Um pedaço dos dados de cada quadro (ou de passagem dos envios), com o conjunto 0 dos descritores apontando para ele.
struct Chunk {
    std::shared_ptr<GBuffer> buf;
    VkDescriptorSet set0 = VK_NULL_HANDLE;
};
using ChunkList = std::vector<std::shared_ptr<Chunk>>;

// Os deltas dos morphs de uma malha (o conjunto 2), com um pool só dele.
struct MorphSet {
    std::shared_ptr<Core> core;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    ~MorphSet() {
        Core* c = core.get();
        if (c == nullptr || pool == VK_NULL_HANDLE) return;
        const VkDescriptorPool p = pool;
        c->retire([c, p] { vkDestroyDescriptorPool(c->device, p, nullptr); });
    }
};

inline GImage* gimage_of(GpuImage* p) { return reinterpret_cast<GImage*>(p); }
inline GpuImage* image_of(GImage* t) { return reinterpret_cast<GpuImage*>(t); }

}  // namespace

uint64_t gs_alpha_of_blend(Blend mode) {
    // O mesmo de renderer_d3d11.cpp (os números de psiSetUpColourBlend).
    switch (mode) {
        case Blend::Additive: return 0x48;
        case Blend::Alpha: return 0x44;
        case Blend::Subtractive: return 0x42;
        default: return 0x2a;
    }
}

const char* gpu_phase_name(GpuPhase p) {
    switch (p) {
        case GpuPhase::Scene: return "cena";
        case GpuPhase::Fog: return "nevoa";
        case GpuPhase::Resolve: return "resolucao";
        case GpuPhase::TwoD: return "2d";
        case GpuPhase::Blur: return "desfoque";
        case GpuPhase::Modern: return "direct2d";
        default: return "?";
    }
}

// O caminho de Direct3D 9 para o RTX Remix só existe no Windows (render/remix_d3d9.h).
std::unique_ptr<RemixD3D9Renderer> create_remix_d3d9(void*, int, int, bool, const AdapterInfo*) {
    throw std::runtime_error("o caminho de Direct3D 9 para o RTX Remix (--renderer d3d9ff) só existe no Windows");
}

void* create_remix_hidden_window(int, int) { return nullptr; }

struct Renderer::Impl {
    struct GpuMesh {
        std::shared_ptr<GBuffer> vb, ib;
        std::vector<SubMesh> submeshes;
        bool skinned = false;
        int bones = 0;
        std::shared_ptr<GBuffer> morph;  // os deltas (float3 sem folga)
        std::shared_ptr<MorphSet> morph_set;
        uint32_t morph_elements = 0;
        int morph_targets = 0;
    };
    struct HdAsset {
        int index = -1, w = 0, h = 0;
        std::shared_ptr<GImage> tex;
        uint64_t used = 0;
    };
    struct Alloc {
        VkBuffer buf = VK_NULL_HANDLE;
        size_t offset = 0;
        uint8_t* ptr = nullptr;
    };
    struct Stream {
        ChunkList used;
        std::shared_ptr<Chunk> cur;
        size_t off = 0;
    };
    struct Inflight {
        uint64_t serial = 0;
        VkFence fence = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
    };
    struct TsEntry {
        uint64_t seq = 0, serial = 0, epoch = 0;
        int slot = 0;
    };

    Impl(void* win, int width, int height, bool vsync, bool offscreen, const AdapterInfo* want);
    ~Impl();

    // ---- o dispositivo e o estado fixo (core primeiro: é destruído por último) ----
    std::shared_ptr<Core> core;
    VkDevice dev = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layouts[3]{};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule modules[kModCount]{};
    std::map<std::array<uint32_t, 4>, VkPipeline> pipelines;
    std::map<std::array<uint32_t, 5>, VkRenderPass> passes;
    VkSampler sampler = VK_NULL_HANDLE;         // a cena: o filtro das opções, repetindo
    VkSampler sampler_point = VK_NULL_HANDLE;   // 2D: de ponto, nível 0, preso
    VkSampler sampler_linear = VK_NULL_HANDLE;  // 2D: bilinear, nível 0, preso (e a redução, o desfoque, o filme)
    VkSampler sampler_smooth = VK_NULL_HANDLE;  // camada HD: bilinear com mipmaps
    std::shared_ptr<GImage> white, no_texture;
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
    VkSampleCountFlags msaa_counts = VK_SAMPLE_COUNT_1_BIT;
    size_t ualign = 256;
    void* window = nullptr;
    AdapterInfo adapter_;
    bool adapter_chosen_ = false;
    std::vector<DisplayMode> modes;
    DisplayMode desktop;

    int width_ = 1, height_ = 1;
    bool vsync_ = true;
    bool offscreen_ = false;
    bool exclusive_ = false;
    TextureFilter filter_ = TextureFilter::Aniso8;

    // ---- a cadeia de troca ----
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat sc_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D sc_extent{0, 0};
    std::vector<std::shared_ptr<GImage>> sc_images;
    std::vector<VkSemaphore> sc_done;
    bool sc_dirty = true;
    struct AcqSem {
        VkSemaphore sem = VK_NULL_HANDLE;
        uint64_t serial = 0;
    };
    std::array<AcqSem, 4> acq{};
    int acq_i = 0;

    // ---- os alvos ----
    std::shared_ptr<GImage> target;          // o quadro (BGRA8, width_ x height_)
    std::shared_ptr<GImage> depth;           // a profundidade da cena (com MSAA ou não)
    std::shared_ptr<GImage> scene_color;     // a cor própria da cena (com escala ou MSAA); nula: a cena vai direto ao alvo
    std::shared_ptr<GImage> scene_resolved;  // com MSAA e escala: a cena resolvida, antes de reduzir
    int scene_w = 0, scene_h = 0, scene_scale = 100, scene_msaa = 1;

    // ---- os recursos da fase e os do port ----
    std::vector<GpuMesh> meshes;
    std::vector<std::shared_ptr<GImage>> textures, hd_textures;
    std::vector<std::pair<uint32_t, std::shared_ptr<GImage>>> ui_images;
    bool use_hd = true;
    std::shared_ptr<GBuffer> quad_ib;  // 0 1 2, 0 2 3 por quadrado (sprites e 2D)
    size_t quad_ib_quads = 0;
    std::shared_ptr<GImage> blur_full, blur_tex[2];
    int blur_w = 0, blur_h = 0;
    std::shared_ptr<GImage> history;  // o quadro anterior (screen_blur)
    bool history_valid = false;
    std::shared_ptr<GImage> snapshot;  // a tela de carga
    std::shared_ptr<GImage> movie_tex;
    uint64_t movie_serial = ~0ull;
    bool movie_on = false;
    float movie_rect[4]{};

    // ---- o desenho vetorial (render/ui_vector.h) ----
    std::unique_ptr<UiVectorCanvas> canvas;
    bool canvas_tried = false;
    const UiHdConfig* hd_config = nullptr;
    bool hd_on = false;
    std::vector<HdAsset> hd_assets;
    std::vector<uint8_t> hd_failed;
    uint64_t frame_no = 0;

    // ---- a gravação dos comandos ----
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> free_cbs;
    std::vector<VkFence> free_fences;
    std::deque<Inflight> inflight;
    VkCommandBuffer cmd = VK_NULL_HANDLE;  // o do quadro
    bool in_pass = false;
    GImage* pass_color = nullptr;
    GImage* pass_depth = nullptr;
    VkPipeline cur_pipe = VK_NULL_HANDLE;
    bool cur_scissor_full = true;
    VkRect2D cur_scissor{};
    FrameU frame_u{};
    bool wireframe = false;
    bool in_2d = false;
    VkCommandBuffer up_cmd = VK_NULL_HANDLE;  // o de envio (texturas, mipmaps)
    size_t up_bytes = 0;
    std::shared_ptr<ChunkList> free_chunks = std::make_shared<ChunkList>();
    std::vector<VkDescriptorPool> chunk_set_pools;
    uint32_t chunk_sets_left = 0;
    Stream frame_stream, up_stream;
    size_t frame_off = SIZE_MAX;  // o FrameU no pedaço atual do quadro
    // Os conjuntos das texturas: em cache por quadro, de pools que voltam ao uso quando a GPU acaba o quadro.
    std::shared_ptr<std::vector<VkDescriptorPool>> free_tex_pools = std::make_shared<std::vector<VkDescriptorPool>>();
    std::vector<VkDescriptorPool> tex_pools;  // os do quadro (o último é o atual)
    std::map<std::array<uint64_t, 4>, VkDescriptorSet> tex_sets;
    uint64_t last_frame_serial = 0;
    std::array<uint64_t, 3> throttle{};
    int throttle_i = 0;

    // ---- os contadores e as medidas ----
    RenderCounters counters_;
    uint32_t env_parts_ = 0, a2c_parts_ = 0;
    bool alpha_coverage = true;
    bool profiling = false;
    VkQueryPool ts_pool = VK_NULL_HANDLE;
    double ts_period = 1.0;
    uint64_t ts_mask = ~0ull;
    std::array<bool, kTsSlots> ts_busy{};
    int ts_next = 0;
    int cur_ts_slot = -1;
    std::deque<TsEntry> ts_pending;
    uint64_t epoch = 0;
    uint64_t gpu_seq = 0, cur_seq = 0;
    bool seq_valid = false;
    std::map<uint64_t, std::pair<int, double>> seq_pending;  // quadro -> (command buffers enviados, ms somados)
    std::map<uint64_t, int> seq_done;                         // quadro -> command buffers acabados
    GpuTimes gpu_times;
    std::vector<GpuFrameTimes> per_frame;

    // ---- o estado fixo ----
    void create_device(const AdapterInfo* want);
    VkRenderPass render_pass(VkFormat color, VkSampleCountFlagBits samples, VkFormat dfmt, VkAttachmentLoadOp cl,
                             VkAttachmentLoadOp dl);
    VkPipeline pipeline(uint32_t prog, const GsBlendEq* gb, bool coverage, VkSampleCountFlagBits samples, bool has_depth,
                        VkFormat format = VK_FORMAT_B8G8R8A8_UNORM, VkCompareOp zfunc = VK_COMPARE_OP_ALWAYS,
                        bool zwrite = false, bool lines = false);
    std::shared_ptr<GImage> make_image(VkFormat format, int w, int h, bool mips, VkSampleCountFlagBits samples,
                                       VkImageUsageFlags usage, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
                                       uint32_t mip_count = 0);
    std::shared_ptr<GBuffer> make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, const void* data = nullptr);
    VkSampler make_sampler(VkFilter filter, VkSamplerMipmapMode mip, VkSamplerAddressMode address, float max_lod, float aniso);
    void create_targets();
    void create_scene_targets();
    bool try_scene_targets(int scale_percent, int msaa);
    bool separate_scene() const { return scene_color != nullptr; }
    GImage* scene_target() const { return scene_color != nullptr ? scene_color.get() : target.get(); }
    GImage* texture_for(int slot) const;

    // ---- os comandos ----
    VkCommandBuffer new_cb();
    VkFence new_fence();
    uint64_t submit(VkCommandBuffer cb, VkSemaphore wait_sem, VkSemaphore signal_sem);
    void poll();
    void wait_serial(uint64_t serial);
    VkCommandBuffer frame_cmd();
    void barrier(VkCommandBuffer cb, GImage* img, VkImageLayout to);
    void transition(GImage* img, VkImageLayout to);
    void ensure_read(GImage* img);
    void end_pass();
    void begin_pass(GImage* color, GImage* depth_img, VkAttachmentLoadOp color_load, const float* clear, bool clear_depth);
    void use_pass(GImage* color, GImage* depth_img);
    void item_pass();
    void target_pass();
    void bind_pipe(VkPipeline p);
    void set_scissor_full();
    bool set_scissor(const float clip[4]);
    void push(const void* bytes, size_t len);
    VkDescriptorSet tex_set(GImage* t0, VkSampler s0, GImage* t1 = nullptr, VkSampler s1 = VK_NULL_HANDLE);
    void bind_tex(GImage* t0, VkSampler s0, GImage* t1 = nullptr, VkSampler s1 = VK_NULL_HANDLE);
    std::shared_ptr<Chunk> get_chunk(size_t want);
    void room(Stream& s, size_t bytes);
    Alloc stream_alloc(Stream& s, size_t bytes, size_t align);
    Alloc transient(size_t bytes);
    void prepare(size_t uniform_bytes);
    size_t frame_offset();
    size_t uniform(const void* data, size_t len);
    void bind_set0(size_t obj, size_t bones, size_t morph, size_t px = 0);
    void retire_stream(Stream& s);
    void commit_frame(VkSemaphore wait_sem = VK_NULL_HANDLE, VkSemaphore signal_sem = VK_NULL_HANDLE);
    VkCommandBuffer upload_cmd();
    uint64_t flush_uploads();
    void upload(GImage* img, uint32_t level, const void* data, size_t bytes_per_row, size_t rows, uint32_t w, uint32_t h,
                VkCommandBuffer cb = VK_NULL_HANDLE);
    void gen_mips(VkCommandBuffer cb, GImage* img);
    void finish_upload(GImage* img, VkCommandBuffer cb = VK_NULL_HANDLE);
    void copy_texture(GImage* src, GImage* dst);
    void draw_full(uint32_t prog, GImage* dst, GImage* src, const void* bytes, size_t len);
    void ensure_quad_ib(size_t quads);

    // ---- o desenho ----
    void render(const std::vector<DrawItem>& items, const FrameParams& frame);
    void render_2d(const std::vector<DrawItem>& items, const FrameParams& frame);
    void set_frame(const FrameParams& frame);
    void draw_item(const DrawItem& inst, const FrameParams& frame);
    size_t set_state(const DrawItem& inst, const GsState& own, uint32_t prog, bool sprite, bool env = false,
                     bool screen = false);
    void draw_mesh(const DrawItem& inst, const FrameParams& frame);
    void draw_sprites(const DrawItem& inst);
    void draw_triangles(const DrawItem& inst);
    void draw_2d(const DrawItem& inst);
    void draw_fog(const DrawItem& inst, const FrameParams& frame);
    void draw_movie();
    void draw_ui(const UiHdBatch& batch);
    void composite(const UiRaster& raster);
    void resolve_scene();
    GImage* hd_asset(int index, int w, int h);
    void ensure_blur_targets();
    void blur_background(float amount);
    void screen_blur(float keep);
    bool create_surface();
    bool create_swapchain();
    void destroy_swapchain(bool now);
    void present();
    void save_png(const std::wstring& path);
    bool read_texture(int slot, int& width, int& height, std::vector<uint8_t>& rgba);
    void resize(int width, int height);
    bool set_scene_quality(int scale_percent, int msaa);
    void set_texture_filter(TextureFilter f);
    void set_texture(int slot, const Texture& t);
    void update_texture(int slot, const Texture& t);
    bool set_texture_dds(int slot, const std::wstring& path);
    void set_ui_image(uint32_t key, const uint8_t* rgba, int width, int height);
    int add_mesh(const EntityMesh& mesh);
    void set_movie_frame(const uint8_t* bgra, int width, int height, uint64_t serial);
    void ensure_canvas();
    void gpu_collect(bool wait);
};

// ---------------------------------------------------------------- o dispositivo

void Renderer::Impl::create_device(const AdapterInfo* want) {
    core = std::make_shared<Core>();
    Core& c = *core;
    // As extensões da instância: as que o SDL pede para a superfície da janela e, com NF_VK_VALIDATION, a camada de
    // validação e o debug_utils.
    std::vector<const char*> exts;
    if (!offscreen_ && window != nullptr) {
        unsigned n = 0;
        auto* win = static_cast<SDL_Window*>(window);
        if (SDL_Vulkan_GetInstanceExtensions(win, &n, nullptr) && n > 0) {
            exts.resize(n);
            SDL_Vulkan_GetInstanceExtensions(win, &n, exts.data());
            exts.resize(n);
        }
    }
    std::vector<const char*> layers;
    const char* val = std::getenv("NF_VK_VALIDATION");
    bool debug_utils = false;
    if (val != nullptr && val[0] != '\0' && val[0] != '0') {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> lp(n);
        vkEnumerateInstanceLayerProperties(&n, lp.data());
        for (const auto& l : lp) {
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) layers.push_back("VK_LAYER_KHRONOS_validation");
        }
        uint32_t ne = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> ep(ne);
        vkEnumerateInstanceExtensionProperties(nullptr, &ne, ep.data());
        for (const auto& e : ep) {
            if (std::strcmp(e.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) debug_utils = true;
        }
        if (debug_utils) exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if (layers.empty()) std::fprintf(stderr, "vulkan: NF_VK_VALIDATION pedido, mas a camada de validação não existe\n");
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Nightfire PC";
    app.applicationVersion = 1;
    app.pEngineName = "nightfire";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app;
    ii.enabledExtensionCount = uint32_t(exts.size());
    ii.ppEnabledExtensionNames = exts.data();
    ii.enabledLayerCount = uint32_t(layers.size());
    ii.ppEnabledLayerNames = layers.data();
    vk_check(vkCreateInstance(&ii, nullptr, &c.instance), "vkCreateInstance");
    if (debug_utils && !layers.empty()) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(c.instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create != nullptr) {
            VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mi.pfnUserCallback = debug_callback;
            create(c.instance, &mi, nullptr, &c.messenger);
        }
    }
    if (!offscreen_ && window != nullptr) create_surface();

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(c.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> all(count);
    vkEnumeratePhysicalDevices(c.instance, &count, all.data());
    if (all.empty()) throw std::runtime_error("sem dispositivo Vulkan");
    // Uma família com gráficos (e a apresentação na superfície, quando há).
    const auto family_of = [&](VkPhysicalDevice p) -> int {
        uint32_t nf = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(p, &nf, nullptr);
        std::vector<VkQueueFamilyProperties> fp(nf);
        vkGetPhysicalDeviceQueueFamilyProperties(p, &nf, fp.data());
        for (uint32_t i = 0; i < nf; ++i) {
            if ((fp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) continue;
            if (surface != VK_NULL_HANDLE) {
                VkBool32 ok = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(p, i, surface, &ok);
                if (!ok) continue;
            }
            return int(i);
        }
        return -1;
    };
    int index = -1;
    // O adaptador pedido ([video] adapter, --adapter): o mesmo fabricante e modelo (os campos do LUID), ou o índice.
    if (want != nullptr) {
        for (uint32_t i = 0; i < count && index < 0; ++i) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(all[i], &p);
            if (p.deviceID == want->luid_low && int32_t(p.vendorID) == want->luid_high && family_of(all[i]) >= 0) {
                index = int(i);
            }
        }
        if (index < 0 && want->index >= 0 && uint32_t(want->index) < count && family_of(all[uint32_t(want->index)]) >= 0) {
            index = want->index;
        }
        adapter_chosen_ = index >= 0;
    }
    // O padrão: dedicado, integrado, virtual, CPU, nessa ordem.
    if (index < 0) {
        int best = -1, best_rank = 99;
        for (uint32_t i = 0; i < count; ++i) {
            if (family_of(all[i]) < 0) continue;
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(all[i], &p);
            int rank = 4;
            switch (p.deviceType) {
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: rank = 0; break;
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: rank = 1; break;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: rank = 2; break;
                case VK_PHYSICAL_DEVICE_TYPE_CPU: rank = 3; break;
                default: break;
            }
            if (rank < best_rank) {
                best_rank = rank;
                best = int(i);
            }
        }
        index = best;
    }
    if (index < 0) throw std::runtime_error("sem dispositivo Vulkan com gráficos (e a apresentação na janela)");
    c.phys = all[size_t(index)];
    c.family = uint32_t(family_of(c.phys));
    vkGetPhysicalDeviceProperties(c.phys, &c.props);
    vkGetPhysicalDeviceMemoryProperties(c.phys, &c.memprops);
    adapter_ = vulkan_adapter_info(c.phys, index);

    VkPhysicalDeviceFeatures have{};
    vkGetPhysicalDeviceFeatures(c.phys, &have);
    VkPhysicalDeviceFeatures on{};
    on.textureCompressionBC = have.textureCompressionBC;
    on.samplerAnisotropy = have.samplerAnisotropy;
    on.sampleRateShading = have.sampleRateShading;
    on.fillModeNonSolid = have.fillModeNonSolid;
    c.features = on;
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = c.family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    std::vector<const char*> dexts;
    if (surface != VK_NULL_HANDLE) dexts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = uint32_t(dexts.size());
    di.ppEnabledExtensionNames = dexts.data();
    di.pEnabledFeatures = &on;
    vk_check(vkCreateDevice(c.phys, &di, nullptr, &c.device), "vkCreateDevice");
    vkGetDeviceQueue(c.device, c.family, 0, &c.queue);
    dev = c.device;

    const VkPhysicalDeviceLimits& lim = c.props.limits;
    ualign = std::max<size_t>({256, size_t(lim.minUniformBufferOffsetAlignment), size_t(lim.minStorageBufferOffsetAlignment)});
    // A profundidade: D32 (a do Metal e do D3D11), senão a melhor que seja anexo e lida pelo shader (a névoa).
    for (VkFormat f : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM}) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(c.phys, f, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((fp.optimalTilingFeatures & need) == need) {
            depth_format = f;
            break;
        }
    }
    // O MSAA: precisa do sample shading (a névoa por amostra) e das contagens da cor, da profundidade e da profundidade lida.
    msaa_counts = VK_SAMPLE_COUNT_1_BIT;
    if (on.sampleRateShading) {
        msaa_counts = lim.framebufferColorSampleCounts & lim.framebufferDepthSampleCounts & lim.sampledImageDepthSampleCounts;
    }
    // As medidas do --bench: os carimbos de tempo, se a fila os tem.
    uint32_t nf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &nf, nullptr);
    std::vector<VkQueueFamilyProperties> fp(nf);
    vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &nf, fp.data());
    const uint32_t bits = fp[c.family].timestampValidBits;
    if (bits > 0 && lim.timestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo qp{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qp.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qp.queryCount = kTsSlots * 2;
        if (vkCreateQueryPool(dev, &qp, nullptr, &ts_pool) != VK_SUCCESS) ts_pool = VK_NULL_HANDLE;
        ts_period = double(lim.timestampPeriod);
        ts_mask = bits >= 64 ? ~0ull : ((uint64_t(1) << bits) - 1);
    }
}

Renderer::Impl::Impl(void* win, int width, int height, bool vsync, bool offscreen, const AdapterInfo* want)
    : window(win), width_(std::max(1, width)), height_(std::max(1, height)), vsync_(vsync), offscreen_(offscreen) {
    create_device(want);

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = core->family;
    vk_check(vkCreateCommandPool(dev, &pi, nullptr, &cmd_pool), "vkCreateCommandPool");

    // Os conjuntos de descritores (o cabeçalho de renderer_vulkan.glsl).
    {
        VkDescriptorSetLayoutBinding b[5]{};
        const VkShaderStageFlags vf = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        for (uint32_t i = 0; i < 4; ++i) {
            b[i].binding = i;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            b[i].descriptorCount = 1;
            b[i].stageFlags = vf;
        }
        b[4].binding = 4;
        b[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        b[4].descriptorCount = 1;
        b[4].stageFlags = vf;
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 5;
        li.pBindings = b;
        vk_check(vkCreateDescriptorSetLayout(dev, &li, nullptr, &set_layouts[0]), "set 0");
        VkDescriptorSetLayoutBinding t[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            t[i].binding = i;
            t[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            t[i].descriptorCount = 1;
            t[i].stageFlags = vf;
        }
        li.bindingCount = 2;
        li.pBindings = t;
        vk_check(vkCreateDescriptorSetLayout(dev, &li, nullptr, &set_layouts[1]), "set 1");
        VkDescriptorSetLayoutBinding m{};
        m.binding = 0;
        m.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        m.descriptorCount = 1;
        m.stageFlags = vf;
        li.bindingCount = 1;
        li.pBindings = &m;
        vk_check(vkCreateDescriptorSetLayout(dev, &li, nullptr, &set_layouts[2]), "set 2");
        VkPushConstantRange pr{vf, 0, kPushBytes};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 3;
        pl.pSetLayouts = set_layouts;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pr;
        vk_check(vkCreatePipelineLayout(dev, &pl, nullptr, &layout), "vkCreatePipelineLayout");
    }

    // Os módulos dos shaders.
    {
        const std::pair<const uint32_t*, size_t> code[kModCount] = {
            {kVsMain, sizeof(kVsMain)},     {kVsSkin, sizeof(kVsSkin)},     {kVsSkinMorph, sizeof(kVsSkinMorph)},
            {kVsFull, sizeof(kVsFull)},     {kVsMovie, sizeof(kVsMovie)},   {kVsRect, sizeof(kVsRect)},
            {kPsMain, sizeof(kPsMain)},     {kPsA2c, sizeof(kPsA2c)},       {kPsFog, sizeof(kPsFog)},
            {kPsFogMs, sizeof(kPsFogMs)},   {kPsBlit, sizeof(kPsBlit)},     {kPsDown, sizeof(kPsDown)},
            {kPsBlur, sizeof(kPsBlur)},     {kPsBlurMix, sizeof(kPsBlurMix)}, {kPsMovie, sizeof(kPsMovie)},
            {kPsComposite, sizeof(kPsComposite)},
        };
        for (int i = 0; i < kModCount; ++i) {
            VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mi.codeSize = code[i].second;
            mi.pCode = code[i].first;
            vk_check(vkCreateShaderModule(dev, &mi, nullptr, &modules[i]), "vkCreateShaderModule");
        }
    }

    // 2D: o nível 0 da textura (o GS não tem mipmaps nos sprites), de ponto nas fontes e bilinear no resto.
    sampler_point = make_sampler(VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0.0f, 0.0f);
    sampler_linear = make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0.0f, 0.0f);
    sampler_smooth = make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1000.0f, 0.0f);
    set_texture_filter(TextureFilter::Aniso8);

    // A textura branca (sem textura) e a 0x80 em tudo (kQuadNoTexture: a modulação devolve a cor e o alfa do vértice).
    const uint32_t white_px = 0xFFFFFFFFu, gs_one = 0x80808080u;
    const VkImageUsageFlags tex_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    white = make_image(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, false, VK_SAMPLE_COUNT_1_BIT, tex_usage);
    no_texture = make_image(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, false, VK_SAMPLE_COUNT_1_BIT, tex_usage);
    if (white == nullptr || no_texture == nullptr) throw std::runtime_error("textura branca: sem memória");
    upload(white.get(), 0, &white_px, 4, 1, 1, 1);
    finish_upload(white.get());
    upload(no_texture.get(), 0, &gs_one, 4, 1, 1, 1);
    finish_upload(no_texture.get());
    flush_uploads();

    system_display_modes(window, &modes, &desktop);
    create_targets();
}

Renderer::Impl::~Impl() {
    try {
        end_pass();
        commit_frame();
        flush_uploads();
    } catch (...) {
    }
    vkDeviceWaitIdle(dev);
    poll();
    core->tag_pending(core->submitted);
    core->completed = core->submitted;
    core->run_grave();
    for (auto& f : inflight) {
        vkDestroyFence(dev, f.fence, nullptr);
    }
    inflight.clear();
    destroy_swapchain(true);
    sc_images.clear();
    for (auto& [k, p] : pipelines) vkDestroyPipeline(dev, p, nullptr);
    for (auto& [k, p] : passes) vkDestroyRenderPass(dev, p, nullptr);
    for (VkSampler s : {sampler, sampler_point, sampler_linear, sampler_smooth}) {
        if (s != VK_NULL_HANDLE) vkDestroySampler(dev, s, nullptr);
    }
    for (VkShaderModule m : modules) {
        if (m != VK_NULL_HANDLE) vkDestroyShaderModule(dev, m, nullptr);
    }
    for (VkDescriptorPool p : chunk_set_pools) vkDestroyDescriptorPool(dev, p, nullptr);
    for (VkDescriptorPool p : tex_pools) vkDestroyDescriptorPool(dev, p, nullptr);
    for (VkDescriptorPool p : *free_tex_pools) vkDestroyDescriptorPool(dev, p, nullptr);
    if (layout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, layout, nullptr);
    for (VkDescriptorSetLayout l : set_layouts) {
        if (l != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, l, nullptr);
    }
    for (VkFence f : free_fences) vkDestroyFence(dev, f, nullptr);
    for (const AcqSem& a : acq) {
        if (a.sem != VK_NULL_HANDLE) vkDestroySemaphore(dev, a.sem, nullptr);
    }
    if (ts_pool != VK_NULL_HANDLE) vkDestroyQueryPool(dev, ts_pool, nullptr);
    if (cmd_pool != VK_NULL_HANDLE) vkDestroyCommandPool(dev, cmd_pool, nullptr);
    if (surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(core->instance, surface, nullptr);
    surface = VK_NULL_HANDLE;
    // Daqui em diante (os membros saindo, as referências de keep_texture) tudo é destruído na hora.
    core->closed = true;
}

VkRenderPass Renderer::Impl::render_pass(VkFormat color, VkSampleCountFlagBits samples, VkFormat dfmt, VkAttachmentLoadOp cl,
                                         VkAttachmentLoadOp dl) {
    const std::array<uint32_t, 5> key{uint32_t(color), uint32_t(samples), uint32_t(dfmt), uint32_t(cl), uint32_t(dl)};
    auto it = passes.find(key);
    if (it != passes.end()) return it->second;
    VkAttachmentDescription a[2]{};
    a[0].format = color;
    a[0].samples = samples;
    a[0].loadOp = cl;
    a[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    a[1].format = dfmt;
    a[1].samples = samples;
    a[1].loadOp = dl;
    a[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    a[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &cref;
    sp.pDepthStencilAttachment = dfmt != VK_FORMAT_UNDEFINED ? &dref : nullptr;
    // Dois passes seguidos nos mesmos anexos (a limpeza do Z do meio do quadro, a névoa) ficam em ordem.
    VkSubpassDependency dep[2]{};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;  // o lado do subpasso só aceita estágios gráficos
    dep[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    dep[1] = dep[0];
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ri.attachmentCount = dfmt != VK_FORMAT_UNDEFINED ? 2 : 1;
    ri.pAttachments = a;
    ri.subpassCount = 1;
    ri.pSubpasses = &sp;
    ri.dependencyCount = 2;
    ri.pDependencies = dep;
    VkRenderPass rp = VK_NULL_HANDLE;
    vk_check(vkCreateRenderPass(dev, &ri, nullptr, &rp), "vkCreateRenderPass");
    passes.emplace(key, rp);
    return rp;
}

VkPipeline Renderer::Impl::pipeline(uint32_t prog, const GsBlendEq* gb, bool coverage, VkSampleCountFlagBits samples,
                                    bool has_depth, VkFormat format, VkCompareOp zfunc, bool zwrite, bool lines) {
    if (!core->features.fillModeNonSolid) lines = false;
    if (!has_depth) {
        zfunc = VK_COMPARE_OP_ALWAYS;
        zwrite = false;
    }
    std::array<uint32_t, 4> key{};
    key[0] = prog | uint32_t(coverage ? 1 : 0) << 5 | uint32_t(samples) << 6 | uint32_t(has_depth ? 1 : 0) << 12 |
             uint32_t(lines ? 1 : 0) << 13 | uint32_t(zwrite ? 1 : 0) << 14 | uint32_t(zfunc) << 15;
    key[1] = uint32_t(format);
    if (gb != nullptr) {
        key[2] = uint32_t(gb->src) | uint32_t(gb->dst) << 8 | uint32_t(gb->op) << 16 | uint32_t(gb->enable ? 1 : 0) << 24 | 1u << 25;
    }
    auto it = pipelines.find(key);
    if (it != pipelines.end()) return it->second;

    int vs = kModVsFull, fs = kModPsBlit;
    switch (prog) {
        case kProgMain: vs = kModVsMain; break;
        case kProgSkin: vs = kModVsSkin; break;
        case kProgMorph: vs = kModVsMorph; break;
        case kProgMovie: vs = kModVsMovie; break;
        case kProgComposite: vs = kModVsRect; break;
        default: break;
    }
    switch (prog) {
        case kProgMain:
        case kProgSkin:
        case kProgMorph: fs = coverage ? kModPsA2c : kModPsMain; break;
        case kProgFog: fs = kModPsFog; break;
        case kProgFogMs: fs = kModPsFogMs; break;
        case kProgDown: fs = kModPsDown; break;
        case kProgBlur: fs = kModPsBlur; break;
        case kProgBlurMix: fs = kModPsBlurMix; break;
        case kProgMovie: fs = kModPsMovie; break;
        case kProgComposite: fs = kModPsComposite; break;
        default: fs = kModPsBlit; break;  // kProgBlit, kProgBlitSample, kProgHistory
    }
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = modules[vs];
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = modules[fs];
    st[1].pName = "main";

    // Os vértices (data/map_item.h): Vertex de 36 bytes e SkinnedVertex de 48.
    VkVertexInputBindingDescription bind{0, 0, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr[7]{};
    uint32_t nattr = 0;
    if (prog == kProgMain || prog == kProgSkin || prog == kProgMorph) {
        bind.stride = prog == kProgMain ? uint32_t(sizeof(Vertex)) : uint32_t(sizeof(SkinnedVertex));
        attr[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, uint32_t(offsetof(Vertex, pos))};
        attr[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, uint32_t(offsetof(Vertex, normal))};
        attr[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(Vertex, uv))};
        attr[3] = {3, 0, VK_FORMAT_R8G8B8A8_UNORM, uint32_t(offsetof(Vertex, color))};
        nattr = 4;
        if (prog != kProgMain) {
            attr[4] = {4, 0, VK_FORMAT_R8G8B8A8_UINT, uint32_t(offsetof(SkinnedVertex, bones))};
            attr[5] = {5, 0, VK_FORMAT_R32_SFLOAT, uint32_t(offsetof(SkinnedVertex, weight))};
            nattr = 6;
        }
        if (prog == kProgMorph) {
            attr[6] = {6, 0, VK_FORMAT_R32_SINT, uint32_t(offsetof(SkinnedVertex, morph))};
            nattr = 7;
        }
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (nattr > 0) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &bind;
        vi.vertexAttributeDescriptionCount = nattr;
        vi.pVertexAttributeDescriptions = attr;
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = lines ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;  // o jogo desenha faces dos dois lados
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples;
    ms.alphaToCoverageEnable = coverage ? VK_TRUE : VK_FALSE;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = has_depth ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = zwrite ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = zfunc;
    VkPipelineColorBlendAttachmentState ca{};
    ca.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (gb != nullptr && gb->enable) {
        // O GS mistura só r, g e b; o alfa gravado é o do pixel (As), que ALPHA com C = Ad lê depois.
        ca.blendEnable = VK_TRUE;
        ca.srcColorBlendFactor = vk_factor(gb->src);
        ca.dstColorBlendFactor = vk_factor(gb->dst);
        ca.colorBlendOp = vk_op(gb->op);
        ca.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ca.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        ca.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (prog == kProgFog || prog == kProgFogMs) {
        // Névoa: cor = névoa * (1 - fica) + quadro * fica, com "fica" no alfa da saída; o alfa do quadro não muda.
        ca.blendEnable = VK_TRUE;
        ca.srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ca.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        ca.colorBlendOp = VK_BLEND_OP_ADD;
        ca.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        ca.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ca.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (prog == kProgHistory) {
        // O rastro: o anterior x keep + o atual x (1 - keep), com keep na constante da mistura; o alfa do alvo fica.
        ca.blendEnable = VK_TRUE;
        ca.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
        ca.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        ca.colorBlendOp = VK_BLEND_OP_ADD;
        ca.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        ca.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ca.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (prog == kProgComposite) {
        // Uma imagem com o alfa pré-multiplicado por cima (source-over), como o Direct2D desenha no alvo.
        ca.blendEnable = VK_TRUE;
        ca.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ca.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ca.colorBlendOp = VK_BLEND_OP_ADD;
        ca.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ca.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ca.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ca;
    const VkDynamicState dyn[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 3;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.stageCount = 2;
    gi.pStages = st;
    gi.pVertexInputState = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = &dy;
    gi.layout = layout;
    gi.renderPass = render_pass(format, samples, has_depth ? depth_format : VK_FORMAT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD,
                                VK_ATTACHMENT_LOAD_OP_LOAD);
    VkPipeline p = VK_NULL_HANDLE;
    vk_check(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gi, nullptr, &p), "vkCreateGraphicsPipelines");
    pipelines.emplace(key, p);
    return p;
}

std::shared_ptr<GImage> Renderer::Impl::make_image(VkFormat format, int w, int h, bool mips, VkSampleCountFlagBits samples,
                                                   VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t mip_count) {
    w = std::max(1, w);
    h = std::max(1, h);
    uint32_t levels = 1;
    if (mip_count > 0) {
        levels = mip_count;
    } else if (mips) {
        for (int m = std::max(w, h); m > 1; m /= 2) ++levels;
    }
    auto img = std::make_shared<GImage>();
    img->core = core;
    img->format = format;
    img->w = w;
    img->h = h;
    img->mips = levels;
    img->samples = samples;
    img->aspect = aspect;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {uint32_t(w), uint32_t(h), 1};
    ci.mipLevels = levels;
    ci.arrayLayers = 1;
    ci.samples = samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ci, nullptr, &img->image) != VK_SUCCESS) {
        img->image = VK_NULL_HANDLE;
        return nullptr;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(dev, img->image, &req);
    img->mem = core->alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, true);
    if (img->mem.mem == VK_NULL_HANDLE || vkBindImageMemory(dev, img->image, img->mem.mem, img->mem.offset) != VK_SUCCESS) {
        return nullptr;  // o destrutor solta a imagem e a memória
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, levels, 0, 1};
    if (vkCreateImageView(dev, &vi, nullptr, &img->view) != VK_SUCCESS) return nullptr;
    img->att_view = img->view;
    if (levels > 1 && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0) {
        vi.subresourceRange.levelCount = 1;
        if (vkCreateImageView(dev, &vi, nullptr, &img->att_view) != VK_SUCCESS) {
            img->att_view = img->view;
            return nullptr;
        }
    }
    return img;
}

std::shared_ptr<GBuffer> Renderer::Impl::make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, const void* data) {
    auto b = std::make_shared<GBuffer>();
    b->core = core;
    b->size = std::max<VkDeviceSize>(size, 4);
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = b->size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &ci, nullptr, &b->buf) != VK_SUCCESS) {
        b->buf = VK_NULL_HANDLE;
        return nullptr;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev, b->buf, &req);
    // Visível pela CPU, de preferência também a da GPU (a memória unificada do Android; o BAR no PC).
    b->mem = core->alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false);
    if (b->mem.mem == VK_NULL_HANDLE || b->mem.ptr == nullptr ||
        vkBindBufferMemory(dev, b->buf, b->mem.mem, b->mem.offset) != VK_SUCCESS) {
        return nullptr;
    }
    if (data != nullptr) std::memcpy(b->mem.ptr, data, size_t(size));
    return b;
}

VkSampler Renderer::Impl::make_sampler(VkFilter filter, VkSamplerMipmapMode mip, VkSamplerAddressMode address, float max_lod,
                                       float aniso) {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = filter;
    si.mipmapMode = mip;
    si.addressModeU = si.addressModeV = si.addressModeW = address;
    si.maxLod = max_lod;
    if (aniso > 1.0f && core->features.samplerAnisotropy) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = std::min(aniso, core->props.limits.maxSamplerAnisotropy);
    }
    VkSampler s = VK_NULL_HANDLE;
    vk_check(vkCreateSampler(dev, &si, nullptr, &s), "vkCreateSampler");
    return s;
}

void Renderer::Impl::create_targets() {
    target = make_image(VK_FORMAT_B8G8R8A8_UNORM, width_, height_, false, VK_SAMPLE_COUNT_1_BIT,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (target == nullptr) throw std::runtime_error("alvo do quadro: sem memória");
    blur_full = nullptr;
    create_scene_targets();
}

// A profundidade da cena e, com escala ou MSAA (só do PC), a cor própria dela. Sem memória ou sem suporte para o pedido,
// tenta sem MSAA e depois em 100% (o caminho de antes).
void Renderer::Impl::create_scene_targets() {
    if (try_scene_targets(scene_scale, scene_msaa)) return;
    if (scene_msaa > 1 && try_scene_targets(scene_scale, 1)) return;
    if (!try_scene_targets(100, 1)) throw std::runtime_error("alvos da cena: sem memória para a profundidade");
}

bool Renderer::Impl::try_scene_targets(int scale_percent, int msaa) {
    const int max_side = std::min(16384, int(core->props.limits.maxImageDimension2D));
    const int w = std::clamp(int(std::lround(double(width_) * scale_percent / 100.0)), 1, max_side);
    const int h = std::clamp(int(std::lround(double(height_) * scale_percent / 100.0)), 1, max_side);
    if (msaa > 1 && (msaa_counts & VkSampleCountFlags(msaa)) == 0) return false;
    const auto samples = VkSampleCountFlagBits(msaa);
    auto d = make_image(depth_format, w, h, false, samples,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (d == nullptr) return false;
    std::shared_ptr<GImage> color, resolved;
    const bool scaled = w != width_ || h != height_;
    if (msaa > 1 || scaled) {
        VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (msaa == 1) usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        color = make_image(VK_FORMAT_B8G8R8A8_UNORM, w, h, false, samples, usage);
        if (color == nullptr) return false;
        if (msaa > 1 && scaled) {
            resolved = make_image(VK_FORMAT_B8G8R8A8_UNORM, w, h, false, VK_SAMPLE_COUNT_1_BIT,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
            if (resolved == nullptr) return false;
        }
    }
    depth = d;
    scene_color = color;
    scene_resolved = resolved;
    scene_w = w;
    scene_h = h;
    scene_scale = scale_percent;
    scene_msaa = msaa;
    return true;
}

bool Renderer::Impl::set_scene_quality(int scale_percent, int msaa) {
    scale_percent = std::clamp(scale_percent, 25, 400);
    msaa = msaa >= 8 ? 8 : msaa >= 4 ? 4 : msaa >= 2 ? 2 : 1;
    if (scale_percent == scene_scale && msaa == scene_msaa && depth != nullptr) return true;
    end_pass();
    scene_scale = scale_percent;
    scene_msaa = msaa;
    create_scene_targets();
    return scene_scale == scale_percent && scene_msaa == msaa;
}

void Renderer::Impl::set_texture_filter(TextureFilter f) {
    const int aniso = texture_filter_anisotropy(f);
    VkSamplerMipmapMode mip = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    if (aniso <= 0 && f == TextureFilter::Bilinear) mip = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    const VkSampler s = make_sampler(VK_FILTER_LINEAR, mip, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_LOD_CLAMP_NONE,
                                     aniso > 0 ? float(aniso) : 0.0f);
    if (sampler != VK_NULL_HANDLE) {
        Core* c = core.get();
        const VkSampler old = sampler;
        c->retire([c, old] { vkDestroySampler(c->device, old, nullptr); });
    }
    sampler = s;
    filter_ = f;
}

void Renderer::Impl::resize(int width, int height) {
    if (width <= 0 || height <= 0 || offscreen_) return;
    end_pass();
    width_ = width;
    height_ = height;
    create_targets();
    sc_dirty = true;
}

GImage* Renderer::Impl::texture_for(int slot) const {
    if (slot == kQuadNoTexture) return no_texture.get();
    if (use_hd && slot >= 0 && slot < int(hd_textures.size()) && hd_textures[size_t(slot)] != nullptr) {
        return hd_textures[size_t(slot)].get();
    }
    if (slot >= 0 && slot < int(textures.size()) && textures[size_t(slot)] != nullptr) return textures[size_t(slot)].get();
    return white.get();
}

// ---------------------------------------------------------------- os comandos

VkCommandBuffer Renderer::Impl::new_cb() {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (!free_cbs.empty()) {
        cb = free_cbs.back();
        free_cbs.pop_back();
    } else {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = cmd_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vk_check(vkAllocateCommandBuffers(dev, &ai, &cb), "vkAllocateCommandBuffers");
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer");
    return cb;
}

VkFence Renderer::Impl::new_fence() {
    if (!free_fences.empty()) {
        VkFence f = free_fences.back();
        free_fences.pop_back();
        return f;
    }
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f = VK_NULL_HANDLE;
    vk_check(vkCreateFence(dev, &fi, nullptr, &f), "vkCreateFence");
    return f;
}

uint64_t Renderer::Impl::submit(VkCommandBuffer cb, VkSemaphore wait_sem, VkSemaphore signal_sem) {
    vk_check(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
    const VkFence fence = new_fence();
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (wait_sem != VK_NULL_HANDLE) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &wait_sem;
        si.pWaitDstStageMask = &wait_stage;
    }
    if (signal_sem != VK_NULL_HANDLE) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &signal_sem;
    }
    vk_check(vkQueueSubmit(core->queue, 1, &si, fence), "vkQueueSubmit");
    Inflight f;
    f.serial = ++core->submitted;
    f.fence = fence;
    f.cb = cb;
    inflight.push_back(f);
    return f.serial;
}

// Os envios que a GPU acabou: o número, os command buffers e as cercas de volta ao uso, a cova.
void Renderer::Impl::poll() {
    while (!inflight.empty() && vkGetFenceStatus(dev, inflight.front().fence) == VK_SUCCESS) {
        const Inflight f = inflight.front();
        inflight.pop_front();
        core->completed = std::max(core->completed, f.serial);
        vkResetFences(dev, 1, &f.fence);
        free_fences.push_back(f.fence);
        vkResetCommandBuffer(f.cb, 0);
        free_cbs.push_back(f.cb);
    }
    core->run_grave();
}

void Renderer::Impl::wait_serial(uint64_t serial) {
    while (!inflight.empty() && inflight.front().serial <= serial) {
        vkWaitForFences(dev, 1, &inflight.front().fence, VK_TRUE, UINT64_MAX);
        poll();
    }
}

VkCommandBuffer Renderer::Impl::frame_cmd() {
    if (cmd != VK_NULL_HANDLE) return cmd;
    cmd = new_cb();
    cur_ts_slot = -1;
    if (profiling && seq_valid && ts_pool != VK_NULL_HANDLE && !ts_busy[size_t(ts_next)]) {
        cur_ts_slot = ts_next;
        ts_next = (ts_next + 1) % kTsSlots;
        ts_busy[size_t(cur_ts_slot)] = true;
        vkCmdResetQueryPool(cmd, ts_pool, uint32_t(cur_ts_slot) * 2, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ts_pool, uint32_t(cur_ts_slot) * 2);
    }
    return cmd;
}

// A transição de layout de uma imagem (todos os níveis), com uma barreira completa.
void Renderer::Impl::barrier(VkCommandBuffer cb, GImage* img, VkImageLayout to) {
    if (img == nullptr || img->layout == to) return;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = img->layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img->image;
    b.subresourceRange = {img->aspect, 0, img->mips, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &b);
    img->layout = to;
}

void Renderer::Impl::transition(GImage* img, VkImageLayout to) {
    if (img == nullptr || img->layout == to) return;
    end_pass();
    barrier(frame_cmd(), img, to);
}

void Renderer::Impl::ensure_read(GImage* img) { transition(img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); }

void Renderer::Impl::end_pass() {
    if (in_pass) vkCmdEndRenderPass(cmd);
    in_pass = false;
    pass_color = nullptr;
    pass_depth = nullptr;
}

// Um render pass novo nos anexos dados: a cor carregada (ou limpa com `clear`, ou sem carregar: DONT_CARE), a
// profundidade carregada ou limpa com 0 (a profundidade invertida: perto = 1). O viewport cobre o anexo.
void Renderer::Impl::begin_pass(GImage* color, GImage* depth_img, VkAttachmentLoadOp color_load, const float* clear,
                                bool clear_depth) {
    end_pass();
    VkCommandBuffer cb = frame_cmd();
    barrier(cb, color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (depth_img != nullptr) barrier(cb, depth_img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    const VkFormat dfmt = depth_img != nullptr ? depth_img->format : VK_FORMAT_UNDEFINED;
    const VkRenderPass rp = render_pass(color->format, color->samples, dfmt, color_load,
                                        clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD);
    const VkRenderPass canon = render_pass(color->format, color->samples, dfmt, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_LOAD);
    const VkFramebuffer fb = core->framebuffer(canon, color->att_view, depth_img != nullptr ? depth_img->view : VK_NULL_HANDLE,
                                               uint32_t(color->w), uint32_t(color->h));
    VkClearValue cv[2]{};
    if (clear != nullptr) {
        for (int k = 0; k < 4; ++k) cv[0].color.float32[k] = clear[k];
    }
    cv[1].depthStencil = {0.0f, 0};
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = rp;
    bi.framebuffer = fb;
    bi.renderArea = {{0, 0}, {uint32_t(color->w), uint32_t(color->h)}};
    bi.clearValueCount = depth_img != nullptr ? 2 : 1;
    bi.pClearValues = cv;
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    in_pass = true;
    pass_color = color;
    pass_depth = depth_img;
    cur_pipe = VK_NULL_HANDLE;
    VkViewport vp{0.0f, 0.0f, float(color->w), float(color->h), 0.0f, 1.0f};
    vkCmdSetViewport(cb, 0, 1, &vp);
    cur_scissor_full = false;
    set_scissor_full();
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    vkCmdSetBlendConstants(cb, zero);
}

void Renderer::Impl::use_pass(GImage* color, GImage* depth_img) {
    if (in_pass && pass_color == color && pass_depth == depth_img) return;
    begin_pass(color, depth_img, VK_ATTACHMENT_LOAD_OP_LOAD, nullptr, false);
}

// O pass dos itens: a cena (com a profundidade) ou, na segunda passada, o 2D no alvo do tamanho da janela.
void Renderer::Impl::item_pass() {
    if (in_2d) {
        use_pass(target.get(), nullptr);
    } else {
        use_pass(scene_target(), depth.get());
    }
}

// O alvo, para o que o Direct2D desenharia por cima: o pass aberto, se for nele (com ou sem a profundidade).
void Renderer::Impl::target_pass() {
    if (in_pass && pass_color == target.get()) return;
    use_pass(target.get(), nullptr);
}

void Renderer::Impl::bind_pipe(VkPipeline p) {
    if (p == cur_pipe) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    cur_pipe = p;
}

void Renderer::Impl::set_scissor_full() {
    if (cur_scissor_full) return;
    VkRect2D r{{0, 0}, {uint32_t(pass_color->w), uint32_t(pass_color->h)}};
    vkCmdSetScissor(cmd, 0, 1, &r);
    cur_scissor_full = true;
}

// O recorte do 2D (o RECT do D3D11: LONG(x0), LONG(y0), LONG(x1 + 0.5), LONG(y1 + 0.5)), preso ao anexo. false: vazio.
bool Renderer::Impl::set_scissor(const float clip[4]) {
    const long w = long(pass_color->w), h = long(pass_color->h);
    const long x0 = std::clamp(long(clip[0]), 0L, w), y0 = std::clamp(long(clip[1]), 0L, h);
    const long x1 = std::clamp(long(clip[2] + 0.5f), 0L, w), y1 = std::clamp(long(clip[3] + 0.5f), 0L, h);
    if (x1 <= x0 || y1 <= y0) return false;
    VkRect2D r{{int32_t(x0), int32_t(y0)}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
    if (!cur_scissor_full && r.offset.x == cur_scissor.offset.x && r.offset.y == cur_scissor.offset.y &&
        r.extent.width == cur_scissor.extent.width && r.extent.height == cur_scissor.extent.height) {
        return true;
    }
    vkCmdSetScissor(cmd, 0, 1, &r);
    cur_scissor = r;
    cur_scissor_full = false;
    return true;
}

void Renderer::Impl::push(const void* bytes, size_t len) {
    uint8_t buf[kPushBytes]{};
    std::memcpy(buf, bytes, std::min<size_t>(len, kPushBytes));
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kPushBytes, buf);
}

// O conjunto 1 com as duas texturas (a segunda, sem uma, a branca), em cache no quadro.
VkDescriptorSet Renderer::Impl::tex_set(GImage* t0, VkSampler s0, GImage* t1, VkSampler s1) {
    if (t1 == nullptr) t1 = white.get();
    if (s1 == VK_NULL_HANDLE) s1 = sampler_linear;
    const std::array<uint64_t, 4> key{hv(t0->view), hv(s0), hv(t1->view), hv(s1)};
    auto it = tex_sets.find(key);
    if (it != tex_sets.end()) return it->second;
    VkDescriptorSet set = VK_NULL_HANDLE;
    for (int attempt = 0; attempt < 2 && set == VK_NULL_HANDLE; ++attempt) {
        if (!tex_pools.empty()) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = tex_pools.back();
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &set_layouts[1];
            if (vkAllocateDescriptorSets(dev, &ai, &set) == VK_SUCCESS) break;
            set = VK_NULL_HANDLE;
        }
        VkDescriptorPool p = VK_NULL_HANDLE;
        if (!free_tex_pools->empty()) {
            p = free_tex_pools->back();
            free_tex_pools->pop_back();
        } else {
            VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2048};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = 1024;
            pi.poolSizeCount = 1;
            pi.pPoolSizes = &ps;
            vk_check(vkCreateDescriptorPool(dev, &pi, nullptr, &p), "vkCreateDescriptorPool");
        }
        tex_pools.push_back(p);
    }
    if (set == VK_NULL_HANDLE) throw std::runtime_error("Vulkan: sem conjuntos de descritores");
    VkDescriptorImageInfo ii[2]{};
    ii[0] = {s0, t0->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    ii[1] = {s1, t1->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
    tex_sets.emplace(key, set);
    return set;
}

void Renderer::Impl::bind_tex(GImage* t0, VkSampler s0, GImage* t1, VkSampler s1) {
    const VkDescriptorSet set = tex_set(t0, s0, t1, s1);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &set, 0, nullptr);
}

// Um pedaço livre de pelo menos `want` bytes (da lista, ou novo, com o conjunto 0 dele).
std::shared_ptr<Chunk> Renderer::Impl::get_chunk(size_t want) {
    want = std::max(want, kChunkBytes);
    for (size_t i = 0; i < free_chunks->size(); ++i) {
        if ((*free_chunks)[i]->buf->size >= want) {
            auto c = (*free_chunks)[i];
            free_chunks->erase(free_chunks->begin() + long(i));
            return c;
        }
    }
    auto c = std::make_shared<Chunk>();
    c->buf = make_buffer(want,
                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                             VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (c->buf == nullptr) throw std::runtime_error("buffer dos dados do quadro: sem memória");
    if (chunk_sets_left == 0) {
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4 * 128},
                                      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 128}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 128;
        pi.poolSizeCount = 2;
        pi.pPoolSizes = ps;
        VkDescriptorPool p = VK_NULL_HANDLE;
        vk_check(vkCreateDescriptorPool(dev, &pi, nullptr, &p), "vkCreateDescriptorPool");
        chunk_set_pools.push_back(p);
        chunk_sets_left = 128;
    }
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = chunk_set_pools.back();
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layouts[0];
    vk_check(vkAllocateDescriptorSets(dev, &ai, &c->set0), "vkAllocateDescriptorSets");
    --chunk_sets_left;
    // As faixas fixas de cada ligação; o deslocamento de cada desenho é dinâmico. Os pixels da composição (a ligação 4)
    // pegam o pedaço inteiro, então só com o deslocamento 0 (a composição começa num pedaço próprio).
    const VkDeviceSize ranges[4] = {sizeof(FrameU), sizeof(ObjectU), kBonesBytes, sizeof(MorphU)};
    VkDescriptorBufferInfo bi[5]{};
    VkWriteDescriptorSet w[5]{};
    for (uint32_t i = 0; i < 5; ++i) {
        bi[i] = {c->buf->buf, 0, i < 4 ? ranges[i] : VK_WHOLE_SIZE};
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = c->set0;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = i < 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(dev, 5, w, 0, nullptr);
    return c;
}

// Garante `bytes` (mais a folga do alinhamento) no pedaço atual do fluxo; senão, um pedaço novo.
void Renderer::Impl::room(Stream& s, size_t bytes) {
    const size_t need = bytes + ualign;
    if (s.cur != nullptr && s.off + need <= size_t(s.cur->buf->size)) return;
    if (s.cur != nullptr) s.used.push_back(s.cur);
    s.cur = get_chunk(need + kBonesBytes);
    s.off = 0;
    if (&s == &frame_stream) frame_off = SIZE_MAX;
}

Renderer::Impl::Alloc Renderer::Impl::stream_alloc(Stream& s, size_t bytes, size_t align) {
    bytes = std::max<size_t>(bytes, 1);
    room(s, bytes + align);
    const size_t at = (s.off + align - 1) / align * align;
    s.off = at + bytes;
    Alloc a;
    a.buf = s.cur->buf->buf;
    a.offset = at;
    a.ptr = s.cur->buf->ptr() + at;
    return a;
}

// Um pedaço dos dados do quadro (vértices, pixels), em qualquer pedaço.
Renderer::Impl::Alloc Renderer::Impl::transient(size_t bytes) { return stream_alloc(frame_stream, bytes, 16); }

// Garante o espaço das constantes de um desenho (e do FrameU) no pedaço atual: todas no mesmo, o do conjunto 0 ligado.
void Renderer::Impl::prepare(size_t uniform_bytes) {
    room(frame_stream, uniform_bytes + 2 * ualign + sizeof(FrameU));
    frame_offset();
}

size_t Renderer::Impl::frame_offset() {
    if (frame_off == SIZE_MAX) {
        frame_off = SIZE_MAX - 1;  // stream_alloc não troca de pedaço depois do prepare
        const Alloc a = stream_alloc(frame_stream, sizeof(FrameU), ualign);
        std::memcpy(a.ptr, &frame_u, sizeof(frame_u));
        frame_off = a.offset;
    }
    return frame_off;
}

size_t Renderer::Impl::uniform(const void* data, size_t len) {
    const Alloc a = stream_alloc(frame_stream, len, ualign);
    std::memcpy(a.ptr, data, len);
    return a.offset;
}

void Renderer::Impl::bind_set0(size_t obj, size_t bones, size_t morph, size_t px) {
    const uint32_t offs[5] = {uint32_t(frame_off == SIZE_MAX ? 0 : frame_off), uint32_t(obj), uint32_t(bones), uint32_t(morph),
                              uint32_t(px)};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &frame_stream.cur->set0, 5, offs);
}

// Os pedaços de um fluxo voltam à lista livre quando a GPU acaba o envio que os usou.
void Renderer::Impl::retire_stream(Stream& s) {
    if (s.cur != nullptr) s.used.push_back(s.cur);
    s.cur = nullptr;
    s.off = 0;
    if (s.used.empty()) return;
    std::shared_ptr<ChunkList> list = free_chunks;
    ChunkList used = std::move(s.used);
    s.used.clear();
    core->retire([list, used] {
        for (const auto& c : used) list->push_back(c);
    });
}

// Envia o command buffer do quadro (depois do de envio, que leva as texturas que ele usa).
void Renderer::Impl::commit_frame(VkSemaphore wait_sem, VkSemaphore signal_sem) {
    end_pass();
    flush_uploads();
    if (cmd == VK_NULL_HANDLE) return;
    retire_stream(frame_stream);
    frame_off = SIZE_MAX;
    if (!tex_pools.empty()) {
        std::shared_ptr<std::vector<VkDescriptorPool>> list = free_tex_pools;
        std::vector<VkDescriptorPool> used = std::move(tex_pools);
        tex_pools.clear();
        VkDevice d = dev;
        core->retire([list, used, d] {
            for (VkDescriptorPool p : used) {
                vkResetDescriptorPool(d, p, 0);
                list->push_back(p);
            }
        });
    }
    tex_sets.clear();
    const int slot = cur_ts_slot;
    if (slot >= 0) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ts_pool, uint32_t(slot) * 2 + 1);
    const uint64_t serial = submit(cmd, wait_sem, signal_sem);
    core->tag_pending(serial);
    if (slot >= 0) {
        TsEntry e;
        e.seq = cur_seq;
        e.serial = serial;
        e.epoch = epoch;
        e.slot = slot;
        ts_pending.push_back(e);
        ++seq_pending[cur_seq].first;
    }
    cur_ts_slot = -1;
    cmd = VK_NULL_HANDLE;
    last_frame_serial = serial;
    poll();
}

VkCommandBuffer Renderer::Impl::upload_cmd() {
    if (up_cmd == VK_NULL_HANDLE) up_cmd = new_cb();
    return up_cmd;
}

// Envia o command buffer de envio. Com o do quadro aberto, o que saiu de uso espera o envio do quadro (ele pode usá-lo).
uint64_t Renderer::Impl::flush_uploads() {
    if (up_cmd == VK_NULL_HANDLE) return 0;
    retire_stream(up_stream);
    const uint64_t serial = submit(up_cmd, VK_NULL_HANDLE, VK_NULL_HANDLE);
    up_cmd = VK_NULL_HANDLE;
    up_bytes = 0;
    if (cmd == VK_NULL_HANDLE) core->tag_pending(serial);
    return serial;
}

// Um nível de uma imagem por um pedaço de passagem (no command buffer de envio, ou no dado).
void Renderer::Impl::upload(GImage* img, uint32_t level, const void* data, size_t bytes_per_row, size_t rows, uint32_t w,
                            uint32_t h, VkCommandBuffer cb) {
    if (img == nullptr || data == nullptr || bytes_per_row == 0 || rows == 0) return;
    const size_t bytes = bytes_per_row * rows;
    Stream& s = cb == VK_NULL_HANDLE ? up_stream : frame_stream;
    if (cb == VK_NULL_HANDLE) cb = upload_cmd();
    const Alloc a = stream_alloc(s, bytes, 16);
    std::memcpy(a.ptr, data, bytes);
    barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy r{};
    r.bufferOffset = a.offset;
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
    r.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cb, a.buf, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    if (&s == &up_stream) up_bytes += bytes;
}

// Os mipmaps a partir do nível 0 (a imagem inteira em TRANSFER_DST), por blits; no fim, tudo para a leitura.
void Renderer::Impl::gen_mips(VkCommandBuffer cb, GImage* img) {
    barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(core->phys, img->format, &fp);
    const VkFilter filter = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR
                                                                                                           : VK_FILTER_NEAREST;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img->image;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    int w = img->w, h = img->h;
    for (uint32_t m = 1; m < img->mips; ++m) {
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 1, 0, 1};
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &b);
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        VkImageBlit r{};
        r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
        r.srcOffsets[1] = {w, h, 1};
        r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        r.dstOffsets[1] = {nw, nh, 1};
        vkCmdBlitImage(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &r, filter);
        w = nw;
        h = nh;
    }
    VkImageMemoryBarrier bb[2] = {b, b};
    uint32_t n = 0;
    if (img->mips > 1) {
        bb[n].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, img->mips - 1, 0, 1};
        bb[n].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        bb[n].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ++n;
    }
    bb[n].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, img->mips - 1, 1, 0, 1};
    bb[n].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bb[n].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ++n;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, n,
                         bb);
    img->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

// O fim de um envio: os mipmaps, ou a imagem pronta para a leitura.
void Renderer::Impl::finish_upload(GImage* img, VkCommandBuffer cb) {
    if (img == nullptr) return;
    const bool up = cb == VK_NULL_HANDLE;
    if (up) cb = upload_cmd();
    if (img->mips > 1) {
        gen_mips(cb, img);
    } else {
        barrier(cb, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (up && up_bytes > kUploadFlushBytes) flush_uploads();
}

// Uma imagem copiada inteira para outra igual, no command buffer do quadro (o CopyResource do D3D11).
void Renderer::Impl::copy_texture(GImage* src, GImage* dst) {
    end_pass();
    VkCommandBuffer cb = frame_cmd();
    barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy r{};
    r.srcSubresource = {src->aspect, 0, 0, 1};
    r.dstSubresource = {dst->aspect, 0, 0, 1};
    r.extent = {uint32_t(std::min(src->w, dst->w)), uint32_t(std::min(src->h, dst->h)), 1};
    vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
}

// Um triângulo que cobre `dst` (no pass dele, carregando o que há) com o programa e as constantes dadas.
void Renderer::Impl::draw_full(uint32_t prog, GImage* dst, GImage* src, const void* bytes, size_t len) {
    ensure_read(src);
    use_pass(dst, nullptr);
    bind_pipe(pipeline(prog, nullptr, false, dst->samples, false, dst->format));
    set_scissor_full();
    if (bytes != nullptr) push(bytes, len);
    bind_tex(src, sampler_linear);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void Renderer::Impl::ensure_quad_ib(size_t quads) {
    if (quads <= quad_ib_quads && quad_ib != nullptr) return;
    quad_ib_quads = std::max<size_t>(quads, 1024);
    std::vector<uint32_t> idx;
    idx.reserve(quad_ib_quads * 6);
    for (uint32_t q = 0; q < uint32_t(quad_ib_quads); ++q) {
        const uint32_t b = q * 4;
        idx.insert(idx.end(), {b, b + 1, b + 2, b, b + 2, b + 3});
    }
    quad_ib = make_buffer(idx.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, idx.data());
    if (quad_ib == nullptr) throw std::runtime_error("índices dos quadrados: sem memória");
}

// ---------------------------------------------------------------- o quadro

void Renderer::Impl::set_frame(const FrameParams& frame) {
    std::memcpy(frame_u.view_proj, frame.view_proj.data(), sizeof(frame_u.view_proj));
    std::memcpy(frame_u.light_dir, frame.light_dir, sizeof(frame_u.light_dir));
    frame_u.ambient = frame.ambient;
    frame_u.debug_shading = frame.debug_shading ? 1.0f : 0.0f;
    frame_u.lights_on = frame.lights ? 1.0f : 0.0f;
    wireframe = frame.wireframe;
    frame_off = SIZE_MAX;
}

void Renderer::Impl::render(const std::vector<DrawItem>& items, const FrameParams& frame) {
    env_parts_ = 0;
    a2c_parts_ = 0;
    in_2d = false;
    set_frame(frame);
    // O alvo da cena limpo (com o filme, de preto: o quadro dele é o fundo) e a profundidade em 0 (longe).
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const bool movie = movie_on && movie_tex != nullptr;
    if (movie) ensure_read(movie_tex.get());
    begin_pass(scene_target(), depth.get(), VK_ATTACHMENT_LOAD_OP_CLEAR, movie ? black : frame.clear_color.data(), true);
    if (movie) {
        draw_movie();
        ++counters_.draws;
    }
    if (!separate_scene()) {
        for (const DrawItem& inst : items) draw_item(inst, frame);
        return;
    }
    // Com escala ou MSAA: a cena no alvo dela, a redução para a janela e então o 2D por cima, no tamanho da janela.
    const auto is_2d = [](const DrawItem& d) { return d.ui != nullptr || !d.quads2d.empty(); };
    for (const DrawItem& inst : items) {
        if (!is_2d(inst)) draw_item(inst, frame);
    }
    resolve_scene();
    in_2d = true;
    for (const DrawItem& inst : items) {
        if (is_2d(inst)) draw_item(inst, frame);
    }
}

void Renderer::Impl::render_2d(const std::vector<DrawItem>& items, const FrameParams& frame) {
    in_2d = true;
    set_frame(frame);
    for (const DrawItem& inst : items) {
        if (inst.ui != nullptr || !inst.quads2d.empty()) draw_item(inst, frame);
    }
}

void Renderer::Impl::draw_item(const DrawItem& inst, const FrameParams& frame) {
    if (inst.kind == ObjectKind::Volume && !frame.show_volumes) return;
    ++counters_.items;
    if (inst.fog_pass) {
        if (frame.fog) draw_fog(inst, frame);
        return;
    }
    if (inst.clear_depth) {
        // psiClearZ: o Z mais longe na tela inteira (0: a profundidade é invertida), no alvo da cena (com MSAA, todas as
        // amostras): um pass novo que limpa a profundidade e carrega a cor.
        if (!in_2d) begin_pass(scene_target(), depth.get(), VK_ATTACHMENT_LOAD_OP_LOAD, nullptr, true);
        return;
    }
    if (inst.ui != nullptr) {
        draw_ui(*inst.ui);
        return;
    }
    if (!inst.quads2d.empty()) {
        draw_2d(inst);
        return;
    }
    if (!inst.sprites.empty()) {
        draw_sprites(inst);
        return;
    }
    if (!inst.triangles.empty()) {
        draw_triangles(inst);
        return;
    }
    draw_mesh(inst, frame);
}

// Estado do GS (mistura, teste de alfa, teste e escrita de Z) e constantes de uma parte do desenho (apply_state do D3D11).
// Liga o pipeline e devolve o deslocamento do ObjectU no pedaço do quadro (já preparado pelo chamador).
size_t Renderer::Impl::set_state(const DrawItem& inst, const GsState& own, uint32_t prog, bool sprite, bool env, bool screen) {
    const DrawState st = resolve_state(own, inst);
    GsBlendEq gb = gs_blend_equation(st.alpha, st.abe, sprite);
    // Só do PC: com MSAA, um desenho opaco recortado pelo teste de alfa vai com o alfa para a cobertura (ps_a2c).
    const bool coverage = alpha_coverage && scene_msaa > 1 && !in_2d && !screen && !sprite && pass_color->samples > 1 &&
                          gs_alpha_coverage_candidate(st.atst, st.aref, st.aint, gb);
    if (coverage) {
        gb.enable = false;  // o teste só deixa passar As >= 1: (Cs - Cd) * As + Cd é Cs
        gb.src = BlendFactor::One;
        gb.dst = BlendFactor::Zero;
        gb.op = BlendOp::Add;
        ++a2c_parts_;
    }
    bind_pipe(pipeline(prog, &gb, coverage, pass_color->samples, pass_depth != nullptr, pass_color->format, st.zfunc, st.zwrite,
                       wireframe && !screen));
    if (uses_fix(gb)) {
        const float c[4] = {gb.fix, gb.fix, gb.fix, gb.fix};
        vkCmdSetBlendConstants(cmd, c);
    }
    ObjectU o{};
    std::memcpy(o.world, inst.world.data(), sizeof(o.world));
    o.premult = (sprite && gb.enable && gb.premultipliable) ? 1.0f : 0.0f;
    o.sprite = sprite ? 1.0f : 0.0f;
    o.env = env ? 1.0f : 0.0f;
    o.num_lights = float(inst.num_lights);
    std::memcpy(o.light_pos, inst.light_pos, sizeof(o.light_pos));
    std::memcpy(o.light_col, inst.light_color, sizeof(o.light_col));
    std::memcpy(o.env_s, inst.env_s, sizeof(inst.env_s));
    std::memcpy(o.env_t, inst.env_t, sizeof(inst.env_t));
    std::memcpy(o.tint, inst.tint.data(), sizeof(o.tint));
    o.atst = st.atst;
    o.aref = st.aref;
    o.aint = st.aint ? 1.0f : 0.0f;
    o.screen = screen ? 1.0f : 0.0f;
    return uniform(&o, sizeof(o));
}

void Renderer::Impl::draw_mesh(const DrawItem& inst, const FrameParams& frame) {
    if (inst.mesh < 0 || inst.mesh >= int(meshes.size())) return;
    const GpuMesh& gm = meshes[size_t(inst.mesh)];
    const bool skinned = !inst.palette.empty();
    if (skinned != gm.skinned || gm.vb == nullptr || gm.ib == nullptr) return;
    if (skinned && inst.palette.size() < size_t(gm.bones)) return;
    // Um rosto com algum peso diferente de 0 usa a variante com os morphs; sem pesos, o skinning comum.
    bool morph = false;
    if (skinned && frame.morphs && gm.morph_set != nullptr) {
        for (const float w : inst.morph) morph = morph || w != 0.0f;
    }
    const uint32_t prog = morph ? kProgMorph : (skinned ? kProgSkin : kProgMain);
    // Para cada textura usada, a leitura antes do pass (uma transição o fecharia).
    for (const SubMesh& sm : gm.submeshes) {
        if (sm.index_count != 0) ensure_read(texture_for(sm.texture));
    }
    item_pass();
    set_scissor_full();
    prepare(ualign * (gm.submeshes.size() + 2) + kBonesBytes + sizeof(MorphU));
    size_t bones_off = 0, morph_off = 0;
    if (skinned) {
        const size_t n = std::min(inst.palette.size(), size_t(kMaxSkinBones));
        const Alloc a = stream_alloc(frame_stream, kBonesBytes, ualign);
        std::memcpy(a.ptr, inst.palette.data(), n * sizeof(inst.palette[0]));
        bones_off = a.offset;
    }
    if (morph) {
        MorphU mu{};
        mu.n = uint32_t(std::min<size_t>({inst.morph.size(), size_t(gm.morph_targets), size_t(kMaxMorphTargets)}));
        mu.count = gm.morph_elements;
        for (uint32_t k = 0; k < mu.n; ++k) mu.w[k][0] = inst.morph[k];
        morph_off = uniform(&mu, sizeof(mu));
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, 1, &gm.morph_set->set, 0, nullptr);
    }
    const VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &gm.vb->buf, &zero);
    vkCmdBindIndexBuffer(cmd, gm.ib->buf, 0, VK_INDEX_TYPE_UINT32);
    for (const SubMesh& sm : gm.submeshes) {
        const size_t obj = set_state(inst, sm.gs, prog, false, sm.env && frame.env_map);
        if (sm.env) ++env_parts_;
        if (sm.index_count == 0) continue;
        bind_set0(obj, bones_off, morph_off);
        bind_tex(texture_for(sm.texture), sampler);
        vkCmdDrawIndexed(cmd, sm.index_count, 1, sm.first_index, 0, 0);
        ++counters_.draws;
    }
}

// Sprites de psiDrawParticleList: quatro vértices por partícula num pedaço do quadro, dois triângulos cada.
void Renderer::Impl::draw_sprites(const DrawItem& inst) {
    const size_t vertices = inst.sprites.size() - inst.sprites.size() % 4;
    if (vertices == 0) return;
    const size_t quads = vertices / 4;
    ensure_quad_ib(quads);
    GImage* tex = texture_for(inst.sprite_texture);
    ensure_read(tex);
    const Alloc a = transient(vertices * sizeof(Vertex));
    std::memcpy(a.ptr, inst.sprites.data(), vertices * sizeof(Vertex));
    item_pass();
    set_scissor_full();
    prepare(ualign);
    const size_t obj = set_state(inst, GsState{}, kProgMain, true);
    bind_set0(obj, 0, 0);
    const VkDeviceSize off = a.offset;
    vkCmdBindVertexBuffers(cmd, 0, 1, &a.buf, &off);
    vkCmdBindIndexBuffer(cmd, quad_ib->buf, 0, VK_INDEX_TYPE_UINT32);
    bind_tex(tex, sampler);
    vkCmdDrawIndexed(cmd, uint32_t(quads * 6), 1, 0, 0, 0);
    ++counters_.draws;
}

// Triângulos soltos (os estilhaços de psiDrawShard), sem índices, com o shader e o estado dos sprites.
void Renderer::Impl::draw_triangles(const DrawItem& inst) {
    const size_t vertices = inst.triangles.size() - inst.triangles.size() % 3;
    if (vertices == 0) return;
    GImage* tex = texture_for(inst.triangle_texture);
    ensure_read(tex);
    const Alloc a = transient(vertices * sizeof(Vertex));
    std::memcpy(a.ptr, inst.triangles.data(), vertices * sizeof(Vertex));
    item_pass();
    set_scissor_full();
    prepare(ualign);
    const size_t obj = set_state(inst, GsState{}, kProgMain, true);
    bind_set0(obj, 0, 0);
    const VkDeviceSize off = a.offset;
    vkCmdBindVertexBuffers(cmd, 0, 1, &a.buf, &off);
    bind_tex(tex, sampler);
    vkCmdDraw(cmd, uint32_t(vertices), 1, 0, 0);
    ++counters_.draws;
}

// 2D (psiDrawSprites): os quadrados já em coordenadas da tela (-1..1); seguidos com a mesma textura, o mesmo ALPHA_1 e
// o mesmo filtro, um desenho só. O estado de psiStartSprites (alfa > 0, sem teste nem escrita de Z) e o recorte.
void Renderer::Impl::draw_2d(const DrawItem& inst) {
    const size_t quads = inst.quads2d.size();
    if (quads == 0) return;
    ensure_quad_ib(quads);
    for (const Quad2D& q : inst.quads2d) ensure_read(q.image != nullptr ? gimage_of(q.image) : texture_for(q.texture));
    const Alloc a = transient(quads * 4 * sizeof(Vertex));
    auto* out = reinterpret_cast<Vertex*>(a.ptr);
    const float sx = 2.0f / float(width_), sy = 2.0f / float(height_);
    for (const Quad2D& q : inst.quads2d) {
        const float x0 = q.x0 * sx - 1.0f, x1 = q.x1 * sx - 1.0f;
        const float y0 = 1.0f - q.y0 * sy, y1 = 1.0f - q.y1 * sy;
        const float corner[4][4] = {{x0, y0, q.u0, q.v0}, {x1, y0, q.u1, q.v0}, {x1, y1, q.u1, q.v1}, {x0, y1, q.u0, q.v1}};
        for (const auto& c : corner) {
            Vertex v{};
            v.pos[0] = c[0];
            v.pos[1] = c[1];
            v.uv[0] = c[2];
            v.uv[1] = c[3];
            std::memcpy(v.color, q.rgba, 4);
            *out++ = v;
        }
    }
    item_pass();
    if (!set_scissor(inst.clip2d)) return;
    prepare(ualign * (quads + 1));
    const VkDeviceSize off = a.offset;
    vkCmdBindVertexBuffers(cmd, 0, 1, &a.buf, &off);
    vkCmdBindIndexBuffer(cmd, quad_ib->buf, 0, VK_INDEX_TYPE_UINT32);
    size_t first = 0;
    while (first < quads) {
        const Quad2D& q = inst.quads2d[first];
        size_t end = first + 1;
        while (end < quads && inst.quads2d[end].texture == q.texture && inst.quads2d[end].alpha == q.alpha &&
               inst.quads2d[end].linear == q.linear && inst.quads2d[end].image == q.image &&
               inst.quads2d[end].smooth == q.smooth) {
            ++end;
        }
        GsState own;
        own.alpha = q.alpha;
        own.test = 0xd;             // ATE, ATST = GREATER, AREF = 0, sem ZTE
        own.zbuf = 0x131000000ull;  // ZMSK
        own.has = GsState::kAlpha | GsState::kTest | GsState::kZbuf;
        const size_t obj = set_state(inst, own, kProgMain, true, false, true);
        bind_set0(obj, 0, 0);
        // Uma substituta HD (pacote do PCSX2) é amostrada com filtro bilinear mesmo nas fontes.
        const bool hd = use_hd && q.texture >= 0 && q.texture < int(hd_textures.size()) &&
                        hd_textures[size_t(q.texture)] != nullptr;
        bind_tex(q.image != nullptr ? gimage_of(q.image) : texture_for(q.texture),
                 q.smooth ? sampler_smooth : (q.linear || hd ? sampler_linear : sampler_point));
        vkCmdDrawIndexed(cmd, uint32_t((end - first) * 6), 1, uint32_t(first * 6), 0, 0);
        ++counters_.draws;
        first = end;
    }
}

// Passada de névoa (psiDrawFogPass): um triângulo que cobre a cena lê a profundidade de cada pixel (num pass sem ela,
// que a lê como textura) e mistura a cor da névoa como o sprite do PS2.
void Renderer::Impl::draw_fog(const DrawItem& inst, const FrameParams& frame) {
    FogU f{};
    for (int k = 0; k < 3; ++k) f.color[k] = float(inst.fog_rgb[k]) / 255.0f;
    f.color[3] = 1.0f;
    vu1::screen_z_coefficients(inst.fog_far1, f.a, f.b);  // Z do GS = A / z + B
    f.nf = frame.z_near * frame.z_far;
    f.fmn = frame.z_far - frame.z_near;
    f.n = frame.z_near;
    GImage* color = scene_target();
    ensure_read(depth.get());
    use_pass(color, nullptr);
    const bool ms = color->samples > 1;
    bind_pipe(pipeline(ms ? kProgFogMs : kProgFog, nullptr, false, color->samples, false, color->format, VK_COMPARE_OP_ALWAYS,
                       false, wireframe));
    set_scissor_full();
    push(&f, sizeof(f));
    bind_tex(depth.get(), sampler_point);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

// O quadro do filme no começo do quadro (o pass da cena acabou de limpar o alvo de preto): a imagem no retângulo da
// tela do PS2, na escala da cena.
void Renderer::Impl::draw_movie() {
    const float sx = float(scene_w) / float(width_), sy = float(scene_h) / float(height_);
    const float rw = float(pass_color->w), rh = float(pass_color->h);
    MovieU u{};
    u.rect[0] = movie_rect[0] * sx;
    u.rect[1] = movie_rect[1] * sy;
    u.rect[2] = movie_rect[2] * sx;
    u.rect[3] = movie_rect[3] * sy;
    const float x0 = std::clamp(u.rect[0], 0.0f, rw), y0 = std::clamp(u.rect[1], 0.0f, rh);
    const float x1 = std::clamp(u.rect[2], 0.0f, rw), y1 = std::clamp(u.rect[3], 0.0f, rh);
    if (x1 - x0 < 1.0f || y1 - y0 < 1.0f) return;
    u.target[0] = rw;
    u.target[1] = rh;
    u.tex_size[0] = float(movie_tex->w);
    u.tex_size[1] = float(movie_tex->h);
    bind_pipe(pipeline(kProgMovie, nullptr, false, pass_color->samples, pass_depth != nullptr, pass_color->format));
    set_scissor_full();
    push(&u, sizeof(u));
    bind_tex(movie_tex.get(), sampler_linear);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

// A cena do alvo próprio para o alvo do quadro. Com MSAA, a resolução (direto no alvo quando o tamanho é o mesmo); com
// escala, ps_down reduz (ou amplia) para a janela.
void Renderer::Impl::resolve_scene() {
    end_pass();
    const bool scaled = scene_w != width_ || scene_h != height_;
    if (scene_msaa > 1) {
        GImage* dst = scaled ? scene_resolved.get() : target.get();
        VkCommandBuffer cb = frame_cmd();
        barrier(cb, scene_color.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageResolve r{};
        r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.extent = {uint32_t(scene_w), uint32_t(scene_h), 1};
        vkCmdResolveImage(cb, scene_color->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        if (!scaled) return;
    }
    DownU d{};
    d.src_size[0] = float(scene_w);
    d.src_size[1] = float(scene_h);
    d.ratio[0] = float(scene_w) / float(width_);
    d.ratio[1] = float(scene_h) / float(height_);
    d.taps = float(std::clamp(int(std::ceil(std::max(d.ratio[0], d.ratio[1]) - 0.01f)), 1, 4));
    draw_full(kProgDown, target.get(), scene_msaa > 1 ? scene_resolved.get() : scene_color.get(), &d, sizeof(d));
}

// Camada 2D em alta definição (render/ui_hd.h): as imagens seguidas com o mesmo recorte viram um item 2D comum (com a
// substituta no lugar da textura e o filtro com mipmaps); os textos e as teclas seguidos, uma imagem da UiVectorCanvas
// composta no alvo.
void Renderer::Impl::draw_ui(const UiHdBatch& batch) {
    if (canvas == nullptr || !canvas->ok() || !hd_on) return;
    DrawItem images;
    images.blend = Blend::Alpha;
    const auto flush_images = [&]() {
        if (images.quads2d.empty()) return;
        draw_2d(images);
        images.quads2d.clear();
    };
    std::vector<const UiHdText*> texts;
    std::vector<const UiHdKey*> keys;
    for (const UiHdText& t : batch.texts) texts.push_back(&t);
    for (const UiHdKey& k : batch.keys) keys.push_back(&k);
    std::vector<UiHdOp> run;
    const auto flush_text = [&]() {
        if (run.empty()) return;
        UiRaster r;
        canvas->draw_hd(texts, keys, run, width_, height_, r);
        composite(r);
        run.clear();
    };
    for (const UiHdOp& op : batch.ops) {
        if (op.kind == UiHdOp::Text || op.kind == UiHdOp::Key) {
            flush_images();
            run.push_back(op);
            continue;
        }
        flush_text();
        const UiHdImage& im = batch.images[op.index];
        if (!images.quads2d.empty() && std::memcmp(images.clip2d, im.clip, sizeof(im.clip)) != 0) flush_images();
        std::memcpy(images.clip2d, im.clip, sizeof(im.clip));
        Quad2D q;
        q.x0 = im.x0;
        q.y0 = im.y0;
        q.x1 = im.x1;
        q.y1 = im.y1;
        q.u0 = im.u0;
        q.v0 = im.v0;
        q.u1 = im.u1;
        q.v1 = im.v1;
        std::memcpy(q.rgba, im.rgba, 4);
        q.texture = im.texture;
        q.alpha = im.alpha;
        q.linear = im.linear;
        q.smooth = im.linear;
        if (im.asset >= 0) {
            GImage* t = hd_asset(im.asset, im.asset_w, im.asset_h);
            q.image = t != nullptr ? image_of(t) : nullptr;
            if (q.image == nullptr) {  // sem a substituta: a textura original, com o ST original
                q.u0 = im.orig_uv[0];
                q.v0 = im.orig_uv[1];
                q.u1 = im.orig_uv[2];
                q.v1 = im.orig_uv[3];
            }
        }
        images.quads2d.push_back(q);
    }
    flush_images();
    flush_text();
}

// Uma imagem da UiVectorCanvas (BGRA, alfa pré-multiplicado) por cima do alvo, na posição dela: os pixels no começo de um
// pedaço próprio do quadro (o storage buffer dinâmico do conjunto 0 só aceita o deslocamento 0), lidos texel a texel.
void Renderer::Impl::composite(const UiRaster& r) {
    if (r.empty() || r.bgra.size() < size_t(r.width) * size_t(r.height) * 4) return;
    const size_t bytes = size_t(r.width) * size_t(r.height) * 4;
    if (frame_stream.cur != nullptr) frame_stream.used.push_back(frame_stream.cur);
    frame_stream.cur = get_chunk(bytes + 4 * ualign + kBonesBytes);
    frame_stream.off = (bytes + ualign - 1) / ualign * ualign;
    frame_off = SIZE_MAX;
    std::memcpy(frame_stream.cur->buf->ptr(), r.bgra.data(), bytes);
    target_pass();
    bind_pipe(pipeline(kProgComposite, nullptr, false, pass_color->samples, pass_depth != nullptr, pass_color->format));
    set_scissor_full();
    RectU u{};
    u.rect[0] = float(r.x);
    u.rect[1] = float(r.y);
    u.rect[2] = float(r.x + r.width);
    u.rect[3] = float(r.y + r.height);
    u.target[0] = float(pass_color->w);
    u.target[1] = float(pass_color->h);
    u.pitch = uint32_t(r.width);
    push(&u, sizeof(u));
    bind_set0(0, 0, 0, 0);
    vkCmdDraw(cmd, 6, 1, 0, 0);
    ++counters_.draws;
}

// A substituta "index" da camada HD rasterizada em w x h (SVG) ou lida (PNG, com mipmaps), em cache como no Direct2D;
// esquecida depois de 300 quadros sem uso. Nula se falhar (fica a textura original, e não tenta de novo).
GImage* Renderer::Impl::hd_asset(int index, int w, int h) {
    if (canvas == nullptr || hd_config == nullptr || index < 0 || size_t(index) >= hd_config->images.size()) return nullptr;
    if (hd_failed.size() < hd_config->images.size()) hd_failed.resize(hd_config->images.size(), 0);
    if (hd_failed[size_t(index)] != 0) return nullptr;
    const bool png = hd_config->images[size_t(index)].type == UiHdAssetType::Png;
    if (png) w = h = 0;
    w = std::clamp(w, png ? 0 : 1, 4096);
    h = std::clamp(h, png ? 0 : 1, 4096);
    for (HdAsset& a : hd_assets) {
        if (a.index == index && a.w == w && a.h == h) {
            a.used = frame_no;
            return a.tex.get();
        }
    }
    std::vector<uint8_t> rgba;
    int ow = 0, oh = 0;
    if (!canvas->asset(index, w, h, rgba, ow, oh) || ow <= 0 || oh <= 0 || rgba.size() < size_t(ow) * size_t(oh) * 4) {
        hd_failed[size_t(index)] = 1;
        return nullptr;
    }
    auto t = make_image(VK_FORMAT_R8G8B8A8_UNORM, ow, oh, png, VK_SAMPLE_COUNT_1_BIT,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (t == nullptr) {
        hd_failed[size_t(index)] = 1;
        return nullptr;
    }
    upload(t.get(), 0, rgba.data(), size_t(ow) * 4, size_t(oh), uint32_t(ow), uint32_t(oh));
    finish_upload(t.get());
    HdAsset a;
    a.index = index;
    a.w = w;
    a.h = h;
    a.tex = t;
    a.used = frame_no;
    hd_assets.push_back(a);
    return t.get();
}

void Renderer::Impl::ensure_blur_targets() {
    const int qw = std::max(1, width_ / 4), qh = std::max(1, height_ / 4);
    if (blur_full == nullptr || blur_full->w != width_ || blur_full->h != height_) {
        blur_full = make_image(VK_FORMAT_B8G8R8A8_UNORM, width_, height_, false, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }
    if (blur_w == qw && blur_h == qh && blur_tex[0] != nullptr) return;
    blur_w = qw;
    blur_h = qh;
    for (int k = 0; k < 2; ++k) {
        blur_tex[k] = make_image(VK_FORMAT_B8G8R8A8_UNORM, qw, qh, false, VK_SAMPLE_COUNT_1_BIT,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    }
}

void Renderer::Impl::blur_background(float amount) {
    if (amount <= 0.001f || target == nullptr) return;
    ensure_blur_targets();
    if (blur_full == nullptr || blur_tex[0] == nullptr || blur_tex[1] == nullptr) return;
    copy_texture(target.get(), blur_full.get());
    // 1/4: a média de 4 x 4 texels (ps_down).
    DownU d{};
    d.src_size[0] = float(width_);
    d.src_size[1] = float(height_);
    d.ratio[0] = float(width_) / float(blur_w);
    d.ratio[1] = float(height_) / float(blur_h);
    d.taps = 4.0f;
    draw_full(kProgDown, blur_tex[0].get(), blur_full.get(), &d, sizeof(d));
    // A gaussiana: horizontal e vertical, duas vezes (a segunda mais espaçada).
    const float spacing[2] = {1.0f, 2.2f};
    for (int pass = 0; pass < 2; ++pass) {
        for (int axis = 0; axis < 2; ++axis) {
            BlurU b{};
            b.texel[0] = 1.0f / float(blur_w);
            b.texel[1] = 1.0f / float(blur_h);
            b.dir[0] = axis == 0 ? spacing[pass] : 0.0f;
            b.dir[1] = axis == 1 ? spacing[pass] : 0.0f;
            const int from = axis == 0 ? 0 : 1, to = 1 - from;
            draw_full(kProgBlur, blur_tex[to].get(), blur_tex[from].get(), &b, sizeof(b));
        }
    }
    // A mistura no alvo.
    BlurU b{};
    b.texel[0] = 1.0f / float(width_);
    b.texel[1] = 1.0f / float(height_);
    b.amount = std::clamp(amount, 0.0f, 1.0f);
    ensure_read(blur_full.get());
    ensure_read(blur_tex[0].get());
    use_pass(target.get(), nullptr);
    bind_pipe(pipeline(kProgBlurMix, nullptr, false, VK_SAMPLE_COUNT_1_BIT, false, target->format));
    set_scissor_full();
    push(&b, sizeof(b));
    bind_tex(blur_full.get(), sampler_linear, blur_tex[0].get(), sampler_linear);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    end_pass();
}

void Renderer::Impl::screen_blur(float keep) {
    if (!(keep > 0.0f) || target == nullptr) {
        history_valid = false;
        return;
    }
    if (history == nullptr || history->w != target->w || history->h != target->h) {
        history = make_image(VK_FORMAT_B8G8R8A8_UNORM, target->w, target->h, false, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        history_valid = false;
        if (history == nullptr) return;
    }
    if (history_valid) {
        const float k = std::clamp(keep, 0.0f, 1.0f);
        ensure_read(history.get());
        use_pass(target.get(), nullptr);
        bind_pipe(pipeline(kProgHistory, nullptr, false, VK_SAMPLE_COUNT_1_BIT, false, target->format));
        set_scissor_full();
        const float c[4] = {k, k, k, k};
        vkCmdSetBlendConstants(cmd, c);
        bind_tex(history.get(), sampler_linear);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        ++counters_.draws;
    }
    copy_texture(target.get(), history.get());
    history_valid = true;
}

// ---------------------------------------------------------------- a cadeia de troca

bool Renderer::Impl::create_surface() {
    if (window == nullptr || offscreen_) return false;
    if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(window), core->instance, &surface)) {
        std::fprintf(stderr, "vulkan: SDL_Vulkan_CreateSurface falhou: %s\n", SDL_GetError());
        surface = VK_NULL_HANDLE;
        return false;
    }
    sc_dirty = true;
    return true;
}

// As imagens, as views e os semáforos da cadeia: soltos quando a GPU acaba o que os usa (now: na hora, depois de esperar).
void Renderer::Impl::destroy_swapchain(bool now) {
    Core* c = core.get();
    const VkDevice d = dev;
    std::vector<VkSemaphore> sems = std::move(sc_done);
    sc_done.clear();
    sc_images.clear();  // as views saem pelo destrutor do GImage (retire)
    const VkSwapchainKHR sc = swapchain;
    swapchain = VK_NULL_HANDLE;
    auto fn = [d, sems, sc] {
        for (VkSemaphore s : sems) vkDestroySemaphore(d, s, nullptr);
        if (sc != VK_NULL_HANDLE) vkDestroySwapchainKHR(d, sc, nullptr);
    };
    if (now) {
        c->tag_pending(c->submitted);
        c->run_grave();
        fn();
    } else {
        c->retire(fn);
    }
}

bool Renderer::Impl::create_swapchain() {
    if (surface == VK_NULL_HANDLE) return false;
    const VkPhysicalDevice phys = core->phys;
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps) != VK_SUCCESS) return false;
    VkSurfaceTransformFlagBitsKHR transform = caps.currentTransform;
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        int w = 0, h = 0;
        SDL_Vulkan_GetDrawableSize(static_cast<SDL_Window*>(window), &w, &h);
        extent.width = uint32_t(std::clamp<int>(w, int(caps.minImageExtent.width), int(caps.maxImageExtent.width)));
        extent.height = uint32_t(std::clamp<int>(h, int(caps.minImageExtent.height), int(caps.maxImageExtent.height)));
    } else if (transform == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR &&
               (caps.currentTransform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR))) {
        // O Android com a tela girada: a extensão é a do sentido natural do aparelho; o compositor gira a imagem.
        std::swap(extent.width, extent.height);
    }
    if (extent.width == 0 || extent.height == 0) return false;
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(nf);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nf, formats.data());
    if (formats.empty()) return false;
    VkSurfaceFormatKHR fmt = formats[0];
    bool found = false;
    for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
        for (const auto& f : formats) {
            if (!found && f.format == want) {
                fmt = f;
                found = true;
            }
        }
    }
    if (fmt.format == VK_FORMAT_UNDEFINED) fmt.format = VK_FORMAT_B8G8R8A8_UNORM;
    uint32_t np = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &np, nullptr);
    std::vector<VkPresentModeKHR> pmodes(np);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &np, pmodes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!vsync_) {
        for (VkPresentModeKHR want : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
            if (mode == VK_PRESENT_MODE_FIFO_KHR && std::find(pmodes.begin(), pmodes.end(), want) != pmodes.end()) mode = want;
        }
    }
    uint32_t images = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount > 0) images = std::min(images, caps.maxImageCount);
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        for (uint32_t b = 1; b != 0; b <<= 1) {
            if (caps.supportedCompositeAlpha & b) {
                alpha = VkCompositeAlphaFlagBitsKHR(b);
                break;
            }
        }
    }
    VkSwapchainCreateInfoKHR si{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    si.surface = surface;
    si.minImageCount = images;
    si.imageFormat = fmt.format;
    si.imageColorSpace = fmt.colorSpace;
    si.imageExtent = extent;
    si.imageArrayLayers = 1;
    si.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    si.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    si.preTransform = transform;
    si.compositeAlpha = alpha;
    si.presentMode = mode;
    si.clipped = VK_TRUE;
    si.oldSwapchain = swapchain;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    const VkResult res = vkCreateSwapchainKHR(dev, &si, nullptr, &sc);
    destroy_swapchain(false);  // a antiga (oldSwapchain) sai quando a GPU acabar o que a usa
    if (res != VK_SUCCESS) {
        if (res == VK_ERROR_SURFACE_LOST_KHR) throw std::runtime_error("surface");
        return false;
    }
    swapchain = sc;
    sc_format = fmt.format;
    sc_extent = extent;
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(dev, sc, &n, nullptr);
    std::vector<VkImage> imgs(n);
    vkGetSwapchainImagesKHR(dev, sc, &n, imgs.data());
    for (VkImage im : imgs) {
        auto g = std::make_shared<GImage>();
        g->core = core;
        g->image = im;
        g->owned = false;
        g->format = sc_format;
        g->w = int(extent.width);
        g->h = int(extent.height);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = sc_format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk_check(vkCreateImageView(dev, &vi, nullptr, &g->view), "vkCreateImageView (cadeia de troca)");
        g->att_view = g->view;
        sc_images.push_back(g);
        VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s = VK_NULL_HANDLE;
        vk_check(vkCreateSemaphore(dev, &semi, nullptr, &s), "vkCreateSemaphore");
        sc_done.push_back(s);
    }
    for (AcqSem& a : acq) {
        if (a.sem == VK_NULL_HANDLE) {
            VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            vk_check(vkCreateSemaphore(dev, &semi, nullptr, &a.sem), "vkCreateSemaphore");
        }
    }
    sc_dirty = false;
    return true;
}

void Renderer::Impl::present() {
    if (!offscreen_ && window != nullptr) {
        try {
            const Uint32 flags = SDL_GetWindowFlags(static_cast<SDL_Window*>(window));
            const bool hidden = (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0;
            if (surface == VK_NULL_HANDLE && !hidden) create_surface();
            if (!hidden && surface != VK_NULL_HANDLE && (sc_dirty || swapchain == VK_NULL_HANDLE)) create_swapchain();
            bool shown = false;
            if (!hidden && swapchain != VK_NULL_HANDLE) {
                AcqSem& a = acq[size_t(acq_i)];
                acq_i = (acq_i + 1) % int(acq.size());
                wait_serial(a.serial);  // o envio que esperou por este semáforo acabou
                uint32_t idx = 0;
                const VkResult r = vkAcquireNextImageKHR(dev, swapchain, 1000000000ull, a.sem, VK_NULL_HANDLE, &idx);
                if (r == VK_ERROR_SURFACE_LOST_KHR) throw std::runtime_error("surface");
                if (r == VK_ERROR_OUT_OF_DATE_KHR) sc_dirty = true;
                if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
                    if (r == VK_SUBOPTIMAL_KHR) sc_dirty = true;
                    GImage* dst = sc_images[idx].get();
                    dst->layout = VK_IMAGE_LAYOUT_UNDEFINED;
                    // O alvo para a imagem: o texel do pixel no mesmo tamanho; noutro, reduzido ou ampliado como a cena.
                    ensure_read(target.get());
                    begin_pass(dst, nullptr, VK_ATTACHMENT_LOAD_OP_DONT_CARE, nullptr, false);
                    if (dst->w == target->w && dst->h == target->h) {
                        draw_full(kProgBlit, dst, target.get(), nullptr, 0);
                    } else {
                        DownU d{};
                        d.src_size[0] = float(target->w);
                        d.src_size[1] = float(target->h);
                        d.ratio[0] = float(target->w) / float(dst->w);
                        d.ratio[1] = float(target->h) / float(dst->h);
                        d.taps = float(std::clamp(int(std::ceil(std::max(d.ratio[0], d.ratio[1]) - 0.01f)), 1, 4));
                        draw_full(kProgDown, dst, target.get(), &d, sizeof(d));
                    }
                    transition(dst, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                    VkSemaphore done = sc_done[idx];
                    commit_frame(a.sem, done);
                    a.serial = last_frame_serial;
                    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
                    pi.waitSemaphoreCount = 1;
                    pi.pWaitSemaphores = &done;
                    pi.swapchainCount = 1;
                    pi.pSwapchains = &swapchain;
                    pi.pImageIndices = &idx;
                    const VkResult pr = vkQueuePresentKHR(core->queue, &pi);
                    if (pr == VK_ERROR_SURFACE_LOST_KHR) throw std::runtime_error("surface");
                    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) sc_dirty = true;
                    shown = true;
                }
            }
            if (!shown) {
                // Sem imagem (a janela minimizada, o app no fundo no Android): o quadro não aparece; com o VSync, o ritmo
                // de 60 Hz sem ela.
                ++counters_.occluded;
                if (vsync_) std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        } catch (const std::runtime_error& e) {
            if (std::strcmp(e.what(), "surface") != 0) throw;
            // A superfície se perdeu (o Android solta a janela nativa quando o app vai para o fundo): tudo o que a usa
            // sai, e a próxima volta cria outra.
            end_pass();
            commit_frame();
            vkDeviceWaitIdle(dev);
            poll();
            destroy_swapchain(true);
            vkDestroySurfaceKHR(core->instance, surface, nullptr);
            surface = VK_NULL_HANDLE;
            for (AcqSem& a : acq) {
                if (a.sem != VK_NULL_HANDLE) vkDestroySemaphore(dev, a.sem, nullptr);
                a = AcqSem{};
            }
            ++counters_.occluded;
        }
    }
    commit_frame();
    if (canvas != nullptr) canvas->end_frame();
    ++frame_no;
    hd_assets.erase(std::remove_if(hd_assets.begin(), hd_assets.end(),
                                   [&](const HdAsset& a) { return a.used + 300 < frame_no; }),
                    hd_assets.end());
    // A CPU no máximo três quadros à frente da GPU (a fila de uma cadeia de troca; fora da tela não há outra).
    const uint64_t old = throttle[size_t(throttle_i)];
    if (old != 0) wait_serial(old);
    throttle[size_t(throttle_i)] = last_frame_serial;
    throttle_i = (throttle_i + 1) % 3;
}

void Renderer::Impl::save_png(const std::wstring& path) {
    const int w = target->w, h = target->h;
    auto rb = make_buffer(VkDeviceSize(w) * VkDeviceSize(h) * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (rb == nullptr) throw std::runtime_error("save_png: sem memória para a leitura");
    end_pass();
    VkCommandBuffer cb = frame_cmd();
    barrier(cb, target.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {uint32_t(w), uint32_t(h), 1};
    vkCmdCopyImageToBuffer(cb, target->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb->buf, 1, &r);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    commit_frame();
    wait_serial(last_frame_serial);
    // Força alfa opaco no arquivo.
    std::vector<uint8_t> pixels(size_t(w) * size_t(h) * 4);
    std::memcpy(pixels.data(), rb->ptr(), pixels.size());
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    write_png_bgra(path, w, h, pixels.data());  // platform/image_io.h; lança se falhar
}

bool Renderer::Impl::read_texture(int slot, int& width, int& height, std::vector<uint8_t>& rgba) {
    if (slot < 0 || slot >= int(textures.size()) || textures[size_t(slot)] == nullptr) return false;
    GImage* tex = textures[size_t(slot)].get();
    const int w = tex->w, h = tex->h;
    auto rb = make_buffer(VkDeviceSize(w) * VkDeviceSize(h) * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (rb == nullptr) return false;
    VkCommandBuffer cb = upload_cmd();
    barrier(cb, tex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {uint32_t(w), uint32_t(h), 1};
    vkCmdCopyImageToBuffer(cb, tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb->buf, 1, &r);
    barrier(cb, tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    wait_serial(flush_uploads());
    width = w;
    height = h;
    rgba.resize(size_t(w) * size_t(h) * 4);
    std::memcpy(rgba.data(), rb->ptr(), rgba.size());
    return true;
}

// ---------------------------------------------------------------- os recursos

int Renderer::Impl::add_mesh(const EntityMesh& mesh) {
    GpuMesh gm;
    gm.skinned = mesh.skinned;
    gm.bones = mesh.bone_count;
    const void* vdata = mesh.skinned ? static_cast<const void*>(mesh.skin_vertices.data()) : mesh.vertices.data();
    const size_t vbytes = mesh.skinned ? mesh.skin_vertices.size() * sizeof(SkinnedVertex) : mesh.vertices.size() * sizeof(Vertex);
    if (vbytes > 0) gm.vb = make_buffer(vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vdata);
    if (!mesh.indices.empty()) {
        gm.ib = make_buffer(mesh.indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, mesh.indices.data());
    }
    gm.submeshes = mesh.submeshes;
    // Os alvos de morph de um rosto: os deltas de todos os elementos do bloco num buffer (float3 sem folga), com o
    // conjunto 2 dele.
    if (mesh.skinned && !mesh.morph.empty()) {
        gm.morph = make_buffer(mesh.morph.deltas.size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               mesh.morph.deltas.data());
        if (gm.morph != nullptr) {
            auto ms = std::make_shared<MorphSet>();
            ms->core = core;
            VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = 1;
            pi.poolSizeCount = 1;
            pi.pPoolSizes = &ps;
            vk_check(vkCreateDescriptorPool(dev, &pi, nullptr, &ms->pool), "vkCreateDescriptorPool (morph)");
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = ms->pool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &set_layouts[2];
            vk_check(vkAllocateDescriptorSets(dev, &ai, &ms->set), "vkAllocateDescriptorSets (morph)");
            VkDescriptorBufferInfo bi{gm.morph->buf, 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = ms->set;
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo = &bi;
            vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
            gm.morph_set = ms;
        }
        gm.morph_elements = uint32_t(mesh.morph.elements());
        gm.morph_targets = int(mesh.morph.targets);
    }
    meshes.push_back(std::move(gm));
    return int(meshes.size()) - 1;
}

void Renderer::Impl::set_texture(int slot, const Texture& t) {
    if (slot < 0 || t.width <= 0 || t.height <= 0 || t.rgba.size() < size_t(t.width) * size_t(t.height) * 4) return;
    auto tex = make_image(VK_FORMAT_R8G8B8A8_UNORM, t.width, t.height, true, VK_SAMPLE_COUNT_1_BIT,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (tex == nullptr) throw std::runtime_error("textura: sem memória");
    upload(tex.get(), 0, t.rgba.data(), size_t(t.width) * 4, size_t(t.height), uint32_t(t.width), uint32_t(t.height));
    finish_upload(tex.get());
    if (slot >= int(textures.size())) textures.resize(size_t(slot) + 1);
    textures[size_t(slot)] = tex;
}

// O conteúdo novo da textura do slot (Renderer::update_texture; o UpdateSubresource + GenerateMips do D3D11). Fora de
// um quadro, pelo command buffer de envio; com um quadro sendo gravado, no command buffer dele, na ordem: os desenhos
// de antes veem o conteúdo antigo, como no contexto imediato do D3D11. A substituta HD do slot sai.
void Renderer::Impl::update_texture(int slot, const Texture& t) {
    if (slot < 0 || t.width <= 0 || t.height <= 0 || t.rgba.size() < size_t(t.width) * size_t(t.height) * 4) return;
    if (slot < int(hd_textures.size())) hd_textures[size_t(slot)] = nullptr;
    GImage* tex = slot < int(textures.size()) ? textures[size_t(slot)].get() : nullptr;
    if (tex == nullptr || tex->w != t.width || tex->h != t.height) {
        set_texture(slot, t);
        return;
    }
    const size_t row = size_t(t.width) * 4;
    if (cmd == VK_NULL_HANDLE) {
        upload(tex, 0, t.rgba.data(), row, size_t(t.height), uint32_t(t.width), uint32_t(t.height));
        finish_upload(tex);
        return;
    }
    end_pass();
    VkCommandBuffer cb = frame_cmd();
    upload(tex, 0, t.rgba.data(), row, size_t(t.height), uint32_t(t.width), uint32_t(t.height), cb);
    finish_upload(tex, cb);
}

bool Renderer::Impl::set_texture_dds(int slot, const std::wstring& path) {
    if (slot < 0) return false;
    FILE* f = os::open_file(path, "rb");
    if (f == nullptr) return false;
    std::vector<uint8_t> file;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size > 128) {
        file.resize(size_t(size));
        if (std::fread(file.data(), 1, file.size(), f) != file.size()) file.clear();
    }
    std::fclose(f);
    if (file.size() <= 128 || std::memcmp(file.data(), "DDS ", 4) != 0) return false;
    const auto u32 = [&](size_t o) {
        uint32_t v;
        std::memcpy(&v, file.data() + o, 4);
        return v;
    };
    const uint32_t height = u32(12), width = u32(16);
    const uint32_t mips = std::max(1u, u32(28));
    const uint32_t pf_flags = u32(80), fourcc = u32(84), rgb_bits = u32(88);
    const auto cc = [](const char* s) {
        return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 | uint32_t(uint8_t(s[3])) << 24;
    };
    // Os formatos do DXGI que o D3D11 aceita aqui, nos do Vulkan (o BC só com textureCompressionBC: as GPUs de celular
    // não têm, e o pacote fica nas originais).
    uint32_t dxgi = 0;
    size_t offset = 128;
    if ((pf_flags & 4) != 0 && fourcc == cc("DX10")) {
        if (file.size() < 148) return false;
        dxgi = u32(128);
        offset = 148;
    } else if ((pf_flags & 4) != 0 && fourcc == cc("DXT1")) {
        dxgi = 71;
    } else if ((pf_flags & 4) != 0 && fourcc == cc("DXT3")) {
        dxgi = 74;
    } else if ((pf_flags & 4) != 0 && fourcc == cc("DXT5")) {
        dxgi = 77;
    } else if ((pf_flags & 0x40) != 0 && rgb_bits == 32 && u32(92) == 0xff && u32(100) == 0xff0000) {
        dxgi = 28;
    }
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    size_t block_bytes = 0;  // 0: sem compressão (4 bytes por texel)
    switch (dxgi) {
        case 71: fmt = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; block_bytes = 8; break;
        case 72: fmt = VK_FORMAT_BC1_RGBA_SRGB_BLOCK; block_bytes = 8; break;
        case 74: fmt = VK_FORMAT_BC2_UNORM_BLOCK; block_bytes = 16; break;
        case 75: fmt = VK_FORMAT_BC2_SRGB_BLOCK; block_bytes = 16; break;
        case 77: fmt = VK_FORMAT_BC3_UNORM_BLOCK; block_bytes = 16; break;
        case 78: fmt = VK_FORMAT_BC3_SRGB_BLOCK; block_bytes = 16; break;
        case 98: fmt = VK_FORMAT_BC7_UNORM_BLOCK; block_bytes = 16; break;
        case 99: fmt = VK_FORMAT_BC7_SRGB_BLOCK; block_bytes = 16; break;
        case 28: fmt = VK_FORMAT_R8G8B8A8_UNORM; break;
        case 29: fmt = VK_FORMAT_R8G8B8A8_SRGB; break;
        default: return false;
    }
    if (block_bytes != 0 && !core->features.textureCompressionBC) return false;
    if (width == 0 || height == 0 || width > 16384 || height > 16384) return false;
    struct Level {
        size_t offset, pitch, rows;
        uint32_t w, h;
    };
    std::vector<Level> levels;
    uint32_t w = width, h = height;
    for (uint32_t m = 0; m < mips; ++m) {
        const size_t pitch = block_bytes ? size_t(std::max(1u, (w + 3) / 4)) * block_bytes : size_t(w) * 4;
        const size_t rows = block_bytes ? std::max(1u, (h + 3) / 4) : h;
        if (offset + pitch * rows > file.size()) break;  // arquivo curto: usa os níveis completos
        levels.push_back({offset, pitch, rows, w, h});
        offset += pitch * rows;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    if (levels.empty()) return false;
    auto tex = make_image(fmt, int(width), int(height), false, VK_SAMPLE_COUNT_1_BIT,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                          uint32_t(levels.size()));
    if (tex == nullptr) return false;
    for (size_t m = 0; m < levels.size(); ++m) {
        const Level& l = levels[m];
        upload(tex.get(), uint32_t(m), file.data() + l.offset, l.pitch, l.rows, l.w, l.h);
    }
    VkCommandBuffer cb = upload_cmd();
    barrier(cb, tex.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (levels.size() == 1 && std::max(width, height) > 1) {
        // Sem mipmaps: copiada para RGBA8 na GPU (um desenho que lê cada texel) e com a cadeia gerada, para não
        // cintilar de longe (o mesmo do D3D11).
        auto full = make_image(VK_FORMAT_R8G8B8A8_UNORM, int(width), int(height), true, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (full != nullptr) {
            barrier(cb, full.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            const VkRenderPass rp = render_pass(full->format, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_UNDEFINED,
                                                VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_LOAD);
            const VkRenderPass canon = render_pass(full->format, VK_SAMPLE_COUNT_1_BIT, VK_FORMAT_UNDEFINED,
                                                   VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_LOAD);
            VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
            bi.renderPass = rp;
            bi.framebuffer = core->framebuffer(canon, full->att_view, VK_NULL_HANDLE, width, height);
            bi.renderArea = {{0, 0}, {width, height}};
            vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
            VkViewport vp{0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
            vkCmdSetViewport(cb, 0, 1, &vp);
            VkRect2D sc{{0, 0}, {width, height}};
            vkCmdSetScissor(cb, 0, 1, &sc);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipeline(kProgBlitSample, nullptr, false, VK_SAMPLE_COUNT_1_BIT, false, VK_FORMAT_R8G8B8A8_UNORM));
            const VkDescriptorSet set = tex_set(tex.get(), sampler_point);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &set, 0, nullptr);
            vkCmdDraw(cb, 3, 1, 0, 0);
            vkCmdEndRenderPass(cb);
            gen_mips(cb, full.get());
            tex = full;
        }
    }
    if (slot >= int(hd_textures.size())) hd_textures.resize(size_t(slot) + 1);
    hd_textures[size_t(slot)] = tex;
    if (up_bytes > kUploadFlushBytes) flush_uploads();
    return true;
}

void Renderer::Impl::set_ui_image(uint32_t key, const uint8_t* rgba, int width, int height) {
    if (rgba == nullptr || width <= 0 || height <= 0) return;
    auto tex = make_image(VK_FORMAT_R8G8B8A8_UNORM, width, height, true, VK_SAMPLE_COUNT_1_BIT,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (tex == nullptr) return;
    upload(tex.get(), 0, rgba, size_t(width) * 4, size_t(height), uint32_t(width), uint32_t(height));
    finish_upload(tex.get());
    for (auto& [k, v] : ui_images) {
        if (k == key) {
            v = tex;
            return;
        }
    }
    ui_images.emplace_back(key, tex);
}

void Renderer::Impl::set_movie_frame(const uint8_t* bgra, int width, int height, uint64_t serial) {
    if (bgra == nullptr || width <= 0 || height <= 0) return;
    if (movie_tex == nullptr || movie_tex->w != width || movie_tex->h != height) {
        movie_tex = make_image(VK_FORMAT_B8G8R8A8_UNORM, width, height, false, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        movie_serial = ~0ull;
        if (movie_tex == nullptr) return;
    }
    if (serial == movie_serial) return;
    if (cmd != VK_NULL_HANDLE) {
        // Com um quadro sendo gravado (que pode já ter lido o quadro anterior), no command buffer dele, na ordem.
        end_pass();
        VkCommandBuffer cb = frame_cmd();
        upload(movie_tex.get(), 0, bgra, size_t(width) * 4, size_t(height), uint32_t(width), uint32_t(height), cb);
        finish_upload(movie_tex.get(), cb);
    } else {
        upload(movie_tex.get(), 0, bgra, size_t(width) * 4, size_t(height), uint32_t(width), uint32_t(height));
        finish_upload(movie_tex.get());
    }
    movie_serial = serial;
}

void Renderer::Impl::ensure_canvas() {
    if (canvas_tried) return;
    canvas_tried = true;
    canvas = create_ui_vector_canvas();
}

// ---------------------------------------------------------------- as medidas do --bench

// Os tempos de GPU: o total de cada quadro (os carimbos no começo e no fim dos command buffers dele); os trechos
// (GpuPhase) ficam em 0, como no Metal.
void Renderer::Impl::gpu_collect(bool wait) {
    if (wait) wait_serial(core->submitted);
    poll();
    while (!ts_pending.empty() && ts_pending.front().serial <= core->completed) {
        const TsEntry e = ts_pending.front();
        ts_pending.pop_front();
        uint64_t t[2]{};
        const VkResult r = vkGetQueryPoolResults(dev, ts_pool, uint32_t(e.slot) * 2, 2, sizeof(t), t, sizeof(uint64_t),
                                                 VK_QUERY_RESULT_64_BIT);
        ts_busy[size_t(e.slot)] = false;
        if (e.epoch != epoch) continue;
        double ms = 0.0;
        if (r == VK_SUCCESS) {
            const uint64_t d = ((t[1] & ts_mask) - (t[0] & ts_mask)) & ts_mask;
            ms = double(d) * ts_period / 1.0e6;
            if (!(ms >= 0.0 && ms < 1.0e6)) ms = 0.0;
        }
        seq_pending[e.seq].second += ms;
        ++seq_done[e.seq];
    }
    for (auto it = seq_pending.begin(); it != seq_pending.end();) {
        const uint64_t seq = it->first;
        const int committed = it->second.first;
        const auto d = seq_done.find(seq);
        const int finished = d != seq_done.end() ? d->second : 0;
        const bool closed = wait || !seq_valid || seq < cur_seq;
        if (!closed || committed == 0 || finished < committed) {
            ++it;
            continue;
        }
        const double total = it->second.second;
        gpu_times.total_ms += total;
        gpu_times.max_ms = std::max(gpu_times.max_ms, total);
        ++gpu_times.frames;
        if (seq < 1000000) {
            if (per_frame.size() <= seq) {
                GpuFrameTimes none;
                none.fill(-1.0f);
                per_frame.resize(size_t(seq) + 1, none);
            }
            GpuFrameTimes tt{};
            tt[0] = float(total);
            per_frame[size_t(seq)] = tt;
        }
        if (d != seq_done.end()) seq_done.erase(d);
        it = seq_pending.erase(it);
    }
}

// ---------------------------------------------------------------- a fachada (render/renderer.h)

Renderer::Renderer(void* window, int width, int height, bool vsync, bool offscreen, const AdapterInfo* adapter)
    : impl_(std::make_unique<Impl>(window, width, height, vsync, offscreen, adapter)) {}
Renderer::~Renderer() = default;

void Renderer::resize(int width, int height) { impl_->resize(width, height); }
void Renderer::reset_resources() {
    impl_->meshes.clear();
    impl_->textures.clear();
    impl_->hd_textures.clear();
}
void Renderer::set_texture(int slot, const Texture& texture) { impl_->set_texture(slot, texture); }
void Renderer::update_texture(int slot, const Texture& texture) { impl_->update_texture(slot, texture); }
bool Renderer::set_texture_dds(int slot, const std::wstring& path) { return impl_->set_texture_dds(slot, path); }
void Renderer::set_ui_image(uint32_t key, const uint8_t* rgba, int width, int height) {
    impl_->set_ui_image(key, rgba, width, height);
}
GpuImage* Renderer::ui_image(uint32_t key) const {
    for (const auto& [k, v] : impl_->ui_images) {
        if (k == key) return image_of(v.get());
    }
    return nullptr;
}
size_t Renderer::ui_images() const { return impl_->ui_images.size(); }
void Renderer::use_hd(bool on) { impl_->use_hd = on; }
bool Renderer::using_hd() const { return impl_->use_hd; }
int Renderer::add_mesh(const EntityMesh& mesh) { return impl_->add_mesh(mesh); }
void Renderer::render(const std::vector<DrawItem>& items, const FrameParams& frame) { impl_->render(items, frame); }
void Renderer::render_2d(const std::vector<DrawItem>& items, const FrameParams& frame) { impl_->render_2d(items, frame); }
void Renderer::present() { impl_->present(); }
void Renderer::save_png(const std::wstring& path) { impl_->save_png(path); }
void Renderer::screen_blur(float keep) { impl_->screen_blur(keep); }
bool Renderer::screen_blur_active() const { return impl_->history_valid; }
void Renderer::clear_target(const float rgba[4]) {
    impl_->begin_pass(impl_->target.get(), nullptr, VK_ATTACHMENT_LOAD_OP_CLEAR, rgba, false);
}
void Renderer::save_loading_snapshot() {
    Impl& m = *impl_;
    if (m.snapshot == nullptr || m.snapshot->w != m.target->w || m.snapshot->h != m.target->h) {
        m.snapshot = m.make_image(VK_FORMAT_B8G8R8A8_UNORM, m.target->w, m.target->h, false, VK_SAMPLE_COUNT_1_BIT,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (m.snapshot == nullptr) return;
    }
    m.copy_texture(m.target.get(), m.snapshot.get());
}
bool Renderer::restore_loading_snapshot() {
    Impl& m = *impl_;
    if (m.snapshot == nullptr) return false;
    if (m.snapshot->w != m.target->w || m.snapshot->h != m.target->h) return false;  // a janela mudou
    m.copy_texture(m.snapshot.get(), m.target.get());
    return true;
}
void Renderer::release_loading_snapshot() { impl_->snapshot = nullptr; }
GpuImageRef Renderer::keep_texture(int slot) const {
    const Impl& m = *impl_;
    if (slot < 0) return nullptr;
    std::shared_ptr<GImage> t;
    if (m.use_hd && slot < int(m.hd_textures.size()) && m.hd_textures[size_t(slot)] != nullptr) {
        t = m.hd_textures[size_t(slot)];
    } else if (slot < int(m.textures.size())) {
        t = m.textures[size_t(slot)];
    }
    if (t == nullptr) return nullptr;
    // A referência própria: uma cópia do shared_ptr da imagem no apagador, solta quando a última cópia sai.
    return GpuImageRef(image_of(t.get()), [t](GpuImage*) mutable { t.reset(); });
}
bool Renderer::offscreen() const { return impl_->offscreen_; }

void Renderer::set_ui_hd(const UiHdConfig* config, const std::wstring& dir) {
    Impl& m = *impl_;
    m.hd_assets.clear();
    m.hd_failed.clear();
    if (config == nullptr) {
        m.hd_on = false;
        m.hd_config = nullptr;
        return;
    }
    m.ensure_canvas();
    m.hd_config = config;
    m.hd_on = m.canvas != nullptr;
    if (m.canvas != nullptr) m.canvas->configure_hd(config, dir);
}
std::string Renderer::ui_hd_describe() const {
    return impl_->hd_on && impl_->canvas != nullptr ? impl_->canvas->hd_describe() : "desligada";
}
float Renderer::ui_hd_measure(const std::wstring& label, float cap_px) const {
    return impl_->hd_on && impl_->canvas != nullptr ? impl_->canvas->hd_measure(label, cap_px) : 0.0f;
}
bool Renderer::ui_hd_ok() const { return impl_->hd_on && impl_->canvas != nullptr && impl_->canvas->ok(); }

bool Renderer::set_modern_ui(const std::wstring& hd_ui_dir) {
    Impl& m = *impl_;
    m.ensure_canvas();
    if (m.canvas == nullptr) return false;
    m.canvas->configure_modern(hd_ui_dir);
    return m.canvas->ok();
}
bool Renderer::modern_ok() const { return impl_->canvas != nullptr && impl_->canvas->ok(); }
float Renderer::modern_measure(const std::wstring& text, const ui::TextStyle& style) const {
    return impl_->canvas != nullptr ? impl_->canvas->measure(text, style) : ui::estimate_width(text, style);
}
void Renderer::blur_background(float amount) { impl_->blur_background(amount); }
void Renderer::draw_modern(const ui::DrawList& list, const ui::Canvas& canvas) {
    Impl& m = *impl_;
    if (m.canvas == nullptr || !m.canvas->ok()) return;
    UiRaster r;
    m.canvas->draw_modern(list, canvas, m.width_, m.height_, r);
    m.composite(r);
}
std::string Renderer::modern_describe() const {
    return impl_->canvas != nullptr ? impl_->canvas->modern_describe() : "desligado (sem o desenho vetorial)";
}
bool Renderer::read_texture(int slot, int& width, int& height, std::vector<uint8_t>& rgba) {
    return impl_->read_texture(slot, width, height, rgba);
}
bool Renderer::write_png(const std::wstring& path, int width, int height, const uint8_t* rgba) {
    return write_png_rgba(path, width, height, rgba);
}

int Renderer::width() const { return impl_->width_; }
int Renderer::height() const { return impl_->height_; }

void Renderer::set_movie_frame(const uint8_t* bgra, int width, int height, uint64_t serial) {
    impl_->set_movie_frame(bgra, width, height, serial);
}
void Renderer::set_movie_background(bool on, const float rect[4]) {
    impl_->movie_on = on && impl_->movie_tex != nullptr;
    std::memcpy(impl_->movie_rect, rect, sizeof(impl_->movie_rect));
}
GpuImage* Renderer::movie_image() const { return impl_->movie_tex != nullptr ? image_of(impl_->movie_tex.get()) : nullptr; }

const AdapterInfo& Renderer::adapter() const { return impl_->adapter_; }
bool Renderer::adapter_chosen() const { return impl_->adapter_chosen_; }
void Renderer::set_profiling(bool on) {
    impl_->profiling = on;
    if (!on) impl_->seq_valid = false;
}
bool Renderer::profiling() const { return impl_->profiling; }
void Renderer::gpu_frame_begin() {
    Impl& m = *impl_;
    if (!m.profiling) return;
    m.cur_seq = m.gpu_seq++;
    m.seq_valid = true;
}
void Renderer::gpu_frame_end() {
    if (!impl_->profiling) return;
    impl_->gpu_collect(false);
}
void Renderer::gpu_collect(bool wait) { impl_->gpu_collect(wait); }
const GpuTimes& Renderer::gpu_times() const { return impl_->gpu_times; }
void Renderer::reset_gpu_times() {
    Impl& m = *impl_;
    m.gpu_times = GpuTimes{};
    m.gpu_seq = 0;
    m.cur_seq = 0;
    m.seq_valid = false;
    m.per_frame.clear();
    m.seq_pending.clear();
    m.seq_done.clear();
    ++m.epoch;  // os command buffers ainda na GPU são de antes: os tempos deles não contam
}
const std::vector<Renderer::GpuFrameTimes>& Renderer::gpu_per_frame() const { return impl_->per_frame; }
const RenderCounters& Renderer::counters() const { return impl_->counters_; }
void Renderer::set_alpha_to_coverage(bool on) { impl_->alpha_coverage = on; }
bool Renderer::alpha_to_coverage() const { return impl_->alpha_coverage; }
uint32_t Renderer::alpha_coverage_parts() const { return impl_->a2c_parts_; }
uint32_t Renderer::env_parts() const { return impl_->env_parts_; }
void Renderer::reset_counters() { impl_->counters_ = RenderCounters{}; }
void Renderer::set_offscreen_throttle(bool on) { (void)on; }  // o Vulkan sempre segura em três quadros

void Renderer::set_vsync(bool on) {
    Impl& m = *impl_;
    if (on == m.vsync_) return;
    m.vsync_ = on;
    m.sc_dirty = true;  // o modo de apresentação é da cadeia de troca
}
bool Renderer::vsync() const { return impl_->vsync_; }
bool Renderer::set_scene_quality(int scale_percent, int msaa) { return impl_->set_scene_quality(scale_percent, msaa); }
int Renderer::scene_scale() const { return impl_->scene_scale; }
int Renderer::scene_msaa() const { return impl_->scene_msaa; }
int Renderer::scene_width() const { return impl_->scene_w; }
int Renderer::scene_height() const { return impl_->scene_h; }
std::vector<int> Renderer::supported_msaa() const {
    std::vector<int> out;
    for (int n : {2, 4, 8}) {
        if (impl_->msaa_counts & VkSampleCountFlags(n)) out.push_back(n);
    }
    return out;
}
void Renderer::set_texture_filter(TextureFilter f) { impl_->set_texture_filter(f); }
TextureFilter Renderer::texture_filter() const { return impl_->filter_; }
bool Renderer::tearing() const { return false; }
// A tela cheia exclusiva do DXGI (o modo do monitor trocado) não existe aqui: o modo Exclusive das opções é a janela sem
// bordas do tamanho da tela, que a Window já abriu. Aceita, sem fazer nada (como o Metal).
bool Renderer::set_exclusive_fullscreen(bool on, int, int) {
    impl_->exclusive_ = on && !impl_->offscreen_;
    return on == impl_->exclusive_;
}
bool Renderer::exclusive_fullscreen() const { return impl_->exclusive_; }
std::vector<DisplayMode> Renderer::display_modes() const { return impl_->modes; }
DisplayMode Renderer::desktop_mode() const { return impl_->desktop; }

}  // namespace nf

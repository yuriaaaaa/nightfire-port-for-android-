// A saída de som do SDL2 (platform/audio_device.h), no Android (o AAudio ou o OpenSL ES, pelo SDL) e no Linux: 48 kHz,
// s16, estéreo, as mesmas regras do XAudio2 e do Core Audio (audio_device_macos.cpp): um anel de 16 pedaços que a thread
// do jogo enche e a função de chamada do SDL esvazia sem travas; played_frames conta o que o aparelho consumiu (o
// relógio dos filmes, platform/psi_movie.h). Sem aparelho, nulo (o som fica mudo).

#include "platform/audio_device.h"

#include <SDL.h>

#include <atomic>
#include <cstring>
#include <vector>

namespace nf {

namespace {

constexpr int kChunks = 16;  // pedaços no anel (audio_out.cpp não deixa passar de 8 na fila)

class SdlAudioDevice final : public AudioDevice {
public:
    ~SdlAudioDevice() override {
        if (device_ != 0) SDL_CloseAudioDevice(device_);
    }

    bool open() {
        if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
        SDL_AudioSpec want{};
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 1024;
        want.callback = &SdlAudioDevice::render;
        want.userdata = this;
        SDL_AudioSpec got{};
        device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
        return device_ != 0;
    }

    int queued_buffers() override {
        return int(head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire));
    }

    bool submit(const int16_t* pcm, int frames) override {
        if (frames <= 0) return true;
        const uint64_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) >= uint64_t(kChunks)) return false;  // o anel cheio
        Chunk& c = ring_[head % kChunks];
        c.pcm.assign(pcm, pcm + size_t(frames) * 2);
        c.frames = frames;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    void start() override {
        if (device_ != 0 && !started_) {
            SDL_PauseAudioDevice(device_, 0);
            started_ = true;
        }
    }

    uint64_t played_frames() override { return played_.load(std::memory_order_acquire); }

private:
    struct Chunk {
        std::vector<int16_t> pcm;
        int frames = 0;
    };

    static void render(void* ref, Uint8* stream, int len) {
        auto* self = static_cast<SdlAudioDevice*>(ref);
        auto* out = reinterpret_cast<int16_t*>(stream);
        const int want = len / 4;
        int done = 0;
        uint64_t tail = self->tail_.load(std::memory_order_relaxed);
        while (done < want) {
            if (tail == self->head_.load(std::memory_order_acquire)) break;
            const Chunk& c = self->ring_[tail % kChunks];
            const int left = c.frames - self->offset_;
            const int take = left < want - done ? left : want - done;
            std::memcpy(out + size_t(done) * 2, c.pcm.data() + size_t(self->offset_) * 2, size_t(take) * 4);
            done += take;
            self->offset_ += take;
            if (self->offset_ >= c.frames) {
                self->offset_ = 0;
                ++tail;
                self->tail_.store(tail, std::memory_order_release);
            }
        }
        if (done < want) std::memset(out + size_t(done) * 2, 0, size_t(want - done) * 4);
        self->played_.fetch_add(uint64_t(done), std::memory_order_acq_rel);
    }

    SDL_AudioDeviceID device_ = 0;
    bool started_ = false;
    Chunk ring_[kChunks];
    std::atomic<uint64_t> head_{0};  // o próximo pedaço a escrever (a thread do jogo)
    std::atomic<uint64_t> tail_{0};  // o pedaço tocando (a thread de som do SDL)
    int offset_ = 0;                 // os frames já tocados do pedaço da cauda (só a thread de som)
    std::atomic<uint64_t> played_{0};
};

}  // namespace

std::unique_ptr<AudioDevice> open_audio_device() {
    auto d = std::make_unique<SdlAudioDevice>();
    if (!d->open()) return nullptr;
    return d;
}

}  // namespace nf

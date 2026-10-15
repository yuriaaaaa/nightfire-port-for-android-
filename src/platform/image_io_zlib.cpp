// As imagens em arquivo com a zlib (platform/image_io.h), para o Android e o Linux: o PNG escrito à mão (IHDR, um IDAT
// comprimido pela zlib com o filtro 0 em cada linha, IEND) e lido para os PNG comuns (8 bits por canal, cinza, cinza com
// alfa, RGB, RGBA ou paleta, sem entrelaçamento: os que as ferramentas gravam). O resto devolve false, como um formato
// que o WIC não conhece.

#include "platform/image_io.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "platform/os.h"

namespace nf {

namespace {

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

void chunk(std::vector<uint8_t>& out, const char* type, const uint8_t* data, size_t size) {
    put32(out, uint32_t(size));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    if (size != 0) out.insert(out.end(), data, data + size);
    put32(out, uint32_t(crc32(0, out.data() + start, uInt(out.size() - start))));
}

// Grava RGBA (bgr = true: os bytes vêm em BGRA). Devolve o passo que falhou, ou vazio.
std::string write_png(const std::wstring& path, int width, int height, const uint8_t* px, bool bgr) {
    if (width <= 0 || height <= 0 || px == nullptr) return "tamanho";
    const size_t row = size_t(width) * 4;
    std::vector<uint8_t> raw;
    raw.reserve((row + 1) * size_t(height));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        const uint8_t* s = px + row * size_t(y);
        if (!bgr) {
            raw.insert(raw.end(), s, s + row);
        } else {
            for (int x = 0; x < width; ++x) {
                raw.push_back(s[x * 4 + 2]);
                raw.push_back(s[x * 4 + 1]);
                raw.push_back(s[x * 4 + 0]);
                raw.push_back(s[x * 4 + 3]);
            }
        }
    }
    uLongf packed_size = compressBound(uLong(raw.size()));
    std::vector<uint8_t> packed(packed_size);
    if (compress2(packed.data(), &packed_size, raw.data(), uLong(raw.size()), 6) != Z_OK) return "compress2";
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<uint8_t> ihdr;
    put32(ihdr, uint32_t(width));
    put32(ihdr, uint32_t(height));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(out, "IHDR", ihdr.data(), ihdr.size());
    chunk(out, "IDAT", packed.data(), packed_size);
    chunk(out, "IEND", nullptr, 0);
    FILE* f = std::fopen(os::to_utf8(path).c_str(), "wb");
    if (f == nullptr) return "fopen";
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    return std::fclose(f) == 0 && ok ? std::string() : std::string("fwrite");
}

uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = p > a ? p - a : a - p;
    const int pb = p > b ? p - b : b - p;
    const int pc = p > c ? p - c : c - p;
    return pa <= pb && pa <= pc ? a : (pb <= pc ? b : c);
}

}  // namespace

bool write_png_rgba(const std::wstring& path, int width, int height, const uint8_t* rgba) {
    return write_png(path, width, height, rgba, false).empty();
}

void write_png_bgra(const std::wstring& path, int width, int height, const uint8_t* bgra) {
    const std::string failed = write_png(path, width, height, bgra, true);
    if (!failed.empty()) throw std::runtime_error("PNG: " + failed + " falhou");
}

bool read_image_bgra(const std::wstring& path, int max_side, int* width, int* height, std::vector<uint8_t>* bgra) {
    FILE* f = std::fopen(os::to_utf8(path).c_str(), "rb");
    if (f == nullptr) return false;
    std::vector<uint8_t> file;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) file.insert(file.end(), buf, buf + n);
    std::fclose(f);
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (file.size() < 8 || std::memcmp(file.data(), sig, 8) != 0) return false;
    uint32_t w = 0, h = 0;
    int depth = 0, color = 0, interlace = 0;
    std::vector<uint8_t> idat, palette, trns;
    for (size_t p = 8; p + 12 <= file.size();) {
        const uint32_t len = get32(&file[p]);
        if (p + 12 + len > file.size()) return false;
        const uint8_t* type = &file[p + 4];
        const uint8_t* data = &file[p + 8];
        if (std::memcmp(type, "IHDR", 4) == 0 && len >= 13) {
            w = get32(data);
            h = get32(data + 4);
            depth = data[8];
            color = data[9];
            interlace = data[12];
        } else if (std::memcmp(type, "PLTE", 4) == 0) {
            palette.assign(data, data + len);
        } else if (std::memcmp(type, "tRNS", 4) == 0) {
            trns.assign(data, data + len);
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idat.insert(idat.end(), data, data + len);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        p += 12 + len;
    }
    int channels = 0;
    switch (color) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: return false;
    }
    if (depth != 8 || interlace != 0 || w == 0 || h == 0) return false;
    if (int(w) > max_side || int(h) > max_side) return false;
    const size_t stride = size_t(w) * size_t(channels);
    std::vector<uint8_t> raw((stride + 1) * h);
    uLongf raw_size = uLongf(raw.size());
    if (uncompress(raw.data(), &raw_size, idat.data(), uLong(idat.size())) != Z_OK || raw_size != raw.size()) return false;
    std::vector<uint8_t> img(stride * h);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t filter = raw[y * (stride + 1)];
        const uint8_t* src = &raw[y * (stride + 1) + 1];
        uint8_t* dst = &img[y * stride];
        const uint8_t* up = y > 0 ? &img[(y - 1) * stride] : nullptr;
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= size_t(channels) ? dst[x - size_t(channels)] : 0;
            const int b = up != nullptr ? up[x] : 0;
            const int c = up != nullptr && x >= size_t(channels) ? up[x - size_t(channels)] : 0;
            int v = src[x];
            switch (filter) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += paeth(a, b, c); break;
                default: return false;
            }
            dst[x] = uint8_t(v);
        }
    }
    bgra->resize(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        uint8_t r, g, b, a = 255;
        const uint8_t* s = &img[i * size_t(channels)];
        switch (color) {
            case 0: r = g = b = s[0]; break;
            case 4: r = g = b = s[0]; a = s[1]; break;
            case 2: r = s[0]; g = s[1]; b = s[2]; break;
            case 6: r = s[0]; g = s[1]; b = s[2]; a = s[3]; break;
            default: {
                const size_t k = size_t(s[0]);
                if (k * 3 + 2 >= palette.size()) return false;
                r = palette[k * 3];
                g = palette[k * 3 + 1];
                b = palette[k * 3 + 2];
                if (k < trns.size()) a = trns[k];
            }
        }
        (*bgra)[i * 4 + 0] = b;
        (*bgra)[i * 4 + 1] = g;
        (*bgra)[i * 4 + 2] = r;
        (*bgra)[i * 4 + 3] = a;
    }
    *width = int(w);
    *height = int(h);
    return true;
}

}  // namespace nf

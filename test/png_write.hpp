// png_write.hpp - writes small test PNGs with no zlib: the image data in stored (uncompressed) deflate
// blocks, so a file's size is about w * h * channels. Used by test_build (decoder checks, nui 1.2.6)
// and test_render (a picture over 1 MiB, read on the decode thread). Header-only.
#ifndef NUI_PNG_WRITE_HPP
#define NUI_PNG_WRITE_HPP

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace pngw {

inline std::uint32_t crc32(unsigned char const* p, std::size_t n, std::uint32_t c = 0) {
    static std::uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t k = i;
            for (int j = 0; j < 8; ++j) k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            table[i] = k;
        }
        ready = true;
    }
    c = ~c;
    for (std::size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

inline void be32(std::vector<unsigned char>& o, std::uint32_t v) {
    o.push_back((unsigned char)(v >> 24));
    o.push_back((unsigned char)(v >> 16));
    o.push_back((unsigned char)(v >> 8));
    o.push_back((unsigned char)v);
}

inline void chunk(std::vector<unsigned char>& o, char const* kind, std::vector<unsigned char> const& data) {
    be32(o, std::uint32_t(data.size()));
    std::size_t const at = o.size();
    o.insert(o.end(), kind, kind + 4);
    o.insert(o.end(), data.begin(), data.end());
    be32(o, crc32(&o[at], o.size() - at));
}

// channels 3 (RGB) or 4 (RGBA); px(x, y) -> 0xAARRGGBB
template <typename Fn>
std::vector<unsigned char> encode(int w, int h, int channels, Fn&& px) {
    std::vector<unsigned char> raw;
    raw.reserve(std::size_t(h) * (std::size_t(w) * channels + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);                 // filter: none
        for (int x = 0; x < w; ++x) {
            std::uint32_t const c = px(x, y);
            raw.push_back((unsigned char)(c >> 16));
            raw.push_back((unsigned char)(c >> 8));
            raw.push_back((unsigned char)c);
            if (channels == 4) raw.push_back((unsigned char)(c >> 24));
        }
    }
    std::vector<unsigned char> z {0x78, 0x01};
    std::size_t done = 0;
    do {
        std::size_t const n = raw.size() - done < 65535 ? raw.size() - done : 65535;
        z.push_back(done + n == raw.size() ? 1 : 0);
        z.push_back((unsigned char)n);
        z.push_back((unsigned char)(n >> 8));
        z.push_back((unsigned char)~n);
        z.push_back((unsigned char)(~n >> 8));
        z.insert(z.end(), raw.begin() + std::ptrdiff_t(done), raw.begin() + std::ptrdiff_t(done + n));
        done += n;
    } while (done < raw.size());
    std::uint32_t a = 1, b = 0;
    for (unsigned char c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    be32(z, (b << 16) | a);
    std::vector<unsigned char> out {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<unsigned char> ihdr;
    be32(ihdr, std::uint32_t(w));
    be32(ihdr, std::uint32_t(h));
    ihdr.push_back(8);
    ihdr.push_back(channels == 4 ? 6 : 2);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return out;
}

inline bool save(std::string const& path, std::vector<unsigned char> const& bytes) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool const ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok;
}

}  // namespace pngw

#endif

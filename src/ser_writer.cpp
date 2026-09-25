// ser_writer.cpp
//
// SerWriter implementation (see ser_writer.h for the format rationale).

#include "ser_writer.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

bool SerWriter::open(const std::string& path, int w, int h, int bitDepth,
                     const std::string& instrument, int colorId)
{
    close();
    w_ = w; h_ = h; bitDepth_ = bitDepth;
    colorId_ = colorId;
    planes_ = serPlanesForColorId(colorId);
    bytePerPx_ = (bitDepth_ <= 8) ? 1 : 2;   // 8-bit -> 1 B/px, 10/14 -> 2 B/px
    shift_ = 16 - bitDepth;                  // 14 -> 2, 10 -> 6, 8 -> 8
    frameCount_ = 0;
    const size_t samples = (size_t)w * h * planes_;
    frameBuf_.assign(samples, 0);
    frameBuf8_.assign(samples, 0);

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "ser: cannot open %s\n", path.c_str()); return false; }
    file_ = f;
    // Large buffer so high-fps writes don't syscall per frame. An RGB .ser is
    // 3x the bytes of a mono one, so this is the one place a bigger buffer
    // would pay off — 4 MiB is still enough to cover several small-ROI frames.
    static char ioBuf[1u << 22];            // 4 MiB
    std::setvbuf(f, ioBuf, _IOFBF, sizeof(ioBuf));

    std::vector<uint8_t> hdr(178, 0);
    auto put32 = [&](size_t off, uint32_t v) {
        hdr[off] = v & 0xff; hdr[off+1] = (v>>8)&0xff; hdr[off+2] = (v>>16)&0xff; hdr[off+3] = (v>>24)&0xff;
    };
    auto putStr = [&](size_t off, const std::string& s, size_t len) {
        size_t n = std::min(s.size(), len);
        std::memcpy(hdr.data() + off, s.data(), n);    // rest stays 0
    };
    std::memcpy(hdr.data(), "LUCAM-RECORDER", 14);      // 1_FileID (exactly 14 bytes)
    put32(14, 0);                            // 2_LuID (unused)
    put32(18, (uint32_t)colorId_);           // 3_ColorID: MONO, or SER_RGB (100)
                                             // for a colour camera — 3 channels
                                             // interleaved per pixel, R first
    put32(22, 0);                            // 4_LittleEndian: DATA ARE LITTLE-ENDIAN,
                                             // so the field is 0. NOTE: the Wilkens/Hahn
                                             // spec text says 1=LE, but the de-facto
                                             // convention (first SER programs, Siril,
                                             // GoQat, FireCapture/PIPP/AutoStakkert — see
                                             // the comment in siril src/io/ser.h) treats
                                             // the field as a BigEndian flag: 0 = LE data,
                                             // 1 = BE data. Siril's reader byte-swaps a
                                             // 16-bit file with field=1; we must write 0.
    put32(26, (uint32_t)w);                  // 5_ImageWidth
    put32(30, (uint32_t)h);                  // 6_ImageHeight
    put32(34, (bitDepth_ <= 8) ? (uint32_t)bitDepth_ : 16);  // 7_PixelDepthPerPlane:
                                             // 16 (container) for 2-byte output — see class comment
    put32(38, 0);                            // 8_FrameCount (patched on close)
    putStr(42, "camera_app", 40);            // 9_Observer
    putStr(82, instrument, 40);              // 10_Instrument (the SDK camera name)
    putStr(122, "", 40);                     // 11_Telescope
    // 12_DateTime / 13_DateTime_UTC left 0 => no timestamp trailer.

    if (std::fwrite(hdr.data(), 1, hdr.size(), file_) != hdr.size())
    {
        std::fprintf(stderr, "ser: header write failed\n");
        std::fclose(file_); file_ = nullptr;
        return false;
    }
    return true;
}

bool SerWriter::push(const uint16_t* raw16)
{
    if (!file_) return false;
    if (planes_ != 1)
    {
        std::fprintf(stderr, "ser: push() on a %d-channel (colour) file - use pushRgb()\n", planes_);
        return false;
    }
    const size_t n = (size_t)w_ * h_;
    size_t bytes = 0;
    if (bytePerPx_ == 1)
    {
        for (size_t i = 0; i < n; ++i)
            frameBuf8_[i] = (uint8_t)(raw16[i] >> shift_);
        bytes = std::fwrite(frameBuf8_.data(), 1, n, file_);
    }
    else
    {
        for (size_t i = 0; i < n; ++i)
            frameBuf_[i] = (uint16_t)((raw16[i] >> shift_) << shift_);
        bytes = std::fwrite(frameBuf_.data(), sizeof(uint16_t), n, file_);
    }
    if (bytes != n)
    {
        std::fprintf(stderr, "ser: frame write failed (disk?); %zu frames written\n", frameCount_);
        return false;
    }
    frameCount_++;
    return true;
}

bool SerWriter::push8(const uint8_t* raw8)
{
    if (!file_ || bytePerPx_ != 1) return false;
    if (planes_ != 1)
    {
        std::fprintf(stderr, "ser: push8() on a %d-channel (colour) file - use pushRgb8()\n", planes_);
        return false;
    }
    const size_t n = (size_t)w_ * h_;
    // The RAW8 (10-bit HSM) readout already delivers 8-bit values — copy
    // straight through, no shift.
    std::memcpy(frameBuf8_.data(), raw8, n);
    const size_t bytes = std::fwrite(frameBuf8_.data(), 1, n, file_);
    if (bytes != n)
    {
        std::fprintf(stderr, "ser: frame write failed (disk?); %zu frames written\n", frameCount_);
        return false;
    }
    frameCount_++;
    return true;
}

bool SerWriter::pushRgb(const uint16_t* rgb)
{
    if (!file_) return false;
    if (planes_ != 3)
    {
        std::fprintf(stderr, "ser: pushRgb() on a mono file\n");
        return false;
    }
    const size_t n = (size_t)w_ * h_ * 3;    // per-pixel interleaved RGBRGB..., R first
    size_t bytes = 0;
    if (bytePerPx_ == 1)
    {
        for (size_t i = 0; i < n; ++i)
            frameBuf8_[i] = (uint8_t)(rgb[i] >> shift_);
        bytes = std::fwrite(frameBuf8_.data(), 1, n, file_);
    }
    else
    {
        // 14-bit: store every channel sample's word as-is (already
        // left-justified in the 16-bit container); the header declares the
        // container width (16).
        for (size_t i = 0; i < n; ++i)
            frameBuf_[i] = (uint16_t)((rgb[i] >> shift_) << shift_);
        bytes = std::fwrite(frameBuf_.data(), sizeof(uint16_t), n, file_);
    }
    if (bytes != n)
    {
        std::fprintf(stderr, "ser: colour frame write failed (disk?); %zu frames written\n", frameCount_);
        return false;
    }
    frameCount_++;
    return true;
}

bool SerWriter::pushRgb8(const uint8_t* rgb)
{
    if (!file_ || bytePerPx_ != 1) return false;
    if (planes_ != 3)
    {
        std::fprintf(stderr, "ser: pushRgb8() on a mono file\n");
        return false;
    }
    const size_t n = (size_t)w_ * h_ * 3;
    std::memcpy(frameBuf8_.data(), rgb, n);
    const size_t bytes = std::fwrite(frameBuf8_.data(), 1, n, file_);
    if (bytes != n)
    {
        std::fprintf(stderr, "ser: colour frame write failed (disk?); %zu frames written\n", frameCount_);
        return false;
    }
    frameCount_++;
    return true;
}

void SerWriter::close()
{
    if (!file_) return;
    // Patch the frame count into the header (offset 38).
    uint32_t cnt = (uint32_t)frameCount_;
    std::fseek(file_, 38, SEEK_SET);
    std::fwrite(&cnt, 1, 4, file_);
    std::fflush(file_);
    std::fclose(file_);
    file_ = nullptr;
    if (frameCount_ > 0)
        std::fprintf(stderr, "[ser] wrote %u frames (%d channel%s)\n", (unsigned)frameCount_,
                     planes_, planes_ > 1 ? "s" : "");
}

bool validateSerFile(const std::string& path, int expW, int expH, int expDepth, int expColorId)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "[ser] INVALID (cannot open): %s\n", path.c_str()); return false; }
    uint8_t hdr[178];
    size_t n = std::fread(hdr, 1, 178, f);
    if (n != 178) { std::fclose(f); std::fprintf(stderr, "[ser] INVALID (short header): %s\n", path.c_str()); return false; }
    auto u32 = [&](size_t off) {
        return (uint32_t)hdr[off] | ((uint32_t)hdr[off+1]<<8) |
               ((uint32_t)hdr[off+2]<<16) | ((uint32_t)hdr[off+3]<<24);
    };
    const uint32_t count = u32(38);
    const int colorId = (int)u32(18);
    const int planes = serPlanesForColorId(colorId);
    bool ok = std::memcmp(hdr, "LUCAM-RECORDER", 14) == 0
           && colorId == expColorId          // MONO (0) or SER_RGB (100)
           && u32(22) == 0                   // LittleEndian field = 0 for LE data
                                              // (de-facto convention inverts the
                                              // spec text's flag — see SerWriter)
           && u32(26) == (uint32_t)expW
           && u32(30) == (uint32_t)expH
           && u32(34) == (uint32_t)expDepth
           && count > 0;
    if (ok)
    {
        const int bpp = (expDepth <= 8) ? 1 : 2;      // 8-bit -> 1 B/px, 9..16 -> 2 B/px
        std::fseek(f, 0, SEEK_END);
        long fileSize = std::ftell(f);
        long expected = 178 + (long)count * planes * expW * expH * bpp;
        if (fileSize != expected)
        { std::fprintf(stderr, "[ser] INVALID (size %ld != %ld): %s\n", (long)fileSize, expected, path.c_str()); ok = false; }
    }
    std::fclose(f);
    if (ok)
        std::fprintf(stderr, "[ser] OK: %s %dx%d %d-bit %d channel%s %u frames\n",
                     path.c_str(), expW, expH, expDepth, planes, planes > 1 ? "s" : "",
                     (unsigned)count);
    return ok;
}

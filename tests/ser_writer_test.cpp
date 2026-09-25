// ser_writer_test.cpp
//
// Headless SerWriter self-test (run via ./camera_app --sertest): writes and
// validates the 8/14-bit .ser ramp samples in samples/. No camera needed.

#include "ser_writer.h"
#include "test_suites.h"

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <vector>

static int serRampValue(int bitDepth, int x, int w)
{
    const int maxv = (1 << bitDepth) - 1;            // 255 / 1023 / 16383
    return (int)((long long)maxv * x / (w - 1));     // full range on each row
}

bool runSerWriterSelfTest()
{
    const int W = 512, H = 512, FRAMES = 5;
    // Self-contained: create the output dir if `build_win.bat clean` (or
    // anything else) removed it, so the test never fails just because the
    // artifacts are gone.
    std::error_code ec;
    std::filesystem::create_directories("samples", ec);
    struct Case { int depth; const char* file; };
    const Case cases[] = {
        { 8,  "samples/sample08.ser" },
        { 14, "samples/sample14.ser" },
    };
    bool allOk = true;
    for (const auto& c : cases)
    {
        SerWriter w;
        if (!w.open(c.file, W, H, c.depth))   // default instrument string, as the app writes
        { allOk = false; continue; }
        const int shift = 16 - c.depth;              // 8 / 2
        const size_t n = (size_t)W * H;
        std::vector<uint16_t> frame(n);
        bool framesOk = true;
        for (int i = 0; i < FRAMES; ++i)
        {
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    // left-justified 16-bit "readout" for the target depth's
                    // value (SerWriter::push re-shifts by 16-bitDepth, so this
                    // round-trips to exactly the left-justified value):
                    frame[(size_t)y * W + x] = (uint16_t)(serRampValue(c.depth, x, W) << shift);
            if (!w.push(frame.data())) framesOk = false;
        }
        w.close();
        if (!framesOk)
        { std::fprintf(stderr, "SERT FAILED: %s (push error)\n", c.file); allOk = false; continue; }

        // 8-bit: also exercise the RAW8 (10-bit HSM) path — the camera delivers
        // the 8-bit values ready-made (1 B/px) and SerWriter::push8 copies them
        // straight through (no shift).
        if (c.depth == 8)
        {
            SerWriter w8;
            const char* f8 = "samples/sample08hsm.ser";
            if (!w8.open(f8, W, H, 8)) { allOk = false; continue; }
            std::vector<uint8_t> f8buf(n);
            bool f8ok = true;
            for (int i = 0; i < FRAMES; ++i)
            {
                for (int y = 0; y < H; ++y)
                    for (int x = 0; x < W; ++x)
                        f8buf[(size_t)y * W + x] = (uint8_t)serRampValue(8, x, W);
                if (!w8.push8(f8buf.data())) f8ok = false;
            }
            w8.close();
            if (!f8ok)
            { std::fprintf(stderr, "SERT FAILED: %s (push8 error)\n", f8); allOk = false; continue; }
            FILE* g = std::fopen(f8, "rb");
            bool ok8 = validateSerFile(f8, W, H, 8);
            if (g && ok8)
            {
                std::vector<uint8_t> d8(FRAMES * n);
                ok8 = std::fseek(g, 178, SEEK_SET) == 0
                    && std::fread(d8.data(), 1, d8.size(), g) == d8.size();
                for (int i = 0; ok8 && i < FRAMES; ++i)
                    for (int y = 0; ok8 && y < H; ++y)
                        for (int x = 0; ok8 && x < W; ++x)
                            ok8 = (d8[((size_t)i * H + y) * W + x] == (uint8_t)serRampValue(8, x, W));
            }
            if (g) std::fclose(g);
            if (!ok8) std::fprintf(stderr, "SERT FAILED: %s (push8 round-trip mismatch)\n", f8);
            allOk = allOk && ok8;
        }

        // Round-trip: re-read the file; header (incl. declared depth 8 or 16),
        // exact size, and every sample == the ramp value (word >> (16-depth)
        // recovers the effective-depth value, as a stacking tool does).
        const int hdrDepth = (c.depth <= 8) ? c.depth : 16;
        bool ok = validateSerFile(c.file, W, H, hdrDepth);
        FILE* f = std::fopen(c.file, "rb");
        if (f && ok)
        {
            const size_t bpp = (c.depth <= 8) ? 1 : 2;
            std::vector<uint8_t> data((size_t)FRAMES * n * bpp);
            ok = std::fseek(f, 178, SEEK_SET) == 0
               && std::fread(data.data(), 1, data.size(), f) == data.size();
            for (int i = 0; ok && i < FRAMES; ++i)
                for (int y = 0; ok && y < H; ++y)
                    for (int x = 0; ok && x < W; ++x)
                    {
                        const size_t p = ((size_t)i * H + y) * W + x;
                        const int got = (c.depth <= 8)
                                ? data[p]
                                : ((data[2*p] | ((int)data[2*p+1] << 8)) >> (16 - c.depth));
                        ok = (got == serRampValue(c.depth, x, W));
                    }
        }
        if (f) std::fclose(f);
        if (!ok) std::fprintf(stderr, "SERT FAILED: %s (round-trip mismatch)\n", c.file);
        allOk = allOk && ok;
    }
    std::printf("SERT %s (8/14-bit .ser ramp samples in samples/)\n", allOk ? "OK" : "FAILED");
    return allOk;
}

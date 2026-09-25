// frame_view_test.cpp
//
// Headless FrameView regression suite (run via ./camera_app --frametest):
// feeds the exact stale-latestCh_ photo scenario that used to SIGSEGV in
// QImage::copy and checks it now degrades to grayscale, plus the 14-bit
// display-downscale regressions. No camera needed.

#include "frame_view.h"
#include "test_suites.h"

#include <display_frame.h>

#include <QApplication>

#include <algorithm>
#include <cstdio>
#include <vector>

bool runFrameViewSelfTest()
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static int argc = 1;
    static char prog[] = "camera_app";
    char* argv[] = { prog, nullptr };
    QApplication app(argc, argv);

    FrameView view;
    view.resize(960, 640);

    const int w = 3096, h = 2080;
    std::vector<unsigned char> gray((size_t)w * h);
    for (size_t i = 0; i < gray.size(); ++i) gray[i] = (unsigned char)(i & 0xFF);

    bool ok = true;
    auto check = [&ok](const char* what, bool cond) {
        std::printf("FRAMETEST %s: %s\n", what, cond ? "ok" : "FAIL");
        if (!cond) ok = false;
    };

    // 1) The exact crash scenario: grayscale data, but the caller says 3 channels.
    view.setFrame(gray.data(), gray.size(), w, h, 3);
    check("mismatch-degrades-to-gray", view.hasImage() && view.imageChannels() == 1);

    // 2) Normal grayscale.
    view.setFrame(gray.data(), gray.size(), w, h, 1);
    check("grayscale", view.hasImage() && view.imageChannels() == 1);

    // 3) Normal 3-channel RGB (a real 3x buffer).
    std::vector<unsigned char> rgb((size_t)w * h * 3, 0x40);
    view.setFrame(rgb.data(), rgb.size(), w, h, 3);
    check("rgb", view.hasImage() && view.imageChannels() == 3);

    // 4) Under-sized buffer: keep the previous image, never crash.
    view.setFrame(gray.data(), 16, w, h, 1);
    check("undersized-keeps-previous", view.hasImage() && view.imageChannels() == 3);

    // 5) Worker display downscale (buildDisplayFrame) — regression for the
    //    14-bit preview-blowout: the old box-average path clamped the 16-bit
    //    average at 255 without the >> 8, so ANY scene with values above 256
    //    (0.4% of full scale) rendered pure white while only true clips were
    //    marked red. 2x2 downscale: 320x240 -> 160x120.
    {
        const int W = 320, H = 240, DW = 160, DH = 120;
        std::vector<unsigned short> src16((size_t)W * H);
        std::vector<unsigned char> d8((size_t)DW * DH), cm((size_t)DW * DH);
        bool anyClip = false;
        auto run16 = [&]() {
            anyClip = false;
            buildDisplayFrame((const unsigned char*)src16.data(), W, H, 2,
                              d8.data(), cm.data(), DW, DH, anyClip);
        };
        auto allEq = [&](unsigned char v) {
            return std::all_of(d8.begin(), d8.end(), [v](unsigned char b) { return b == v; });
        };

        std::fill(src16.begin(), src16.end(), 32768);   // 50% of 65528
        run16();
        check("downscale16-midgray", !anyClip && allEq(128));

        std::fill(src16.begin(), src16.end(), 65528);   // full scale
        run16();
        check("downscale16-fullclips", anyClip && allEq(255));

        std::fill(src16.begin(), src16.end(), 200);     // near-black (200>>8 = 0)
        run16();
        check("downscale16-dark", !anyClip && allEq(0));

        // mixed: half full (clips) half mid — clips flagged, mid stays 128
        for (size_t i = 0; i < src16.size(); ++i)
            src16[i] = (i < src16.size() / 2) ? 65528 : 32768;
        run16();
        bool mixedOk = anyClip;
        for (size_t i = 0; i < d8.size(); ++i)
            if ((i < d8.size() / 2) != (cm[i] == 1)) mixedOk = false;
        check("downscale16-mixed", mixedOk);

        // small-ROI (no scaling) 16-bit: p>>8 mapping + clip at 65528
        const int SW = 64, SH = 64;
        std::vector<unsigned short> s16((size_t)SW * SH, 32768);
        s16[10] = 65528;
        std::vector<unsigned char> sd8(SW * SH), scm(SW * SH);
        anyClip = false;
        buildDisplayFrame((const unsigned char*)s16.data(), SW, SH, 2,
                          sd8.data(), scm.data(), SW, SH, anyClip);
        check("downscale16-smallroi",
              anyClip && scm[10] == 1 && sd8[10] == 255 && sd8[11] == 128);

        // 8-bit box average regression (must not be affected)
        std::vector<unsigned char> src8((size_t)W * H, 100);
        anyClip = false;
        buildDisplayFrame(src8.data(), W, H, 1, d8.data(), cm.data(), DW, DH, anyClip);
        check("downscale8-uniform", !anyClip && allEq(100));
    }

    std::printf("FRAMETEST %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

// usb_bench.cpp — raw-SDK USB throughput benchmark (no Qt, no display, no disk).
//
// Opens the camera, sets ROI + pixel format + exposure, starts the continuous
// video stream, then drains it in a TIGHT loop (ASIGetVideoData with 0 ms
// wait) for a fixed duration. This isolates the camera -> USB -> host data
// path from the app: no GUI, no thumbnail, no histogram, no .ser writer.
//
// If the delivered fps here matches what the app records (e.g. ~112 fps for
// 480x320 RAW16), the app is not the bottleneck — the USB data rate is.
// If this bench runs much faster than the app, the app-side processing is.
//
// Build:  CMake -DBUILD_PROBES=ON (or by hand: cl /O2 /EHsc /std:c++17 /utf-8
//          tests\usb_bench.cpp /Fe:usb_bench.exe -IASI_SDK
//          /link ASI_SDK\x64\ASICamera2.lib)
//
// Usage:   usb_bench.exe <width> <height> <format> <exposure_us> <duration_s>
//          [change_exposure_us] [hsm]
//          <format> is 16|8 (RAW16/RAW8) or a format NAME: raw8 | rgb24 | raw16 | y8.
//          The names matter on a COLOR camera: RAW8/RAW16 are the Bayer mosaic,
//          RGB24 is the camera-demosaiced 8-bit colour output, Y8 is luminance.
//          [change_exposure_us] optionally switches the exposure to this value
//          1 s after the measurement starts; [hsm] 1|0 forces ASI_HIGH_SPEED_MODE
//          (default: 1 for RAW8, 0 otherwise).
// Example: ./usb_bench 480 320 16 2475 3
//          ./usb_bench 3096 2080 rgb24 2000 4

#include <ASICamera2.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

int main(int argc, char** argv)
{
    int w = 480, h = 320, expUs = 2475, durS = 3;
    int changeUs = 0;
    ASI_IMG_TYPE it = ASI_IMG_RAW16;
    const char* fmtName = "RAW16";
    auto parseFmt = [](const char* s) -> ASI_IMG_TYPE {
        const std::string v = s;
        if (v == "16" || v == "raw16" || v == "RAW16") return ASI_IMG_RAW16;
        if (v == "8"  || v == "raw8"  || v == "RAW8")  return ASI_IMG_RAW8;
        if (v == "rgb24" || v == "RGB24")              return ASI_IMG_RGB24;
        if (v == "y8" || v == "Y8")                    return ASI_IMG_Y8;
        return ASI_IMG_END;
    };
    int bpp = 2;
    auto setFmt = [&](const char* s) {
        it = parseFmt(s);
        if (it == ASI_IMG_END) { std::fprintf(stderr, "bad format '%s'\n", s); std::exit(2); }
        bpp = (it == ASI_IMG_RGB24) ? 3 : (it == ASI_IMG_RAW16 ? 2 : 1);
        fmtName = (it == ASI_IMG_RGB24) ? "RGB24" : (it == ASI_IMG_RAW16 ? "RAW16"
                                 : (it == ASI_IMG_Y8 ? "Y8" : "RAW8"));
    };
    int hsm = -1;   // -1 = default (1 for RAW8, else 0)
    if (argc >= 6) { w = std::atoi(argv[1]); h = std::atoi(argv[2]); setFmt(argv[3]); expUs = std::atoi(argv[4]); durS = std::atoi(argv[5]); }
    if (argc >= 7) { changeUs = std::atoi(argv[6]); }
    if (argc >= 8) { hsm = std::atoi(argv[7]); }
    if (hsm < 0) hsm = (it == ASI_IMG_RAW8) ? 1 : 0;

    if (ASIGetNumOfConnectedCameras() <= 0) { std::fprintf(stderr, "no camera\n"); return 1; }
    ASI_CAMERA_INFO info = {};
    if (ASIGetCameraProperty(&info, 0) != ASI_SUCCESS) { std::fprintf(stderr, "prop failed\n"); return 1; }
    int id = info.CameraID;
    if (ASIOpenCamera(id) != ASI_SUCCESS || ASIInitCamera(id) != ASI_SUCCESS)
    { std::fprintf(stderr, "open/init failed\n"); return 1; }

    // The camera persists a BandWidth governor (fraction of total bandwidth
    // it may use, 40..100; persisted 60 on this unit) that throttles video
    // well below what the link can carry. Raise it to 100 so the bench
    // measures the link/camera, not the persisted setting.
    {
        ASI_ERROR_CODE be = ASISetControlValue(id, ASI_BANDWIDTHOVERLOAD, 100, ASI_FALSE);
        long brb = -1; ASI_BOOL bauto = ASI_FALSE;
        if (ASIGetControlValue(id, ASI_BANDWIDTHOVERLOAD, &brb, &bauto) == ASI_SUCCESS)
            std::fprintf(stderr, "USBBENCH bandwidth set=100 err=%d readback=%ld\n", (int)be, brb);
    }

    if (ASISetROIFormat(id, w, h, 1, it) != ASI_SUCCESS) { std::fprintf(stderr, "ROI failed\n"); return 1; }
    // HighSpeedMode: only RAW8 output honours it (the 10-bit fast readout
    // delivered 1 byte/px); RAW16 ignores it and always reads out full depth.
    {
        const ASI_ERROR_CODE he = ASISetControlValue(id, ASI_HIGH_SPEED_MODE, hsm, ASI_FALSE);
        long hrb = -1; ASI_BOOL hauto = ASI_FALSE;
        ASIGetControlValue(id, ASI_HIGH_SPEED_MODE, &hrb, &hauto);
        std::fprintf(stderr, "USBBENCH hsm set=%d err=%d readback=%ld\n", hsm, (int)he, hrb);
    }
    int aw = 0, ah = 0, ab = 0;
    ASI_IMG_TYPE ait = ASI_IMG_END;
    ASIGetROIFormat(id, &aw, &ah, &ab, &ait);
    ASISetControlValue(id, ASI_EXPOSURE, expUs, ASI_FALSE);
    long rb = 0; ASI_BOOL autoE = ASI_FALSE;
    if (ASIGetControlValue(id, ASI_EXPOSURE, &rb, &autoE) == ASI_SUCCESS)
        std::fprintf(stderr, "USBBENCH %dx%d %s exp set=%dus readback=%dus auto=%d (ROI %dx%d)\n",
                     w, h, fmtName, expUs, (int)rb, (int)autoE, aw, ah);
    std::fflush(stderr);

    std::vector<unsigned char> buf((size_t)w * h * bpp);
    if (ASIStartVideoCapture(id) != ASI_SUCCESS) { std::fprintf(stderr, "start capture failed\n"); return 1; }

    // warm-up: let the stream reach steady state
    for (int i = 0; i < 50; ++i)
        if (ASIGetVideoData(id, buf.data(), (long)buf.size(), 0) != ASI_SUCCESS)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

    auto tEnd = Clock::now() + std::chrono::seconds(durS);
    auto tWin = Clock::now();
    auto tChange = changeUs > 0 ? Clock::now() + std::chrono::seconds(1) : Clock::time_point{};
    bool changed = false;
    long frames = 0, window = 0;
    int sdkDrops0 = -1;

    while (Clock::now() < tEnd)
    {
        if (!changed && changeUs > 0 && Clock::now() >= tChange)
        {
            ASISetControlValue(id, ASI_EXPOSURE, changeUs, ASI_FALSE);
            long rb2 = 0; ASI_BOOL autoE2 = ASI_FALSE;
            ASIGetControlValue(id, ASI_EXPOSURE, &rb2, &autoE2);
            std::fprintf(stderr, "USBBENCH t=1s: exposure changed to %dus (readback %dus)\n", changeUs, (int)rb2);
            std::fflush(stderr);
            changed = true;
        }
        if (ASIGetVideoData(id, buf.data(), (long)buf.size(), 0) == ASI_SUCCESS)
        {
            ++frames; ++window;
            auto now = Clock::now();
            if (std::chrono::duration<double>(now - tWin).count() >= 1.0)
            {
                double dt = std::chrono::duration<double>(now - tWin).count();
                int sdk = -1;
                if (ASIGetDroppedFrames(id, &sdk) == ASI_SUCCESS)
                {
                    if (sdkDrops0 < 0) sdkDrops0 = sdk;
                    std::fprintf(stderr, "USBBENCH 1s: %.1f fps, %.1f MB/s, camDropped(+%d)\n",
                                 window / dt, (double)window * buf.size() / dt / (1024.0 * 1024.0),
                                 sdk - sdkDrops0);
                    std::fflush(stderr);
                    tWin = now; window = 0;
                }
            }
        }
        else
            std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    int sdk = -1;
    if (ASIGetDroppedFrames(id, &sdk) == ASI_SUCCESS)
        std::fprintf(stderr, "USBBENCH total: %ld frames in %d s (avg %.1f fps), camDropped total=%d\n",
                     frames, durS, (double)frames / durS, sdk);
    std::fflush(stderr);
    ASIStopVideoCapture(id);
    ASICloseCamera(id);
    return 0;
}

// camera_probe.cpp — standalone SDK capability dump (no Qt, no display).
//
// Enumerates every connected ASI camera and prints exactly the capability
// fields the app relies on to adapt itself to the connected model: name,
// sensor size, IsColorCam + BayerPattern, the SupportedVideoFormat list,
// BitDepth, supported binning, pixel size, and every control's caps.
//
// This is the tool to answer "what does THIS camera support?" — the app
// drives its ROI list, pixel formats and bit depths from these fields, so
// when a different model is plugged in, run this first and compare.
//
// Build: CMake -DBUILD_PROBES=ON (or by hand: cl /O2 /EHsc /std:c++17 /utf-8
// tests\camera_probe.cpp /Fe:camera_probe.exe -IASI_SDK /link
// ASI_SDK\x64\ASICamera2.lib). It has its own
// main(), so it is never linked into camera_app.
//
// Usage: camera_probe.exe            (also probes ROI sizes: --roi)
//        camera_probe.exe --roi       additionally tries the app's candidate ROI
//                                    list and prints the sizes the camera keeps

#include <ASICamera2.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static const char* imgName(ASI_IMG_TYPE t)
{
    switch (t)
    {
    case ASI_IMG_RAW8:  return "RAW8";
    case ASI_IMG_RGB24: return "RGB24";
    case ASI_IMG_RAW16: return "RAW16";
    case ASI_IMG_Y8:    return "Y8";
    default:            return "END";
    }
}

static const char* bayerName(ASI_BAYER_PATTERN p)
{
    switch (p)
    {
    case ASI_BAYER_RG: return "RGGB";
    case ASI_BAYER_BG: return "BGGR";
    case ASI_BAYER_GR: return "GRBG";
    case ASI_BAYER_GB: return "GBRG";
    }
    return "?";
}

static int imgBpp(ASI_IMG_TYPE t)
{
    switch (t)
    {
    case ASI_IMG_RAW8:  return 1;
    case ASI_IMG_RGB24: return 3;
    case ASI_IMG_RAW16: return 2;
    case ASI_IMG_Y8:    return 1;
    default:            return 0;
    }
}

int main(int argc, char** argv)
{
    const bool probeRoi = (argc > 1 && std::strcmp(argv[1], "--roi") == 0);

    const int n = ASIGetNumOfConnectedCameras();
    std::printf("connected cameras: %d\n", n);
    if (n <= 0) return 1;

    for (int i = 0; i < n; ++i)
    {
        ASI_CAMERA_INFO info = {};
        if (ASIGetCameraProperty(&info, i) != ASI_SUCCESS)
        {
            std::printf("[%d] ASIGetCameraProperty FAILED\n", i);
            continue;
        }
        std::printf("=== camera index %d ===\n", i);
        std::printf("Name              : %s\n", info.Name);
        std::printf("CameraID          : %d\n", info.CameraID);
        std::printf("MaxWidth x Height : %ld x %ld (%ld MP)\n", info.MaxWidth, info.MaxHeight,
                    (info.MaxWidth * info.MaxHeight + 499999) / 1000000);
        std::printf("IsColorCam        : %d\n", (int)info.IsColorCam);
        std::printf("BayerPattern      : %d (%s)\n", (int)info.BayerPattern, bayerName(info.BayerPattern));
        std::printf("BitDepth          : %d\n", info.BitDepth);
        std::printf("PixelSize         : %.3f um\n", info.PixelSize);
        std::printf("ElecPerADU        : %g\n", info.ElecPerADU);
        std::printf("MechanicalShutter : %d  ST4Port %d  Cooler %d  USB3host %d  USB3cam %d  Trigger %d\n",
                    (int)info.MechanicalShutter, (int)info.ST4Port, (int)info.IsCoolerCam,
                    (int)info.IsUSB3Host, (int)info.IsUSB3Camera, (int)info.IsTriggerCam);
        std::printf("SupportedBins     :");
        for (int b = 0; b < 16 && info.SupportedBins[b] != 0; ++b)
            std::printf(" bin%d", info.SupportedBins[b]);
        std::printf("\nSupportedVideoFormat:");
        for (int f = 0; f < 8 && info.SupportedVideoFormat[f] != ASI_IMG_END; ++f)
            std::printf(" %s(%dB/px)", imgName(info.SupportedVideoFormat[f]), imgBpp(info.SupportedVideoFormat[f]));
        std::printf("\n");

        if (ASIOpenCamera(info.CameraID) != ASI_SUCCESS)
        {
            std::printf("(open failed - control caps not probed; another process may hold the camera)\n");
            continue;
        }
        ASIInitCamera(info.CameraID);

        int nControls = 0;
        if (ASIGetNumOfControls(info.CameraID, &nControls) == ASI_SUCCESS)
            std::printf("NumOfControls     : %d\n", nControls);
        for (int c = 0; c < nControls; ++c)
        {
            ASI_CONTROL_CAPS cap = {};
            if (ASIGetControlCaps(info.CameraID, c, &cap) != ASI_SUCCESS)
            {
                std::printf("  [%2d] ASIGetControlCaps FAILED (index %d)\n", c, c);
                continue;
            }
            long val = 0; ASI_BOOL isAuto = ASI_FALSE;
            const ASI_ERROR_CODE ge = ASIGetControlValue(info.CameraID, cap.ControlType, &val, &isAuto);
            std::printf("  [%2d] %-22s type=%2d  min=%-12ld max=%-12ld def=%-10ld val=%-12ld %s%s\n",
                        c, cap.Name, (int)cap.ControlType, cap.MinValue, cap.MaxValue, cap.DefaultValue,
                        (ge == ASI_SUCCESS) ? val : -1,
                        cap.IsWritable ? "W" : "R", cap.IsAutoSupported ? " auto" : "");
        }

        if (probeRoi)
        {
            // Candidate sizes the app generates (see roi_plan.h): full sensor,
            // integer sensor bins, standard 16:9 / 4:3 sizes and a square crop.
            std::printf("--- ROI candidates ---\n");
            std::vector<std::pair<int,int>> cands;
            const int W = (int)info.MaxWidth, H = (int)info.MaxHeight;
            cands.push_back({W, H});
            for (int b = 2; b <= 6; ++b)
                cands.push_back({(W / b) & ~1, (H / b) & ~1});
            const int std169[][2] = {{3840,2160},{1920,1080},{1280,720},{640,360}};
            const int std43[][2]  = {{2048,1536},{1600,1200},{1280,960},{1024,768},{800,600},{640,480}};
            for (auto& s : std169) cands.push_back({s[0], s[1]});
            for (auto& s : std43)  cands.push_back({s[0], s[1]});
            cands.push_back({std::min(W, H), std::min(W, H)});
            for (int fmt = 0; fmt < 8 && info.SupportedVideoFormat[fmt] != ASI_IMG_END; ++fmt)
            {
                const ASI_IMG_TYPE it = info.SupportedVideoFormat[fmt];
                std::printf("  format %s:\n", imgName(it));
                for (auto [w, h] : cands)
                {
                    if (w < 8 || h < 8 || w > W || h > H) continue;
                    const ASI_ERROR_CODE e = ASISetROIFormat(info.CameraID, w, h, 1, it);
                    int aw = 0, ah = 0, ab = 0; ASI_IMG_TYPE ait = ASI_IMG_END;
                    if (e != ASI_SUCCESS) { std::printf("    %5dx%-5d REJECT (e=%d)\n", w, h, (int)e); continue; }
                    if (ASIGetROIFormat(info.CameraID, &aw, &ah, &ab, &ait) == ASI_SUCCESS && (aw != w || ah != h))
                        std::printf("    %5dx%-5d CLAMPED -> %dx%d bin=%d fmt=%s\n", w, h, aw, ah, ab, imgName(ait));
                    else
                        std::printf("    %5dx%-5d ok bin=%d fmt=%s\n", w, h, ab, imgName(ait));
                }
            }
        }

        // Live value readback via ASIGetControlValue (the control TYPE, which is
        // the other API — ASIGetControlCaps takes the list INDEX). This is what
        // the app does for gain/exposure/temperature, so a control the SDK only
        // reports through the caps listing shows up here as an error.
        std::printf("  ASIGetControlValue readback:\n");
        for (int idx = 0; idx < nControls; ++idx)
        {
            ASI_CONTROL_CAPS cap = {};
            if (ASIGetControlCaps(info.CameraID, idx, &cap) != ASI_SUCCESS) continue;
            long v = 0; ASI_BOOL au = ASI_FALSE;
            const ASI_ERROR_CODE e = ASIGetControlValue(info.CameraID, cap.ControlType, &v, &au);
            if (e == ASI_SUCCESS)
                std::printf("    %-24s type=%2d -> %ld (auto %d)\n", cap.Name,
                            (int)cap.ControlType, v, (int)au);
            else
                std::printf("    %-24s type=%2d -> ERROR %d\n", cap.Name,
                            (int)cap.ControlType, (int)e);
        }
        ASICloseCamera(info.CameraID);
    }
    return 0;
}

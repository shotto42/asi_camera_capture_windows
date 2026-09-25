// gst_encode_test.cpp
//
// Headless validation of the app's exact H.264->MP4 encoder pipeline
// (see gst_video_encoder.cpp) using SYNTHETIC frames — no camera needed.
// Mirrors the app's pipeline and pushing style (BGR input, explicit
// PTS/DURATION per buffer, push-driven pacing at the caps framerate):
//
//   appsrc name=src is-live=true block=false max-bytes=67108864
//          format=time caps=video/x-raw,format=BGR,width=W,height=H,framerate=F/1
//   ! videoconvert ! x264enc speed-preset=ultrafast tune=zerolatency
//          bitrate=10000 ! video/x-h264,profile=baseline ! mp4mux
//   ! filesink location=OUT
//
// Encodes a short clip of synthetic (moving gradient) BGR frames, ends the
// stream, and verifies the MP4 on disk is a real ISOBMFF container (ftyp box
// at offset 4) with plausible size. Exits 0 on success.
//
// This is a standalone probe (own main), built separately — like the other
// raw probes in tests/. It is NOT linked into camera_app.
//
//   gst_encode_test.exe [out.mp4] [seconds] [fps]

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

#include <windows.h>

namespace {

constexpr int kWidth = 640;
constexpr int kHeight = 480;

// A moving diagonal gradient so the x264 encoder has real motion to work on.
void makeFrame(std::vector<uint8_t>& bgr, int frame)
{
    const size_t n = (size_t)kWidth * kHeight;
    for (size_t i = 0; i < n; ++i)
    {
        int x = (int)(i % kWidth);
        int y = (int)(i / kWidth);
        int v = (x + y + frame * 8) % 256;
        bgr[i * 3 + 0] = (uint8_t)v;            // B
        bgr[i * 3 + 1] = (uint8_t)((v + 40) % 256);  // G
        bgr[i * 3 + 2] = (uint8_t)((v + 120) % 256); // R
    }
}

bool checkMp4(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "FAIL: cannot open %s\n", path.c_str()); return false; }
    std::vector<char> head(32);
    f.read(head.data(), 32);
    if (f.gcount() < 32) { std::fprintf(stderr, "FAIL: %s is too small\n", path.c_str()); return false; }
    // ISOBMFF: bytes 4..8 must be 'ftyp'.
    const bool ok = head[4] == 'f' && head[5] == 't' && head[6] == 'y' && head[7] == 'p';
    f.seekg(0, std::ios::end);
    const long sz = (long)f.tellg();
    std::printf("MP4 %s: %ld bytes, ftyp=%s\n", path.c_str(), sz, ok ? "yes" : "NO");
    if (sz < 1000) { std::fprintf(stderr, "FAIL: MP4 suspiciously small\n"); return false; }
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    const std::string outPath = argc > 1 ? argv[1] : "gst_encode_test.mp4";
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 2;
    const int fps = argc > 3 ? std::atoi(argv[3]) : 30;
    const int frameMs = (int)(1000 / fps);

    gst_init(nullptr, nullptr);

    // The EXACT pipeline string the app builds (GstVideoEncoder::start).
    char desc[1200];
    std::snprintf(desc, sizeof(desc),
                  "appsrc name=src is-live=true block=false max-bytes=67108864 "
                  "format=time caps=video/x-raw,format=BGR,width=%d,height=%d,framerate=%d/1 "
                  "! videoconvert ! x264enc speed-preset=ultrafast tune=zerolatency bitrate=10000 "
                  "! video/x-h264,profile=baseline ! mp4mux ! filesink location=%s",
                  kWidth, kHeight, fps, outPath.c_str());
    std::printf("pipeline: %s\n", desc);

    GError* err = nullptr;
    GstElement* pipe = gst_parse_launch(desc, &err);
    if (!pipe || err)
    {
        std::fprintf(stderr, "FAIL: pipeline parse failed: %s\n", err ? err->message : "?");
        g_clear_error(&err);
        return 1;
    }
    GstElement* src = gst_bin_get_by_name(GST_BIN(pipe), "src");
    if (!src) { std::fprintf(stderr, "FAIL: no appsrc in pipeline\n"); return 1; }
    gst_element_set_state(pipe, GST_STATE_PLAYING);

    // Push `seconds * fps` synthetic BGR frames at the cap framerate, with the
    // explicit PTS/DURATION the app sets (GstVideoEncoder::push).
    std::vector<uint8_t> frameData((size_t)kWidth * kHeight * 3);
    const int nFrames = seconds * fps;
    bool pushFailed = false;
    for (int i = 0; i < nFrames; ++i)
    {
        makeFrame(frameData, i);
        GstClockTime dur = (GstClockTime)(GST_SECOND / fps);
        GstBuffer* buf = gst_buffer_new_and_alloc(frameData.size());
        if (gst_buffer_fill(buf, 0, frameData.data(), frameData.size()) != (gssize)frameData.size())
        {
            gst_buffer_unref(buf);
            pushFailed = true;
            break;
        }
        GST_BUFFER_PTS(buf) = (GstClockTime)i * dur;
        GST_BUFFER_DURATION(buf) = dur;
        GstFlowReturn fr = gst_app_src_push_buffer(GST_APP_SRC(src), buf);
        if (fr != GST_FLOW_OK)
        {
            std::fprintf(stderr, "FAIL: push %d: flow %d\n", i, (int)fr);
            pushFailed = true;
            break;
        }
        Sleep((DWORD)frameMs);   // the app is paced by camera frames; we sleep
        // Watch the bus for errors while we feed (non-destructive pop-filter
        // of just ERROR, so EOS still arrives later).
        GstBus* bus = gst_element_get_bus(pipe);
        GstMessage* msg = gst_bus_timed_pop_filtered(bus, 0, GST_MESSAGE_ERROR);
        if (msg)
        {
            GError* e = nullptr; gchar* dbg = nullptr;
            gst_message_parse_error(msg, &e, &dbg);
            std::fprintf(stderr, "FAIL: pipeline error: %s\n", e ? e->message : "?");
            if (e) g_error_free(e);
            g_free(dbg);
            gst_message_unref(msg);
            gst_object_unref(bus);
            gst_object_unref(src);
            gst_object_unref(pipe);
            return 1;
        }
        gst_object_unref(bus);
    }
    if (pushFailed)
    {
        gst_app_src_end_of_stream(GST_APP_SRC(src));
        gst_element_set_state(pipe, GST_STATE_NULL);
        gst_object_unref(src);
        gst_object_unref(pipe);
        return 1;
    }

    // End of stream + bounded drain (the app's GstVideoEncoder::stop does the
    // same: end_of_stream, then pop EOS|ERROR up to 8 s, then NULL).
    gst_app_src_end_of_stream(GST_APP_SRC(src));
    GstBus* bus = gst_element_get_bus(pipe);
    GstClockTime deadline = gst_util_get_timestamp() + 10 * GST_SECOND;
    bool done = false;
    while (gst_util_get_timestamp() < deadline && !done)
    {
        GstMessage* msg = gst_bus_timed_pop_filtered(
            bus, 200 * GST_MSECOND,
            (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (!msg) continue;
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
        {
            GError* e = nullptr; gchar* dbg = nullptr;
            gst_message_parse_error(msg, &e, &dbg);
            std::fprintf(stderr, "FAIL: encoder error: %s\n", e ? e->message : "?");
            if (e) g_error_free(e);
            g_free(dbg);
        }
        else done = true;
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
    gst_element_set_state(pipe, GST_STATE_NULL);
    gst_object_unref(src);
    gst_object_unref(pipe);

    if (!done) { std::fprintf(stderr, "FAIL: no EOS before deadline\n"); return 1; }
    std::printf("EOS received, %d frames encoded\n", nFrames);
    return checkMp4(outPath) ? 0 : 1;
}

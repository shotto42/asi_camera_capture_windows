// gst_video_encoder.cpp
//
// GstVideoEncoder implementation (see gst_video_encoder.h).

#include "gst_video_encoder.h"

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include <cmath>
#include <cstdio>

bool GstVideoEncoder::start(const std::string& path, int w, int h, double fps)
{
    stop();
    int fpsNum = (int)std::lround(fps);
    char desc[1200];
    std::snprintf(desc, sizeof(desc),
                  "appsrc name=src is-live=true block=false max-bytes=67108864 "
                  "format=time caps=video/x-raw,format=BGR,width=%d,height=%d,framerate=%d/1 "
                  "! videoconvert ! x264enc speed-preset=ultrafast tune=zerolatency bitrate=10000 "
                  "! video/x-h264,profile=baseline ! mp4mux ! filesink location=%s",
                  w, h, fpsNum, path.c_str());

    GError* err = nullptr;
    pipeline_ = gst_parse_launch(desc, &err);
    if (!pipeline_ || err)
    {
        if (err) { std::fprintf(stderr, "gst: %s\n", err->message); g_error_free(err); }
        else     { std::fprintf(stderr, "gst: parse failed\n"); }
        pipeline_ = nullptr;
        return false;
    }
    src_ = GST_APP_SRC(gst_bin_get_by_name(GST_BIN(pipeline_), "src"));
    if (!src_) { gst_object_unref(pipeline_); pipeline_ = nullptr; return false; }

    gst_element_set_state(pipeline_, GST_STATE_PLAYING);
    GstStateChangeReturn ret =
        gst_element_get_state(GST_ELEMENT(src_), nullptr, nullptr, 2 * GST_SECOND);
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        stop();
        return false;
    }
    fps_ = fps;
    frameIndex_ = 0;
    return true;
}

bool GstVideoEncoder::push(const unsigned char* bgr, int w, int h)
{
    if (!src_) return false;
    GstClockTime dur = (GstClockTime)std::llround(GST_SECOND / fps_);
    gsize size = (gsize)w * h * 3;

    GstBuffer* buf = gst_buffer_new_and_alloc(size);
    if (gst_buffer_fill(buf, 0, bgr, size) != size)
    {
        gst_buffer_unref(buf);
        return false;
    }
    GST_BUFFER_PTS(buf) = (GstClockTime)frameIndex_ * dur;
    GST_BUFFER_DURATION(buf) = dur;
    frameIndex_++;

    // push_buffer takes ownership of the buffer (transfer full) - do NOT unref.
    GstFlowReturn fr = gst_app_src_push_buffer(src_, buf);
    return fr == GST_FLOW_OK;
}

void GstVideoEncoder::stop()
{
    if (!pipeline_) return;
    if (src_)
    {
        gst_app_src_end_of_stream(src_);
        gst_object_unref(src_);
        src_ = nullptr;
    }
    // Wait (bounded) for the encoder to flush the queue and finish.
    GstBus* bus = gst_element_get_bus(pipeline_);
    GstClockTime deadline = gst_util_get_timestamp() + 8 * GST_SECOND;
    bool done = false;
    while (gst_util_get_timestamp() < deadline && !done)
    {
        GstMessage* msg = gst_bus_timed_pop_filtered(
            bus, 200 * GST_MSECOND,
            (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (!msg) continue;
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
        {
            GError* e = nullptr;
            gchar* dbg = nullptr;
            gst_message_parse_error(msg, &e, &dbg);
            std::fprintf(stderr, "gst encoder error: %s\n", e ? e->message : "?");
            if (e) g_error_free(e);
            g_free(dbg);
            break;
        }
        done = true;
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
}

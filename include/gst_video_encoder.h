// gst_video_encoder.h
//
// GstVideoEncoder: H.264 -> MP4 via a custom GStreamer pipeline.
//
// A bounded appsrc queue (block=false) means push() never blocks: when the
// encoder falls behind, frames are dropped (and counted) instead of stalling
// the capture loop, and stopping is always prompt.
#pragma once

#include <cstdint>
#include <string>

// Lightweight forward declarations (the .cpp includes the full GStreamer
// headers; this keeps them out of every TU that includes this header).
struct _GstElement; typedef _GstElement GstElement;
struct _GstAppSrc;  typedef _GstAppSrc GstAppSrc;

class GstVideoEncoder
{
public:
    ~GstVideoEncoder() { stop(); }

    bool start(const std::string& path, int w, int h, double fps);

    // Accepts a BGR frame. Returns false if the frame was dropped (queue full).
    bool push(const unsigned char* bgr, int w, int h);

    void stop();

private:
    GstElement* pipeline_ = nullptr;
    GstAppSrc* src_ = nullptr;
    double fps_ = 10.0;
    uint64_t frameIndex_ = 0;
};

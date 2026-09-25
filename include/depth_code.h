// depth_code.h
//
// Bit-depth combo item-data encoding: the low byte is the bit depth (8/14);
// the high flag (0x100) marks "record as .ser" — only meaningful for 8-bit
// video (14-bit is always .ser; stills never set it). This lets the video
// combo offer BOTH an 8-bit H.264 and an 8-bit .ser entry.
#pragma once

constexpr int kSerCodeFlag = 0x100;

inline int  depthCode(int depth, bool ser) { return depth | (ser ? kSerCodeFlag : 0); }
inline int  codeDepth(int code)            { return code & 0xff; }
inline bool codeSer(int code)              { return (code & kSerCodeFlag) != 0; }

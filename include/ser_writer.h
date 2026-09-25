// ser_writer.h
//
// SerWriter: writes a planetary ".ser" file (format v3, Wilkens/Hahn spec).
// A .ser is a plain container:
//
//   [178-byte header][frame 0][frame 1]...   (+ optional per-frame timestamp
//                                             trailer if DateTime > 0)
//
// and every frame is either MONO (ColorID=0, ONE plane of W*H samples) or, for
// a colour camera, RGB (ColorID=100). The plane count is derived from ColorID
// by the readers — the header does not carry it — and this writer follows the
// same rule Siril does (src/io/ser.c: number_of_planes = 3 for SER_RGB/SER_BGR,
// else 1; a 3-plane frame is W*H*byteDepth*3 bytes with R first).
//
// The intra-frame layout of an RGB frame is PER-PIXEL INTERLEAVED RGBRGB...
// (one R,G,B triple per pixel, R first). It is NOT full planes one after the
// other (all R, then all G, then all B). Verified against both readers in the
// ecosystem, 2026-09-23 (the same day an earlier change wrongly pinned the
// planar layout and blamed the players):
//   - Siril src/io/ser.c, ser_read_frame() SER_RGB branch de-interleaves the
//     frame buffer with "for (i = 0, j = 0; j < rx*ry; i += 3, j++)
//     pdata[R]=tmp[i+0]; pdata[G]=tmp[i+1]; pdata[B]=tmp[i+2]"; its crop
//     helper says outright "reorder the RGBRGB to RRGGBB and crop" — i.e. file
//     bytes are RGBRGB, Siril's internal planes are RRGGBB; AND Siril's own
//     writer, ser_write_frame_from_fit_internal(), emits interleaved bytes
//     ("dest = plane; ... dest += number_of_planes").
//   - Ser-Player (cgarry/ser-player, PIPP pipp_ser.cpp) reads the COLOURID_RGB
//     path as "r = *read_ptr++; g = *read_ptr++; b = *read_ptr++" per pixel.
// A planar file under these readers renders as a 3x3 mosaic of the same
// (compressed, colour-fringed) picture — exactly the artifact users reported
// in SIRIL and Ser-Player; simulated byte-for-byte and reproduced.
// Byte COUNTS are identical for either layout, so a wrong order is silent to
// every size check — --colourtest pins the on-disk interleaved order
// byte-for-byte. Do not flip it back to planar.
//
// The camera's RAW16 output path always reads out at full depth (on the ASI178:
// 14-bit, values 0..65528 = raw14<<2); the deep .ser capture uses it. The 8-bit
// .ser capture runs on the 10-bit HSM readout instead (RAW8 output +
// ASI_HIGH_SPEED_MODE=1; the RAW16 path ignores HSM) — those 8-bit values
// arrive 1 byte/px, ready to store (push8 / pushRgb8). A .ser keeps the
// target depth's value LEFT-justified (MSB-aligned) in the 16-bit container,
// and declares PixelDepthPerPlane = 16 for 2-byte output (the container width;
// 8-bit stays 8, 1 byte/px):
//   14-bit -> word = raw14 << 2          (0..65528, top 2 bits 0; 2 bytes/px)
//   10-bit -> word = (raw14 >> 4) << 6   (top 10 bits, 0..65472; 2 bytes/px)
//   8-bit  -> byte = raw16 >> 8 or the RAW8 HSM value (0..255; 1 byte/px)
//
// Why declare 16 instead of 10/14? The spec TEXT prescribes LSB alignment for
// 9..16-bit (value in the low bits), but the readers in the ecosystem don't
// agree on how to undo a declared sub-16 depth: Siril's reader takes the
// 16-bit word as-is (no shift) and its own writer ALWAYS labels 2-byte files
// 16; SER.Lib reads the word as-is and normalises by 2^PixelDepthPerPlane-1.
// With the effective depth (10/14) declared, a left-justified value clips to
// a white image under such a reader (65528/16383), and a spec-LSB value
// renders at 1/4..1/64 brightness under an as-is reader. Declaring the
// container width (16) with the value left-justified is the one combination
// that every reader model decodes to the full-range image (as-is readers see
// the value filling 0..65535; shift-by-(16-depth) readers shift by 0;
// depth-normalising readers divide by 65535). We follow the tools, not the
// spec text (verified the hard way: both LSB- and MSB-declared 10/14-bit
// files were misread by the user's stacking tool). The same rule applies to
// every channel sample of an interleaved RGB frame.
//
// The header's offset-22 "LittleEndian" field is written 0 for our
// little-endian pixel data — the de-facto convention INVERTS the spec text:
// the first SER programs (and later Siril and GoQat, see the comment in
// siril src/io/ser.h: "SER_LITTLE_ENDIAN = 0") treat it as a BigEndian flag:
// 0 = LE data, 1 = BE data. Siril's reader byte-swaps every 16-bit word when
// the field is 1; writing 1 turned clean ramps into a 0,32768,1,32769...
// checkerboard ("noise"). De-facto writers (FireCapture/PIPP/AutoStakkert,
// the SerCapture NINA plugin) all emit LE data with field 0. 8-bit data is
// 1 byte/px and ignores the field.
//
// The frame count lives in the header but is only known once recording stops,
// so the header is written with count=0 up front and the count is patched in
// place (fseek to offset 38) on close(). A large stdio buffer keeps the per-
// frame fwrite from hitting the disk on every call.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// SER v3 ColorID values (the table Siril/GoQat and the capture tools use;
// see siril src/io/ser.h: SER_MONO = 0, SER_BAYER_RGGB = 8 ... SER_RGB = 100,
// SER_BGR = 101). BAYER patterns 8..11 are NOT used here: a colour camera's
// .ser is written demosaiced (the app saves RGB, never a mosaic).
constexpr int kSerColorMono = 0;
constexpr int kSerColorRgb  = 100;

// Channel count a given ColorID implies (the same rule Siril derives on read;
// it sizes the frame — for RGB the bytes are interleaved per pixel, R first).
inline int serPlanesForColorId(int colorId)
{
    return (colorId == kSerColorRgb || colorId == 101 /* SER_BGR */) ? 3 : 1;
}

class SerWriter
{
public:
    ~SerWriter() { close(); }

    // bitDepth 1..16 (1..8 stored as 1 byte/channel-sample, 9..16 as 2
    // bytes/channel-sample). colorId selects MONO (1 plane) or RGB (the three
    // channels interleaved per pixel, R first); the header declares
    // PixelDepthPerPlane = 16 for 9..16-bit
    // (the container width; see the class comment). Returns false if the file
    // could not be opened or the header could not be written.
    bool open(const std::string& path, int w, int h, int bitDepth,
              const std::string& instrument = "ZWO ASI camera", int colorId = kSerColorMono);

    // ---- MONO files (colorId == kSerColorMono) ----------------------------
    // Append one frame. raw16 is the camera's RAW16 readout (uint16, LE on x86),
    // values 0..65528 on an ASI178 — a full-depth value left-justified in the
    // 16-bit container. Little-endian.
    //   8-bit  (1 B/px): take the top 8 bits (raw16 >> 8) — fills the byte.
    //   9..16-bit (2 B/px): keep the top `bitDepth` bits left-justified in the
    //   16-bit container, i.e. (raw16 >> shift_) << shift_ with shift_ =
    //   16-bitDepth. So 14-bit stores the raw readout as-is (top 14 bits) and
    //   10-bit stores the top 10 bits (lower 6 bits cleared). The header
    //   declares the container width (16) and the de-facto endianness flag
    //   (offset 22 = 0 for our LE data), so every reader decodes the word
    //   as-is to the full sensor range; the effective depth stays implicit
    //   in the data (14-bit tops out at 65528, 10-bit at multiples of 64).
    // Returns false on write error (disk full/slow).
    bool push(const uint16_t* raw16);

    // Append one 8-bit frame from the camera's RAW8 readout (the 10-bit HSM
    // readout, delivered 1 byte/px, values 0..255) — already 8-bit, so it is
    // copied straight through with no shift. Only valid for an 8-bit (1 B/px)
    // file. Returns false on write error.
    bool push8(const uint8_t* raw8);

    // ---- RGB files (colorId == kSerColorRgb) ------------------------------
    // `rgb` is the SER frame layout already: 3*w*h samples, PER-PIXEL
    // INTERLEAVED RGBRGB... with R first — the SER v3 layout for ColorID 100
    // that both Siril's reader/writer and Ser-Player decode (see the file
    // header; NOT full planes RRR...GGG...BBB; --colourtest pins the on-disk
    // interleaved order). colour.h's demosaicBayerToRgb produces it straight
    // from the Bayer readout. The same left-justifying shift is applied per
    // channel sample as push() applies to a mono frame.
    bool pushRgb(const uint16_t* rgb);
    // The 8-bit variant: the samples arrive as 1 byte each ready for the file
    // (from the RAW8/HSM readout), so the frame is copied straight through.
    bool pushRgb8(const uint8_t* rgb);

    void close();

    uint64_t frameCount() const { return frameCount_; }
    int colorId() const { return colorId_; }
    int planes() const { return planes_; }

private:
    FILE* file_ = nullptr;
    int w_ = 0, h_ = 0, bitDepth_ = 14, shift_ = 2, bytePerPx_ = 2;
    int colorId_ = kSerColorMono, planes_ = 1;
    uint64_t frameCount_ = 0;
    std::vector<uint16_t> frameBuf_;
    std::vector<uint8_t>  frameBuf8_;
};

// Validate a .ser file against the v3 spec (used by the smoke test and the
// headless SerWriter self-test): check the 178-byte header (magic, ColorID,
// endianness field, width/height/declared depth, frame count > 0) and that the
// file size == 178 + count * planes * W * H * bpp (bpp = 1 for 8-bit, 2 for
// 9..16-bit; planes = 1 for MONO, 3 for RGB).
bool validateSerFile(const std::string& path, int expW, int expH, int expDepth,
                     int expColorId = kSerColorMono);

// capwin2 - capture the main window of a running camera_app (class
// Qt673QWindowIcon) to a 32bpp BMP. Native C++ because the sandboxed
// user32 in this environment hides GetDC/GetWindowDC from managed P/Invoke
// (gdi32 BitBlt etc. are available; the native CRT resolves the entry).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 2) { std::printf("usage: capwin2 <out.bmp>\n"); return 1; }
  HWND hw = FindWindowW(L"Qt673QWindowIcon", NULL);
  if (!hw) { std::printf("Qt673QWindowIcon window not found\n"); return 1; }
  RECT rc;
  if (!GetWindowRect(hw, &rc)) { std::printf("GetWindowRect failed\n"); return 1; }
  int w = rc.right - rc.left, h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) { std::printf("bad rect\n"); return 1; }

  HDC src = GetWindowDC(hw);
  if (!src) { std::printf("GetWindowDC failed\n"); return 1; }
  HDC dst = CreateCompatibleDC(src);

  // 32bpp top-down DIB section with a direct pixel pointer (avoids
  // GetDIBits, whose signature changed in the new SDK headers).
  BITMAPINFO bi = {};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;  // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(dst, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
  if (!bmp || !bits) { std::printf("CreateDIBSection failed\n"); return 1; }
  HGDIOBJ old = SelectObject(dst, bmp);
  BOOL ok = BitBlt(dst, 0, 0, w, h, src, rc.left, rc.top, SRCCOPY);
  if (!ok) { std::printf("BitBlt failed\n"); return 1; }
  std::vector<unsigned char> buf((unsigned char*)bits,
                                 (unsigned char*)bits + (size_t)w * h * 4);

  unsigned char hdr[14] = {'B','M',0,0,0,0,0,0,0,0,54,0,0,0};
  FILE* f = fopen(argv[1], "wb");
  if (!f) { std::printf("cannot write %s\n", argv[1]); return 1; }
  unsigned int size = 54 + (unsigned int)buf.size();
  hdr[2] = (unsigned char)(size & 0xFF);
  hdr[3] = (unsigned char)((size >> 8) & 0xFF);
  hdr[4] = (unsigned char)((size >> 16) & 0xFF);
  hdr[5] = (unsigned char)((size >> 24) & 0xFF);
  fwrite(hdr, 1, 14, f);
  fwrite(&bi.bmiHeader, 1, 40, f);
  fwrite(buf.data(), 1, buf.size(), f);
  fclose(f);

  SelectObject(dst, old);
  DeleteObject(bmp);
  DeleteDC(dst);
  ReleaseDC(hw, src);
  std::printf("captured %dx%d -> %s\n", w, h, argv[1]);
  return 0;
}

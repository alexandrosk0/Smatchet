#pragma once

// A decoded icon: tightly packed top-down RGBA32 pixels (`Width * Height * 4` bytes). Produced on any
// thread by SmatchetImageTextureCache::DecodeIconBytesAnyThread and consumed on the UI thread by
// SmatchetImageTextureCache::GetOrCreateFromRgba, so no stb decode ever runs inside a frame.

#include <vector>

struct DecodedIcon {
    std::vector<unsigned char> Rgba;
    int Width = 0;
    int Height = 0;
};

#pragma once

#include "see/frame.hpp"

#include <string>

namespace sp {

bool load_image(const std::wstring& path, Frame& out);
bool save_png(const std::wstring& path, int w, int h, const uint32_t* bgra, int stride_px);

}  // namespace sp

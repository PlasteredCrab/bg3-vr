#pragma once

#include <string>
#include <string_view>

namespace bg3vr {

std::string to_utf8(std::wstring_view wide);

} // namespace bg3vr

#pragma once

// CommonLibSSE-NG must come first — it owns the Windows.h include and sets
// up WIN32 defines, COM, DirectX, and the REX/W32 wrappers correctly.
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

// STL
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

// spdlog
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

// SimpleIni (header-only, installed by vcpkg into the include path)
#include <SimpleIni.h>

using namespace std::string_view_literals;

namespace logger = SKSE::log;

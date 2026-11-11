#pragma once
#include "document.hpp"
#include <filesystem>

namespace pf {
State open(const std::filesystem::path& path);
State demo();
void save(const State& state, const std::filesystem::path& path);
void exportImage(const State& state, const std::filesystem::path& path);
State loadPsd(const std::filesystem::path& path);
void savePsd(const State& state, const std::filesystem::path& path);
} // namespace pf

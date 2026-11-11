#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace pf {
inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
class Reader {
public:
    explicit Reader(const std::filesystem::path& path) : file(path, std::ios::binary) {
        require(bool(file), "Unable to open the file.");
        file.seekg(0, std::ios::end);
        end_ = std::uint64_t(file.tellg());
        seek(0);
    }
    void bytes(void* dest, std::size_t count) {
        require(bool(file.read(static_cast<char*>(dest), std::streamsize(count))), "Unexpected end of file.");
    }
    std::uint8_t u8() { std::uint8_t v; bytes(&v, 1); return v; }
    std::uint16_t u16() { const auto hi = u8(); return std::uint16_t((hi << 8) | u8()); }
    std::uint32_t u32() { const auto hi = u16(); return (std::uint32_t(hi) << 16) | u16(); }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    std::string str(std::size_t n) { std::string s(n, '\0'); bytes(s.data(), n); return s; }
    std::uint64_t pos() { return std::uint64_t(file.tellg()); }
    std::uint64_t limit() const { return end_; }
    void skip(std::uint64_t count) { seek(pos() + count); }
    void seek(std::uint64_t offset) {
        require(offset <= end_ && bool(file.seekg(std::streamoff(offset))), "Invalid file offset.");
    }
private:
    std::ifstream file;
    std::uint64_t end_{};
};

class Writer {
public:
    explicit Writer(const std::filesystem::path& path) : file(path, std::ios::binary | std::ios::trunc) {
        require(bool(file), "Unable to create the file.");
    }
    void bytes(const void* data, std::size_t count) {
        require(bool(file.write(static_cast<const char*>(data), std::streamsize(count))), "Unable to write the file.");
    }
    void u8(std::uint8_t value) { bytes(&value, 1); }
    void u16(std::uint16_t v) { u8(std::uint8_t(v >> 8)); u8(std::uint8_t(v)); }
    void u32(std::uint32_t v) { u16(std::uint16_t(v >> 16)); u16(std::uint16_t(v)); }
    void str(std::string_view s) { bytes(s.data(), s.size()); }
    void close() { file.close(); require(bool(file), "Unable to finish writing the file."); }
private:
    std::ofstream file;
};
} // namespace pf

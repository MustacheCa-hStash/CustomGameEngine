#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>

inline std::uint64_t sourceFingerprint(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Could not open the file for fingerprinting.");
    std::array<char, 16384> buffer;
    std::uint64_t hash = 14695981039346656037ull;
    while (file)
    {
        file.read(buffer.data(), buffer.size());
        for (std::streamsize i = 0; i < file.gcount(); ++i)
        {
            hash = (hash ^ static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)])) * 1099511628211ull;
        }
    }
    if (!file.eof()) throw std::runtime_error("Could not read the file for fingerprinting.");
    return hash;
}

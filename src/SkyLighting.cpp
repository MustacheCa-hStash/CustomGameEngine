#include "SkyLighting.h"

#include <DirectXPackedVector.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numbers>
#include <span>
#include <sstream>
#include <stdexcept>

using namespace DirectX;

namespace
{
constexpr double pi = std::numbers::pi;

void readScanline(std::istream& file, std::span<std::uint8_t> scanline, unsigned width)
{
    std::array<std::uint8_t, 4> marker{};
    file.read(reinterpret_cast<char*>(marker.data()), marker.size());
    if (!file || marker[0] != 2 || marker[1] != 2 || (marker[2] * 256u + marker[3]) != width)
        throw std::runtime_error("Invalid RGBE sky scanline.");

    // RLE packets are stored separately for R, G, B, and the shared exponent.
    for (std::size_t channel = 0; channel < 4; ++channel)
    {
        auto plane = scanline.subspan(channel * width, width);
        while (!plane.empty())
        {
            const auto packet = file.get();
            if (packet <= 0) throw std::runtime_error("Invalid RGBE sky packet.");
            const bool run = packet > 128;
            const auto count = static_cast<std::size_t>(run ? packet - 128 : packet);
            if (count > plane.size()) throw std::runtime_error("RGBE sky packet exceeds the scanline.");
            if (run)
            {
                const auto value = file.get();
                if (value < 0) throw std::runtime_error("Truncated RGBE sky packet.");
                std::fill_n(plane.begin(), count, static_cast<std::uint8_t>(value));
            }
            else if (!file.read(reinterpret_cast<char*>(plane.data()), count))
                throw std::runtime_error("Truncated RGBE sky packet.");
            plane = plane.subspan(count);
        }
    }
}

std::vector<XMFLOAT3> readRadiance(const std::filesystem::path& path, unsigned& width, unsigned& height)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Could not open the sky panorama.");
    std::string line;
    std::getline(file, line);
    if (line != "#?RADIANCE") throw std::runtime_error("Expected a Radiance RGBE sky panorama.");
    bool rgbe = false;
    while (std::getline(file, line) && !line.empty())
        if (line == "FORMAT=32-bit_rle_rgbe") rgbe = true;
    std::getline(file, line);
    std::istringstream resolution(line);
    std::string vertical, horizontal;
    if (!rgbe || !(resolution >> vertical >> height >> horizontal >> width) || vertical != "-Y" ||
        horizontal != "+X" || width < 8 || width > 8192 || height != width / 2 || !std::has_single_bit(width))
        throw std::runtime_error("Expected an RLE RGBE 2:1 sky panorama with power-of-two dimensions and -Y +X orientation.");

    std::vector<std::uint8_t> scanline(static_cast<std::size_t>(width) * 4);
    std::vector<XMFLOAT3> pixels(static_cast<std::size_t>(width) * height);
    for (unsigned y = 0; y < height; ++y)
    {
        readScanline(file, scanline, width);
        for (unsigned x = 0; x < width; ++x)
        {
            const auto exponent = scanline[3 * width + x];
            if (!exponent) continue;
            const float scale = std::ldexp(1.0f, static_cast<int>(exponent) - 136);
            auto& pixel = pixels[static_cast<std::size_t>(y) * width + x];
            pixel = {(scanline[x] + 0.5f) * scale, (scanline[width + x] + 0.5f) * scale,
                     (scanline[2 * width + x] + 0.5f) * scale};
            if (std::max({pixel.x, pixel.y, pixel.z}) > 65504.0f)
                throw std::runtime_error("Sky radiance exceeds the RGBA16F range.");
        }
    }
    return pixels;
}

SkyIrradiance projectIrradiance(std::span<const XMFLOAT3> radiance, unsigned width, unsigned height, float& gain)
{
    // Use the same longitude and latitude as Sky.hlsl. Only the upper hemisphere supplies sky light.
    const double longitudeStep = 2 * pi / width;
    const double latitudeStep = pi / height;
    std::vector<double> sinLongitude(width), cosLongitude(width);
    for (unsigned x = 0; x < width; ++x)
    {
        const double longitude = (x + 0.5) * longitudeStep - pi;
        sinLongitude[x] = std::sin(longitude);
        cosLongitude[x] = std::cos(longitude);
    }
    std::array<std::array<double, 3>, 9> moments{};
    for (unsigned row = 0; row < height / 2; ++row)
    {
        const double latitude = pi / 2 - (row + 0.5) * latitudeStep;
        const double ringRadius = std::cos(latitude);
        const double y = std::sin(latitude);
        const double solidAngle = longitudeStep * (std::sin(latitude + latitudeStep / 2) -
                                                   std::sin(latitude - latitudeStep / 2));
        for (unsigned col = 0; col < width; ++col)
        {
            const double x = ringRadius * sinLongitude[col];
            const double z = ringRadius * cosLongitude[col];
            const double basis[] = {1, x, y, z, x * y, y * z, 3 * y * y - 1, x * z, x * x - z * z};
            const auto& rgb = radiance[static_cast<std::size_t>(row) * width + col];
            for (std::size_t i = 0; i < moments.size(); ++i)
            {
                const double weight = basis[i] * solidAngle;
                moments[i][0] += rgb.x * weight;
                moments[i][1] += rgb.y * weight;
                moments[i][2] += rgb.z * weight;
            }
        }
    }

    // Fold both SH basis normalizations and the Lambertian convolution divided by pi into
    // the coefficients, so the shader can evaluate the polynomial directly.
    constexpr double shScale[] = {1 / (4 * pi), 3 / (4 * pi), 3 / (4 * pi), 3 / (4 * pi),
                                  15 / (4 * pi), 15 / (4 * pi), 5 / (16 * pi), 15 / (4 * pi), 15 / (16 * pi)};
    SkyIrradiance result{};
    for (std::size_t i = 0; i < result.size(); ++i)
    {
        const double band = i == 0 ? 1.0 : i < 4 ? 2.0 / 3.0 : 0.25;
        const double scale = band * shScale[i];
        result[i] = {static_cast<float>(moments[i][0] * scale), static_cast<float>(moments[i][1] * scale),
                     static_cast<float>(moments[i][2] * scale), 0.0f};
    }

    // Match the legacy diffuse fill on upward-facing neutral surfaces at the same exposure.
    const auto up = XMLoadFloat4(&result[0]) + XMLoadFloat4(&result[2]) + 2.0f * XMLoadFloat4(&result[6]);
    const float upLuminance = XMVectorGetX(XMVector3Dot(up, XMVectorSet(0.2126f, 0.7152f, 0.0722f, 0.0f)));
    if (upLuminance > 0.0f)
    {
        gain = 0.35f / upLuminance;
        for (auto& coefficient : result)
            XMStoreFloat4(&coefficient, XMLoadFloat4(&coefficient) * gain);
    }
    return result;
}

TextureMip packHalfMip(std::span<const XMFLOAT3> pixels, unsigned width, unsigned height)
{
    TextureMip mip{width, height, {}};
    mip.rgba.resize(pixels.size() * sizeof(PackedVector::XMHALF4));
    auto* destination = mip.rgba.data();
    for (const auto& pixel : pixels)
    {
        PackedVector::XMHALF4 encoded;
        PackedVector::XMStoreHalf4(&encoded, XMVectorSet(pixel.x, pixel.y, pixel.z, 1.0f));
        std::memcpy(destination, &encoded, sizeof(encoded));
        destination += sizeof(encoded);
    }
    return mip;
}

std::vector<XMFLOAT3> downsample(std::span<const XMFLOAT3> pixels, unsigned width, unsigned height)
{
    const auto nextHeight = std::max(1u, height / 2);
    std::vector<XMFLOAT3> next(static_cast<std::size_t>(width / 2) * nextHeight);
    for (unsigned y = 0; y < nextHeight; ++y)
    {
        const auto top = static_cast<std::size_t>(y * 2) * width;
        const auto bottom = height > 1 ? top + width : top;
        for (unsigned x = 0; x < width; x += 2)
        {
            const auto sum = XMLoadFloat3(&pixels[top + x]) + XMLoadFloat3(&pixels[top + x + 1]) +
                             XMLoadFloat3(&pixels[bottom + x]) + XMLoadFloat3(&pixels[bottom + x + 1]);
            XMStoreFloat3(&next[static_cast<std::size_t>(y) * (width / 2) + x / 2], sum * 0.25f);
        }
    }
    return next;
}
}

SkyLighting loadSkyLighting(const std::filesystem::path& panorama)
{
    unsigned width = 0, height = 0;
    auto pixels = readRadiance(panorama, width, height);
    SkyLighting sky{};
    // The low-order projection only needs a small mip. The camera keeps the full-resolution dome.
    const auto projectionWidth = std::min(width, 256u);
    for (;;)
    {
        if (width == projectionWidth) sky.irradiance = projectIrradiance(pixels, width, height, sky.radianceScale);
        sky.radianceMips.push_back(packHalfMip(pixels, width, height));
        if (width == 1) break;
        pixels = downsample(pixels, width, height);
        width /= 2;
        height = std::max(1u, height / 2);
    }
    return sky;
}

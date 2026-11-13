#include "TextureLoader.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace
{
void checkImage(HRESULT result, const char* message)
{
    if (FAILED(result))
    {
        throw std::runtime_error(message);
    }
}

struct ComApartment
{
    const HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ~ComApartment()
    {
        if (SUCCEEDED(status))
        {
            CoUninitialize();
        }
    }
};

TextureMip decodeImage(std::span<const std::uint8_t> encoded)
{
    if (encoded.empty() || encoded.size() > MAXDWORD)
    {
        throw std::runtime_error("Invalid embedded texture size.");
    }
    const ComApartment apartment;
    if (apartment.status != RPC_E_CHANGED_MODE)
    {
        checkImage(apartment.status, "Could not initialize the image decoder.");
    }
    ComPtr<IWICImagingFactory> factory;
    checkImage(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)), "Could not create the image decoder.");
    ComPtr<IWICStream> stream;
    checkImage(factory->CreateStream(&stream), "Could not create the image stream.");
    checkImage(stream->InitializeFromMemory(const_cast<BYTE*>(encoded.data()),
                                           static_cast<DWORD>(encoded.size())),
               "Could not open the embedded image stream.");
    ComPtr<IWICBitmapDecoder> decoder;
    checkImage(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                                &decoder), "Could not decode the embedded image.");
    ComPtr<IWICBitmapFrameDecode> frame;
    checkImage(decoder->GetFrame(0, &frame), "Could not read the embedded image.");
    ComPtr<IWICFormatConverter> converter;
    checkImage(factory->CreateFormatConverter(&converter), "Could not create the image converter.");
    checkImage(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom),
               "Could not convert the embedded image to RGBA.");

    TextureMip mip{};
    checkImage(converter->GetSize(&mip.width, &mip.height), "Could not read image dimensions.");
    if (mip.width == 0 || mip.height == 0 || mip.width > 16384 || mip.height > 16384)
    {
        throw std::runtime_error("Embedded image dimensions are outside the supported range.");
    }
    const UINT rowBytes = mip.width * 4;
    mip.rgba.resize(static_cast<std::size_t>(rowBytes) * mip.height);
    checkImage(converter->CopyPixels(nullptr, rowBytes, static_cast<UINT>(mip.rgba.size()),
                                     mip.rgba.data()), "Could not read the embedded image pixels.");
    return mip;
}

std::uint8_t toByte(float value)
{
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 255.0f)));
}

std::uint8_t encodeSrgb(float linear)
{
    const float encoded = linear <= 0.0031308f ? 12.92f * linear
                                               : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    return toByte(encoded * 255.0f);
}

TextureMip downsample(const TextureMip& source, bool masked)
{
    static const auto linearRgb = [] {
        std::array<float, 256> table{};
        for (std::size_t i = 0; i < table.size(); ++i)
        {
            const auto encoded = static_cast<float>(i) / 255.0f;
            table[i] = encoded <= 0.04045f ? encoded / 12.92f
                                           : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
        }
        return table;
    }();

    TextureMip mip{std::max(1u, source.width / 2), std::max(1u, source.height / 2), {}};
    mip.rgba.resize(static_cast<std::size_t>(mip.width) * mip.height * 4);
    for (std::uint32_t y = 0; y < mip.height; ++y)
    {
        const auto top = y * source.height / mip.height;
        const auto bottom = (y + 1) * source.height / mip.height;
        for (std::uint32_t x = 0; x < mip.width; ++x)
        {
            const auto left = x * source.width / mip.width;
            const auto right = (x + 1) * source.width / mip.width;
            const float count = static_cast<float>((right - left) * (bottom - top));
            float alpha = 0.0f;
            float weightedRgb[3]{};
            float rgb[3]{};
            for (auto sy = top; sy < bottom; ++sy)
            {
                for (auto sx = left; sx < right; ++sx)
                {
                    const auto* pixel = &source.rgba[(sy * source.width + sx) * 4];
                    const float weight = static_cast<float>(pixel[3]);
                    alpha += weight;
                    for (int channel = 0; channel < 3; ++channel)
                    {
                        const float linear = linearRgb[pixel[channel]];
                        weightedRgb[channel] += linear * weight;
                        rgb[channel] += linear;
                    }
                }
            }
            auto* pixel = &mip.rgba[(y * mip.width + x) * 4];
            for (int channel = 0; channel < 3; ++channel)
            {
                const float linear = masked && alpha > 0.0f ? weightedRgb[channel] / alpha
                                                             : rgb[channel] / count;
                pixel[channel] = encodeSrgb(linear);
            }
            pixel[3] = toByte(alpha / count);
        }
    }
    return mip;
}

using AlphaHistogram = std::array<std::uint32_t, 256>;

AlphaHistogram alphaHistogram(const TextureMip& mip)
{
    AlphaHistogram histogram{};
    for (std::size_t i = 3; i < mip.rgba.size(); i += 4)
    {
        ++histogram[mip.rgba[i]];
    }
    return histogram;
}

std::uint32_t coveredTexels(const AlphaHistogram& histogram, std::uint8_t cutoff, float scale)
{
    std::uint32_t count = 0;
    for (std::size_t alpha = 1; alpha < histogram.size(); ++alpha)
    {
        if (toByte(static_cast<float>(alpha) * scale) >= cutoff)
        {
            count += histogram[alpha];
        }
    }
    return count;
}

void preserveCoverage(TextureData& mips, float cutoff)
{
    const auto threshold = static_cast<std::uint8_t>(std::ceil(cutoff * 255.0f));
    const auto& base = mips.front();
    const float coverage = static_cast<float>(coveredTexels(alphaHistogram(base), threshold, 1.0f)) /
                           static_cast<float>(base.width * base.height);
    for (std::size_t level = 1; level < mips.size(); ++level)
    {
        auto& mip = mips[level];
        const auto histogram = alphaHistogram(mip);
        const float desired = coverage * static_cast<float>(mip.width * mip.height);
        float bestScale = 1.0f;
        float bestError = std::abs(static_cast<float>(coveredTexels(histogram, threshold, 1.0f)) - desired);
        float low = 0.0f;
        float high = 4.0f;
        for (int iteration = 0; iteration < 12; ++iteration)
        {
            const float scale = (low + high) * 0.5f;
            const float count = static_cast<float>(coveredTexels(histogram, threshold, scale));
            const float error = std::abs(count - desired);
            if (error < bestError || (error == bestError && std::abs(scale - 1.0f) < std::abs(bestScale - 1.0f)))
            {
                bestScale = scale;
                bestError = error;
            }
            if (count < desired)
            {
                low = scale;
            }
            else
            {
                high = scale;
            }
        }
        for (std::size_t i = 3; i < mip.rgba.size(); i += 4)
        {
            mip.rgba[i] = toByte(static_cast<float>(mip.rgba[i]) * bestScale);
        }
    }
}
}

TextureData loadColorTexture(std::span<const std::uint8_t> encoded, float coverageCutoff)
{
    TextureData mips;
    mips.push_back(decodeImage(encoded));
    const bool masked = coverageCutoff > 0.0f && coverageCutoff <= 1.0f;
    while (mips.back().width > 1 || mips.back().height > 1)
    {
        mips.push_back(downsample(mips.back(), masked));
    }
    if (masked)
    {
        preserveCoverage(mips, coverageCutoff);
    }
    return mips;
}

#include "ShaderCompiler.h"

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <bit>
#include <filesystem>
#include <format>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace
{
using Microsoft::WRL::ComPtr;

std::string displayPath(const std::filesystem::path& path)
{
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

void check(HRESULT result, const char* operation)
{
    if (FAILED(result))
    {
        throw std::runtime_error(std::format("{} failed (0x{:08X}).", operation,
                                             static_cast<unsigned long>(result)));
    }
}

struct Stage
{
    const wchar_t* entry;
    const wchar_t* target;
    std::vector<std::byte> MeshShaders::* output;
};

constexpr Stage stages[] = {
    {L"VSMain", L"vs_6_0", &MeshShaders::cameraVertex},
    {L"PSMain", L"ps_6_0", &MeshShaders::cameraPixel},
    {L"ShadowVS", L"vs_6_0", &MeshShaders::shadowVertex},
    {L"ShadowPS", L"ps_6_0", &MeshShaders::shadowPixel},
};
}

ShaderCompilation compileMeshShaders()
{
    const auto source = std::filesystem::path(VIEWER_PROJECT_DIRECTORY) / L"shaders" / L"Mesh.hlsl";
    const auto compilerPath = std::filesystem::path(VIEWER_DXC_EXECUTABLE).parent_path() / L"dxcompiler.dll";
    ShaderCompilation result;
    try
    {
        if (!std::filesystem::is_regular_file(source))
        {
            throw std::runtime_error("Mesh.hlsl is unavailable. Restore the source at the path below and retry.");
        }

        // DXC looks up the validator during compilation. Load the matching copy first so
        // it can sign the DXIL even when the SDK bin directory is outside PATH.
        constexpr DWORD loadFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
        const auto validatorPath = compilerPath.parent_path() / L"dxil.dll";
        const std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)> validator(
            LoadLibraryExW(validatorPath.c_str(), nullptr, loadFlags), FreeLibrary);
        if (!validator)
        {
            const auto error = GetLastError();
            throw std::runtime_error(std::format("Could not load DXIL validator {} (Win32 error {}).",
                                                 displayPath(validatorPath), error));
        }
        // Keep the DLLs alive until every DXC interface has been released. Only bytecode is returned.
        const std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)> library(
            LoadLibraryExW(compilerPath.c_str(), nullptr, loadFlags), FreeLibrary);
        if (!library)
        {
            const auto error = GetLastError();
            throw std::runtime_error(std::format(
                "Could not load DXC (Win32 error {}). Check dxcompiler.dll and its matching dependencies.",
                error));
        }
        const auto create = std::bit_cast<DxcCreateInstanceProc>(GetProcAddress(library.get(), "DxcCreateInstance"));
        if (!create) throw std::runtime_error("dxcompiler.dll does not export DxcCreateInstance.");

        ComPtr<IDxcUtils> utils;
        ComPtr<IDxcCompiler3> compiler;
        ComPtr<IDxcIncludeHandler> includes;
        ComPtr<IDxcBlobEncoding> text;
        check(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "Create DXC utilities");
        check(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "Create DXC compiler");
        check(utils->CreateDefaultIncludeHandler(&includes), "Create DXC include handler");
        check(utils->LoadFile(source.c_str(), nullptr, &text), "Read Mesh.hlsl");
        UINT32 encoding = DXC_CP_UTF8;
        BOOL encodingKnown = FALSE;
        check(text->GetEncoding(&encodingKnown, &encoding), "Read shader source encoding");
        const DxcBuffer buffer{text->GetBufferPointer(), text->GetBufferSize(),
                               encodingKnown ? encoding : DXC_CP_UTF8};
        const auto includeDirectory = source.parent_path();
        MeshShaders shaders;
        for (const auto& stage : stages)
        {
            result.diagnostics += std::format("{} ({})\n", displayPath(stage.entry), displayPath(stage.target));
            const wchar_t* arguments[] = {source.c_str(), L"-E", stage.entry, L"-T", stage.target,
                                          L"-I", includeDirectory.c_str()};
            ComPtr<IDxcResult> compilation;
            check(compiler->Compile(&buffer, arguments, static_cast<UINT32>(std::size(arguments)),
                                    includes.Get(), IID_PPV_ARGS(&compilation)), "Invoke DXC");
            ComPtr<IDxcBlobUtf8> messages;
            check(compilation->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&messages), nullptr), "Read DXC diagnostics");
            if (messages && messages->GetStringLength())
            {
                result.diagnostics.append(messages->GetStringPointer(), messages->GetStringLength());
            }
            HRESULT status;
            check(compilation->GetStatus(&status), "Read DXC compilation status");
            check(status, "Compile shader");
            ComPtr<IDxcBlob> object;
            check(compilation->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr), "Read compiled shader");
            if (!object || !object->GetBufferSize()) throw std::runtime_error("DXC returned empty bytecode.");
            const auto* bytes = static_cast<const std::byte*>(object->GetBufferPointer());
            (shaders.*stage.output).assign(bytes, bytes + object->GetBufferSize());
            result.diagnostics += "Compiled.\n";
        }
        result.shaders = std::move(shaders);
    }
    catch (const std::exception& error)
    {
        result.diagnostics += std::format("\n{}\n", error.what());
    }
    result.diagnostics += std::format("\nSource  {}\nDXC  {}\n", displayPath(source), displayPath(compilerPath));
    return result;
}

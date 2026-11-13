#pragma once

#include "D3D12Renderer.h"

#include <bitset>
#include <string>

struct InspectorActions
{
    std::optional<SceneKind> scene;
    std::optional<CameraPreset> camera;
    bool sunChanged = false;
    bool toggleWind = false;
    bool toggleMsaa = false;
    bool treeCountChanged = false;
    bool settingsChanged = false;
};

class SceneInspector
{
public:
    SceneInspector(HWND window, const D3D12Renderer& renderer);
    ~SceneInspector();
    SceneInspector(const SceneInspector&) = delete;
    SceneInspector& operator=(const SceneInspector&) = delete;

    static bool handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static bool wantsKeyboard();

    void toggle() { visible_ = !visible_; }
    void resetSelection();
    InspectorActions frame(ViewerScene& scene, const D3D12Renderer& renderer,
                           const D3D12Renderer::FrameTiming& timing, const std::string& sceneError,
                           bool hasImportedScene);
    void draw(ID3D12GraphicsCommandList* commands) const;

private:
    enum class Page { Object, Mesh, Material, Texture };
    bool inspect(ViewerScene& scene, const D3D12Renderer& renderer);
    void showTexture(const TextureAsset& texture, std::size_t index, const D3D12Renderer& renderer) const;

    static constexpr unsigned previewSlot = 16;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptors_;
    std::bitset<previewSlot> fontSlots_;
    UINT descriptorSize_ = 0;
    bool visible_ = true;
    Page page_ = Page::Object;
    std::size_t meshIndex_ = 0;
    int objectIndex_ = 0;
    bool ground_ = false;
};

// Copyright (C) 2026 Jake Rieger
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, version 3. This program is distributed in the hope that it will
// be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details. You should have received a copy of the GNU General
// Public License along with this program. If not, see https://www.gnu.org/licenses/.

#pragma once

#include <DirectXMath.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <imgui.h>
#include <wrl/client.h>

#include <cstdint>
#include <span>
#include <vector>

namespace Scarlet {
    class CharacterModel;
    struct SubMesh;

    /// @brief A vertex of a line: a position and an RGBA color (red in the low byte).
    struct LineVertex {
        DirectX::XMFLOAT3 Position;
        uint32_t Color;
    };

    /// @brief Packs a color as LineVertex expects it.
    constexpr uint32_t Rgba(uint8_t R, uint8_t G, uint8_t B, uint8_t A = 255) {
        return uint32_t(R) | (uint32_t(G) << 8) | (uint32_t(B) << 16) | (uint32_t(A) << 24);
    }

    /// @brief What the viewport draws in a frame, all in model space.
    struct Scene {
        DirectX::XMFLOAT4X4 ViewProjection;                ///< Model space to clip space.
        DirectX::XMFLOAT3 LightDirection {0.f, 1.f, 0.f};  ///< Toward the light, normalized.
        std::span<const LineVertex> Lines;                 ///< Hidden behind the model (the floor grid).
        std::span<const LineVertex> OverlayLines;          ///< Drawn over everything (the skeleton).
        std::span<const DirectX::XMFLOAT4X4> Skin;         ///< The model's skinning matrices; empty to not draw it.
        std::span<const SubMesh> Meshes;                   ///< Which of the model's meshes to draw.
    };

    /// @brief The Direct3D 11 device, the window's swap chain (which ImGui draws into), and the 3D viewport, drawn
    ///        offscreen and shown as an ImGui image.
    class Renderer {
    public:
        /// @brief Creates the device and the swap chain; false (with a message box) on failure.
        bool Initialize(HWND Window);
        /// @brief Resizes the swap chain to the window's new client size.
        void Resize(UINT Width, UINT Height);
        /// @brief Binds and clears the back buffer for ImGui to draw into.
        void BeginFrame();
        /// @brief Presents the back buffer.
        void Present();

        /// @brief Uploads a model's vertices, indices and textures for the viewport to draw (nullptr to draw none).
        void SetModel(const CharacterModel* Model);
        /// @brief Draws a scene into the viewport at the given size; its image is then ViewportTexture().
        void RenderViewport(UINT Width, UINT Height, const Scene& Frame);
        /// @brief The viewport's last image, for ImGui::Image.
        ImTextureID ViewportTexture() const;

        ID3D11Device* Device() const { return _D3DDevice.Get(); }
        ID3D11DeviceContext* Context() const { return _D3DContext.Get(); }

    private:
        template<typename T>
        using ComPtr = Microsoft::WRL::ComPtr<T>;

        ComPtr<ID3D11Device> _D3DDevice;
        ComPtr<ID3D11DeviceContext> _D3DContext;
        ComPtr<IDXGISwapChain1> _SwapChain;
        ComPtr<ID3D11RenderTargetView> _BackBufferView;

        // The viewport: multisampled color and depth, resolved into a texture ImGui can sample.
        UINT _ViewportWidth = 0, _ViewportHeight = 0, _SampleCount = 1;
        ComPtr<ID3D11Texture2D> _ColorTarget, _DepthTarget, _Resolved;
        ComPtr<ID3D11RenderTargetView> _ColorView;
        ComPtr<ID3D11DepthStencilView> _DepthView;
        ComPtr<ID3D11ShaderResourceView> _ResolvedView;

        // Shared state.
        ComPtr<ID3D11Buffer> _Constants;
        ComPtr<ID3D11DepthStencilState> _DepthTested, _DepthIgnored;
        ComPtr<ID3D11RasterizerState> _Rasterizer;

        // Lines.
        ComPtr<ID3D11VertexShader> _LineVertexShader;
        ComPtr<ID3D11PixelShader> _LinePixelShader;
        ComPtr<ID3D11InputLayout> _LineLayout;
        ComPtr<ID3D11Buffer> _LineVertices;
        UINT _LineCapacity = 0;

        // The skinned model.
        ComPtr<ID3D11VertexShader> _MeshVertexShader;
        ComPtr<ID3D11PixelShader> _MeshPixelShader;
        ComPtr<ID3D11InputLayout> _MeshLayout;
        ComPtr<ID3D11Buffer> _MeshVertices, _MeshIndices;
        ComPtr<ID3D11Buffer> _Bones;
        ComPtr<ID3D11ShaderResourceView> _BonesView;
        UINT _BoneCapacity = 0;
        std::vector<ComPtr<ID3D11ShaderResourceView>> _Textures;  ///< the model's, by index
        ComPtr<ID3D11ShaderResourceView> _DefaultAlbedo;          ///< for meshes whose texture wasn't found
        ComPtr<ID3D11SamplerState> _Sampler;

        void CreateBackBufferView();
        bool CreatePipeline();
        void CreateViewportTargets(UINT Width, UINT Height);
        void DrawLines(std::span<const LineVertex> Lines, ID3D11DepthStencilState* Depth);
        void DrawModel(const Scene& Frame);
    };
}  // namespace Scarlet

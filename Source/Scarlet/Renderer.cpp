// Copyright (C) 2026 Jake Rieger
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, version 3. This program is distributed in the hope that it will
// be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details. You should have received a copy of the GNU General
// Public License along with this program. If not, see https://www.gnu.org/licenses/.

#include "Renderer.hpp"

#include "Model.hpp"

#include <d3dcompiler.h>
#include <libSouls/Formats/DDS.hpp>

#include <algorithm>
#include <cstring>
#include <string>

namespace Scarlet {
    namespace {
        constexpr char LineShader[] = R"(
cbuffer Frame : register(b0) { row_major float4x4 ViewProjection; float4 LightDirection; };
struct VertexIn  { float3 Position : POSITION; float4 Color : COLOR; };
struct VertexOut { float4 Position : SV_POSITION; float4 Color : COLOR; };
VertexOut VSMain(VertexIn In) {
    VertexOut Out;
    Out.Position = mul(float4(In.Position, 1.0), ViewProjection);
    Out.Color    = In.Color;
    return Out;
}
float4 PSMain(VertexOut In) : SV_TARGET { return In.Color; }
)";

        // Linear blend skinning: each vertex is moved by up to four bones' matrices (row vectors, v * M), weighted.
        constexpr char MeshShader[] = R"(
cbuffer Frame : register(b0) { row_major float4x4 ViewProjection; float4 LightDirection; };
struct BoneMatrix { float4 R0; float4 R1; float4 R2; float4 R3; };
StructuredBuffer<BoneMatrix> Bones : register(t0);
struct VertexIn {
    float3 Position : POSITION;
    float3 Normal   : NORMAL;
    uint4  Bones    : BLENDINDICES;
    float4 Weights  : BLENDWEIGHT;
    float2 UV       : TEXCOORD0;
};
Texture2D Albedo : register(t1);
SamplerState Linear : register(s0);
struct VertexOut { float4 Position : SV_POSITION; float3 Normal : NORMAL; float2 UV : TEXCOORD0; };
float3 Transform(BoneMatrix M, float4 V) { return V.x * M.R0.xyz + V.y * M.R1.xyz + V.z * M.R2.xyz + V.w * M.R3.xyz; }
VertexOut VSMain(VertexIn In) {
    float3 Position = 0, Normal = 0;
    [unroll] for (int K = 0; K < 4; ++K) {
        BoneMatrix M = Bones[In.Bones[K]];
        Position += In.Weights[K] * Transform(M, float4(In.Position, 1));
        Normal   += In.Weights[K] * Transform(M, float4(In.Normal, 0));
    }
    VertexOut Out;
    Out.Position = mul(float4(Position, 1), ViewProjection);
    Out.Normal   = Normal;
    Out.UV       = In.UV;
    return Out;
}
float4 PSMain(VertexOut In) : SV_TARGET {
    // The albedo (sRGB textures sample as linear; transparent texels cut out), lit by a light from the camera, a little sky light from above,
    // and ambient, both sides lit; then encoded for the display, since the target isn't sRGB.
    float4 Texel   = Albedo.Sample(Linear, In.UV);
    clip(Texel.a - 0.5);  // cutouts: torn cloth, hair, chain mail
    float3 Base    = Texel.rgb;
    float3 N       = normalize(In.Normal);
    float  Diffuse = abs(dot(N, LightDirection.xyz));
    float  Sky     = 0.5 + 0.5 * N.y;
    float3 Color   = Base * (0.12 + 0.12 * Sky + 0.95 * Diffuse);
    return float4(pow(saturate(Color), 1.0 / 2.2), 1);
}
)";

        constexpr float ViewportClear[4] = {0.11f, 0.11f, 0.13f, 1.f};
        constexpr float WindowClear[4]   = {0.06f, 0.06f, 0.07f, 1.f};

        struct FrameConstants {
            DirectX::XMFLOAT4X4 ViewProjection;
            DirectX::XMFLOAT4 LightDirection;
        };

        Microsoft::WRL::ComPtr<ID3DBlob>
        Compile(const char* Source, size_t Size, const char* Entry, const char* Target) {
            Microsoft::WRL::ComPtr<ID3DBlob> Code, Errors;
            const HRESULT Result = D3DCompile(Source,
                                              Size,
                                              "Scarlet",
                                              nullptr,
                                              nullptr,
                                              Entry,
                                              Target,
                                              D3DCOMPILE_OPTIMIZATION_LEVEL3,
                                              0,
                                              &Code,
                                              &Errors);
            if (FAILED(Result)) {
                const std::string Message =
                  Errors ? std::string(static_cast<const char*>(Errors->GetBufferPointer()), Errors->GetBufferSize())
                         : std::string("unknown error");
                MessageBoxA(nullptr, Message.c_str(), "Scarlet: shader compilation failed", MB_ICONERROR);
                return nullptr;
            }
            return Code;
        }

        // Bytes per 4x4 block of a block-compressed format, or 0 if it isn't one.
        UINT BlockBytes(DXGI_FORMAT Format) {
            switch (Format) {
                case DXGI_FORMAT_BC1_TYPELESS:
                case DXGI_FORMAT_BC1_UNORM:
                case DXGI_FORMAT_BC1_UNORM_SRGB:
                case DXGI_FORMAT_BC4_TYPELESS:
                case DXGI_FORMAT_BC4_UNORM:
                case DXGI_FORMAT_BC4_SNORM:
                    return 8;
                case DXGI_FORMAT_BC2_TYPELESS:
                case DXGI_FORMAT_BC2_UNORM:
                case DXGI_FORMAT_BC2_UNORM_SRGB:
                case DXGI_FORMAT_BC3_TYPELESS:
                case DXGI_FORMAT_BC3_UNORM:
                case DXGI_FORMAT_BC3_UNORM_SRGB:
                case DXGI_FORMAT_BC5_TYPELESS:
                case DXGI_FORMAT_BC5_UNORM:
                case DXGI_FORMAT_BC5_SNORM:
                case DXGI_FORMAT_BC6H_TYPELESS:
                case DXGI_FORMAT_BC6H_UF16:
                case DXGI_FORMAT_BC6H_SF16:
                case DXGI_FORMAT_BC7_TYPELESS:
                case DXGI_FORMAT_BC7_UNORM:
                case DXGI_FORMAT_BC7_UNORM_SRGB:
                    return 16;
                default:
                    return 0;
            }
        }

        // Bytes per pixel of the uncompressed formats textures use, or 0 for others.
        UINT PixelBytes(DXGI_FORMAT Format) {
            switch (Format) {
                case DXGI_FORMAT_R8G8B8A8_UNORM:
                case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                case DXGI_FORMAT_B8G8R8A8_UNORM:
                case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                case DXGI_FORMAT_B8G8R8X8_UNORM:
                    return 4;
                case DXGI_FORMAT_R8G8_UNORM:
                    return 2;
                case DXGI_FORMAT_R8_UNORM:
                case DXGI_FORMAT_A8_UNORM:
                    return 1;
                case DXGI_FORMAT_R16G16B16A16_FLOAT:
                    return 8;
                default:
                    return 0;
            }
        }

        // A DDS file as a shader resource: its first image (the first face of a cubemap) with all its mips. nullptr
        // for formats this doesn't handle or data that ends early.
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> LoadDds(ID3D11Device* Device, std::span<const uint8_t> Bytes) {
            Souls::DDS Header;
            try {
                Header = Souls::DDS::Read(Bytes);
            } catch (const std::exception&) { return nullptr; }

            DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
            size_t Offset      = 4 + 124;
            if (Header.DX10) {
                Format = static_cast<DXGI_FORMAT>(Header.DX10->Format);
                Offset += 20;
            } else {
                const std::string& Code = Header.Pixels.FourCC;
                if (Code == "DXT1") Format = DXGI_FORMAT_BC1_UNORM;
                else if (Code == "DXT3") Format = DXGI_FORMAT_BC2_UNORM;
                else if (Code == "DXT5") Format = DXGI_FORMAT_BC3_UNORM;
                else if (Code == "ATI1" || Code == "BC4U") Format = DXGI_FORMAT_BC4_UNORM;
                else if (Code == "ATI2" || Code == "BC5U") Format = DXGI_FORMAT_BC5_UNORM;
                else if (Header.Pixels.RGBBitCount == 32)
                    Format = Header.Pixels.RBitMask == 0xFF ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
            }
            const UINT Block = BlockBytes(Format), Pixel = PixelBytes(Format);
            if (Header.Width <= 0 || Header.Height <= 0 || (Block == 0 && Pixel == 0)) return nullptr;

            const UINT Mips = std::max(1, Header.MipMapCount);
            std::vector<D3D11_SUBRESOURCE_DATA> Levels;
            UINT Width = UINT(Header.Width), Height = UINT(Header.Height);
            for (UINT Mip = 0; Mip < Mips; ++Mip) {
                const UINT Pitch = Block ? std::max(1u, (Width + 3) / 4) * Block : Width * Pixel;
                const UINT Rows  = Block ? std::max(1u, (Height + 3) / 4) : Height;
                if (Offset + size_t(Pitch) * Rows > Bytes.size()) break;
                Levels.push_back({Bytes.data() + Offset, Pitch, 0});
                Offset += size_t(Pitch) * Rows;
                Width  = std::max(1u, Width / 2);
                Height = std::max(1u, Height / 2);
            }
            if (Levels.empty()) return nullptr;

            D3D11_TEXTURE2D_DESC Desc {};
            Desc.Width            = UINT(Header.Width);
            Desc.Height           = UINT(Header.Height);
            Desc.MipLevels        = UINT(Levels.size());
            Desc.ArraySize        = 1;
            Desc.Format           = Format;
            Desc.SampleDesc.Count = 1;
            Desc.Usage            = D3D11_USAGE_IMMUTABLE;
            Desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> Texture;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> View;
            if (FAILED(Device->CreateTexture2D(&Desc, Levels.data(), &Texture))) return nullptr;
            if (FAILED(Device->CreateShaderResourceView(Texture.Get(), nullptr, &View))) return nullptr;
            return View;
        }
    }  // namespace

    bool Renderer::Initialize(HWND Window) {
        UINT Flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifndef NDEBUG
        Flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        const D3D_FEATURE_LEVEL Levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        HRESULT Result                   = D3D11CreateDevice(nullptr,
                                                             D3D_DRIVER_TYPE_HARDWARE,
                                                             nullptr,
                                                             Flags,
                                                             Levels,
                                                             static_cast<UINT>(std::size(Levels)),
                                                             D3D11_SDK_VERSION,
                                                             &_D3DDevice,
                                                             nullptr,
                                                             &_D3DContext);
        if (FAILED(Result) && (Flags & D3D11_CREATE_DEVICE_DEBUG)) {
            // The debug layer isn't installed (Graphics Tools optional feature); carry on without it.
            Flags &= ~D3D11_CREATE_DEVICE_DEBUG;
            Result = D3D11CreateDevice(nullptr,
                                       D3D_DRIVER_TYPE_HARDWARE,
                                       nullptr,
                                       Flags,
                                       Levels,
                                       static_cast<UINT>(std::size(Levels)),
                                       D3D11_SDK_VERSION,
                                       &_D3DDevice,
                                       nullptr,
                                       &_D3DContext);
        }
        if (FAILED(Result)) {
            MessageBoxA(nullptr, "Couldn't create a Direct3D 11 device.", "Scarlet", MB_ICONERROR);
            return false;
        }

        ComPtr<IDXGIDevice> DxgiDevice;
        ComPtr<IDXGIAdapter> Adapter;
        ComPtr<IDXGIFactory2> Factory;
        _D3DDevice.As(&DxgiDevice);
        DxgiDevice->GetAdapter(&Adapter);
        Adapter->GetParent(IID_PPV_ARGS(&Factory));

        DXGI_SWAP_CHAIN_DESC1 Desc {};
        Desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        Desc.SampleDesc.Count = 1;
        Desc.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        Desc.BufferCount      = 2;
        Desc.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        Desc.Scaling          = DXGI_SCALING_NONE;
        if (FAILED(Factory->CreateSwapChainForHwnd(_D3DDevice.Get(), Window, &Desc, nullptr, nullptr, &_SwapChain))) {
            MessageBoxA(nullptr, "Couldn't create the swap chain.", "Scarlet", MB_ICONERROR);
            return false;
        }
        Factory->MakeWindowAssociation(Window, DXGI_MWA_NO_ALT_ENTER);
        CreateBackBufferView();

        // 4x MSAA for the viewport, if the device has it for this format.
        UINT Quality = 0;
        if (SUCCEEDED(_D3DDevice->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 4, &Quality)) &&
            Quality > 0)
            _SampleCount = 4;
        return CreatePipeline();
    }

    void Renderer::CreateBackBufferView() {
        ComPtr<ID3D11Texture2D> BackBuffer;
        _SwapChain->GetBuffer(0, IID_PPV_ARGS(&BackBuffer));
        _D3DDevice->CreateRenderTargetView(BackBuffer.Get(), nullptr, &_BackBufferView);
    }

    bool Renderer::CreatePipeline() {
        const auto LineVS = Compile(LineShader, sizeof(LineShader) - 1, "VSMain", "vs_5_0");
        const auto LinePS = Compile(LineShader, sizeof(LineShader) - 1, "PSMain", "ps_5_0");
        const auto MeshVS = Compile(MeshShader, sizeof(MeshShader) - 1, "VSMain", "vs_5_0");
        const auto MeshPS = Compile(MeshShader, sizeof(MeshShader) - 1, "PSMain", "ps_5_0");
        if (!LineVS || !LinePS || !MeshVS || !MeshPS) return false;

        _D3DDevice->CreateVertexShader(LineVS->GetBufferPointer(),
                                       LineVS->GetBufferSize(),
                                       nullptr,
                                       &_LineVertexShader);
        _D3DDevice->CreatePixelShader(LinePS->GetBufferPointer(), LinePS->GetBufferSize(), nullptr, &_LinePixelShader);
        const D3D11_INPUT_ELEMENT_DESC LineLayout[] = {
          {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
          {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        _D3DDevice->CreateInputLayout(LineLayout,
                                      static_cast<UINT>(std::size(LineLayout)),
                                      LineVS->GetBufferPointer(),
                                      LineVS->GetBufferSize(),
                                      &_LineLayout);

        _D3DDevice->CreateVertexShader(MeshVS->GetBufferPointer(),
                                       MeshVS->GetBufferSize(),
                                       nullptr,
                                       &_MeshVertexShader);
        _D3DDevice->CreatePixelShader(MeshPS->GetBufferPointer(), MeshPS->GetBufferSize(), nullptr, &_MeshPixelShader);
        const D3D11_INPUT_ELEMENT_DESC MeshLayout[] = {
          {"POSITION",
           0,
           DXGI_FORMAT_R32G32B32_FLOAT,
           0,
           offsetof(MeshVertex, Position),
           D3D11_INPUT_PER_VERTEX_DATA,
           0},
          {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(MeshVertex, Normal), D3D11_INPUT_PER_VERTEX_DATA, 0},
          {"BLENDINDICES",
           0,
           DXGI_FORMAT_R16G16B16A16_UINT,
           0,
           offsetof(MeshVertex, Bones),
           D3D11_INPUT_PER_VERTEX_DATA,
           0},
          {"BLENDWEIGHT",
           0,
           DXGI_FORMAT_R32G32B32A32_FLOAT,
           0,
           offsetof(MeshVertex, Weights),
           D3D11_INPUT_PER_VERTEX_DATA,
           0},
          {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(MeshVertex, UV), D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        _D3DDevice->CreateInputLayout(MeshLayout,
                                      static_cast<UINT>(std::size(MeshLayout)),
                                      MeshVS->GetBufferPointer(),
                                      MeshVS->GetBufferSize(),
                                      &_MeshLayout);

        D3D11_BUFFER_DESC Buffer {};
        Buffer.ByteWidth      = sizeof(FrameConstants);
        Buffer.Usage          = D3D11_USAGE_DYNAMIC;
        Buffer.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        Buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        _D3DDevice->CreateBuffer(&Buffer, nullptr, &_Constants);

        D3D11_DEPTH_STENCIL_DESC Depth {};
        Depth.DepthEnable    = TRUE;
        Depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        Depth.DepthFunc      = D3D11_COMPARISON_LESS_EQUAL;
        _D3DDevice->CreateDepthStencilState(&Depth, &_DepthTested);
        Depth.DepthEnable    = FALSE;
        Depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        _D3DDevice->CreateDepthStencilState(&Depth, &_DepthIgnored);

        // No culling: the scene is mirrored into Direct3D's handedness, and some of a model's faces are two-sided.
        D3D11_RASTERIZER_DESC Raster {};
        Raster.FillMode              = D3D11_FILL_SOLID;
        Raster.CullMode              = D3D11_CULL_NONE;
        Raster.DepthClipEnable       = TRUE;
        Raster.MultisampleEnable     = _SampleCount > 1;
        Raster.AntialiasedLineEnable = _SampleCount == 1;
        _D3DDevice->CreateRasterizerState(&Raster, &_Rasterizer);

        D3D11_SAMPLER_DESC Sampler {};
        Sampler.Filter        = D3D11_FILTER_ANISOTROPIC;
        Sampler.MaxAnisotropy = 8;
        Sampler.AddressU = Sampler.AddressV = Sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        Sampler.MaxLOD                                         = D3D11_FLOAT32_MAX;
        _D3DDevice->CreateSamplerState(&Sampler, &_Sampler);

        // Meshes whose texture wasn't found are drawn a neutral grey.
        const uint32_t Grey = 0xFFB4B8BC;  // sRGB (188, 184, 180)
        D3D11_TEXTURE2D_DESC Desc {};
        Desc.Width = Desc.Height = Desc.MipLevels = Desc.ArraySize = 1;
        Desc.Format                                                = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        Desc.SampleDesc.Count                                      = 1;
        Desc.Usage                                                 = D3D11_USAGE_IMMUTABLE;
        Desc.BindFlags                                             = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA Data {&Grey, 4, 0};
        ComPtr<ID3D11Texture2D> Texture;
        _D3DDevice->CreateTexture2D(&Desc, &Data, &Texture);
        _D3DDevice->CreateShaderResourceView(Texture.Get(), nullptr, &_DefaultAlbedo);
        return true;
    }

    void Renderer::Resize(UINT Width, UINT Height) {
        if (!_SwapChain || Width == 0 || Height == 0) return;
        _BackBufferView.Reset();
        _SwapChain->ResizeBuffers(0, Width, Height, DXGI_FORMAT_UNKNOWN, 0);
        CreateBackBufferView();
    }

    void Renderer::BeginFrame() {
        _D3DContext->OMSetRenderTargets(1, _BackBufferView.GetAddressOf(), nullptr);
        _D3DContext->ClearRenderTargetView(_BackBufferView.Get(), WindowClear);
    }

    void Renderer::Present() {
        _SwapChain->Present(1, 0);
    }

    void Renderer::SetModel(const CharacterModel* Model) {
        _MeshVertices.Reset();
        _MeshIndices.Reset();
        _Textures.clear();
        if (!Model || Model->Vertices.empty() || Model->Indices.empty()) return;
        for (const ModelTexture& Texture : Model->Textures)
            _Textures.push_back(LoadDds(_D3DDevice.Get(), Texture.Dds));

        D3D11_BUFFER_DESC Desc {};
        Desc.Usage     = D3D11_USAGE_IMMUTABLE;
        Desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        Desc.ByteWidth = static_cast<UINT>(Model->Vertices.size() * sizeof(MeshVertex));
        D3D11_SUBRESOURCE_DATA Data {Model->Vertices.data(), 0, 0};
        _D3DDevice->CreateBuffer(&Desc, &Data, &_MeshVertices);

        Desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        Desc.ByteWidth = static_cast<UINT>(Model->Indices.size() * sizeof(uint32_t));
        Data.pSysMem   = Model->Indices.data();
        _D3DDevice->CreateBuffer(&Desc, &Data, &_MeshIndices);
    }

    void Renderer::CreateViewportTargets(UINT Width, UINT Height) {
        _ViewportWidth  = Width;
        _ViewportHeight = Height;

        D3D11_TEXTURE2D_DESC Desc {};
        Desc.Width              = Width;
        Desc.Height             = Height;
        Desc.MipLevels          = 1;
        Desc.ArraySize          = 1;
        Desc.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
        Desc.SampleDesc.Count   = _SampleCount;
        Desc.SampleDesc.Quality = 0;
        Desc.BindFlags          = D3D11_BIND_RENDER_TARGET;
        _D3DDevice->CreateTexture2D(&Desc, nullptr, _ColorTarget.ReleaseAndGetAddressOf());
        _D3DDevice->CreateRenderTargetView(_ColorTarget.Get(), nullptr, _ColorView.ReleaseAndGetAddressOf());

        Desc.Format    = DXGI_FORMAT_D32_FLOAT;
        Desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        _D3DDevice->CreateTexture2D(&Desc, nullptr, _DepthTarget.ReleaseAndGetAddressOf());
        _D3DDevice->CreateDepthStencilView(_DepthTarget.Get(), nullptr, _DepthView.ReleaseAndGetAddressOf());

        Desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        Desc.SampleDesc.Count = 1;
        Desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
        _D3DDevice->CreateTexture2D(&Desc, nullptr, _Resolved.ReleaseAndGetAddressOf());
        _D3DDevice->CreateShaderResourceView(_Resolved.Get(), nullptr, _ResolvedView.ReleaseAndGetAddressOf());
    }

    void Renderer::RenderViewport(UINT Width, UINT Height, const Scene& Frame) {
        if (Width == 0 || Height == 0) return;
        if (Width != _ViewportWidth || Height != _ViewportHeight || !_ColorTarget) CreateViewportTargets(Width, Height);

        D3D11_MAPPED_SUBRESOURCE Mapped;
        if (SUCCEEDED(_D3DContext->Map(_Constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped))) {
            auto* Constants           = static_cast<FrameConstants*>(Mapped.pData);
            Constants->ViewProjection = Frame.ViewProjection;
            Constants->LightDirection = {Frame.LightDirection.x, Frame.LightDirection.y, Frame.LightDirection.z, 0.f};
            _D3DContext->Unmap(_Constants.Get(), 0);
        }

        _D3DContext->OMSetRenderTargets(1, _ColorView.GetAddressOf(), _DepthView.Get());
        _D3DContext->ClearRenderTargetView(_ColorView.Get(), ViewportClear);
        _D3DContext->ClearDepthStencilView(_DepthView.Get(), D3D11_CLEAR_DEPTH, 1.f, 0);
        const D3D11_VIEWPORT Viewport {0.f, 0.f, float(Width), float(Height), 0.f, 1.f};
        _D3DContext->RSSetViewports(1, &Viewport);
        _D3DContext->RSSetState(_Rasterizer.Get());
        _D3DContext->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        _D3DContext->VSSetConstantBuffers(0, 1, _Constants.GetAddressOf());
        _D3DContext->PSSetConstantBuffers(0, 1, _Constants.GetAddressOf());

        DrawModel(Frame);
        DrawLines(Frame.Lines, _DepthTested.Get());
        DrawLines(Frame.OverlayLines, _DepthIgnored.Get());

        if (_SampleCount > 1) {
            _D3DContext->ResolveSubresource(_Resolved.Get(), 0, _ColorTarget.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
        } else {
            _D3DContext->CopyResource(_Resolved.Get(), _ColorTarget.Get());
        }
        // ImGui draws into the back buffer next.
        _D3DContext->OMSetRenderTargets(1, _BackBufferView.GetAddressOf(), nullptr);
    }

    void Renderer::DrawLines(std::span<const LineVertex> Lines, ID3D11DepthStencilState* Depth) {
        if (Lines.empty()) return;
        // Grow the vertex buffer as needed (it's rewritten for every batch).
        if (Lines.size() > _LineCapacity) {
            _LineCapacity = static_cast<UINT>(std::max<size_t>(Lines.size(), size_t(_LineCapacity) * 2));
            D3D11_BUFFER_DESC Desc {};
            Desc.ByteWidth      = _LineCapacity * sizeof(LineVertex);
            Desc.Usage          = D3D11_USAGE_DYNAMIC;
            Desc.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
            Desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            _D3DDevice->CreateBuffer(&Desc, nullptr, _LineVertices.ReleaseAndGetAddressOf());
        }
        D3D11_MAPPED_SUBRESOURCE Mapped;
        if (FAILED(_D3DContext->Map(_LineVertices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped))) return;
        std::memcpy(Mapped.pData, Lines.data(), Lines.size_bytes());
        _D3DContext->Unmap(_LineVertices.Get(), 0);

        const UINT Stride = sizeof(LineVertex), Offset = 0;
        _D3DContext->OMSetDepthStencilState(Depth, 0);
        _D3DContext->IASetInputLayout(_LineLayout.Get());
        _D3DContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        _D3DContext->IASetVertexBuffers(0, 1, _LineVertices.GetAddressOf(), &Stride, &Offset);
        _D3DContext->VSSetShader(_LineVertexShader.Get(), nullptr, 0);
        _D3DContext->PSSetShader(_LinePixelShader.Get(), nullptr, 0);
        _D3DContext->Draw(static_cast<UINT>(Lines.size()), 0);
    }

    void Renderer::DrawModel(const Scene& Frame) {
        if (!_MeshVertices || !_MeshIndices || Frame.Skin.empty()) return;

        // The bone matrices, in a structured buffer grown as needed.
        if (Frame.Skin.size() > _BoneCapacity) {
            _BoneCapacity = static_cast<UINT>(std::max<size_t>(Frame.Skin.size(), 256));
            D3D11_BUFFER_DESC Desc {};
            Desc.ByteWidth           = _BoneCapacity * sizeof(DirectX::XMFLOAT4X4);
            Desc.Usage               = D3D11_USAGE_DYNAMIC;
            Desc.BindFlags           = D3D11_BIND_SHADER_RESOURCE;
            Desc.CPUAccessFlags      = D3D11_CPU_ACCESS_WRITE;
            Desc.MiscFlags           = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            Desc.StructureByteStride = sizeof(DirectX::XMFLOAT4X4);
            _D3DDevice->CreateBuffer(&Desc, nullptr, _Bones.ReleaseAndGetAddressOf());
            _D3DDevice->CreateShaderResourceView(_Bones.Get(), nullptr, _BonesView.ReleaseAndGetAddressOf());
        }
        D3D11_MAPPED_SUBRESOURCE Mapped;
        if (FAILED(_D3DContext->Map(_Bones.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &Mapped))) return;
        std::memcpy(Mapped.pData, Frame.Skin.data(), Frame.Skin.size_bytes());
        _D3DContext->Unmap(_Bones.Get(), 0);

        const UINT Stride = sizeof(MeshVertex), Offset = 0;
        _D3DContext->OMSetDepthStencilState(_DepthTested.Get(), 0);
        _D3DContext->IASetInputLayout(_MeshLayout.Get());
        _D3DContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _D3DContext->IASetVertexBuffers(0, 1, _MeshVertices.GetAddressOf(), &Stride, &Offset);
        _D3DContext->IASetIndexBuffer(_MeshIndices.Get(), DXGI_FORMAT_R32_UINT, 0);
        _D3DContext->VSSetShader(_MeshVertexShader.Get(), nullptr, 0);
        _D3DContext->VSSetShaderResources(0, 1, _BonesView.GetAddressOf());
        _D3DContext->PSSetShader(_MeshPixelShader.Get(), nullptr, 0);
        _D3DContext->PSSetSamplers(0, 1, _Sampler.GetAddressOf());
        for (const SubMesh& Mesh : Frame.Meshes) {
            if (!Mesh.Visible || Mesh.IndexCount == 0) continue;
            ID3D11ShaderResourceView* Albedo = _DefaultAlbedo.Get();
            if (Mesh.Albedo >= 0 && size_t(Mesh.Albedo) < _Textures.size() && _Textures[Mesh.Albedo])
                Albedo = _Textures[Mesh.Albedo].Get();
            _D3DContext->PSSetShaderResources(1, 1, &Albedo);
            _D3DContext->DrawIndexed(Mesh.IndexCount, Mesh.IndexStart, 0);
        }
    }

    ImTextureID Renderer::ViewportTexture() const {
        return reinterpret_cast<ImTextureID>(_ResolvedView.Get());
    }
}  // namespace Scarlet

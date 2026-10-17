// Copyright (C) 2026 Jake Rieger
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, version 3. This program is distributed in the hope that it will
// be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details. You should have received a copy of the GNU General
// Public License along with this program. If not, see https://www.gnu.org/licenses/.

#include "Model.hpp"

#include <libSouls/Binders/BND4.hpp>
#include <libSouls/Formats/TPF.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

using namespace DirectX;

namespace Scarlet {
    namespace {
        XMFLOAT4X4 ToFloat4x4(const Souls::Matrix4x4& M) {
            // Both are 16 floats, row by row.
            XMFLOAT4X4 Result;
            static_assert(sizeof(Souls::Matrix4x4) == sizeof(XMFLOAT4X4));
            std::memcpy(&Result, &M, sizeof(Result));
            return Result;
        }

        std::string Lower(std::string Text) {
            for (char& C : Text)
                C = static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
            return Text;
        }

        bool EndsWithI(const std::string& Text, std::string_view Suffix) {
            if (Text.size() < Suffix.size()) return false;
            for (size_t I = 0; I < Suffix.size(); ++I) {
                if (std::tolower(static_cast<unsigned char>(Text[Text.size() - Suffix.size() + I])) != Suffix[I])
                    return false;
            }
            return true;
        }
    }  // namespace

    std::shared_ptr<CharacterModel> CharacterModel::Load(const GameData& Game, const std::string& Id) {
        const auto Bytes = Game.Read("/chr/" + Id + ".chrbnd.dcx");
        if (!Bytes) return nullptr;
        const Souls::BND4 Binder = Souls::BND4::Read(*Bytes);
        for (const Souls::BinderFile& File : Binder.Files) {
            if (!File.Name || !EndsWithI(*File.Name, ".flver")) continue;
            const Souls::FLVER2 Flver = Souls::FLVER2::Read(File.Bytes);
            if (Flver.Meshes.empty()) return nullptr;
            auto Result  = std::make_shared<CharacterModel>();
            Result->Name = File.Name->substr(File.Name->find_last_of("\\/") + 1);
            Result->Build(Flver);
            Result->FindTextures(Game, Id, Flver);
            return Result;
        }
        return nullptr;
    }

    void CharacterModel::Build(const Souls::FLVER2& Flver) {
        // The bones: bind pose, parents, and an order that puts parents first (a FLVER parent can come after its
        // child).
        const size_t Count = Flver.Bones.size();
        _BoneNames.resize(Count);
        _Parents.resize(Count);
        _BindLocal.resize(Count);
        for (size_t I = 0; I < Count; ++I) {
            const auto& Bone = Flver.Bones[I];
            _BoneNames[I]    = Bone.Name;
            _Parents[I]      = Bone.ParentIndex >= 0 && size_t(Bone.ParentIndex) < Count ? Bone.ParentIndex : -1;
            _BindLocal[I]    = ToFloat4x4(Bone.ComputeLocalTransform());
        }
        std::vector<int> Depth(Count, -1);
        for (size_t I = 0; I < Count; ++I) {
            int D = 0;
            for (int P = _Parents[I]; P >= 0 && D < 1024; P = _Parents[P])
                ++D;
            Depth[I] = D;
        }
        _Order.resize(Count);
        for (size_t I = 0; I < Count; ++I)
            _Order[I] = static_cast<int>(I);
        std::stable_sort(_Order.begin(), _Order.end(), [&](int A, int B) { return Depth[A] < Depth[B]; });

        std::vector<XMFLOAT4X4> BindWorld(Count);
        _InverseBindWorld.resize(Count);
        for (const int I : _Order) {
            XMMATRIX World = XMLoadFloat4x4(&_BindLocal[I]);
            if (_Parents[I] >= 0) World = XMMatrixMultiply(World, XMLoadFloat4x4(&BindWorld[_Parents[I]]));
            XMStoreFloat4x4(&BindWorld[I], World);
            XMStoreFloat4x4(&_InverseBindWorld[I], XMMatrixInverse(nullptr, World));
        }
        _SkeletonBone.assign(Count, -1);

        // The meshes: the full-detail face set of each (not the lower levels of detail or the motion blur copies).
        for (const auto& Mesh : Flver.Meshes) {
            const Souls::FLVER2::FaceSet* Faces = nullptr;
            for (const auto& Set : Mesh.FaceSets) {
                if (Set.Flags == Souls::FLVER2::FaceSet::FSFlags::None) {
                    Faces = &Set;
                    break;
                }
            }
            if (!Faces && !Mesh.FaceSets.empty()) Faces = &Mesh.FaceSets[0];
            if (!Faces) continue;

            const uint32_t Base = static_cast<uint32_t>(Vertices.size());
            for (const auto& V : Mesh.Vertices) {
                MeshVertex Out;
                Out.Position = {V.Position.X, V.Position.Y, V.Position.Z};
                Out.Normal   = {V.Normal.X, V.Normal.Y, V.Normal.Z};
                if (!V.UVs.empty()) Out.UV = {V.UVs[0].X, V.UVs[0].Y};
                float Total = 0.f;
                for (int K = 0; K < 4; ++K) {
                    // Older games index the mesh's own bone list; Elden Ring indexes the FLVER's bones directly.
                    int Bone = V.BoneIndices[K];
                    if (!Mesh.BoneIndices.empty() && Bone >= 0 && size_t(Bone) < Mesh.BoneIndices.size())
                        Bone = Mesh.BoneIndices[Bone];
                    Out.Bones[K]   = static_cast<uint16_t>(Bone >= 0 && size_t(Bone) < Count ? Bone : 0);
                    Out.Weights[K] = V.BoneWeights[K];
                    Total += V.BoneWeights[K];
                }
                if (Total > 0.f) {
                    for (float& W : Out.Weights)
                        W /= Total;
                } else {
                    Out.Weights[0] = 1.f;  // rigidly bound to its first bone
                }
                Vertices.push_back(Out);
            }

            SubMesh Sub;
            Sub.IndexStart = static_cast<uint32_t>(Indices.size());
            for (const int32_t Index : Faces->Triangulate(Mesh.Vertices.size() < 0xFFFF)) {
                if (Index >= 0 && size_t(Index) < Mesh.Vertices.size()) Indices.push_back(Base + uint32_t(Index));
            }
            Indices.resize(Sub.IndexStart + (Indices.size() - Sub.IndexStart) / 3 * 3);
            Sub.IndexCount = static_cast<uint32_t>(Indices.size()) - Sub.IndexStart;
            if (Mesh.MaterialIndex >= 0 && size_t(Mesh.MaterialIndex) < Flver.Materials.size()) {
                Sub.Material      = Flver.Materials[Mesh.MaterialIndex].Name;
                Sub.MaterialIndex = Mesh.MaterialIndex;
            }
            Meshes.push_back(std::move(Sub));
        }
    }

    void CharacterModel::FindTextures(const GameData& Game, const std::string& Id, const Souls::FLVER2& Flver) {
        // The character's own textures: /chr/<Id>_h.texbnd.dcx holds TPFs of DDS textures, named like "c4290_BD_a".
        std::unordered_map<std::string, const Souls::TPF::Texture*> ByName;
        std::vector<Souls::TPF> Tpfs;
        for (const char* Suffix : {"_h.texbnd.dcx", "_l.texbnd.dcx"}) {
            const auto Bytes = Game.Read("/chr/" + Id + Suffix);
            if (!Bytes) continue;
            const Souls::BND4 Binder = Souls::BND4::Read(*Bytes);
            for (const Souls::BinderFile& File : Binder.Files) {
                if (File.Name && EndsWithI(*File.Name, ".tpf")) Tpfs.push_back(Souls::TPF::Read(File.Bytes));
            }
            break;
        }
        for (const Souls::TPF& Tpf : Tpfs) {
            for (const auto& Texture : Tpf.Textures)
                ByName.emplace(Lower(Texture.Name), &Texture);
        }
        // Then the shared detail textures, for materials that only tile one of those.
        for (const auto& Texture : Game.CommonTextures().Textures)
            ByName.emplace(Lower(Texture.Name), &Texture);

        // Each material's base color: the first albedo sampler of its matbin whose texture the character has (a
        // material can also have detail albedos that tile over it, kept elsewhere).
        std::unordered_map<std::string, int> Added;
        auto Resolve = [&](const std::string& Path) -> int {
            std::string Name = Lower(Path.substr(Path.find_last_of("\\/") + 1));
            Name             = Name.substr(0, Name.find_last_of('.'));
            if (Name.empty()) return -1;
            if (const auto Found = Added.find(Name); Found != Added.end()) return Found->second;
            const auto Texture = ByName.find(Name);
            if (Texture == ByName.end()) return -1;
            Textures.push_back({Texture->second->Name, Texture->second->Bytes});
            return Added[Name] = static_cast<int>(Textures.size() - 1);
        };
        std::vector<int> ByMaterial(Flver.Materials.size(), -1);
        for (size_t M = 0; M < Flver.Materials.size(); ++M) {
            const auto& Material = Flver.Materials[M];
            if (const auto Bin = Game.FindMaterial(Material.MTD)) {
                for (const auto& Sampler : Bin->Samplers) {
                    if (Sampler.Type.find("AlbedoMap") == std::string::npos || Sampler.Path.empty()) continue;
                    if ((ByMaterial[M] = Resolve(Sampler.Path)) >= 0) break;
                }
            }
            // Older materials name their textures in the FLVER itself.
            for (size_t T = 0; ByMaterial[M] < 0 && T < Material.Textures.size(); ++T) {
                const auto& Texture = Material.Textures[T];
                if (Texture.Type.find("Albedo") != std::string::npos ||
                    Texture.Type.find("Diffuse") != std::string::npos)
                    ByMaterial[M] = Resolve(Texture.Path);
            }
        }
        for (SubMesh& Mesh : Meshes) {
            if (Mesh.MaterialIndex >= 0) Mesh.Albedo = ByMaterial[Mesh.MaterialIndex];
        }
    }

    void CharacterModel::BindSkeleton(const Souls::Havok::Skeleton& Skeleton) {
        for (size_t I = 0; I < _BoneNames.size(); ++I)
            _SkeletonBone[I] = Skeleton.FindBone(_BoneNames[I]);
    }

    size_t CharacterModel::BoundBoneCount() const {
        return static_cast<size_t>(
          std::count_if(_SkeletonBone.begin(), _SkeletonBone.end(), [](int B) { return B >= 0; }));
    }

    void CharacterModel::SkinMatrices(std::span<const Souls::Havok::QsTransform> ModelPose,
                                      std::vector<XMFLOAT4X4>& Out) const {
        // Each bone's posed model-space matrix: from the skeleton if it drives the bone, otherwise its bind pose
        // relative to its (posed) parent.
        std::vector<XMFLOAT4X4> World(_BindLocal.size());
        for (const int I : _Order) {
            const int Bone = _SkeletonBone[I];
            if (Bone >= 0 && size_t(Bone) < ModelPose.size()) {
                World[I] = ToFloat4x4(ModelPose[Bone].ToMatrix());
            } else if (_Parents[I] >= 0) {
                XMStoreFloat4x4(&World[I],
                                XMMatrixMultiply(XMLoadFloat4x4(&_BindLocal[I]), XMLoadFloat4x4(&World[_Parents[I]])));
            } else {
                World[I] = _BindLocal[I];
            }
        }
        Out.resize(World.size());
        for (size_t I = 0; I < World.size(); ++I) {
            XMStoreFloat4x4(&Out[I],
                            XMMatrixMultiply(XMLoadFloat4x4(&_InverseBindWorld[I]), XMLoadFloat4x4(&World[I])));
        }
    }
}  // namespace Scarlet

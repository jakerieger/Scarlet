// Copyright (C) 2026 Jake Rieger
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, version 3. This program is distributed in the hope that it will
// be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details. You should have received a copy of the GNU General
// Public License along with this program. If not, see https://www.gnu.org/licenses/.

#pragma once

#include "GameData.hpp"

#include <DirectXMath.h>
#include <libSouls/Formats/FLVER/FLVER2.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Scarlet {
    /// @brief A vertex as the mesh shader takes it: model-space position and normal, four bones and their weights.
    struct MeshVertex {
        DirectX::XMFLOAT3 Position;
        DirectX::XMFLOAT3 Normal;
        uint16_t Bones[4] {};
        float Weights[4] {};
        DirectX::XMFLOAT2 UV;
    };

    /// @brief A mesh of a model: a range of the model's indices drawn with one material.
    struct SubMesh {
        uint32_t IndexStart = 0;
        uint32_t IndexCount = 0;
        std::string Material;    ///< The material's name.
        int MaterialIndex = -1;  ///< The FLVER material.
        int Albedo        = -1;  ///< The model's texture for its base color, or -1 for none found.
        bool Visible      = true;
    };

    /// @brief A texture of a model, as the game stores it.
    struct ModelTexture {
        std::string Name;          ///< Such as "c4290_BD_a".
        std::vector<uint8_t> Dds;  ///< The DDS file.
    };

    /// @brief A character's model (its FLVER), ready to draw skinned to an animated Havok skeleton.
    ///
    /// The FLVER's bones are matched to the skeleton's by name. Bones the skeleton doesn't have (cloth and other
    /// helper bones) keep their bind pose relative to their parent.
    class CharacterModel {
    public:
        std::string Name;                    ///< The FLVER's file name.
        std::vector<MeshVertex> Vertices;    ///< Every mesh's vertices.
        std::vector<uint32_t> Indices;       ///< Triangle lists, indexing Vertices.
        std::vector<SubMesh> Meshes;         ///< One per FLVER mesh.
        std::vector<ModelTexture> Textures;  ///< The textures the meshes use.

        /// @brief Reads /chr/<Id>.chrbnd.dcx's model. nullptr if the character has no model with meshes (such as the
        ///        player, whose body is assembled from parts); throws if the model can't be read.
        static std::shared_ptr<CharacterModel> Load(const GameData& Game, const std::string& Id);

        /// @brief Matches the FLVER's bones to a skeleton's, by name. Call before SkinMatrices.
        void BindSkeleton(const Souls::Havok::Skeleton& Skeleton);
        /// @brief How many of the FLVER's bones the skeleton drives.
        size_t BoundBoneCount() const;
        /// @brief How many bones the FLVER has (the size of SkinMatrices' output).
        size_t BoneCount() const { return _BindLocal.size(); }

        /// @brief The skinning matrix of each FLVER bone (inverse bind pose, then the posed bone), for a pose of the
        ///        skeleton in model space. Row-vector convention (v * M).
        void SkinMatrices(std::span<const Souls::Havok::QsTransform> ModelPose,
                          std::vector<DirectX::XMFLOAT4X4>& Out) const;

    private:
        std::vector<DirectX::XMFLOAT4X4> _BindLocal;         ///< each bone relative to its parent
        std::vector<DirectX::XMFLOAT4X4> _InverseBindWorld;  ///< model space to each bone's space, in the bind pose
        std::vector<int> _Parents;                           ///< each bone's FLVER parent, or -1
        std::vector<int> _Order;                             ///< bones with parents before children
        std::vector<std::string> _BoneNames;
        std::vector<int> _SkeletonBone;  ///< each FLVER bone's skeleton bone, or -1

        void Build(const Souls::FLVER2& Flver);
        void FindTextures(const GameData& Game, const std::string& Id, const Souls::FLVER2& Flver);
    };
}  // namespace Scarlet

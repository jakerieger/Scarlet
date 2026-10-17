// Copyright (C) 2026 Jake Rieger
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, version 3. This program is distributed in the hope that it will
// be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details. You should have received a copy of the GNU General
// Public License along with this program. If not, see https://www.gnu.org/licenses/.

#pragma once

#include <libSouls/Formats/Havok/Animation.hpp>

#include <map>
#include <span>
#include <vector>

namespace Scarlet {
    /// @brief A bone's offset from the animation at a frame.
    struct BoneKey {
        float Frame = 0.f;
        Souls::Vector3 Translation;      ///< Added to the bone's translation, in its parent's space.
        Souls::Vector3 RotationDegrees;  ///< Turned about the bone's own X, then Y, then Z axes.

        bool operator==(const BoneKey&) const = default;
    };

    /// @brief Edits to a clip: keyed offsets per bone, layered over the animation without changing it.
    ///
    /// Between two keys a bone's offset is interpolated linearly; before its first key and after its last it holds
    /// that key's offset (so one key offsets the whole animation).
    class EditLayer {
    public:
        /// @brief Each edited bone's keys, in frame order, by skeleton bone.
        std::map<int, std::vector<BoneKey>> Keys;

        /// @brief Whether there are no edits.
        bool Empty() const;
        /// @brief A bone's offset at a frame (zero if the bone has no keys).
        BoneKey OffsetAt(int Bone, float Frame) const;
        /// @brief The bone's key at a frame (within half a frame), or nullptr.
        const BoneKey* KeyAt(int Bone, float Frame) const;
        /// @brief Adds a key, or replaces the bone's key at the same frame.
        void SetKey(int Bone, const BoneKey& Key);
        /// @brief Removes the bone's key at a frame, if it has one.
        void RemoveKey(int Bone, float Frame);
        /// @brief Removes all of a bone's keys.
        void ClearBone(int Bone);

        /// @brief Applies the offsets at a frame to a local pose (one transform per skeleton bone).
        void Apply(float Frame, std::span<Souls::Havok::QsTransform> Pose) const;
        /// @brief A local transform with an offset applied.
        static Souls::Havok::QsTransform Apply(const Souls::Havok::QsTransform& Base, const BoneKey& Offset);

        bool operator==(const EditLayer&) const = default;
    };
}  // namespace Scarlet

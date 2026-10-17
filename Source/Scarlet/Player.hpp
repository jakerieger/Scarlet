// Copyright (C) 2026 Jake Rieger
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, version 3. This program is distributed in the hope that it will
// be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details. You should have received a copy of the GNU General
// Public License along with this program. If not, see https://www.gnu.org/licenses/.

#pragma once

#include "GameData.hpp"

#include <memory>
#include <vector>

namespace Scarlet {
    /// @brief Plays a clip on a skeleton: the playhead, and the pose it gives.
    class Player {
    public:
        float Time      = 0.f;   ///< The playhead, in seconds.
        float Speed     = 1.f;   ///< Playback rate.
        bool Playing    = true;  ///< Whether the playhead advances.
        bool Loop       = true;  ///< Whether playback wraps at the end (or stops).
        bool RootMotion = true;  ///< Whether to move the character by the clip's root motion.

        /// @brief Starts playing a clip from the beginning (nullptr to show the reference pose).
        void SetClip(std::shared_ptr<const Clip> NewClip);
        const std::shared_ptr<const Clip>& CurrentClip() const { return _Current; }

        /// @brief Advances the playhead by a frame's time.
        void Update(float DeltaSeconds);
        /// @brief Moves the playhead to a frame (clamped to the clip).
        void SeekFrame(float Frame);
        /// @brief The playhead as a frame number (fractional between frames).
        float Frame() const;
        /// @brief The clip's duration, or 0 without a clip.
        float Duration() const;

        /// @brief The skeleton's local pose at the playhead: the reference pose with the clip applied.
        std::vector<Souls::Havok::QsTransform> LocalPose(const Souls::Havok::Skeleton& Skeleton) const;
        /// @brief The pose in model space, moved by root motion if that's on.
        std::vector<Souls::Havok::QsTransform> ModelPose(const Souls::Havok::Skeleton& Skeleton) const;

    private:
        std::shared_ptr<const Clip> _Current;
    };
}  // namespace Scarlet

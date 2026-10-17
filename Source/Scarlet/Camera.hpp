// Copyright (C) 2026 Jake Rieger
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, version 3. This program is distributed in the hope that it will
// be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details. You should have received a copy of the GNU General
// Public License along with this program. If not, see https://www.gnu.org/licenses/.

#pragma once

#include <DirectXMath.h>

namespace Scarlet {
    /// @brief A camera that orbits a target point (Y up, left-handed like Direct3D).
    class OrbitCamera {
    public:
        DirectX::XMFLOAT3 Target {0.f, 1.f, 0.f};  ///< The point the camera looks at and orbits.
        float Yaw         = 0.6f;                   ///< Around the up axis, in radians.
        float Pitch       = 0.25f;                  ///< Above (positive) or below the target, in radians.
        float Distance    = 4.f;                    ///< From the target.
        float FieldOfView = 0.8f;                   ///< Vertical, in radians.

        /// @brief Turns the camera around the target by a mouse movement in pixels.
        void Orbit(float DeltaX, float DeltaY);
        /// @brief Moves the target across the view by a mouse movement in pixels.
        void Pan(float DeltaX, float DeltaY, float ViewportHeight);
        /// @brief Moves toward (positive) or away from the target by mouse wheel steps.
        void Zoom(float WheelSteps);
        /// @brief Looks at a sphere so it fills the view.
        void Frame(DirectX::XMFLOAT3 Center, float Radius);

        DirectX::XMVECTOR Eye() const;
        DirectX::XMMATRIX View() const;
        DirectX::XMMATRIX Projection(float AspectRatio) const;
    };
}  // namespace Scarlet

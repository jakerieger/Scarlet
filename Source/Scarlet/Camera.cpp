// Copyright (C) 2026 Jake Rieger
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, version 3. This program is distributed in the hope that it will
// be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details. You should have received a copy of the GNU General
// Public License along with this program. If not, see https://www.gnu.org/licenses/.

#include "Camera.hpp"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace Scarlet {
    void OrbitCamera::Orbit(float DeltaX, float DeltaY) {
        constexpr float Speed = 0.008f;
        Yaw += DeltaX * Speed;
        Pitch = std::clamp(Pitch + DeltaY * Speed, -1.55f, 1.55f);
    }

    void OrbitCamera::Pan(float DeltaX, float DeltaY, float ViewportHeight) {
        // Move by as much as the mouse moved across the view, at the target's depth.
        const float Scale     = 2.f * Distance * std::tan(FieldOfView * 0.5f) / std::max(ViewportHeight, 1.f);
        const XMMATRIX ToView = View();
        const XMMATRIX ToWorld = XMMatrixInverse(nullptr, ToView);
        const XMVECTOR Right  = ToWorld.r[0];
        const XMVECTOR Up     = ToWorld.r[1];
        XMVECTOR NewTarget    = XMLoadFloat3(&Target);
        NewTarget             = XMVectorAdd(NewTarget, XMVectorScale(Right, -DeltaX * Scale));
        NewTarget             = XMVectorAdd(NewTarget, XMVectorScale(Up, DeltaY * Scale));
        XMStoreFloat3(&Target, NewTarget);
    }

    void OrbitCamera::Zoom(float WheelSteps) {
        Distance = std::clamp(Distance * std::pow(0.88f, WheelSteps), 0.05f, 500.f);
    }

    void OrbitCamera::Frame(XMFLOAT3 Center, float Radius) {
        Target   = Center;
        Distance = std::max(Radius, 0.1f) / std::sin(FieldOfView * 0.5f);
    }

    XMVECTOR OrbitCamera::Eye() const {
        const XMVECTOR Offset = XMVectorSet(std::cos(Pitch) * std::sin(Yaw) * Distance,
                                            std::sin(Pitch) * Distance,
                                            -std::cos(Pitch) * std::cos(Yaw) * Distance,
                                            0.f);
        return XMVectorAdd(XMLoadFloat3(&Target), Offset);
    }

    XMMATRIX OrbitCamera::View() const {
        return XMMatrixLookAtLH(Eye(), XMLoadFloat3(&Target), XMVectorSet(0.f, 1.f, 0.f, 0.f));
    }

    XMMATRIX OrbitCamera::Projection(float AspectRatio) const {
        return XMMatrixPerspectiveFovLH(FieldOfView, AspectRatio, 0.01f, 1000.f);
    }
}  // namespace Scarlet

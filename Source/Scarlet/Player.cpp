// Copyright (C) 2026 Jake Rieger
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, version 3. This program is distributed in the hope that it will
// be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details. You should have received a copy of the GNU General
// Public License along with this program. If not, see https://www.gnu.org/licenses/.

#include "Player.hpp"

#include <algorithm>
#include <cmath>

using Souls::Havok::QsTransform;

namespace Scarlet {
    void Player::SetClip(std::shared_ptr<const Clip> NewClip) {
        _Current = std::move(NewClip);
        Time     = 0.f;
    }

    float Player::Duration() const {
        return _Current ? _Current->Animation.Duration : 0.f;
    }

    void Player::Update(float DeltaSeconds) {
        if (!_Current || !Playing) return;
        const float Length = Duration();
        Time += DeltaSeconds * Speed;
        if (Length <= 0.f) {
            Time = 0.f;
        } else if (Loop) {
            Time = std::fmod(Time, Length);
            if (Time < 0.f) Time += Length;
        } else if (Time >= Length || Time < 0.f) {
            Time    = std::clamp(Time, 0.f, Length);
            Playing = false;
        }
    }

    float Player::Frame() const {
        if (!_Current || _Current->Animation.FrameDuration <= 0.f) return 0.f;
        return Time / _Current->Animation.FrameDuration;
    }

    void Player::SeekFrame(float Frame) {
        if (!_Current) return;
        const auto& A = _Current->Animation;
        Time          = std::clamp(Frame, 0.f, float(A.FrameCount > 0 ? A.FrameCount - 1 : 0)) * A.FrameDuration;
    }

    std::vector<QsTransform> Player::LocalPose(const Souls::Havok::Skeleton& Skeleton) const {
        std::vector<QsTransform> Pose = Skeleton.ReferencePose;
        if (_Current) _Current->Binding.Apply(_Current->Animation, Time, Pose);
        if (_Current && Edits) Edits->Apply(Frame(), Pose);
        return Pose;
    }

    std::vector<QsTransform> Player::ModelPose(const Souls::Havok::Skeleton& Skeleton) const {
        std::vector<QsTransform> Pose = Skeleton.ModelSpace(LocalPose(Skeleton));
        if (!RootMotion || !_Current || !_Current->Animation.RootMotion) return Pose;

        // Root motion: a translation, and a turn of W radians about the up axis.
        const auto& Motion          = *_Current->Animation.RootMotion;
        const Souls::Vector4 Sample = Motion.Sample(Time);
        Souls::Vector3 Up {Motion.Up.X, Motion.Up.Y, Motion.Up.Z};
        const float Length = std::sqrt(Up.X * Up.X + Up.Y * Up.Y + Up.Z * Up.Z);
        Up =
          Length > 0.f ? Souls::Vector3 {Up.X / Length, Up.Y / Length, Up.Z / Length} : Souls::Vector3 {0.f, 1.f, 0.f};
        const float Half = Sample.W * 0.5f;
        QsTransform Root;
        Root.Translation = {Sample.X, Sample.Y, Sample.Z};
        Root.Rotation    = {Up.X * std::sin(Half), Up.Y * std::sin(Half), Up.Z * std::sin(Half), std::cos(Half)};
        for (QsTransform& Bone : Pose)
            Bone = QsTransform::Multiply(Root, Bone);
        return Pose;
    }
}  // namespace Scarlet

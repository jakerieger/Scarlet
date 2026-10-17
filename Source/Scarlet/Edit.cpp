// Copyright (C) 2026 Jake Rieger
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, version 3. This program is distributed in the hope that it will
// be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details. You should have received a copy of the GNU General
// Public License along with this program. If not, see https://www.gnu.org/licenses/.

#include "Edit.hpp"

#include <algorithm>
#include <cmath>

using Souls::Havok::QsTransform;

namespace Scarlet {
    namespace {
        // A rotation of Radians about an axis, as a QsTransform (so Multiply composes it).
        QsTransform Turn(float X, float Y, float Z, float Radians) {
            const float S = std::sin(Radians * 0.5f);
            QsTransform Result;
            Result.Rotation = {X * S, Y * S, Z * S, std::cos(Radians * 0.5f)};
            return Result;
        }

        constexpr float DegreesToRadians = 3.14159265f / 180.f;
    }  // namespace

    bool EditLayer::Empty() const {
        return std::all_of(Keys.begin(), Keys.end(), [](const auto& Entry) { return Entry.second.empty(); });
    }

    BoneKey EditLayer::OffsetAt(int Bone, float Frame) const {
        BoneKey Result;
        Result.Frame     = Frame;
        const auto Entry = Keys.find(Bone);
        if (Entry == Keys.end() || Entry->second.empty()) return Result;
        const auto& List = Entry->second;
        if (Frame <= List.front().Frame) return {Frame, List.front().Translation, List.front().RotationDegrees};
        if (Frame >= List.back().Frame) return {Frame, List.back().Translation, List.back().RotationDegrees};

        // Between two keys: linear.
        const auto Next =
          std::upper_bound(List.begin(), List.end(), Frame, [](float F, const BoneKey& K) { return F < K.Frame; });
        const BoneKey& B = *Next;
        const BoneKey& A = *(Next - 1);
        const float T    = B.Frame > A.Frame ? (Frame - A.Frame) / (B.Frame - A.Frame) : 0.f;
        auto Lerp        = [T](const Souls::Vector3& P, const Souls::Vector3& Q) {
            return Souls::Vector3 {P.X + (Q.X - P.X) * T, P.Y + (Q.Y - P.Y) * T, P.Z + (Q.Z - P.Z) * T};
        };
        Result.Translation     = Lerp(A.Translation, B.Translation);
        Result.RotationDegrees = Lerp(A.RotationDegrees, B.RotationDegrees);
        return Result;
    }

    const BoneKey* EditLayer::KeyAt(int Bone, float Frame) const {
        const auto Entry = Keys.find(Bone);
        if (Entry == Keys.end()) return nullptr;
        for (const BoneKey& Key : Entry->second) {
            if (std::abs(Key.Frame - Frame) < 0.5f) return &Key;
        }
        return nullptr;
    }

    void EditLayer::SetKey(int Bone, const BoneKey& Key) {
        auto& List = Keys[Bone];
        for (BoneKey& Existing : List) {
            if (std::abs(Existing.Frame - Key.Frame) < 0.5f) {
                Existing = Key;
                return;
            }
        }
        List.push_back(Key);
        std::sort(List.begin(), List.end(), [](const BoneKey& A, const BoneKey& B) { return A.Frame < B.Frame; });
    }

    void EditLayer::RemoveKey(int Bone, float Frame) {
        const auto Entry = Keys.find(Bone);
        if (Entry == Keys.end()) return;
        std::erase_if(Entry->second, [Frame](const BoneKey& Key) { return std::abs(Key.Frame - Frame) < 0.5f; });
        if (Entry->second.empty()) Keys.erase(Entry);
    }

    void EditLayer::ClearBone(int Bone) {
        Keys.erase(Bone);
    }

    QsTransform EditLayer::Apply(const QsTransform& Base, const BoneKey& Offset) {
        // The rotation turns about the bone's own axes (after its animated rotation); the translation moves it in its
        // parent's space.
        const auto& R = Offset.RotationDegrees;
        QsTransform Spin =
          QsTransform::Multiply(QsTransform::Multiply(Scarlet::Turn(1.f, 0.f, 0.f, R.X * DegreesToRadians),
                                                      Scarlet::Turn(0.f, 1.f, 0.f, R.Y * DegreesToRadians)),
                                Scarlet::Turn(0.f, 0.f, 1.f, R.Z * DegreesToRadians));
        QsTransform Rotated;
        Rotated.Rotation   = Base.Rotation;
        QsTransform Result = Base;
        Result.Rotation    = QsTransform::Multiply(Rotated, Spin).Rotation;
        Result.Translation = {Base.Translation.X + Offset.Translation.X,
                              Base.Translation.Y + Offset.Translation.Y,
                              Base.Translation.Z + Offset.Translation.Z};
        return Result;
    }

    void EditLayer::Apply(float Frame, std::span<QsTransform> Pose) const {
        for (const auto& [Bone, List] : Keys) {
            if (!List.empty() && Bone >= 0 && size_t(Bone) < Pose.size())
                Pose[Bone] = Apply(Pose[Bone], OffsetAt(Bone, Frame));
        }
    }
}  // namespace Scarlet

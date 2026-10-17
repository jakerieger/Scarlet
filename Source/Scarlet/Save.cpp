// Copyright (C) 2026 Jake Rieger
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, version 3. This program is distributed in the hope that it will
// be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details. You should have received a copy of the GNU General
// Public License along with this program. If not, see https://www.gnu.org/licenses/.

#include "Save.hpp"

#include <algorithm>
#include <stdexcept>

namespace fs = std::filesystem;

namespace Scarlet {
    namespace {
        std::string LowerBaseName(const std::string& Path) {
            std::string Name = Path.substr(Path.find_last_of("\\/") + 1);
            std::transform(Name.begin(), Name.end(), Name.begin(), [](unsigned char C) {
                return static_cast<char>(std::tolower(C));
            });
            return Name;
        }
    }  // namespace

    std::vector<uint8_t> EncodeEditedClip(const Clip& Source, const EditLayer& Edits) {
        const Souls::Havok::Animation& Animation      = Source.Animation;
        const uint32_t Tracks                         = Animation.TrackCount;
        std::vector<Souls::Havok::QsTransform> Frames = Animation.Bake();

        // The edits, on the tracks of the bones they offset.
        for (uint32_t Track = 0; Track < Tracks && Track < Source.Binding.TrackToBone.size(); ++Track) {
            const int Bone = Source.Binding.TrackToBone[Track];
            if (Edits.Keys.find(Bone) == Edits.Keys.end()) continue;
            for (uint32_t Frame = 0; Frame < Animation.FrameCount; ++Frame) {
                auto& Transform = Frames[size_t(Frame) * Tracks + Track];
                Transform       = EditLayer::Apply(Transform, Edits.OffsetAt(Bone, float(Frame)));
            }
        }

        Souls::Havok::Animation Edited =
          Souls::Havok::Animation::FromFrames(std::move(Frames), Tracks, Animation.FrameDuration);
        Edited.Duration   = Animation.Duration;
        Edited.RootMotion = Animation.RootMotion;

        Souls::Havok::Tagfile File = Source.File;
        const auto Objects         = File.ObjectsOfType("hkaAnimation");
        if (Objects.empty()) throw std::runtime_error(Source.Name + " has no animation to write into");
        Edited.WriteTo(File, Objects[0]);
        return File.Write();
    }

    void
    SaveBinder(const LoadedBinder& Source, const std::vector<BinderReplacement>& Replacements, const fs::path& Path) {
        Souls::BND4 Binder = fs::exists(Path) ? Souls::BND4::Read(Path) : Source.Binder;
        for (const BinderReplacement& Replacement : Replacements) {
            const std::string Wanted = LowerBaseName(Replacement.FileName);
            auto File                = std::find_if(Binder.Files.begin(), Binder.Files.end(), [&](const auto& F) {
                return F.Name && LowerBaseName(*F.Name) == Wanted;
            });
            if (File == Binder.Files.end())
                throw std::runtime_error(Replacement.FileName + " isn't in " + Path.filename().string());
            File->Bytes = Replacement.Bytes;
        }

        // Write next to the destination first, so a failure never leaves a broken file behind.
        fs::path Temporary = Path;
        Temporary += ".tmp";
        Binder.Write(Temporary);
        fs::rename(Temporary, Path);
    }
}  // namespace Scarlet

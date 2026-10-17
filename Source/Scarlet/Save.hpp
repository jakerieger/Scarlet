// Copyright (C) 2026 Jake Rieger
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, version 3. This program is distributed in the hope that it will
// be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details. You should have received a copy of the GNU General
// Public License along with this program. If not, see https://www.gnu.org/licenses/.

#pragma once

#include "Edit.hpp"
#include "GameData.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace Scarlet {
    /// @brief A clip's .hkx with its edits baked in: every frame of the animation with the edits applied, re-encoded
    ///        into a copy of the clip's tagfile.
    std::vector<uint8_t> EncodeEditedClip(const Clip& Source, const EditLayer& Edits);

    /// @brief A file to replace in a binder: its file name (as in the binder, without folders) and new contents.
    struct BinderReplacement {
        std::string FileName;
        std::vector<uint8_t> Bytes;
    };

    /// @brief Writes a binder with some files replaced to Path. If Path already exists (an earlier save into a mod
    ///        folder), that binder is the base, so changes already there are kept; otherwise Source is. Throws if a
    ///        file to replace isn't in the binder, or on failure to write (the file is replaced only once the new
    ///        one is written).
    void SaveBinder(const LoadedBinder& Source,
                    const std::vector<BinderReplacement>& Replacements,
                    const std::filesystem::path& Path);
}  // namespace Scarlet

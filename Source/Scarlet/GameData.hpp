// Copyright (C) 2026 Jake Rieger
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, version 3. This program is distributed in the hope that it will
// be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details. You should have received a copy of the GNU General
// Public License along with this program. If not, see https://www.gnu.org/licenses/.

#pragma once

#include <libSouls/Binders/ArchiveSet.hpp>
#include <libSouls/Binders/BND4.hpp>
#include <libSouls/Formats/Havok/Animation.hpp>
#include <libSouls/Formats/MATBIN.hpp>
#include <libSouls/Formats/TAE3.hpp>
#include <libSouls/Formats/TPF.hpp>

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Scarlet {
    /// @brief A path as UTF-8 text, for display.
    std::string ToUtf8(const std::filesystem::path& Path);

    /// @brief A character's animation binders, by game path: /chr/<Id>.anibnd.dcx (the skeleton) and any
    ///        /chr/<Id>_*.anibnd.dcx (more animations).
    struct CharacterFiles {
        std::string Id;                    ///< Such as "c0000".
        std::vector<std::string> Binders;  ///< Game paths, the main binder first.
    };

    /// @brief An Elden Ring game folder (the one with eldenring.exe): files are read loose if they're there (an
    ///        unpacked or modded game), otherwise from the game's archives.
    ///
    /// Reads are safe from several threads at once.
    class GameData {
    public:
        /// @brief Opens a game folder: its archives (with the keys and path lists in KeysFolder, as BinderKeys lays
        ///        them out) and any loose files, and points libSouls at the game's Oodle DLL. Throws
        ///        std::runtime_error with a message for the user if the folder isn't usable.
        static std::shared_ptr<const GameData> Open(const std::filesystem::path& Root,
                                                    const std::filesystem::path& KeysFolder);

        const std::filesystem::path& Root() const { return _Folder; }
        const std::vector<CharacterFiles>& Characters() const { return _Found; }
        /// @brief Where files come from, for display: "archives", "loose files" or both.
        const std::string& SourceDescription() const { return _Description; }

        /// @brief A file by its game path ("/chr/c0000.anibnd.dcx"), as stored (DCX-compressed if it is); nullopt if
        ///        the game doesn't have it.
        std::optional<std::vector<uint8_t>> Read(const std::string& GamePath) const;

        /// @brief The material a FLVER material uses, by the FLVER's MTD path (its .matbin has the same base name);
        ///        nullptr if the game has none. The game's material binders are read on first use.
        std::shared_ptr<const Souls::MATBIN> FindMaterial(const std::string& MtdPath) const;
        /// @brief The textures characters and equipment share (/parts/common_body.tpf.dcx: the tiling detail
        ///        textures, such as chain mail and fabric), read on first use; empty if the game has none.
        const Souls::TPF& CommonTextures() const;

    private:
        std::filesystem::path _Folder;
        Souls::ArchiveSet _Archives;
        std::vector<CharacterFiles> _Found;
        std::string _Description;

        // Materials: every .matbin of the game by lowercase base name, parsed when first asked for.
        mutable std::once_flag _MaterialsIndexed;
        mutable std::mutex _MaterialLock;
        mutable std::unordered_map<std::string, std::vector<uint8_t>> _MaterialFiles;
        mutable std::unordered_map<std::string, std::shared_ptr<const Souls::MATBIN>> _Materials;

        void IndexMaterials() const;

        mutable std::once_flag _CommonRead;
        mutable Souls::TPF _Common;
    };

    /// @brief An animation in one of a character's binders.
    struct AnimationEntry {
        std::string Name;  ///< The file name without extension, such as "a000_003000".
        size_t File = 0;   ///< Its index in the binder's files.
    };

    /// @brief One animation binder, read: its files, its compendium, the animations it holds and its TAE files (the
    ///        animations' events; a character's main binder has them).
    class LoadedBinder {
    public:
        std::string Name;
        Souls::BND4 Binder;
        std::optional<Souls::Havok::Compendium> Compendium;
        std::vector<AnimationEntry> Animations;
        std::unordered_map<std::string, size_t> TaeFiles;  ///< file indices by lowercase name without extension

        /// @brief Reads a binder from the game. Throws on failure.
        static std::unique_ptr<LoadedBinder> Load(const GameData& Game, const std::string& GamePath);
        /// @brief A TAE file of the binder by name ("a02", "c4290"), parsed on first use; nullptr if there's none.
        ///        Safe from several threads at once.
        std::shared_ptr<const Souls::TAE3> Tae(const std::string& Name) const;

    private:
        mutable std::mutex _TaeLock;
        mutable std::unordered_map<std::string, std::shared_ptr<const Souls::TAE3>> _Taes;
    };

    /// @brief An animation ready to play: decoded, with its binding to the skeleton, and its tagfile kept for saving.
    struct Clip {
        std::string Name;
        Souls::Havok::Tagfile File;
        Souls::Havok::Animation Animation;
        Souls::Havok::AnimationBinding Binding;
        /// @brief The animation's events (its TimeAct), if its TAE has it.
        std::optional<Souls::TAE3::Animation> Events;
        /// @brief The TAE file the events are from, such as "a02.tae".
        std::string EventSource;

        /// @brief Decodes an animation of a binder and finds its events. Main is the character's main binder: its
        ///        compendium is used if the binder has none, and its TAE files hold the events. Throws on failure.
        static std::shared_ptr<Clip>
        Load(const LoadedBinder& Binder, const AnimationEntry& Entry, const LoadedBinder* Main);

        /// @brief Finds the clip's events in the main binder's TAE files: aCCC_NNNNNN is in the player's per-category
        ///        "aCC.tae" (two digits at least), or in the character's own "<id>.tae".
        void FindEvents(const LoadedBinder& Main);
    };

    /// @brief The skeleton of a character, from its main binder. Throws if there is none.
    Souls::Havok::Skeleton LoadSkeleton(const LoadedBinder& Main);
}  // namespace Scarlet

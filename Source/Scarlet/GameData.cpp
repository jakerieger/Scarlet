// Copyright (C) 2026 Jake Rieger
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, version 3. This program is distributed in the hope that it will
// be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details. You should have received a copy of the GNU General
// Public License along with this program. If not, see https://www.gnu.org/licenses/.

#include "GameData.hpp"

#include <libSouls/Oodle26.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;

namespace Scarlet {
    namespace {
        constexpr std::string_view AnibndSuffix = ".anibnd.dcx";

        bool EndsWith(std::string_view Text, std::string_view Suffix) {
            return Text.size() >= Suffix.size() && Text.substr(Text.size() - Suffix.size()) == Suffix;
        }

        std::string Lower(std::string Text) {
            std::transform(Text.begin(), Text.end(), Text.begin(), [](unsigned char C) {
                return static_cast<char>(std::tolower(C));
            });
            return Text;
        }

        // The file name of a path, without its folders ("N:\GR\data\...\a000_003000.hkx" -> "a000_003000.hkx").
        std::string BaseName(const std::string& Path) {
            const size_t Slash = Path.find_last_of("\\/");
            return Slash == std::string::npos ? Path : Path.substr(Slash + 1);
        }

        std::vector<uint8_t> ReadAll(const fs::path& Path) {
            std::ifstream File(Path, std::ios::binary);
            return {std::istreambuf_iterator<char>(File), std::istreambuf_iterator<char>()};
        }

        // A game path ("/chr/c0000.anibnd.dcx") under the game folder.
        fs::path LoosePath(const fs::path& Root, const std::string& GamePath) {
            const std::string Relative = GamePath.empty() || GamePath[0] != '/' ? GamePath : GamePath.substr(1);
            return Root / fs::path(std::u8string(Relative.begin(), Relative.end()));
        }
    }  // namespace

    std::string ToUtf8(const fs::path& Path) {
        const std::u8string Text = Path.u8string();
        return {reinterpret_cast<const char*>(Text.data()), Text.size()};
    }

    std::shared_ptr<const GameData> GameData::Open(const fs::path& Root, const fs::path& KeysFolder) {
        const fs::path Oodle = Root / "oo2core_6_win64.dll";
        if (!fs::exists(Root / "eldenring.exe") || !fs::exists(Oodle)) {
            throw std::runtime_error("\"" + ToUtf8(Root) +
                                     "\" isn't an Elden Ring game folder. Choose the folder with eldenring.exe in it "
                                     "(ELDEN RING/Game).");
        }
        Souls::Oodle26::SetDLLPath(Oodle);  // false if already loaded from an earlier folder, which is fine

        auto Result     = std::make_shared<GameData>();
        Result->_Folder = Root;
        std::set<std::string> Binders;  // game paths, lowercase

        // The archives, and the anibnd paths of the dictionaries that they hold.
        if (fs::is_directory(KeysFolder / "Key")) {
            try {
                Result->_Archives =
                  Souls::ArchiveSet::OpenWithKeys(Root, Souls::BHD5::Game::EldenRing, KeysFolder / "Key");
            } catch (const std::exception& E) {
                throw std::runtime_error(std::string("Couldn't open the game's archives: ") + E.what());
            }
            if (fs::is_directory(KeysFolder / "Hash")) {
                for (const fs::directory_entry& Entry : fs::directory_iterator(KeysFolder / "Hash")) {
                    std::ifstream In(Entry.path());
                    std::string Line;
                    while (std::getline(In, Line)) {
                        if (!Line.empty() && Line.back() == '\r') Line.pop_back();
                        if (Line.rfind("/chr/", 0) == 0 && EndsWith(Line, AnibndSuffix) &&
                            Result->_Archives.Contains(Line))
                            Binders.insert(Lower(Line));
                    }
                }
            }
        }

        // Loose files (an unpacked or modded game) too.
        bool Loose = false;
        if (fs::is_directory(Root / "chr")) {
            for (const fs::directory_entry& Entry : fs::directory_iterator(Root / "chr")) {
                const std::string Name = Lower(ToUtf8(Entry.path().filename()));
                if (Entry.is_regular_file() && EndsWith(Name, AnibndSuffix)) {
                    Binders.insert("/chr/" + Name);
                    Loose = true;
                }
            }
        }
        if (Binders.empty()) {
            throw std::runtime_error(
              "Found no character animations in \"" + ToUtf8(Root) +
              "\": the game isn't unpacked, and its archives couldn't be read (are the BinderKeys files next to "
              "Scarlet.exe?).");
        }
        const bool Packed    = Result->_Archives.FileCount() != 0;
        Result->_Description = Packed && Loose ? "archives and loose files" : Packed ? "archives" : "loose files";

        std::map<std::string, CharacterFiles> ById;
        for (const std::string& Path : Binders) {
            const std::string Name = Path.substr(5);  // after "/chr/"
            const std::string Stem = Name.substr(0, Name.size() - AnibndSuffix.size());
            const std::string Id   = Stem.substr(0, Stem.find('_'));
            CharacterFiles& Files  = ById[Id];
            Files.Id               = Id;
            Files.Binders.push_back(Path);
        }
        for (auto& [Id, Files] : ById) {
            // The main binder (exactly <Id>.anibnd.dcx) first; the set already sorted the rest.
            const std::string Main = "/chr/" + Id + std::string(AnibndSuffix);
            std::stable_partition(Files.Binders.begin(), Files.Binders.end(), [&](const std::string& P) {
                return P == Main;
            });
            Result->_Found.push_back(std::move(Files));
        }
        return Result;
    }

    std::optional<std::vector<uint8_t>> GameData::Read(const std::string& GamePath) const {
        const fs::path Loose = LoosePath(_Folder, GamePath);
        if (fs::is_regular_file(Loose)) return ReadAll(Loose);
        return _Archives.Read(GamePath);
    }

    void GameData::IndexMaterials() const {
        // The base game's materials, then each DLC's (which override any of the same name).
        for (const char* Path : {"/material/allmaterial.matbinbnd.dcx",
                                 "/material/allmaterial_dlc01.matbinbnd.dcx",
                                 "/material/allmaterial_dlc02.matbinbnd.dcx"}) {
            const auto Bytes = Read(Path);
            if (!Bytes) continue;
            Souls::BND4 Binder = Souls::BND4::Read(*Bytes);
            for (Souls::BinderFile& File : Binder.Files) {
                if (File.Name) _MaterialFiles[Lower(BaseName(*File.Name))] = std::move(File.Bytes);
            }
        }
    }

    const Souls::TPF& GameData::CommonTextures() const {
        std::call_once(_CommonRead, [this] {
            if (const auto Bytes = Read("/parts/common_body.tpf.dcx")) _Common = Souls::TPF::Read(*Bytes);
        });
        return _Common;
    }

    std::shared_ptr<const Souls::MATBIN> GameData::FindMaterial(const std::string& MtdPath) const {
        std::call_once(_MaterialsIndexed, [this] { IndexMaterials(); });
        // "N:\...\C[c4290]_AM_Metal.mtd" -> "c[c4290]_am_metal.matbin"
        std::string Name = Lower(BaseName(MtdPath));
        Name             = Name.substr(0, Name.find_last_of('.')) + ".matbin";

        std::lock_guard Guard(_MaterialLock);
        if (const auto Found = _Materials.find(Name); Found != _Materials.end()) return Found->second;
        const auto File = _MaterialFiles.find(Name);
        if (File == _MaterialFiles.end()) return nullptr;
        auto Material    = std::make_shared<const Souls::MATBIN>(Souls::MATBIN::Read(File->second));
        _Materials[Name] = Material;
        return Material;
    }

    std::unique_ptr<LoadedBinder> LoadedBinder::Load(const GameData& Game, const std::string& GamePath) {
        const std::optional<std::vector<uint8_t>> Bytes = Game.Read(GamePath);
        if (!Bytes) throw std::runtime_error(GamePath + " isn't in the game");
        auto Result    = std::make_unique<LoadedBinder>();
        Result->Name   = BaseName(GamePath);
        Result->Binder = Souls::BND4::Read(*Bytes);
        for (size_t I = 0; I < Result->Binder.Files.size(); ++I) {
            const Souls::BinderFile& File = Result->Binder.Files[I];
            if (!File.Name) continue;
            const std::string Name = BaseName(*File.Name);
            const std::string Low  = Lower(Name);
            if (EndsWith(Low, ".compendium")) {
                Result->Compendium = Souls::Havok::Compendium::Read(File.Bytes);
            } else if (EndsWith(Low, ".tae")) {
                Result->TaeFiles[Low.substr(0, Low.size() - 4)] = I;
            } else if (EndsWith(Low, ".hkx") && Low.size() > 1 && Low[0] == 'a' && Low.find('_') != std::string::npos) {
                Result->Animations.push_back({Name.substr(0, Name.size() - 4), I});
            }
        }
        std::sort(Result->Animations.begin(), Result->Animations.end(), [](const auto& A, const auto& B) {
            return A.Name < B.Name;
        });
        return Result;
    }

    std::shared_ptr<const Souls::TAE3> LoadedBinder::Tae(const std::string& TaeName) const {
        const auto File = TaeFiles.find(TaeName);
        if (File == TaeFiles.end()) return nullptr;
        std::lock_guard Guard(_TaeLock);
        if (const auto Found = _Taes.find(TaeName); Found != _Taes.end()) return Found->second;
        auto Parsed    = std::make_shared<const Souls::TAE3>(Souls::TAE3::Read(Binder.Files[File->second].Bytes));
        _Taes[TaeName] = Parsed;
        return Parsed;
    }

    std::shared_ptr<Clip>
    Clip::Load(const LoadedBinder& Binder, const AnimationEntry& Entry, const LoadedBinder* Main) {
        const Souls::Havok::Compendium* Fallback = Main && Main->Compendium ? &*Main->Compendium : nullptr;
        const Souls::Havok::Compendium* Types    = Binder.Compendium ? &*Binder.Compendium : Fallback;
        auto Result                              = std::make_shared<Clip>();
        Result->Name                             = Entry.Name;
        Result->File = Souls::Havok::Tagfile::Read(Binder.Binder.Files.at(Entry.File).Bytes, Types);

        const auto Animations = Result->File.ObjectsOfType("hkaAnimation");
        const auto Bindings   = Result->File.ObjectsOfType("hkaAnimationBinding");
        if (Animations.empty() || Bindings.empty()) throw std::runtime_error(Entry.Name + " holds no animation");
        Result->Animation = Souls::Havok::Animation::FromValue(Animations[0]);
        Result->Binding   = Souls::Havok::AnimationBinding::FromValue(Bindings[0]);
        if (Main) Result->FindEvents(*Main);
        return Result;
    }

    void Clip::FindEvents(const LoadedBinder& Main) {
        const std::string Wanted = Lower(Name);
        int Category             = -1;
        long Number              = -1;
        if (Wanted.size() == 11 && Wanted[0] == 'a' && Wanted[4] == '_') {
            Category = std::atoi(Wanted.substr(1, 3).c_str());
            Number   = std::atol(Wanted.substr(5).c_str());
        }
        std::vector<std::string> Candidates;
        if (Category >= 0) {
            char Buffer[16];
            std::snprintf(Buffer, sizeof(Buffer), "a%02d", Category);
            Candidates.push_back(Buffer);
        }
        Candidates.push_back(Lower(Main.Name.substr(0, Main.Name.find('.'))));  // "c4290"

        for (const std::string& Candidate : Candidates) {
            const auto Tae = Main.Tae(Candidate);
            if (!Tae) continue;
            const Souls::TAE3::Animation* ById = nullptr;
            for (const auto& Entry : Tae->Animations) {
                // The animation's file ("a000_003000.hkt") names the clip; failing that, a per-category file's IDs
                // are the clip numbers.
                std::string FileName = Lower(Entry.AnimFileName);
                FileName             = FileName.substr(0, FileName.find('.'));
                if (FileName == Wanted) {
                    Events      = Entry;
                    EventSource = Candidate + ".tae";
                    return;
                }
                if (Candidate[0] == 'a' && Entry.ID == Number) ById = &Entry;
            }
            if (ById) {
                Events      = *ById;
                EventSource = Candidate + ".tae";
                return;
            }
        }
    }

    Souls::Havok::Skeleton LoadSkeleton(const LoadedBinder& Main) {
        for (const Souls::BinderFile& File : Main.Binder.Files) {
            if (!File.Name || !EndsWith(Lower(*File.Name), ".hkx")) continue;
            try {
                const Souls::Havok::Tagfile Tag = Souls::Havok::Tagfile::Read(File.Bytes);
                const auto Skeletons            = Tag.ObjectsOfType("hkaSkeleton");
                if (!Skeletons.empty()) return Souls::Havok::Skeleton::FromValue(Skeletons[0]);
            } catch (const std::exception&) {
                // Not a standalone tagfile (an animation whose types are in the compendium); keep looking.
            }
        }
        throw std::runtime_error(Main.Name + " has no skeleton");
    }
}  // namespace Scarlet

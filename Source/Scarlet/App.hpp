// Copyright (C) 2026 Jake Rieger
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, version 3. This program is distributed in the hope that it will
// be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details. You should have received a copy of the GNU General
// Public License along with this program. If not, see https://www.gnu.org/licenses/.

#pragma once

#include "Camera.hpp"
#include "GameData.hpp"
#include "Model.hpp"
#include "Player.hpp"
#include "Renderer.hpp"

#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Scarlet {
    class App {
    public:
        int Run(HINSTANCE Instance);

    private:
        HWND _Window       = nullptr;
        UINT _PendingWidth = 0, _PendingHeight = 0;  ///< a resize to apply before the next frame
        Renderer _Graphics;
        OrbitCamera _Camera;
        Player _Playback;
        std::shared_ptr<const GameData> _Game;
        std::filesystem::path _KeysFolder;  ///< BinderKeys' Elden Ring folder, next to the executable

        std::filesystem::path _SettingsFolder;
        std::string _IniPath;
        bool _BuildLayout = false;

        // The selected character: its binders (loaded when first opened), skeleton and selected bone.
        struct BinderSlot {
            std::string GamePath;
            std::shared_ptr<const LoadedBinder> Loaded;
            bool Requested = false;
        };
        int _SelectedCharacter = -1;
        uint64_t _Generation   = 0;  ///< bumped on every character change, so stale loads are dropped
        std::vector<BinderSlot> _Binders;
        std::optional<Souls::Havok::Skeleton> _Skeleton;
        std::shared_ptr<CharacterModel> _Model;  ///< null if the character has none (or it couldn't be read)
        std::string _ModelStatus;                ///< why there's no model, for the Model panel
        std::vector<DirectX::XMFLOAT4X4> _Skin;  ///< the model's skinning matrices, rebuilt every frame
        int _SelectedBone     = -1;
        int _SelectedEvent    = -1;     ///< in the playing clip's events
        bool _ScrubbingEvents = false;  ///< dragging the playhead across the event track

        // What the viewport shows.
        bool _ShowMesh     = true;
        bool _ShowSkeleton = true;
        bool _ShowGrid     = true;
        int _ClipBinder = -1, _ClipIndex = -1;

        // Loading happens on a worker thread, one job at a time; a job returns what to apply on the UI thread.
        struct Job {
            std::string Description;
            std::function<std::function<void()>()> Work;
        };
        std::vector<Job> _Queue;
        std::future<std::function<void()>> _Worker;
        std::string _RunningDescription;
        std::string _ErrorMessage;  ///< shown in a popup

        char _CharacterFilter[64] = {};
        char _AnimationFilter[64] = {};

        static LRESULT CALLBACK WindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam);
        bool CreateMainWindow(HINSTANCE Instance);
        void InitializeImGui();
        void LoadSettings();
        void SaveSettings() const;
        static void ApplyLookAndFeel();

        void OpenGameFolder(const std::filesystem::path& Folder);
        void SelectCharacter(int Index);
        void RequestBinder(size_t Index);
        void PlayClip(size_t Binder, size_t Animation);
        void Enqueue(std::string Description, std::function<std::function<void()>()> Work);
        void PumpJobs();

        void DrawFrame();
        void DrawMenu();
        void DrawDockspace();
        void DrawCharacters();
        void DrawAnimations();
        void DrawViewport();
        void DrawTimeline();
        void DrawSkeleton();
        void DrawClipInfo();
        void DrawModelPanel();
        void DrawEvents();
        void DrawEventTrack(const Clip& Current);
        void HandleShortcuts();
        static std::vector<LineVertex> GridLines();
        std::vector<LineVertex> SkeletonLines(const std::vector<Souls::Havok::QsTransform>& Pose) const;
        void FrameCharacter();
    };
}  // namespace Scarlet

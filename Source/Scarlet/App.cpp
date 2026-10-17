// Copyright (C) 2026 Jake Rieger
//
// This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, version 3. This program is distributed in the hope that it will
// be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
// PURPOSE. See the GNU General Public License for more details. You should have received a copy of the GNU General
// Public License along with this program. If not, see https://www.gnu.org/licenses/.

#include "App.hpp"
#include "Resource.h"
#include "Version.h"

#include <ShlObj.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <fstream>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam);

namespace fs = std::filesystem;
using Souls::Havok::QsTransform;

namespace Scarlet {
    namespace {
        constexpr wchar_t WindowClass[] = L"ScarletWindow";

        bool Contains(const std::string& Text, const char* Filter) {
            if (!Filter[0]) return true;
            const std::string_view Needle(Filter);
            return std::search(Text.begin(), Text.end(), Needle.begin(), Needle.end(), [](char A, char B) {
                       return std::tolower(static_cast<unsigned char>(A)) ==
                              std::tolower(static_cast<unsigned char>(B));
                   }) != Text.end();
        }

        std::optional<fs::path> PickFolder(HWND Owner) {
            Microsoft::WRL::ComPtr<IFileOpenDialog> Dialog;
            if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Dialog))))
                return std::nullopt;
            DWORD Options = 0;
            Dialog->GetOptions(&Options);
            Dialog->SetOptions(Options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
            Dialog->SetTitle(L"Choose the Elden Ring game folder (the one with eldenring.exe)");
            if (FAILED(Dialog->Show(Owner))) return std::nullopt;
            Microsoft::WRL::ComPtr<IShellItem> Item;
            if (FAILED(Dialog->GetResult(&Item))) return std::nullopt;
            PWSTR Path = nullptr;
            if (FAILED(Item->GetDisplayName(SIGDN_FILESYSPATH, &Path))) return std::nullopt;
            fs::path Result(Path);
            CoTaskMemFree(Path);
            return Result;
        }

        fs::path LocalAppData() {
            PWSTR Path = nullptr;
            fs::path Result;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &Path))) Result = Path;
            CoTaskMemFree(Path);
            return Result;
        }

        DirectX::XMFLOAT3 ToFloat3(const Souls::Vector3& V) {
            return {V.X, V.Y, V.Z};
        }

        // A point given in a bone's space, in model space.
        Souls::Vector3 Apply(const QsTransform& Bone, Souls::Vector3 Offset) {
            QsTransform Point;
            Point.Translation = Offset;
            return QsTransform::Multiply(Bone, Point).Translation;
        }

        // An event type's number with the name libSouls has for it (Dark Souls III's event list; most are "UnkNNN").
        std::string EventTypeLabel(uint64_t Type) {
            const char* Name = Souls::TAE3EventTypeName(static_cast<Souls::TAE3EventType>(Type));
            return Name ? std::to_string(Type) + "  " + Name : std::to_string(Type);
        }

        // A color per event type, spread around the hue circle.
        ImU32 EventColor(uint64_t Type, float Brightness) {
            float R, G, B;
            ImGui::ColorConvertHSVtoRGB(std::fmod(float(Type % 997) * 0.618034f, 1.f), 0.5f, Brightness, R, G, B);
            return ImGui::GetColorU32(ImVec4(R, G, B, 1.f));
        }

        // When an event ends, within the animation (events that last "until the end" store a huge time).
        float EventEnd(const Souls::TAE3::Event& Event, float Duration) {
            return Event.EndTime < Event.StartTime || Event.EndTime > Duration ? Duration : Event.EndTime;
        }

        void AddLine(std::vector<LineVertex>& Lines, Souls::Vector3 A, Souls::Vector3 B, uint32_t Color) {
            Lines.push_back({ToFloat3(A), Color});
            Lines.push_back({ToFloat3(B), Color});
        }
    }  // namespace

#pragma region Window and settings
    int App::Run(HINSTANCE Instance) {
        ImGui_ImplWin32_EnableDpiAwareness();
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (!CreateMainWindow(Instance) || !_Graphics.Initialize(_Window)) return 1;
        InitializeImGui();
        LoadSettings();
        ShowWindow(_Window, SW_SHOWMAXIMIZED);
        UpdateWindow(_Window);

        bool Open = true;
        while (Open) {
            MSG Message;
            while (PeekMessageW(&Message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&Message);
                DispatchMessageW(&Message);
                if (Message.message == WM_QUIT) Open = false;
            }
            if (!Open) break;
            if (IsIconic(_Window)) {
                Sleep(16);
                continue;
            }
            if (_PendingWidth && _PendingHeight) {
                _Graphics.Resize(_PendingWidth, _PendingHeight);
                _PendingWidth = _PendingHeight = 0;
            }
            DrawFrame();
        }

        if (_Worker.valid()) _Worker.wait();
        SaveSettings();
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        CoUninitialize();
        return 0;
    }

    bool App::CreateMainWindow(HINSTANCE Instance) {
        WNDCLASSEXW Class {};
        Class.cbSize        = sizeof(Class);
        Class.style         = CS_HREDRAW | CS_VREDRAW;
        Class.lpfnWndProc   = WindowProc;
        Class.hInstance     = Instance;
        Class.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        Class.hIcon         = LoadIconW(Instance, MAKEINTRESOURCE(APPICON));
        Class.lpszClassName = WindowClass;
        RegisterClassExW(&Class);
        _Window = CreateWindowExW(0,
                                  WindowClass,
                                  L"Scarlet",
                                  WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT,
                                  CW_USEDEFAULT,
                                  1600,
                                  900,
                                  nullptr,
                                  nullptr,
                                  Instance,
                                  this);

        return _Window != nullptr;
    }

    LRESULT CALLBACK App::WindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam) {
        if (ImGui_ImplWin32_WndProcHandler(Window, Message, WParam, LParam)) return 1;
        if (Message == WM_NCCREATE) {
            const auto* Create = reinterpret_cast<const CREATESTRUCTW*>(LParam);
            SetWindowLongPtrW(Window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(Create->lpCreateParams));
        }
        auto* Self = reinterpret_cast<App*>(GetWindowLongPtrW(Window, GWLP_USERDATA));
        switch (Message) {
            case WM_SIZE:
                if (Self && WParam != SIZE_MINIMIZED) {
                    Self->_PendingWidth  = LOWORD(LParam);
                    Self->_PendingHeight = HIWORD(LParam);
                }
                return 0;
            case WM_DPICHANGED: {
                const RECT* Suggested = reinterpret_cast<const RECT*>(LParam);
                SetWindowPos(Window,
                             nullptr,
                             Suggested->left,
                             Suggested->top,
                             Suggested->right - Suggested->left,
                             Suggested->bottom - Suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            case WM_SYSCOMMAND:
                if ((WParam & 0xFFF0) == SC_KEYMENU) return 0;  // Alt doesn't open a window menu
                break;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
            default:
                break;
        }
        return DefWindowProcW(Window, Message, WParam, LParam);
    }

    void App::InitializeImGui() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& IO = ImGui::GetIO();
        IO.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
        IO.ConfigDpiScaleFonts = true;

        wchar_t Exe[MAX_PATH] {};
        GetModuleFileNameW(nullptr, Exe, MAX_PATH);
        _KeysFolder = fs::path(Exe).parent_path() / "BinderKeys" / "EldenRing_PC";

        _SettingsFolder = LocalAppData() / "Scarlet";
        std::error_code Ignored;
        fs::create_directories(_SettingsFolder, Ignored);
        _IniPath       = ToUtf8(_SettingsFolder / "imgui.ini");
        IO.IniFilename = _IniPath.c_str();
        _BuildLayout   = !fs::exists(_SettingsFolder / "imgui.ini");

        const fs::path Font = "C:/Windows/Fonts/segoeui.ttf";
        if (fs::exists(Font)) IO.Fonts->AddFontFromFileTTF(ToUtf8(Font).c_str(), 17.f);

        ImGui::StyleColorsDark();
        ImGuiStyle& Style = ImGui::GetStyle();
        ApplyLookAndFeel();
        const float Scale = ImGui_ImplWin32_GetDpiScaleForHwnd(_Window);
        Style.ScaleAllSizes(Scale);
        Style.FontScaleDpi = Scale;

        ImGui_ImplWin32_Init(_Window);
        ImGui_ImplDX11_Init(_Graphics.Device(), _Graphics.Context());
    }

    void App::LoadSettings() {
        std::ifstream In(_SettingsFolder / "settings.ini");
        std::string Line;
        while (std::getline(In, Line)) {
            constexpr std::string_view Key = "GameFolder=";
            if (Line.rfind(Key, 0) == 0) {
                const std::string Value = Line.substr(Key.size());
                if (!Value.empty()) OpenGameFolder(fs::path(std::u8string(Value.begin(), Value.end())));
            }
        }
    }

    void App::SaveSettings() const {
        std::ofstream Out(_SettingsFolder / "settings.ini");
        Out << "GameFolder=" << (_Game ? ToUtf8(_Game->Root()) : std::string()) << "\n";
    }

    void App::ApplyLookAndFeel() {
        ImGuiStyle& Style = ImGui::GetStyle();
        ImVec4* Colors    = Style.Colors;

        constexpr float Rounding = 4.0f;
        constexpr ImVec2 Padding = {6.0f, 6.0f};

        Style.TabRounding      = 0.0f;
        Style.WindowRounding   = Rounding;
        Style.FrameRounding    = Rounding;
        Style.ChildRounding    = Rounding;
        Style.WindowBorderSize = 1.0f;
        Style.FrameBorderSize  = 1.0f;

        Style.WindowPadding = Padding;
        Style.FramePadding  = Padding;

        const ImVec4 WindowBg      = ImGui::ColorConvertU32ToFloat4(IM_COL32(17, 17, 17, 255));
        const ImVec4 PanelBg       = ImGui::ColorConvertU32ToFloat4(IM_COL32(21, 21, 21, 255));
        const ImVec4 Border        = ImGui::ColorConvertU32ToFloat4(IM_COL32(59, 59, 58, 255));
        const ImVec4 TabActive     = ImGui::ColorConvertU32ToFloat4(IM_COL32(54, 54, 54, 255));
        const ImVec4 TabInactive   = ImGui::ColorConvertU32ToFloat4(IM_COL32(31, 31, 31, 255));
        const ImVec4 Button        = ImGui::ColorConvertU32ToFloat4(IM_COL32(65, 65, 64, 255));
        const ImVec4 ButtonHovered = ImGui::ColorConvertU32ToFloat4(IM_COL32(65, 65, 64, 220));
        const ImVec4 ButtonActive  = ImGui::ColorConvertU32ToFloat4(IM_COL32(65, 65, 64, 200));
        const ImVec4 Input         = ImGui::ColorConvertU32ToFloat4(IM_COL32(32, 32, 31, 255));
        const ImVec4 TextPrimary   = ImGui::ColorConvertU32ToFloat4(IM_COL32(211, 210, 207, 255));
        const ImVec4 TextSecondary = ImGui::ColorConvertU32ToFloat4(IM_COL32(121, 120, 114, 255));
        const ImVec4 TextDisabled  = ImGui::ColorConvertU32ToFloat4(IM_COL32(112, 110, 105, 255));
        const ImVec4 Header        = ImGui::ColorConvertU32ToFloat4(IM_COL32(64, 87, 111, 255));
        const ImVec4 HeaderHovered = ImGui::ColorConvertU32ToFloat4(IM_COL32(70, 94, 120, 255));

        Colors[ImGuiCol_BorderShadow]       = ImVec4(0.f, 0.f, 0.f, 0.f);
        Colors[ImGuiCol_Border]             = Border;
        Colors[ImGuiCol_ButtonActive]       = ButtonActive;
        Colors[ImGuiCol_ButtonHovered]      = ButtonHovered;
        Colors[ImGuiCol_Button]             = Button;
        Colors[ImGuiCol_CheckMark]          = TextPrimary;
        Colors[ImGuiCol_CheckboxSelectedBg] = WindowBg;
        Colors[ImGuiCol_ChildBg]            = PanelBg;
        Colors[ImGuiCol_DockingPreview]     = TextPrimary;
        Colors[ImGuiCol_DragDropTarget]     = TextPrimary;
        Colors[ImGuiCol_FrameBgActive]      = ImVec4(Input.x, Input.y, Input.z, 0.4f);
        Colors[ImGuiCol_FrameBgHovered]     = ImVec4(Input.x, Input.y, Input.z, 0.7f);
        ;
        Colors[ImGuiCol_FrameBg]               = Input;
        Colors[ImGuiCol_HeaderActive]          = Border;  // Selected item in listbox
        Colors[ImGuiCol_HeaderHovered]         = HeaderHovered;
        Colors[ImGuiCol_Header]                = Header;
        Colors[ImGuiCol_MenuBarBg]             = Input;
        Colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.00f, 0.00f, 0.00f, 0.5f);
        Colors[ImGuiCol_NavHighlight]          = ImVec4(30.f / 255.f, 30.f / 255.f, 30.f / 255.f, 1.00f);
        Colors[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
        Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
        Colors[ImGuiCol_PlotHistogramHovered]  = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
        Colors[ImGuiCol_PlotHistogram]         = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
        Colors[ImGuiCol_PlotLinesHovered]      = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
        Colors[ImGuiCol_PlotLines]             = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
        Colors[ImGuiCol_PopupBg]               = WindowBg;
        Colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.26f, 0.59f, 0.98f, 0.95f);
        Colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
        Colors[ImGuiCol_ResizeGrip]            = ImVec4(0.26f, 0.59f, 0.98f, 0.20f);
        Colors[ImGuiCol_ScrollbarBg]           = Input;
        Colors[ImGuiCol_ScrollbarGrabActive]   = TextPrimary;
        Colors[ImGuiCol_ScrollbarGrabHovered]  = TextPrimary;
        Colors[ImGuiCol_ScrollbarGrab]         = TextPrimary;
        Colors[ImGuiCol_SeparatorActive]       = TextPrimary;
        Colors[ImGuiCol_SeparatorHovered]      = TextPrimary;
        Colors[ImGuiCol_Separator]             = Border;
        Colors[ImGuiCol_SliderGrabActive]      = TextPrimary;
        Colors[ImGuiCol_SliderGrab]            = TextPrimary;
        Colors[ImGuiCol_TabActive]             = TabActive;
        Colors[ImGuiCol_TabHovered]            = TabActive;
        Colors[ImGuiCol_TabUnfocusedActive]    = Colors[ImGuiCol_TabActive];
        Colors[ImGuiCol_TabUnfocused]          = Colors[ImGuiCol_Tab];
        Colors[ImGuiCol_Tab]                   = TabInactive;
        Colors[ImGuiCol_TableBorderLight]      = ImVec4(0.23f, 0.23f, 0.25f, 1.00f);  // Prefer using Alpha=1.0 here
        Colors[ImGuiCol_TableBorderLight]      = ImVec4(0.f, 0.f, 0.f, 0.f);
        Colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.31f, 0.31f, 0.35f, 1.00f);  // Prefer using Alpha=1.0 here
        Colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.f, 0.f, 0.f, 0.f);
        Colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.19f, 0.19f, 0.20f, 1.00f);
        Colors[ImGuiCol_TableRowBgAlt]         = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
        Colors[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        Colors[ImGuiCol_TextDisabled]          = TextSecondary;
        Colors[ImGuiCol_TextSelectedBg]   = ImVec4(TextPrimary.x, TextPrimary.y, TextPrimary.z, 0.5f);  // 50% alpha
        Colors[ImGuiCol_Text]             = TextPrimary;
        Colors[ImGuiCol_TitleBgActive]    = PanelBg;
        Colors[ImGuiCol_TitleBgCollapsed] = PanelBg;
        Colors[ImGuiCol_TitleBg]          = PanelBg;
        Colors[ImGuiCol_WindowBg]         = WindowBg;
    }
#pragma endregion

#pragma region Loading
    void App::OpenGameFolder(const fs::path& Folder) {
        const fs::path Keys = _KeysFolder;
        Enqueue("Opening " + ToUtf8(Folder), [this, Folder, Keys]() -> std::function<void()> {
            std::shared_ptr<const GameData> Opened = GameData::Open(Folder, Keys);
            return [this, Opened] {
                SelectCharacter(-1);
                _Game = Opened;
                SaveSettings();
            };
        });
    }

    void App::SelectCharacter(int Index) {
        ++_Generation;
        _SelectedCharacter = Index;
        _Binders.clear();
        _Skeleton.reset();
        _Model.reset();
        _ModelStatus.clear();
        _Graphics.SetModel(nullptr);
        _SelectedBone = -1;
        _ClipBinder = _ClipIndex = -1;
        _Playback.SetClip(nullptr);
        _Queue.clear();
        if (!_Game || Index < 0 || Index >= static_cast<int>(_Game->Characters().size())) return;
        for (const std::string& Path : _Game->Characters()[Index].Binders)
            _Binders.push_back({Path, nullptr, false});

        // The main binder holds the skeleton; the chrbnd, the model.
        _Binders[0].Requested                  = true;
        const std::string Path                 = _Binders[0].GamePath;
        const std::string Id                   = _Game->Characters()[Index].Id;
        const uint64_t For                     = _Generation;
        std::shared_ptr<const GameData> Source = _Game;
        Enqueue("Loading " + Id, [this, Source, Path, Id, For]() -> std::function<void()> {
            std::shared_ptr<const LoadedBinder> Loaded = LoadedBinder::Load(*Source, Path);
            Souls::Havok::Skeleton Read                = LoadSkeleton(*Loaded);
            std::shared_ptr<CharacterModel> Model;
            std::string Status;
            try {
                Model = CharacterModel::Load(*Source, Id);
                if (!Model) {
                    Status = Id == "c0000" ? "The player's body is assembled from equipment parts, which Scarlet "
                                             "doesn't load yet."
                                           : Id + " has no model of its own.";
                }
            } catch (const std::exception& E) { Status = std::string("The model couldn't be read: ") + E.what(); }
            return [this, Loaded, Read = std::move(Read), Model, Status, For]() mutable {
                if (For != _Generation) return;
                _Binders[0].Loaded = Loaded;
                _Skeleton          = std::move(Read);
                _Model             = Model;
                _ModelStatus       = Status;
                if (_Model) _Model->BindSkeleton(*_Skeleton);
                _Graphics.SetModel(_Model.get());
                FrameCharacter();
            };
        });
    }

    void App::RequestBinder(size_t Index) {
        if (Index >= _Binders.size() || _Binders[Index].Requested) return;
        _Binders[Index].Requested              = true;
        const std::string Path                 = _Binders[Index].GamePath;
        const uint64_t For                     = _Generation;
        std::shared_ptr<const GameData> Source = _Game;
        Enqueue("Loading " + Path, [this, Source, Path, Index, For]() -> std::function<void()> {
            std::shared_ptr<const LoadedBinder> Loaded = LoadedBinder::Load(*Source, Path);
            return [this, Loaded, Index, For] {
                if (For == _Generation && Index < _Binders.size()) _Binders[Index].Loaded = Loaded;
            };
        });
    }

    void App::PlayClip(size_t BinderIndex, size_t AnimationIndex) {
        if (BinderIndex >= _Binders.size() || !_Binders[BinderIndex].Loaded) return;
        std::shared_ptr<const LoadedBinder> Binder = _Binders[BinderIndex].Loaded;
        std::shared_ptr<const LoadedBinder> Main   = _Binders[0].Loaded;  // its compendium, for binders without one
        const AnimationEntry Entry                 = Binder->Animations.at(AnimationIndex);
        const uint64_t For                         = _Generation;
        _ClipBinder                                = static_cast<int>(BinderIndex);
        _ClipIndex                                 = static_cast<int>(AnimationIndex);
        Enqueue("Decoding " + Entry.Name, [this, Binder, Main, Entry, For]() -> std::function<void()> {
            std::shared_ptr<const Clip> Loaded = Clip::Load(*Binder, Entry, Main.get());
            return [this, Loaded, For] {
                if (For != _Generation) return;
                const bool WasPlaying = _Playback.Playing;
                _SelectedEvent        = -1;
                _Playback.SetClip(Loaded);
                _Playback.Playing = WasPlaying || !_Playback.CurrentClip();
            };
        });
    }

    void App::Enqueue(std::string Description, std::function<std::function<void()>()> Work) {
        _Queue.push_back({std::move(Description), std::move(Work)});
    }

    void App::PumpJobs() {
        if (_Worker.valid() && _Worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            std::function<void()> Finish = _Worker.get();
            if (Finish) Finish();
            _RunningDescription.clear();
        }
        if (!_Worker.valid() && !_Queue.empty()) {
            Job Next = std::move(_Queue.front());
            _Queue.erase(_Queue.begin());
            _RunningDescription = Next.Description;
            _Worker =
              std::async(std::launch::async,
                         [Work = std::move(Next.Work), this, What = Next.Description]() -> std::function<void()> {
                             try {
                                 return Work();
                             } catch (const std::exception& E) {
                                 std::string Message = What + " failed: " + E.what();
                                 return [this, Message] { _ErrorMessage = Message; };
                             }
                         });
        }
    }
#pragma endregion

#pragma region Frame
    void App::DrawFrame() {
        PumpJobs();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        _Playback.Update(ImGui::GetIO().DeltaTime);
        HandleShortcuts();
        DrawMenu();
        DrawDockspace();
        DrawCharacters();
        DrawAnimations();
        DrawSkeleton();
        DrawClipInfo();
        DrawModelPanel();
        DrawEvents();
        DrawTimeline();
        DrawViewport();

        if (!_ErrorMessage.empty()) ImGui::OpenPopup("Error");
        if (ImGui::BeginPopupModal("Error", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.f);
            ImGui::TextUnformatted(_ErrorMessage.c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::Button("OK", ImVec2(ImGui::GetFontSize() * 6.f, 0.f))) {
                _ErrorMessage.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::Render();
        _Graphics.BeginFrame();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        _Graphics.Present();
    }

    void App::DrawMenu() {
        if (!ImGui::BeginMainMenuBar()) return;
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Game Folder...")) {
                if (const auto Folder = PickFolder(_Window)) OpenGameFolder(*Folder);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) PostMessageW(_Window, WM_CLOSE, 0, 0);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Model", nullptr, &_ShowMesh);
            ImGui::MenuItem("Skeleton", nullptr, &_ShowSkeleton);
            ImGui::MenuItem("Grid", nullptr, &_ShowGrid);
            ImGui::Separator();
            if (ImGui::MenuItem("Frame Character", "F")) FrameCharacter();
            if (ImGui::MenuItem("Reset Layout")) _BuildLayout = true;
            ImGui::EndMenu();
        }
        // What's loading, on the right of the menu bar.
        if (!_RunningDescription.empty()) {
            const std::string Text = _RunningDescription + "...";
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(Text.c_str()).x - ImGui::GetFontSize());
            ImGui::TextDisabled("%s", Text.c_str());
        }
        ImGui::EndMainMenuBar();
    }

    void App::DrawDockspace() {
        const ImGuiID Dock = ImGui::GetID("ScarletDockspace");
        if (_BuildLayout) {
            _BuildLayout                  = false;
            const ImGuiViewport* Viewport = ImGui::GetMainViewport();
            ImGui::DockBuilderRemoveNode(Dock);
            ImGui::DockBuilderAddNode(Dock, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(Dock, Viewport->WorkSize);
            ImGuiID Left, Rest, LeftTop, LeftBottom, Right, Middle, RightTop, RightBottom, Bottom, Center;
            ImGui::DockBuilderSplitNode(Dock, ImGuiDir_Left, 0.2f, &Left, &Rest);
            ImGui::DockBuilderSplitNode(Left, ImGuiDir_Up, 0.35f, &LeftTop, &LeftBottom);
            ImGui::DockBuilderSplitNode(Rest, ImGuiDir_Right, 0.25f, &Right, &Middle);
            ImGui::DockBuilderSplitNode(Right, ImGuiDir_Up, 0.6f, &RightTop, &RightBottom);
            ImGui::DockBuilderSplitNode(Middle, ImGuiDir_Down, 0.16f, &Bottom, &Center);
            ImGui::DockBuilderDockWindow("Characters", LeftTop);
            ImGui::DockBuilderDockWindow("Animations", LeftBottom);
            ImGui::DockBuilderDockWindow("Skeleton", RightTop);
            ImGui::DockBuilderDockWindow("Clip", RightBottom);
            ImGui::DockBuilderDockWindow("Model", RightBottom);
            ImGui::DockBuilderDockWindow("Events", RightBottom);
            ImGui::DockBuilderDockWindow("Timeline", Bottom);
            ImGui::DockBuilderDockWindow("Viewport", Center);
            ImGui::DockBuilderFinish(Dock);
        }
        ImGui::DockSpaceOverViewport(Dock, ImGui::GetMainViewport());
    }

    void App::HandleShortcuts() {
        const ImGuiIO& IO = ImGui::GetIO();
        if (IO.WantTextInput || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) return;
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) _Playback.Playing = !_Playback.Playing;
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) FrameCharacter();
        if (_Playback.CurrentClip()) {
            const float Frame = std::round(_Playback.Frame());
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
                _Playback.Playing = false;
                _Playback.SeekFrame(Frame - 1.f);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
                _Playback.Playing = false;
                _Playback.SeekFrame(Frame + 1.f);
            }
        }
    }
#pragma endregion

#pragma region Panels
    void App::DrawCharacters() {
        if (ImGui::Begin("Characters")) {
            if (!_Game) {
                ImGui::TextWrapped("Open Elden Ring's game folder, the one with eldenring.exe (File > Open Game "
                                   "Folder).");
                if (ImGui::Button("Open Game Folder...")) {
                    if (const auto Folder = PickFolder(_Window)) OpenGameFolder(*Folder);
                }
            } else {
                ImGui::TextDisabled("%zu characters, from %s",
                                    _Game->Characters().size(),
                                    _Game->SourceDescription().c_str());
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputTextWithHint("##filter", "Filter", _CharacterFilter, sizeof(_CharacterFilter));
                if (ImGui::BeginChild("list")) {
                    const auto& Characters = _Game->Characters();
                    for (int I = 0; I < static_cast<int>(Characters.size()); ++I) {
                        const CharacterFiles& C = Characters[I];
                        if (!Contains(C.Id, _CharacterFilter)) continue;
                        const std::string Label = C.Id + "  (" + std::to_string(C.Binders.size()) + ")";
                        if (ImGui::Selectable(Label.c_str(), I == _SelectedCharacter)) SelectCharacter(I);
                    }
                }
                ImGui::EndChild();
            }
        }
        ImGui::End();
    }

    void App::DrawAnimations() {
        if (ImGui::Begin("Animations")) {
            if (_Binders.empty()) {
                ImGui::TextDisabled("Choose a character.");
            } else {
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::InputTextWithHint("##filter", "Filter", _AnimationFilter, sizeof(_AnimationFilter));
                if (ImGui::BeginChild("list")) {
                    for (size_t B = 0; B < _Binders.size(); ++B) {
                        BinderSlot& Slot       = _Binders[B];
                        const std::string Name = Slot.GamePath.substr(Slot.GamePath.find_last_of('/') + 1);
                        // Binders open when expanded; the main one is usually just the skeleton.
                        const ImGuiTreeNodeFlags Flags = _AnimationFilter[0] ? ImGuiTreeNodeFlags_DefaultOpen : 0;
                        if (!ImGui::TreeNodeEx(Name.c_str(), Flags)) continue;
                        if (!Slot.Loaded) {
                            RequestBinder(B);
                            ImGui::TextDisabled("Loading...");
                        } else if (Slot.Loaded->Animations.empty()) {
                            ImGui::TextDisabled("No animations");
                        } else {
                            const auto& Animations = Slot.Loaded->Animations;
                            for (size_t A = 0; A < Animations.size(); ++A) {
                                if (!Contains(Animations[A].Name, _AnimationFilter)) continue;
                                const bool Selected =
                                  _ClipBinder == static_cast<int>(B) && _ClipIndex == static_cast<int>(A);
                                ImGui::PushID(static_cast<int>(A));
                                if (ImGui::Selectable(Animations[A].Name.c_str(), Selected)) PlayClip(B, A);
                                ImGui::PopID();
                            }
                        }
                        ImGui::TreePop();
                    }
                }
                ImGui::EndChild();
            }
        }
        ImGui::End();
    }

    void App::DrawSkeleton() {
        if (ImGui::Begin("Skeleton")) {
            if (!_Skeleton) {
                ImGui::TextDisabled("No skeleton loaded.");
            } else {
                ImGui::Text("%s: %zu bones", _Skeleton->Name.c_str(), _Skeleton->BoneNames.size());
                const float DetailsHeight = _SelectedBone >= 0 ? ImGui::GetTextLineHeightWithSpacing() * 5.f : 0.f;
                if (ImGui::BeginChild("tree", ImVec2(0.f, -DetailsHeight), ImGuiChildFlags_Borders)) {
                    // Children of each bone, so the tree can be drawn from the roots down.
                    std::vector<std::vector<int>> Children(_Skeleton->BoneNames.size());
                    std::vector<int> Roots;
                    for (int I = 0; I < static_cast<int>(_Skeleton->Parents.size()); ++I) {
                        const int Parent = _Skeleton->Parents[I];
                        (Parent >= 0 ? Children[Parent] : Roots).push_back(I);
                    }
                    const std::function<void(int)> Draw = [&](int Bone) {
                        ImGuiTreeNodeFlags Flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                                                   ImGuiTreeNodeFlags_DefaultOpen;
                        if (Children[Bone].empty()) Flags |= ImGuiTreeNodeFlags_Leaf;
                        if (Bone == _SelectedBone) Flags |= ImGuiTreeNodeFlags_Selected;
                        const bool Open = ImGui::TreeNodeEx(reinterpret_cast<void*>(intptr_t(Bone)),
                                                            Flags,
                                                            "%s",
                                                            _Skeleton->BoneNames[Bone].c_str());
                        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) _SelectedBone = Bone;
                        if (Open) {
                            for (const int Child : Children[Bone])
                                Draw(Child);
                            ImGui::TreePop();
                        }
                    };
                    for (const int Root : Roots)
                        Draw(Root);
                }
                ImGui::EndChild();

                if (_SelectedBone >= 0) {
                    // The bone's transform relative to its parent, at the playhead.
                    const QsTransform T = _Playback.LocalPose(*_Skeleton)[_SelectedBone];
                    ImGui::Text("%s", _Skeleton->BoneNames[_SelectedBone].c_str());
                    ImGui::Text("Translation  %.4f  %.4f  %.4f", T.Translation.X, T.Translation.Y, T.Translation.Z);
                    ImGui::Text("Rotation     %.4f  %.4f  %.4f  %.4f",
                                T.Rotation.X,
                                T.Rotation.Y,
                                T.Rotation.Z,
                                T.Rotation.W);
                    ImGui::Text("Scale        %.4f  %.4f  %.4f", T.Scale.X, T.Scale.Y, T.Scale.Z);
                }
            }
        }
        ImGui::End();
    }

    void App::DrawClipInfo() {
        if (ImGui::Begin("Clip")) {
            const auto& Current = _Playback.CurrentClip();
            if (!Current) {
                ImGui::TextDisabled("No animation playing.");
            } else {
                const Souls::Havok::Animation& A = Current->Animation;
                const auto Hint                  = Current->Binding.BlendHint;
                if (ImGui::BeginTable("info", 2, ImGuiTableFlags_SizingFixedFit)) {
                    auto Row = [](const char* Label, const std::string& Value) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", Label);
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(Value.c_str());
                    };
                    char Buffer[64];
                    Row("Name", Current->Name);
                    std::snprintf(Buffer, sizeof(Buffer), "%.3f s", A.Duration);
                    Row("Duration", Buffer);
                    std::snprintf(Buffer,
                                  sizeof(Buffer),
                                  "%u at %.0f fps",
                                  A.FrameCount,
                                  A.FrameDuration > 0.f ? 1.f / A.FrameDuration : 0.f);
                    Row("Frames", Buffer);
                    Row("Tracks", std::to_string(A.TrackCount));
                    Row("Storage",
                        A.Storage == Souls::Havok::Animation::Compression::Spline ? "Spline-compressed"
                                                                                  : "Uncompressed");
                    Row("Blending",
                        Hint == Souls::Havok::AnimationBinding::Blend::Normal     ? "Normal"
                        : Hint == Souls::Havok::AnimationBinding::Blend::Additive ? "Additive"
                                                                                  : "Additive (deprecated)");
                    Row("Root motion",
                        A.RootMotion ? std::to_string(A.RootMotion->Samples.size()) + " samples" : "None");
                    ImGui::EndTable();
                }
                if (Hint != Souls::Havok::AnimationBinding::Blend::Normal)
                    ImGui::TextWrapped("Additive: shown on top of the reference pose.");
            }
        }
        ImGui::End();
    }

    void App::DrawModelPanel() {
        // A layout saved before this panel existed doesn't place it; open it docked with the clip info then.
        if (const ImGuiWindow* Clip = ImGui::FindWindowByName("Clip"); Clip && Clip->DockId)
            ImGui::SetNextWindowDockID(Clip->DockId, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Model")) {
            if (!_Model) {
                ImGui::TextWrapped("%s", _ModelStatus.empty() ? "No model loaded." : _ModelStatus.c_str());
            } else {
                ImGui::Text("%s", _Model->Name.c_str());
                ImGui::TextDisabled("%zu vertices, %zu triangles, %zu of %zu bones animated",
                                    _Model->Vertices.size(),
                                    _Model->Indices.size() / 3,
                                    _Model->BoundBoneCount(),
                                    _Model->BoneCount());
                ImGui::Checkbox("Model", &_ShowMesh);
                ImGui::SameLine();
                ImGui::Checkbox("Skeleton", &_ShowSkeleton);
                ImGui::SameLine();
                ImGui::Checkbox("Grid", &_ShowGrid);
                ImGui::SeparatorText("Meshes");
                if (ImGui::BeginChild("meshes")) {
                    for (size_t I = 0; I < _Model->Meshes.size(); ++I) {
                        SubMesh& Mesh           = _Model->Meshes[I];
                        const std::string Label = std::to_string(I) + "  " + Mesh.Material + "  (" +
                                                  std::to_string(Mesh.IndexCount / 3) + " triangles)";
                        ImGui::PushID(static_cast<int>(I));
                        ImGui::Checkbox(Label.c_str(), &Mesh.Visible);
                        ImGui::SameLine();
                        if (Mesh.Albedo >= 0 && size_t(Mesh.Albedo) < _Model->Textures.size())
                            ImGui::TextDisabled("%s", _Model->Textures[Mesh.Albedo].Name.c_str());
                        else ImGui::TextDisabled("no texture found");
                        ImGui::PopID();
                    }
                }
                ImGui::EndChild();
            }
        }
        ImGui::End();
    }

    void App::DrawTimeline() {
        if (ImGui::Begin("Timeline")) {
            const auto& Current = _Playback.CurrentClip();
            ImGui::BeginDisabled(!Current);
            const float Last = Current ? float(std::max<uint32_t>(Current->Animation.FrameCount, 1) - 1) : 0.f;
            if (ImGui::Button("|<")) _Playback.SeekFrame(0.f);
            ImGui::SameLine();
            if (ImGui::Button(_Playback.Playing ? "Pause" : "Play")) _Playback.Playing = !_Playback.Playing;
            ImGui::SameLine();
            if (ImGui::Button(">|")) _Playback.SeekFrame(Last);
            ImGui::SameLine();
            ImGui::Checkbox("Loop", &_Playback.Loop);
            ImGui::SameLine();
            ImGui::Checkbox("Root motion", &_Playback.RootMotion);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.f);
            ImGui::DragFloat("Speed", &_Playback.Speed, 0.01f, 0.f, 4.f, "%.2fx");
            ImGui::SameLine();
            ImGui::Text("%.3f / %.3f s", _Playback.Time, _Playback.Duration());

            float Frame = _Playback.Frame();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderFloat("##frame", &Frame, 0.f, Last, "Frame %.1f")) {
                _Playback.Playing = false;
                _Playback.SeekFrame(Frame);
            }
            ImGui::EndDisabled();
            if (Current && Current->Events) DrawEventTrack(*Current);
        }
        ImGui::End();
    }

    void App::DrawEventTrack(const Clip& Current) {
        const auto& Events   = Current.Events->Events;
        const float Duration = std::max(Current.Animation.Duration, 1e-3f);

        // One row per event type.
        std::vector<uint64_t> Rows;
        for (const auto& Event : Events)
            Rows.push_back(Event.TypeNumber());
        std::sort(Rows.begin(), Rows.end());
        Rows.erase(std::unique(Rows.begin(), Rows.end()), Rows.end());

        if (!ImGui::BeginChild("event track", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders)) {
            ImGui::EndChild();
            return;
        }
        ImDrawList* Draw    = ImGui::GetWindowDrawList();
        const ImVec2 Origin = ImGui::GetCursorScreenPos();
        const float RowH    = ImGui::GetTextLineHeight() + 4.f;
        const float LabelW  = ImGui::GetFontSize() * 10.f;
        const float Width   = std::max(ImGui::GetContentRegionAvail().x, LabelW + 10.f);
        const float TrackX  = Origin.x + LabelW;
        const float TrackW  = Width - LabelW;
        const float Height  = RowH * float(Rows.size());
        const ImVec2 Mouse  = ImGui::GetIO().MousePos;
        auto X              = [&](float Time) { return TrackX + std::clamp(Time / Duration, 0.f, 1.f) * TrackW; };
        auto RowOf          = [&](uint64_t Type) {
            return int(std::lower_bound(Rows.begin(), Rows.end(), Type) - Rows.begin());
        };

        for (size_t R = 0; R < Rows.size(); ++R) {
            const float Y = Origin.y + RowH * float(R);
            if (R % 2)
                Draw->AddRectFilled(ImVec2(Origin.x, Y),
                                    ImVec2(Origin.x + Width, Y + RowH),
                                    IM_COL32(255, 255, 255, 8));
            Draw->AddText(ImVec2(Origin.x + 4.f, Y + 2.f),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled),
                          EventTypeLabel(Rows[R]).c_str());
        }

        int Hovered = -1;
        for (size_t I = 0; I < Events.size(); ++I) {
            const auto& Event = Events[I];
            const float Y     = Origin.y + RowH * float(RowOf(Event.TypeNumber()));
            const float X0    = X(Event.StartTime);
            const float X1    = std::max(X(EventEnd(Event, Duration)), X0 + 3.f);
            const bool Active = _Playback.Time >= Event.StartTime && _Playback.Time <= EventEnd(Event, Duration);
            const ImVec2 Min(X0, Y + 2.f), Max(X1, Y + RowH - 2.f);
            Draw->AddRectFilled(Min, Max, EventColor(Event.TypeNumber(), Active ? 0.95f : 0.55f), 2.f);
            if (static_cast<int>(I) == _SelectedEvent)
                Draw->AddRect(Min, Max, IM_COL32(255, 255, 255, 255), 2.f, 0, 2.f);
            if (Mouse.x >= Min.x && Mouse.x <= Max.x && Mouse.y >= Min.y && Mouse.y <= Max.y)
                Hovered = static_cast<int>(I);
        }
        // The playhead.
        const float Head = X(_Playback.Time);
        Draw->AddLine(ImVec2(Head, Origin.y), ImVec2(Head, Origin.y + Height), IM_COL32(255, 210, 60, 255), 2.f);

        // Clicking an event selects it; clicking or dragging elsewhere on the track moves the playhead.
        ImGui::SetCursorScreenPos(ImVec2(TrackX, Origin.y));
        ImGui::InvisibleButton("track", ImVec2(TrackW, std::max(Height, 1.f)));
        if (ImGui::IsItemClicked()) {
            _ScrubbingEvents = Hovered < 0;
            if (Hovered >= 0) _SelectedEvent = Hovered;
        }
        if (!ImGui::IsItemActive()) _ScrubbingEvents = false;
        if (_ScrubbingEvents) {
            _Playback.Playing = false;
            _Playback.Time    = std::clamp((Mouse.x - TrackX) / TrackW, 0.f, 1.f) * Duration;
        }
        if (Hovered >= 0 && ImGui::IsItemHovered() && !_ScrubbingEvents) {
            const auto& Event = Events[Hovered];
            ImGui::SetTooltip("%s\n%.3f - %s s",
                              EventTypeLabel(Event.TypeNumber()).c_str(),
                              Event.StartTime,
                              Event.EndTime > Duration ? "end" : std::to_string(Event.EndTime).substr(0, 5).c_str());
        }
        ImGui::EndChild();
    }

    void App::DrawEvents() {
        if (const ImGuiWindow* Clip = ImGui::FindWindowByName("Clip"); Clip && Clip->DockId)
            ImGui::SetNextWindowDockID(Clip->DockId, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Events")) {
            const auto& Current = _Playback.CurrentClip();
            if (!Current) {
                ImGui::TextDisabled("No animation playing.");
            } else if (!Current->Events) {
                ImGui::TextWrapped("No events (TAE) found for %s.", Current->Name.c_str());
            } else {
                const auto& Events   = Current->Events->Events;
                const float Duration = std::max(Current->Animation.Duration, 1e-3f);
                ImGui::Text("%zu events, from %s", Events.size(), Current->EventSource.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                      "Elden Ring's event fields aren't decoded yet: types are shown by number, with the "
                      "name libSouls has for it (from Dark Souls III's list), and fields as raw values.");

                const float Half = ImGui::GetContentRegionAvail().y * 0.5f;
                if (ImGui::BeginTable("events",
                                      3,
                                      ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                                        ImGuiTableFlags_Resizable,
                                      ImVec2(0.f, Half))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.f);
                    ImGui::TableSetupColumn("End", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4.f);
                    ImGui::TableHeadersRow();
                    for (size_t I = 0; I < Events.size(); ++I) {
                        const auto& Event = Events[I];
                        const bool Active =
                          _Playback.Time >= Event.StartTime && _Playback.Time <= EventEnd(Event, Duration);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::PushID(static_cast<int>(I));
                        if (Active)
                            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImVec4(1.f, 0.83f, 0.3f, 1.f)));
                        if (ImGui::Selectable(EventTypeLabel(Event.TypeNumber()).c_str(),
                                              static_cast<int>(I) == _SelectedEvent,
                                              ImGuiSelectableFlags_SpanAllColumns))
                            _SelectedEvent = static_cast<int>(I);
                        ImGui::TableNextColumn();
                        ImGui::Text("%.3f", Event.StartTime);
                        ImGui::TableNextColumn();
                        if (Event.EndTime > Duration) ImGui::TextUnformatted("end");
                        else ImGui::Text("%.3f", Event.EndTime);
                        if (Active) ImGui::PopStyleColor();
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }

                if (_SelectedEvent >= 0 && size_t(_SelectedEvent) < Events.size()) {
                    const auto& Event = Events[_SelectedEvent];
                    ImGui::SeparatorText(EventTypeLabel(Event.TypeNumber()).c_str());
                    if (ImGui::Button("Go to start")) {
                        _Playback.Playing = false;
                        _Playback.Time    = std::clamp(Event.StartTime, 0.f, Duration);
                    }
                    const std::vector<uint8_t>& Raw = Event.RawData;
                    ImGui::TextDisabled("%zu bytes of fields", Raw.size());
                    // The fields as 4-byte values: raw hex, and read as an integer and as a float.
                    if (ImGui::BeginTable("fields",
                                          4,
                                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                            ImGuiTableFlags_BordersInnerV)) {
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableSetupColumn("Offset");
                        ImGui::TableSetupColumn("Bytes");
                        ImGui::TableSetupColumn("Int32");
                        ImGui::TableSetupColumn("Float");
                        ImGui::TableHeadersRow();
                        for (size_t At = 0; At + 4 <= Raw.size(); At += 4) {
                            int32_t Int;
                            float Float;
                            std::memcpy(&Int, Raw.data() + At, 4);
                            std::memcpy(&Float, Raw.data() + At, 4);
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Text("0x%02zX", At);
                            ImGui::TableNextColumn();
                            ImGui::Text("%02X %02X %02X %02X", Raw[At], Raw[At + 1], Raw[At + 2], Raw[At + 3]);
                            ImGui::TableNextColumn();
                            ImGui::Text("%d", Int);
                            ImGui::TableNextColumn();
                            if (std::isfinite(Float) && (Float == 0.f || std::abs(Float) > 1e-6f) &&
                                std::abs(Float) < 1e9f)
                                ImGui::Text("%g", Float);
                            else ImGui::TextDisabled("-");
                        }
                        ImGui::EndTable();
                    }
                }
            }
        }
        ImGui::End();
    }

    void App::DrawViewport() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        if (ImGui::Begin("Viewport")) {
            const ImVec2 Size = ImGui::GetContentRegionAvail();
            const UINT Width  = static_cast<UINT>(std::max(Size.x, 1.f));
            const UINT Height = static_cast<UINT>(std::max(Size.y, 1.f));
            // The game's data is right-handed (Y up, characters facing +Z); Direct3D is left-handed, so the scene is
            // mirrored in Z on its way to the camera.
            const DirectX::XMMATRIX Mirror = DirectX::XMMatrixScaling(1.f, 1.f, -1.f);
            const DirectX::XMMATRIX ViewProjection =
              Mirror * _Camera.View() * _Camera.Projection(float(Width) / float(Height));

            std::vector<QsTransform> Pose;
            if (_Skeleton) Pose = _Playback.ModelPose(*_Skeleton);
            const std::vector<LineVertex> Grid = _ShowGrid ? GridLines() : std::vector<LineVertex>();
            const std::vector<LineVertex> Skeleton =
              _ShowSkeleton && _Skeleton ? SkeletonLines(Pose) : std::vector<LineVertex>();
            _Skin.clear();
            if (_ShowMesh && _Model && _Skeleton) _Model->SkinMatrices(Pose, _Skin);

            Scene Frame;
            DirectX::XMStoreFloat4x4(&Frame.ViewProjection, ViewProjection);
            // A headlight: from the camera toward what it looks at, in the data's (unmirrored) space.
            DirectX::XMFLOAT3 Toward;
            DirectX::XMStoreFloat3(&Toward,
                                   DirectX::XMVector3Normalize(
                                     DirectX::XMVectorSubtract(_Camera.Eye(), DirectX::XMLoadFloat3(&_Camera.Target))));
            Frame.LightDirection = {Toward.x, Toward.y, -Toward.z};
            Frame.Lines          = Grid;
            Frame.OverlayLines   = Skeleton;
            Frame.Skin           = _Skin;
            if (_Model) Frame.Meshes = _Model->Meshes;
            _Graphics.RenderViewport(Width, Height, Frame);
            ImGui::Image(ImTextureRef(_Graphics.ViewportTexture()), ImVec2(float(Width), float(Height)));

            // Camera: left or right drag orbits, middle drag pans, the wheel zooms.
            if (ImGui::IsItemHovered()) {
                const ImGuiIO& IO = ImGui::GetIO();
                if (IO.MouseWheel != 0.f) _Camera.Zoom(IO.MouseWheel);
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Right))
                    _Camera.Orbit(IO.MouseDelta.x, IO.MouseDelta.y);
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
                    _Camera.Pan(IO.MouseDelta.x, IO.MouseDelta.y, float(Height));
            }

            // What's shown, top left.
            ImGui::SetCursorPos(ImVec2(ImGui::GetFontSize() * 0.6f, ImGui::GetFontSize() * 0.4f));
            if (_SelectedCharacter >= 0) {
                const auto& Current = _Playback.CurrentClip();
                ImGui::Text("%s%s%s",
                            _Game->Characters()[_SelectedCharacter].Id.c_str(),
                            Current ? "  " : "",
                            Current ? Current->Name.c_str() : "");
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
#pragma endregion

#pragma region Scene
    std::vector<LineVertex> App::GridLines() {
        std::vector<LineVertex> Lines;

        // A floor grid, half a metre apart.
        constexpr int Half    = 10;
        constexpr float Step  = 0.5f;
        constexpr float Reach = Half * Step;
        for (int I = -Half; I <= Half; ++I) {
            const float At       = I * Step;
            const uint32_t Color = I == 0 ? Rgba(90, 90, 100) : Rgba(52, 52, 60);
            AddLine(Lines, {At, 0.f, -Reach}, {At, 0.f, Reach}, Color);
            AddLine(Lines, {-Reach, 0.f, At}, {Reach, 0.f, At}, Color);
        }
        return Lines;
    }

    std::vector<LineVertex> App::SkeletonLines(const std::vector<QsTransform>& Pose) const {
        std::vector<LineVertex> Lines;
        for (size_t I = 0; I < Pose.size(); ++I) {
            const int Parent    = _Skeleton->Parents[I];
            const bool Selected = static_cast<int>(I) == _SelectedBone;
            if (Parent >= 0)
                AddLine(Lines,
                        Pose[Parent].Translation,
                        Pose[I].Translation,
                        Selected ? Rgba(255, 210, 60) : Rgba(120, 200, 255));
            // A small cross at each joint.
            constexpr float Size   = 0.01f;
            const Souls::Vector3 P = Pose[I].Translation;
            const uint32_t Joint   = Selected ? Rgba(255, 210, 60) : Rgba(200, 230, 255);
            AddLine(Lines, {P.X - Size, P.Y, P.Z}, {P.X + Size, P.Y, P.Z}, Joint);
            AddLine(Lines, {P.X, P.Y - Size, P.Z}, {P.X, P.Y + Size, P.Z}, Joint);
            AddLine(Lines, {P.X, P.Y, P.Z - Size}, {P.X, P.Y, P.Z + Size}, Joint);
        }
        // The selected bone's axes: X red, Y green, Z blue.
        if (_SelectedBone >= 0 && _SelectedBone < static_cast<int>(Pose.size())) {
            const QsTransform& Bone = Pose[_SelectedBone];
            constexpr float Length  = 0.12f;
            AddLine(Lines, Bone.Translation, Apply(Bone, {Length, 0.f, 0.f}), Rgba(240, 70, 70));
            AddLine(Lines, Bone.Translation, Apply(Bone, {0.f, Length, 0.f}), Rgba(70, 220, 90));
            AddLine(Lines, Bone.Translation, Apply(Bone, {0.f, 0.f, Length}), Rgba(80, 130, 255));
        }
        return Lines;
    }

    void App::FrameCharacter() {
        if (!_Skeleton) return;
        // The bounds of the pose as it stands now.
        const std::vector<QsTransform> Pose = _Playback.ModelPose(*_Skeleton);
        Souls::Vector3 Min {1e9f, 1e9f, 1e9f}, Max {-1e9f, -1e9f, -1e9f};
        for (const QsTransform& Bone : Pose) {
            Min = {std::min(Min.X, Bone.Translation.X),
                   std::min(Min.Y, Bone.Translation.Y),
                   std::min(Min.Z, Bone.Translation.Z)};
            Max = {std::max(Max.X, Bone.Translation.X),
                   std::max(Max.Y, Bone.Translation.Y),
                   std::max(Max.Z, Bone.Translation.Z)};
        }
        // The camera works in the mirrored space the viewport draws in.
        const DirectX::XMFLOAT3 Center {(Min.X + Max.X) * 0.5f, (Min.Y + Max.Y) * 0.5f, -(Min.Z + Max.Z) * 0.5f};
        const float DX = Max.X - Min.X, DY = Max.Y - Min.Y, DZ = Max.Z - Min.Z;
        _Camera.Frame(Center, 0.5f * std::sqrt(DX * DX + DY * DY + DZ * DZ));
    }
#pragma endregion
}  // namespace Scarlet

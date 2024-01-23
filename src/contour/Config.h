// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Actions.h>
#include <contour/ConfigDocumentation.h>
#include <contour/display/ShaderConfig.h>

#include <vtbackend/Color.h>
#include <vtbackend/ColorPalette.h>
#include <vtbackend/ControlCode.h>
#include <vtbackend/InputBinding.h>
#include <vtbackend/InputGenerator.h>
#include <vtbackend/MatchModes.h>
#include <vtbackend/Settings.h>
#include <vtbackend/VTType.h>
#include <vtbackend/primitives.h> // CursorDisplay

#include <vtpty/ImageSize.h>
#include <vtpty/PageSize.h>
#include <vtpty/Process.h>
#include <vtpty/SshSession.h>

#include <vtrasterizer/Decorator.h>
#include <vtrasterizer/FontDescriptions.h>

#include <text_shaper/font.h>
#include <text_shaper/mock_font_locator.h>

#include <crispy/StrongLRUHashtable.h>
#include <crispy/assert.h>
#include <crispy/flags.h>
#include <crispy/logstore.h>
#include <crispy/size.h>
#include <crispy/utils.h>

#include <fmt/core.h>

#include <yaml-cpp/emitter.h>
#include <yaml-cpp/node/detail/iterator_fwd.h>
#include <yaml-cpp/ostream_wrapper.h>
#include <yaml-cpp/yaml.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <variant>


namespace contour::config
{

enum class ScrollBarPosition
{
    Hidden,
    Left,
    Right
};

enum class Permission
{
    Deny,
    Allow,
    Ask
};

enum class SelectionAction
{
    Nothing,
    CopyToSelectionClipboard,
    CopyToClipboard,
};

using ActionList = std::vector<actions::Action>;
using KeyInputMapping = vtbackend::InputBinding<vtbackend::Key, ActionList>;
using CharInputMapping = vtbackend::InputBinding<char32_t, ActionList>;
using MouseInputMapping = vtbackend::InputBinding<vtbackend::MouseButton, ActionList>;

struct InputMappings
{
    std::vector<KeyInputMapping> keyMappings;
    std::vector<CharInputMapping> charMappings;
    std::vector<MouseInputMapping> mouseMappings;
};

namespace helper
{
    inline bool testMatchMode(uint8_t actualModeFlags,
                              vtbackend::MatchModes expected,
                              vtbackend::MatchModes::Flag testFlag)
    {
        using MatchModes = vtbackend::MatchModes;
        switch (expected.status(testFlag))
        {
            case MatchModes::Status::Enabled:
                if (!(actualModeFlags & testFlag))
                    return false;
                break;
            case MatchModes::Status::Disabled:
                if ((actualModeFlags & testFlag))
                    return false;
                break;
            case MatchModes::Status::Any: break;
        }
        return true;
    }

    inline bool testMatchMode(uint8_t actualModeFlags, vtbackend::MatchModes expected)
    {
        using Flag = vtbackend::MatchModes::Flag;
        return testMatchMode(actualModeFlags, expected, Flag::AlternateScreen)
               && testMatchMode(actualModeFlags, expected, Flag::AppCursor)
               && testMatchMode(actualModeFlags, expected, Flag::AppKeypad)
               && testMatchMode(actualModeFlags, expected, Flag::Select)
               && testMatchMode(actualModeFlags, expected, Flag::Insert)
               && testMatchMode(actualModeFlags, expected, Flag::Search)
               && testMatchMode(actualModeFlags, expected, Flag::Trace);
    }

} // namespace helper

template <typename Input>
std::vector<actions::Action> const* apply(
    std::vector<vtbackend::InputBinding<Input, ActionList>> const& mappings,
    Input input,
    vtbackend::Modifiers modifiers,
    uint8_t actualModeFlags)
{
    for (vtbackend::InputBinding<Input, ActionList> const& mapping: mappings)
    {
        if (mapping.modifiers == modifiers && mapping.input == input
            && helper::testMatchMode(actualModeFlags, mapping.modes))
        {
            return &mapping.binding;
        }
    }
    return nullptr;
}

struct CursorConfig
{
    vtbackend::CursorShape cursorShape { vtbackend::CursorShape::Block };
    vtbackend::CursorDisplay cursorDisplay { vtbackend::CursorDisplay::Steady };
    std::chrono::milliseconds cursorBlinkInterval;
};

struct InputModeConfig
{
    CursorConfig cursor;
};

struct DualColorConfig
{
    std::string colorSchemeLight = "default";
    std::string colorSchemeDark = "default";
    vtbackend::ColorPalette darkMode {};
    vtbackend::ColorPalette lightMode {};
};

struct SimpleColorConfig
{
    std::string colorScheme = "default";
    vtbackend::ColorPalette colors {};
};

using ColorConfig = std::variant<SimpleColorConfig, DualColorConfig>;

enum class RenderingBackend
{
    Default,
    Software,
    OpenGL,
};

struct WindowMargins
{
    unsigned horizontal = 0; // TODO use boxed
    unsigned vertical = 0;   // TODO use boxed
};

template <typename T, documentation::StringLiteral doc>
struct ConfigEntry
{
    using value_type = T;

    std::string documentation = doc.value;
    constexpr ConfigEntry(): _value {} {}
    constexpr ConfigEntry(T&& in): _value { std::forward<T>(in) } {}

    template <typename F>
    constexpr ConfigEntry(F in): _value { static_cast<T>(in) }
    {
    }

    [[nodiscard]] constexpr T const& get() const { return _value; }
    [[nodiscard]] constexpr T& get() { return _value; }

    constexpr ConfigEntry(ConfigEntry const&) = default;
    constexpr ConfigEntry& operator=(ConfigEntry const&) = default;
    constexpr ConfigEntry(ConfigEntry&&) noexcept = default;
    constexpr ConfigEntry& operator=(ConfigEntry&&) noexcept = default;
    ~ConfigEntry() = default;

  private:
    T _value;
};

template <typename T>
concept ConfigEntryConcept = requires(T t) {
    t.makeDocumentation();
    t._value;
};

struct Bell
{
    std::string sound = "default";
    bool alert = true;
    float volume = 1.0f;
};

const inline vtrasterizer::FontDescriptions defaultFont = vtrasterizer::FontDescriptions {
    .dpiScale = 1.0,
    .dpi = { 0, 0 },
    .size = { 12 },
    .regular = text::font_description { .familyName = { "monospace" },
                                        .weight = text::font_weight::normal,
                                        .slant = text::font_slant::normal,
                                        .spacing = text::font_spacing::proportional,
                                        .strictSpacing = false,
                                        .features = {} },
    .bold = text::font_description { .familyName = { "monospace" },
                                     .weight = text::font_weight::bold,
                                     .slant = text::font_slant::normal,
                                     .spacing = text::font_spacing::proportional,
                                     .strictSpacing = false,
                                     .features = {} },
    .italic = text::font_description { .familyName = { "monospace" },
                                       .weight = text::font_weight::normal,
                                       .slant = text::font_slant::italic,
                                       .spacing = text::font_spacing::proportional,
                                       .strictSpacing = false,
                                       .features = {} },
    .boldItalic = text::font_description { .familyName = { "monospace" },
                                           .weight = text::font_weight::bold,
                                           .slant = text::font_slant::italic,
                                           .spacing = text::font_spacing::proportional,
                                           .strictSpacing = false,
                                           .features = {} },
    .emoji = text::font_description { .familyName = { "emoji" } }, // TODO(pr)
    .renderMode = text::render_mode::gray,
    .textShapingEngine = vtrasterizer::TextShapingEngine::OpenShaper,
    .fontLocator = vtrasterizer::FontLocatorEngine::FontConfig,
    .builtinBoxDrawing = true,
};

struct TerminalProfile
{

    ConfigEntry<vtpty::Process::ExecInfo, documentation::Shell> shell {
        { .program = "bash", .arguments = {}, .workingDirectory = "", .env = {} }
    };
    ConfigEntry<vtpty::SshHostConfig, documentation::SshHostConfig> ssh {};
    ConfigEntry<bool, documentation::Maximized> maximized { false };
    ConfigEntry<bool, documentation::Fullscreen> fullscreen {
        false,
    };
    ConfigEntry<bool, documentation::ShowTitleBar> showTitleBar { true };
    ConfigEntry<bool, "\n"> sizeIndicatorOnResize { true };
    ConfigEntry<bool, documentation::MouseHideWhileTyping> mouseHideWhileTyping { true };
    ConfigEntry<vtbackend::LineOffset, documentation::CopyLastMarkRangeOffset> copyLastMarkRangeOffset = {
        0
    };
    ConfigEntry<std::string, documentation::WMClass> wmClass = { "contour" };
    ConfigEntry<WindowMargins, documentation::Margins> margins = { { 0, 0 } };
    ConfigEntry<vtbackend::PageSize, documentation::TerminalSize> terminalSize {
        { vtbackend::LineCount(25), vtbackend::ColumnCount(80) }
    };
    ConfigEntry<vtbackend::VTType, documentation::TerminalId> terminalId { vtbackend::VTType::VT525 };
    ConfigEntry<vtbackend::MaxHistoryLineCount, documentation::MaxHistoryLineCount> maxHistoryLineCount {
        vtbackend::LineCount(1000)
    };
    ConfigEntry<vtbackend::LineCount, documentation::HistoryScrollMultiplier> historyScrollMultiplier {
        vtbackend::LineCount(3)
    };
    ConfigEntry<ScrollBarPosition, documentation::ScrollbarPosition> scrollbarPosition {
        ScrollBarPosition::Right
    };
    ConfigEntry<vtbackend::StatusDisplayPosition, documentation::StatusDisplayPosition>
        statusDisplayPosition { vtbackend::StatusDisplayPosition::Bottom };
    ConfigEntry<bool, documentation::SyncWindowTitleWithHostWritableStatusDisplay>
        syncWindowTitleWithHostWritableStatusDisplay { false };
    ConfigEntry<bool, documentation::HideScrollbarInAltScreen> hideScrollbarInAltScreen { true };
    ConfigEntry<bool, documentation::Dummy> optionKeyAsAlt { false }; // TODO(pr)
    ConfigEntry<bool, documentation::AutoScrollOnUpdate> autoScrollOnUpdate { true };
    ConfigEntry<vtrasterizer::FontDescriptions, documentation::Fonts> fonts { defaultFont };
    ConfigEntry<Permission, documentation::CaptureBuffer> captureBuffer { Permission::Ask };
    ConfigEntry<Permission, documentation::ChangeFont> changeFont { Permission::Ask };
    ConfigEntry<Permission, documentation::DisplayHostWritableStatusLine> displayHostWritableStatusLine {
        Permission::Ask
    };
    ConfigEntry<bool, documentation::DrawBoldTextWithBrightColors> drawBoldTextWithBrightColors { false };
    ConfigEntry<ColorConfig, documentation::Colors> colors { SimpleColorConfig {} };
    ConfigEntry<vtbackend::LineCount, documentation::ModalCursorScrollOff> modalCursorScrollOff {
        vtbackend::LineCount { 8 }
    };
    ConfigEntry<InputModeConfig, documentation::ModeInsert> modeInsert { CursorConfig {
        vtbackend::CursorShape::Bar, vtbackend::CursorDisplay::Steady, std::chrono::milliseconds { 500 } } };
    ConfigEntry<InputModeConfig, documentation::ModeNormal> modeNormal { CursorConfig {
        vtbackend::CursorShape::Block,
        vtbackend::CursorDisplay::Steady,
        std::chrono::milliseconds { 500 } } };
    ConfigEntry<InputModeConfig, documentation::ModeVisual> modeVisual { CursorConfig {
        vtbackend::CursorShape::Block,
        vtbackend::CursorDisplay::Steady,
        std::chrono::milliseconds { 500 } } };
    ConfigEntry<std::chrono::milliseconds, documentation::SmoothLineScrolling> smoothLineScrolling { 100 };
    ConfigEntry<std::chrono::milliseconds, documentation::HighlightTimeout> highlightTimeout { 100 };
    ConfigEntry<bool, documentation::HighlightDoubleClickerWord> highlightDoubleClickedWord { true };
    ConfigEntry<vtbackend::StatusDisplayType, documentation::InitialStatusLine> initialStatusDisplayType {
        vtbackend::StatusDisplayType::None
    };
    ConfigEntry<vtbackend::Opacity, documentation::BackgroundOpacity> backgroundOpacity { vtbackend::Opacity(
        0xFF) };
    ConfigEntry<bool, documentation::BackgroundBlur> backgroundBlur { false };
    ConfigEntry<std::optional<contour::display::ShaderConfig>, documentation::Dummy>
        backgroundShader {}; // TODO(pr)
    ConfigEntry<std::optional<contour::display::ShaderConfig>, documentation::Dummy>
        textShader {}; // TODO(pr)
    ConfigEntry<vtrasterizer::Decorator, "normal: {} \n"> hyperlinkDecorationNormal {
        vtrasterizer::Decorator::DottedUnderline
    };
    ConfigEntry<vtrasterizer::Decorator, "hover: {} \n"> hyperlinkDecorationHover {
        vtrasterizer::Decorator::Underline
    };
    ConfigEntry<Bell, documentation::Bell> bell { { .sound = "default", .alert = true, .volume = 1.0f } };
    ConfigEntry<std::map<vtbackend::DECMode, bool>, "fmt formatted doc {} \n"> frozenModes {};
};

const inline TerminalProfile defaultProfile = TerminalProfile {};
const inline InputMappings defaultInputMappings = InputMappings {
    .keyMappings {
        KeyInputMapping { .modes = []() consteval -> vtbackend::MatchModes {
                             auto mods = vtbackend::MatchModes();
                             mods.enable(vtbackend::MatchModes::Select);
                             mods.enable(vtbackend::MatchModes::Insert);
                             return mods;
                         }(),
                          .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt } },
                          .input = vtbackend::Key::Enter,
                          .binding = { { actions::ToggleFullscreen {} } } },
        KeyInputMapping { .modes {},
                          .modifiers { vtbackend::Modifiers {} },
                          .input = vtbackend::Key::Escape,
                          .binding = { { actions::CancelSelection {} } } },
        KeyInputMapping { .modes { vtbackend::MatchModes {} },
                          .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                          .input = vtbackend::Key::DownArrow,
                          .binding = { { actions::ScrollOneDown {} } } },
        KeyInputMapping { .modes { vtbackend::MatchModes {} },
                          .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                          .input = vtbackend::Key::End,
                          .binding = { { actions::ScrollToBottom {} } } },
        KeyInputMapping { .modes { vtbackend::MatchModes {} },
                          .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                          .input = vtbackend::Key::Home,
                          .binding = { { actions::ScrollToTop {} } } },
        KeyInputMapping { .modes { vtbackend::MatchModes {} },
                          .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                          .input = vtbackend::Key::PageDown,
                          .binding = { { actions::ScrollPageDown {} } } },
        KeyInputMapping { .modes { vtbackend::MatchModes {} },
                          .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                          .input = vtbackend::Key::PageUp,
                          .binding = { { actions::ScrollPageUp {} } } },
        KeyInputMapping { .modes { vtbackend::MatchModes {} },
                          .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                          .input = vtbackend::Key::UpArrow,
                          .binding = { { actions::ScrollOneUp {} } } },
        KeyInputMapping { .modes { vtbackend::MatchModes {} },
                          .modifiers { vtbackend::Modifiers {} },
                          .input = vtbackend::Key::F3,
                          .binding = { { actions::FocusNextSearchMatch {} } } },
        KeyInputMapping { .modes { vtbackend::MatchModes {} },
                          .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                          .input = vtbackend::Key::F3,
                          .binding = { { actions::FocusPreviousSearchMatch {} } } },
        //     KeyInputMapping { .modes { vtbackend::MatchModes {} },
        //                       .modifiers { vtbackend::Modifiers {  vtbackend::Modifier {
        //                       vtbackend::Modifier::Shift }
        //                                    | vtbackend::Modifiers { vtbackend::Modifier::Control }
        //                                    }
        //                                    },
        //                       .input { vtbackend::Key::Plus },
        //                       .binding = { { actions::IncreaseFontSize {} } } },
        //     KeyInputMapping { .modes { vtbackend::MatchModes {} },
        //                       .modifiers { vtbackend::Modifiers {  vtbackend::Modifier {
        //                       vtbackend::Modifier::Shift }
        //                                    | vtbackend::Modifier { vtbackend::Modifier::Control }
        //                                    }
        //                                    },
        //                       .input { vtbackend::Key::Minus },
        //                       .binding = { { actions::DecreaseFontSize {} } } },
    },
    .charMappings {

        CharInputMapping {
            .modes { vtbackend::MatchModes {} },
            .modifiers { vtbackend::Modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                                | vtbackend::Modifiers { vtbackend::Modifier::Control } } },
            .input = '_',
            .binding = { { actions::DecreaseFontSize {} } } },
        CharInputMapping {
            .modes { vtbackend::MatchModes {} },
            .modifiers { vtbackend::Modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                                | vtbackend::Modifiers { vtbackend::Modifier::Control } } },
            .input = 'N',
            .binding = { actions::NewTerminal {} } },
        CharInputMapping {
            .modes { vtbackend::MatchModes {} },
            .modifiers { vtbackend::Modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                                | vtbackend::Modifiers { vtbackend::Modifier::Control } } },
            .input = 'V',
            .binding = { { actions::PasteClipboard { .strip = false } } } },
        CharInputMapping {
            .modes { vtbackend::MatchModes {} },
            .modifiers { vtbackend::Modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                                | vtbackend::Modifiers { vtbackend::Modifier::Control } } },
            .input = 'V',
            .binding = { { actions::PasteClipboard { .strip = false } } } },

        CharInputMapping { .modes { vtbackend::MatchModes {} },
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt }
                                        | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'S',
                           .binding = { { actions::ScreenshotVT {} } } },
        CharInputMapping { .modes { vtbackend::MatchModes {} },
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'O',
                           .binding = { { actions::ResetFontSize {} } } },
        CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                              auto mods = vtbackend::MatchModes();
                              mods.enable(vtbackend::MatchModes::Select);
                              mods.enable(vtbackend::MatchModes::Insert);
                              return mods;
                          }(),
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'C',
                           .binding = { { actions::CopySelection {} } } },
        CharInputMapping { .modes = []() consteval -> vtbackend::MatchModes {
                              auto mods = vtbackend::MatchModes();
                              mods.enable(vtbackend::MatchModes::Select);
                              mods.enable(vtbackend::MatchModes::Insert);
                              return mods;
                          }(),
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'C',
                           .binding = { { actions::CancelSelection {} } } },
        CharInputMapping { .modes = []() consteval -> vtbackend::MatchModes {
                              auto mods = vtbackend::MatchModes();
                              mods.enable(vtbackend::MatchModes::Select);
                              mods.enable(vtbackend::MatchModes::Insert);
                              return mods;
                          }(),
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'V',
                           .binding = { { actions::PasteClipboard {} } } },
        CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                              auto mods = vtbackend::MatchModes();
                              mods.enable(vtbackend::MatchModes::Select);
                              mods.enable(vtbackend::MatchModes::Insert);
                              return mods;
                          }(),
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'V',
                           .binding = { { actions::CancelSelection {} } } },
        CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                              auto mods = vtbackend::MatchModes();
                              mods.enable(vtbackend::MatchModes::Select);
                              return mods;
                          }(),
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                        | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = ' ', // SPACE
                           .binding = { { actions::ViNormalMode {} } } },
        CharInputMapping { .modes { vtbackend::MatchModes {} },
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                        | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = ',',
                           .binding = { { actions::OpenConfiguration {} } } },
        CharInputMapping { .modes { vtbackend::MatchModes {} },
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                        | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'Q',
                           .binding = { { actions::Quit {} } } },
        CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                              auto mods = vtbackend::MatchModes();
                              mods.disable(vtbackend::MatchModes::AlternateScreen);
                              return mods;
                          }(),
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt }
                                        | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'K',
                           .binding = { { actions::ScrollMarkUp {} } } },
        CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                              auto mods = vtbackend::MatchModes();
                              mods.disable(vtbackend::MatchModes::AlternateScreen);
                              return mods;
                          }(),
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt }
                                        | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'J',
                           .binding = { { actions::ScrollMarkDown {} } } },
        CharInputMapping { .modes { vtbackend::MatchModes {} },
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt }
                                        | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'O',
                           .binding = { { actions::OpenFileManager {} } } },
        CharInputMapping { .modes { vtbackend::MatchModes {} },
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt }
                                        | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = '.',
                           .binding = { { actions::ToggleStatusLine {} } } },
        CharInputMapping { .modes { vtbackend::MatchModes {} },
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                        | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'F',
                           .binding = { { actions::SearchReverse {} } } },
        CharInputMapping { .modes { vtbackend::MatchModes {} },
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift }
                                        | vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'H',
                           .binding = { { actions::NoSearchHighlight {} } } },
    },
    .mouseMappings {
        MouseInputMapping { .modes { vtbackend::MatchModes {} },
                            .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                            .input = vtbackend::MouseButton::Left,
                            .binding = { { actions::FollowHyperlink {} } } },
        MouseInputMapping { .modes { vtbackend::MatchModes {} },
                            .modifiers { vtbackend::Modifiers { vtbackend::Modifier::None } },
                            .input = vtbackend::MouseButton::Middle,
                            .binding = { { actions::PasteSelection {} } } },
        MouseInputMapping { .modes { vtbackend::MatchModes {} },
                            .modifiers { vtbackend::Modifiers { vtbackend::Modifier::None } },
                            .input = vtbackend::MouseButton::WheelDown,
                            .binding = { { actions::ScrollDown {} } } },
        MouseInputMapping { .modes { vtbackend::MatchModes {} },
                            .modifiers { vtbackend::Modifiers { vtbackend::Modifier::None } },
                            .input = vtbackend::MouseButton::WheelUp,
                            .binding = { { actions::ScrollUp {} } } },
        MouseInputMapping { .modes { vtbackend::MatchModes {} },
                            .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt } },
                            .input = vtbackend::MouseButton::WheelDown,
                            .binding = { { actions::DecreaseOpacity {} } } },
        MouseInputMapping { .modes { vtbackend::MatchModes {} },
                            .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Alt } },
                            .input = vtbackend::MouseButton::WheelUp,
                            .binding = { { actions::IncreaseOpacity {} } } },
        MouseInputMapping { .modes { vtbackend::MatchModes {} },
                            .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                            .input = vtbackend::MouseButton::WheelDown,
                            .binding = { { actions::DecreaseFontSize {} } } },
        MouseInputMapping { .modes { vtbackend::MatchModes {} },
                            .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                            .input = vtbackend::MouseButton::WheelUp,
                            .binding = { { actions::IncreaseFontSize {} } } },
        MouseInputMapping { .modes { vtbackend::MatchModes {} },
                            .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                            .input = vtbackend::MouseButton::WheelDown,
                            .binding = { { actions::ScrollPageDown {} } } },
        MouseInputMapping { .modes { vtbackend::MatchModes {} },
                            .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Shift } },
                            .input = vtbackend::MouseButton::WheelUp,
                            .binding = { { actions::ScrollPageUp {} } } },
    }
};
const inline vtbackend::ColorPalette defaultColorSchemes = vtbackend::ColorPalette {};

struct Config
{
    std::filesystem::path backingFilePath { "none" };
    ConfigEntry<bool, documentation::Live> live { false };
    ConfigEntry<std::string, documentation::PlatformPlugin> platformPlugin { "xcb" };
    ConfigEntry<RenderingBackend, documentation::RenderingBackend> renderingBackend {
        RenderingBackend::Default
    };
    ConfigEntry<bool, documentation::TextureAtlasDirectMapping> textureAtlasDirectMapping { false };
    ConfigEntry<crispy::strong_hashtable_size, documentation::TextureAtlasHashtableSlots>
        textureAtlasHashtableSlots { 4096 };
    ConfigEntry<crispy::lru_capacity, documentation::TextureAtlasTileCount> textureAtlasTileCount { 4000 };
    ConfigEntry<int, documentation::PTYReadBufferSize> ptyReadBufferSize { 16384 };
    ConfigEntry<int, documentation::PTYBufferObjectSize> ptyBufferObjectSize { 1024 * 1024 };
    ConfigEntry<bool, documentation::ReflowOnResize> reflowOnResize { true };
    ConfigEntry<std::unordered_map<std::string, vtbackend::ColorPalette>, documentation::ColorSchemes>
        colorschemes { { { "default", defaultColorSchemes } } };
    ConfigEntry<std::unordered_map<std::string, TerminalProfile>, documentation::Profiles> profiles {
        { { "main", defaultProfile } }
    };
    ConfigEntry<std::string, "default_profile: {} \n"> defaultProfileName { "main" };
    ConfigEntry<std::string, documentation::WordDelimiters> wordDelimiters {
        " /\\\\()\\\"'-.,:;<>~!@#$%^&*+=[]{}~?|│"
    };
    ConfigEntry<vtbackend::Modifiers, documentation::BypassMouseProtocolModifiers>
        bypassMouseProtocolModifiers { vtbackend::Modifier::Shift };
    ConfigEntry<contour::config::SelectionAction, documentation::OnMouseSelection> onMouseSelection {
        contour::config::SelectionAction::CopyToSelectionClipboard
    };
    ConfigEntry<vtbackend::Modifiers, documentation::MouseBlockSelectionModifiers>
        mouseBlockSelectionModifiers { vtbackend::Modifier::Control };
    ConfigEntry<InputMappings, documentation::InputMappings> inputMappings { defaultInputMappings };
    ConfigEntry<bool, documentation::SpawnNewProcess> spawnNewProcess { false };
    ConfigEntry<bool, documentation::SixelScrolling> sixelScrolling { true };
    ConfigEntry<vtbackend::ImageSize, documentation::MaxImageSize> maxImageSize { { vtpty::Width { 0 },
                                                                                    vtpty::Height { 0 } } };
    ConfigEntry<int, documentation::MaxImageColorRegisters> maxImageColorRegisters { 4096 };
    ConfigEntry<std::set<std::string>, documentation::ExperimentalFeatures> experimentalFeatures {};

    TerminalProfile* profile(std::string const& name) noexcept
    {
        assert(!name.empty());
        if (auto i = profiles.get().find(name); i != profiles.get().end())
            return &i->second;
        assert(false && "Profile not found.");
        return nullptr;
    }

    [[nodiscard]] TerminalProfile const* profile(std::string const& name) const
    {
        assert(!name.empty());
        if (auto i = profiles.get().find(name); i != profiles.get().end())
            return &i->second;
        assert(false && "Profile not found.");
        crispy::unreachable();
    }

    TerminalProfile& profile() noexcept
    {
        if (auto* prof = profile(defaultProfileName.get()); prof)
            return *prof;
        crispy::unreachable();
    }

    [[nodiscard]] TerminalProfile const& profile() const noexcept
    {
        if (auto const* prof = profile(defaultProfileName.get()); prof)
            return *prof;
        crispy::unreachable();
    }
};

struct YAMLVisitor
{

    YAML::Node doc;
    logstore::category logger;

    YAMLVisitor(std::string const& filename, auto log): logger { log }
    {
        try
        {
            doc = YAML::LoadFile(filename);
        }
        catch (std::exception const& e)
        {
            errorLog()("Configuration file is corrupted. {} \n Default config will be loaded", e.what());
        }
    }

    template <typename T, documentation::StringLiteral D>
    void loadFromEntry(YAML::Node node, std::string entry, ConfigEntry<T, D>& where)
    {
        logger()("Loading entry: {}", entry);
        try
        {
            loadFromEntry(node, entry, where.get());
        }
        catch (std::exception const& e)
        {
            logger()("Failed, default value will be used");
        }
    }

    template <typename T, documentation::StringLiteral D>
    void loadFromEntry(std::string entry, ConfigEntry<T, D>& where)
    {
        loadFromEntry(doc, entry, where.get());
    }

    template <typename T>
    void loadFromEntry(YAML::Node, std::string, T&)
    {
        static_assert(false);
    }

    template <typename T>
        requires std::is_scalar<T>::value
    void loadFromEntry(YAML::Node node, std::string entry, T& where)
    {
        auto const child = node[entry];
        if (child)
            where = child.as<T>();
        logger()("Loading entry: {}, value {}", entry, where);
    }

    template <typename V, typename T>
    void loadFromEntry(YAML::Node node, std::string entry, boxed::boxed<V, T>& where)
    {
        auto const child = node[entry];
        if (child)
            where = boxed::boxed<V, T>(child.as<V>());
        logger()("Loading entry: {}, value {}", entry, where.template as<V>());
    }

    // Used for terminal profile and color scheme loading
    template <typename T>
    void loadFromEntry(YAML::Node node, std::string entry, std::unordered_map<std::string, T>& where)
    {
        auto const child = node[entry];

        if (child)
        {
            if (child.IsMap())
            {
                for (auto entry = child.begin(); entry != child.end(); ++entry)
                {
                    logger()("Loading map with entry: {}", entry->first.as<std::string>());
                    auto const& name = entry->first.as<std::string>();
                    loadFromEntry(child, name, where[name]);
                }
            }
        }
    }

    template <typename T>
    void loadFromEntry(YAML::Node node, std::string entry, std::chrono::duration<long, T>& where)
    {
        auto const child = node[entry];
        if (child)
            where = std::chrono::milliseconds(child.as<int>());
        logger()("Loading entry: {}, value {}", entry, where.count());
    }

    template <typename Input>
    void appendOrCreateBinding(std::vector<vtbackend::InputBinding<Input, ActionList>>& bindings,
                               vtbackend::MatchModes modes,
                               vtbackend::Modifiers modifier,
                               Input input,
                               actions::Action action)
    {
        for (auto& binding: bindings)
        {
            if (match(binding, modes, modifier, input))
            {
                binding.binding.emplace_back(std::move(action));
                return;
            }
        }

        bindings.emplace_back(vtbackend::InputBinding<Input, ActionList> {
            modes, modifier, input, ActionList { std::move(action) } });
    }

    void loadFromEntry(YAML::Node node, std::string entry, std::filesystem::path& where);
    void loadFromEntry(YAML::Node node, std::string entry, RenderingBackend& where);
    void loadFromEntry(YAML::Node node, std::string entry, crispy::strong_hashtable_size& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtbackend::MaxHistoryLineCount& where);
    void loadFromEntry(YAML::Node node, std::string entry, crispy::lru_capacity& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtbackend::CursorDisplay& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtbackend::Modifiers& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtbackend::CursorShape& where);
    void loadFromEntry(YAML::Node node, std::string entry, contour::config::SelectionAction& where);
    void loadFromEntry(YAML::Node node, std::string entry, InputMappings& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtbackend::ImageSize& where);
    void loadFromEntry(YAML::Node node, std::string entry, std::set<std::string>& where)
    {
        // TODO(pr)
    }
    void loadFromEntry(YAML::Node node, std::string entry, std::string& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtbackend::StatusDisplayPosition& where);
    void loadFromEntry(YAML::Node node, std::string entry, ScrollBarPosition& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtrasterizer::FontDescriptions& where);
    void loadFromEntry(YAML::Node node, std::string entry, text::render_mode& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtrasterizer::FontLocatorEngine& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtrasterizer::TextShapingEngine& where);
    void loadFromEntry(YAML::Node node, std::string entry, ColorConfig& where);
    void loadFromEntry(YAML::Node node, std::string entry, text::font_description& where);
    void loadFromEntry(YAML::Node node, std::string entry, std::vector<text::font_feature>& where)
    {
        // TODO(pr)
    }
    void loadFromEntry(YAML::Node node, std::string entry, text::font_weight& where);
    void loadFromEntry(YAML::Node node, std::string entry, text::font_slant& where);
    void loadFromEntry(YAML::Node node, std::string entry, text::font_size& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtbackend::LineCount& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtbackend::VTType& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtpty::PageSize& where);
    void loadFromEntry(YAML::Node node, std::string entry, WindowMargins& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtbackend::LineOffset& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtpty::Process::ExecInfo& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtpty::SshHostConfig& where);
    void loadFromEntry(YAML::Node node, std::string entry, Bell& where);
    void loadFromEntry(YAML::Node node, std::string entry, std::map<vtbackend::DECMode, bool>& where)
    {
        // TODO(pr)
    }
    void loadFromEntry(YAML::Node node, std::string entry, vtrasterizer::Decorator& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtbackend::Opacity& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtbackend::StatusDisplayType& where);
    void loadFromEntry(YAML::Node node, std::string entry, Permission& where);
    void loadFromEntry(YAML::Node node, std::string entry, std::shared_ptr<vtbackend::BackgroundImage> where);
    void loadFromEntry(YAML::Node node, std::string entry, vtbackend::CellRGBColor& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtbackend::CursorColor& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtbackend::RGBColor& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtbackend::RGBColorPair& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtbackend::CellRGBColorAndAlphaPair& where);
    void loadFromEntry(YAML::Node node, std::string entry, vtbackend::ColorPalette::Palette& colors);
    void loadFromEntry(YAML::Node node, std::string entry, vtbackend::ColorPalette& where);
    void loadFromEntry(YAML::Node node, std::string entry, TerminalProfile& where);

    void load(Config& c);

    std::optional<actions::Action> parseAction(YAML::Node node);
    std::optional<vtbackend::Modifiers> parseModifierKey(std::string const& key);
    std::optional<vtbackend::Modifiers> parseModifier(YAML::Node nodeYAML);
    static std::optional<vtbackend::MatchModes> parseMatchModes(YAML::Node nodeYAML);
    std::optional<vtbackend::Key> parseKey(std::string const& name);
    std::optional<std::variant<vtbackend::Key, char32_t>> parseKeyOrChar(std::string const& name);

    bool tryAddKey(InputMappings& inputMappings,
                   vtbackend::MatchModes modes,
                   vtbackend::Modifiers modifier,
                   YAML::Node const& node,
                   actions::Action action);
    std::optional<vtbackend::MouseButton> parseMouseButton(YAML::Node const& node);
    bool tryAddMouse(std::vector<MouseInputMapping>& bindings,
                     vtbackend::MatchModes modes,
                     vtbackend::Modifiers modifier,
                     YAML::Node const& node,
                     actions::Action action);
};

struct YAMLConfigWriter
{

    std::string addOffset(std::string const& doc, size_t off)
    {
        auto offset = std::string(off, ' ');
        return std::regex_replace(doc, std::regex(".+\n"), offset + "$&");
    }

    struct Offset
    {
        static int levels;
        Offset() { levels++; }
        ~Offset() { --levels; }
    };

    std::string createString(Config const& c);

    template <typename... T>
    [[nodiscard]] std::string format(std::string_view doc, T... args)
    {
        return fmt::format(fmt::runtime(doc), args..., fmt::arg("comment", "#"));
    }

    [[nodiscard]] std::string format(KeyInputMapping v)
    {
        return fmt::format(fmt::runtime("{:<30},{:<30},{:<30}\n"),
                           fmt::format(fmt::runtime("- {{ mods: [{}]"), v.modifiers),
                           fmt::format(fmt::runtime(" key: '{}'"), v.input),
                           fmt::format(fmt::runtime(" action: {} }}"), v.binding[0]));
    }

    [[nodiscard]] std::string format(CharInputMapping v)
    {

        auto actionAndModes = fmt::format(fmt::runtime(" action: {} }}"), v.binding[0]);
        if (v.modes.any())
        {
            actionAndModes = fmt::format(fmt::runtime(" action: {}, mode: [{}] }}"), v.binding[0], v.modes);
        }
        return fmt::format(fmt::runtime("{:<30},{:<30},{:<30}\n"),
                           fmt::format(fmt::runtime("- {{ mods: [{}]"), v.modifiers),
                           fmt::format(fmt::runtime(" key: '{}'"), static_cast<char>(v.input)),
                           actionAndModes);
    }

    [[nodiscard]] std::string format(MouseInputMapping v)
    {
        auto actionAndModes = fmt::format(fmt::runtime(" action: {} }}"), v.binding[0]);
        return fmt::format(fmt::runtime("{:<30},{:<30},{:<30}\n"),
                           fmt::format(fmt::runtime("- {{ mods: [{}]"), v.modifiers),
                           fmt::format(fmt::runtime(" mouse: {}"), v.input),
                           actionAndModes);
    }

    [[nodiscard]] std::string format(std::string_view doc, vtrasterizer::FontDescriptions v)
    {
        return format(doc,
                      v.size.pt,
                      v.fontLocator,
                      v.textShapingEngine,
                      v.builtinBoxDrawing,
                      v.renderMode,
                      "true",
                      v.regular.familyName,
                      v.regular.weight,
                      v.regular.slant,
                      "", // font features // TODO(pr)
                      v.emoji.familyName);
    }

    template <typename T, typename R>
    [[nodiscard]] std::string format(std::string_view doc, std::chrono::duration<T, R> v)
    {
        return format(doc, v.count());
    }

    [[nodiscard]] std::string format(std::string_view doc, vtpty::Process::ExecInfo v)
    {
        auto args = std::string { "[" };
        for (auto&& arg: v.arguments)
        {
            args.append(arg);
            args.append(",");
        }
        args.append("]");
        return format(doc, v.program, args);
    }

    [[nodiscard]] std::string format(std::string_view doc, vtbackend::MaxHistoryLineCount v)
    {
        if (std::holds_alternative<vtbackend::Infinite>(v))
            return format(doc, -1);
        auto number = unbox(std::get<vtbackend::LineCount>(v));
        return format(doc, number);
    }

    [[nodiscard]] std::string format(std::string_view doc, vtbackend::ImageSize v)
    {
        return format(doc, unbox(v.width), unbox(v.height));
    }

    [[nodiscard]] std::string format(std::string_view doc, vtbackend::PageSize v)
    {
        return format(doc, unbox(v.columns), unbox(v.lines));
    }


    [[nodiscard]] std::string format(std::string_view doc, ColorConfig& v)
    {

        if (auto* simple = get_if<SimpleColorConfig>(&v))
            return format(doc,simple->colorScheme);
        else if (auto* dual = get_if<DualColorConfig>(&v))
        {
            return format(doc, fmt::format(fmt::runtime("\n"
                                                         "    light: {}\n"
                                                         "    dark: {}\n"
                                                         "\n"),
                                           dual->colorSchemeLight,
                                           dual->colorSchemeDark));
        }

        return format(doc, "BAD");
    }

    [[nodiscard]] std::string format(std::string_view doc, Bell& v)
    {
        return format(doc, v.sound, v.volume, v.alert);
    }

    [[nodiscard]] std::string format(std::string_view doc, WindowMargins& v)
    {
        return format(doc, v.horizontal, v.vertical);
    }

    [[nodiscard]] std::string format(std::string_view doc, InputModeConfig v)
    {
        auto shapeType = v.cursor.cursorShape;
        auto shape = "block";
        switch (shapeType)
        {
            case vtbackend::CursorShape::Block: break;
            case vtbackend::CursorShape::Rectangle: shape = "rectangle"; break;
            case vtbackend::CursorShape::Underscore: shape = "underscore"; break;
            case vtbackend::CursorShape::Bar: shape = "bar"; break;
        };
        auto blinking = v.cursor.cursorDisplay == vtbackend::CursorDisplay::Blink ? true : false;
        auto blinkingInterval = v.cursor.cursorBlinkInterval.count();
        return format(doc, shape, blinking, blinkingInterval);
    }
};

std::filesystem::path configHome();
std::filesystem::path configHome(std::string const& programName);

std::optional<std::string> readConfigFile(std::string const& filename);

void loadConfigFromFile(Config& config, std::filesystem::path const& fileName);
Config loadConfigFromFile(std::filesystem::path const& fileName);
Config loadConfig();

std::string defaultConfigString();
std::error_code createDefaultConfig(std::filesystem::path const& path);
std::string defaultConfigFilePath();

} // namespace contour::config

// {{{ fmtlib custom formatter support

template <>
struct fmt::formatter<contour::config::Permission>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(contour::config::Permission perm, FormatContext& ctx)
    {
        switch (perm)
        {
            case contour::config::Permission::Allow: return fmt::format_to(ctx.out(), "allow");
            case contour::config::Permission::Deny: return fmt::format_to(ctx.out(), "deny");
            case contour::config::Permission::Ask: return fmt::format_to(ctx.out(), "ask");
        }
        return ctx.out();
    }
};

template <>
struct fmt::formatter<vtbackend::Opacity>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(vtbackend::Opacity value, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "{}", static_cast<int>(value) / std::numeric_limits<uint8_t>::max());
    }
};

template <>
struct fmt::formatter<crispy::strong_hashtable_size>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(crispy::strong_hashtable_size value, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "{}", value.value);
    }
};

template <>
struct fmt::formatter<vtbackend::StatusDisplayPosition>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(vtbackend::StatusDisplayPosition value, FormatContext& ctx)
    {
        switch (value)
        {
            case vtbackend::StatusDisplayPosition::Bottom: return fmt::format_to(ctx.out(), "Bottom");
            case vtbackend::StatusDisplayPosition::Top: return fmt::format_to(ctx.out(), "Top");
        }
    }
};

template <>
struct fmt::formatter<vtbackend::BackgroundImage>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(vtbackend::BackgroundImage value, FormatContext& ctx)
    {
        if (auto loc = std::get_if<std::filesystem::path>(&value.location))
            return fmt::format_to(ctx.out(), loc);
        return fmt::format_to(ctx.out(), "Image");
    }
};

template <>
struct fmt::formatter<vtbackend::StatusDisplayType>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(vtbackend::StatusDisplayType value, FormatContext& ctx)
    {
        switch (value)
        {
            case vtbackend::StatusDisplayType::None: return fmt::format_to(ctx.out(), "none");
            case vtbackend::StatusDisplayType::Indicator: return fmt::format_to(ctx.out(), "indicator");
            case vtbackend::StatusDisplayType::HostWritable:
                return fmt::format_to(ctx.out(), "host writable");
        }
    }
};

template <>
struct fmt::formatter<crispy::lru_capacity>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(crispy::lru_capacity value, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "{}", value.value);
    }
};

template <>
struct fmt::formatter<vtbackend::ColorPalette>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(vtbackend::ColorPalette value, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "color paletter TODO(pr)");
    }
};

template <>
struct fmt::formatter<std::unordered_map<std::basic_string<char>, vtbackend::ColorPalette>>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(std::unordered_map<std::basic_string<char>, vtbackend::ColorPalette> value,
                FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "map color palette : TODO(pr)");
    }
};

template <>
struct fmt::formatter<std::set<std::basic_string<char>>>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(std::set<std::basic_string<char>> value, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "set string :TODO(pr)");
    }
};

template <>
struct fmt::formatter<contour::config::SelectionAction>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    using SelectionAction = contour::config::SelectionAction;
    template <typename FormatContext>
    auto format(SelectionAction value, FormatContext& ctx)
    {
        switch (value)
        {
            case SelectionAction::CopyToClipboard: return fmt::format_to(ctx.out(), "CopyToClipboard");
            case SelectionAction::CopyToSelectionClipboard:
                return fmt::format_to(ctx.out(), "CopyToSelectionClipboard");
            case SelectionAction::Nothing: return fmt::format_to(ctx.out(), "Waiting");
        }
        return fmt::format_to(ctx.out(), "{}", static_cast<unsigned>(value));
    }
};

template <>
struct fmt::formatter<contour::config::ScrollBarPosition>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    using ScrollBarPosition = contour::config::ScrollBarPosition;
    template <typename FormatContext>
    auto format(ScrollBarPosition value, FormatContext& ctx)
    {
        switch (value)
        {
            case ScrollBarPosition::Hidden: return fmt::format_to(ctx.out(), "Hidden");
            case ScrollBarPosition::Left: return fmt::format_to(ctx.out(), "Left");
            case ScrollBarPosition::Right: return fmt::format_to(ctx.out(), "Right");
        }
        return ctx.out();
    }
};

template <>
struct fmt::formatter<contour::config::RenderingBackend>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }

    static auto format(contour::config::RenderingBackend const& val, fmt::format_context& ctx)
    {
        switch (val)
        {
            case contour::config::RenderingBackend::Default: return fmt::format_to(ctx.out(), "default");
            case contour::config::RenderingBackend::OpenGL: return fmt::format_to(ctx.out(), "OpenGL");
            case contour::config::RenderingBackend::Software: return fmt::format_to(ctx.out(), "software");
        }
        return fmt::format_to(ctx.out(), "{}", "none");
    };
};

template <>
struct fmt::formatter<contour::config::WindowMargins>: public fmt::formatter<std::string>
{
    using WindowMargins = contour::config::WindowMargins;
    auto format(WindowMargins margins, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(fmt::format("{}x+{}y", margins.horizontal, margins.vertical),
                                              ctx);
    }
};

template <typename T, contour::config::documentation::StringLiteral D>
struct fmt::formatter<contour::config::ConfigEntry<T, D>>
{
    auto format(contour::config::ConfigEntry<T, D> const& c, fmt::format_context& ctx)
    {
        return fmt::format_to(ctx.out(), "{}", c.get());
    };
};
// }}}

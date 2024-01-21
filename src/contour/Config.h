// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Actions.h>
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
    vtbackend::ColorPalette darkMode {};
    vtbackend::ColorPalette lightMode {};
};

struct SimpleColorConfig
{
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

template <std::size_t N>
struct StringLiteral
{
    constexpr StringLiteral(const char (&str)[N]) { std::copy_n(str, N, value); }

    char value[N];
};

template <typename T, StringLiteral doc>
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
    .emoji = text::font_description { .familyName = {"emoji"}}, // TODO(pr)
    .renderMode = text::render_mode::gray,
    .textShapingEngine = vtrasterizer::TextShapingEngine::OpenShaper,
    .fontLocator = vtrasterizer::FontLocatorEngine::FontConfig,
    .builtinBoxDrawing = true,
};

struct TerminalProfile
{

    ConfigEntry<vtpty::Process::ExecInfo,
                "{comment} You can override the process to be started inside the terminal."
                "{comment} If nothing is specified, the users' default login shell will be used.\n"
                "{comment} But you may as well log in to a remote host.\n"
                "shell: {}\n"
                "arguments: {}\n"
                "\n">
        shell { { .program = "bash", .arguments = {}, .workingDirectory = "", .env = {} } };
    ConfigEntry<
        vtpty::SshHostConfig,
        "{comment} Builtin SSH-client configuration.\n"
        "{comment} Use this to directly connect to an SSH server.\n"
        "{comment} This will bypass the local PTY creation\n"
        "{comment} ssh:\n"
        "{comment}     {comment} Target host name to connect to via SSH. This may be a DNS name or IPv4 or "
        "IPv6 address.\n"
        "{comment}     {comment} This value MUST be provided when attempting to directly establish a "
        "connection via SSH.\n"
        "{comment}     {comment}\n"
        "{comment}     {comment} Note, that based on this hostname, the ~/.ssh/config will be looked up and\n"
        "{comment}     {comment} be used as default values when connecting to this host.\n"
        "{comment}     host: example.com\n"
        "{comment}\n"
        "{comment}     {comment} TCP/IP port to use to talk to the remote SSH server. This value defaults to "
        "22.\n"
        "{comment}     port: 22\n"
        "{comment}\n"
        "{comment}     {comment} Remote user name to use for logging into the the SSH server.\n"
        "{comment}     {comment} If not specified, the current local user name will be used as remote SSH "
        "login username.\n"
        "{comment}     user: somebody\n"
        "{comment}\n"
        "{comment}     {comment} When attempting to authenticate with an SSH key, at least the private key "
        "must be provided.\n"
        "{comment}     {comment} This usually is something similar to \"~/.ssh/id_rsa\", but can vary.\n"
        "{comment}     private_key: "
        "\n"
        "{comment}\n"
        "{comment}     {comment} The public key (e.g. \"~/.ssh/your_key.pub\") is usually not required, but "
        "some backends (not OpenSSL) may require it.\n"
        "{comment}     {comment} Defaults to an empty string (not specified).\n"
        "{comment}     public_key: "
        "\n"
        "{comment}\n"
        "{comment}     {comment} This mandates where to look up for known hosts to guard against MITM "
        "attacks.\n"
        "{comment}     {comment} This file is compatible to OpenSSH and thus defaults\n"
        "{comment}     {comment} to the location of OpenSSH's known_hosts, \"~/.ssh/known_hosts\".\n"
        "{comment}     known_hosts: \"~/.ssh/known_hosts\"\n"
        "{comment}\n"
        "{comment}     {comment} Mandates whether or not to enable SSH agent forwarding.\n"
        "{comment}     {comment} Default value currently is `false` (agent forwarding disabled),\n"
        "{comment}     {comment} and is for security reasons also the recommended way.\n"
        "{comment}     forward_agent: false\n"
        "\n">
        ssh {};
    ConfigEntry<bool,
                "{comment} When this profile is *activated*, this flag decides\n"
                "{comment} whether or not to put the window into maximized mode.\n"
                "maximized: {}"
                "\n"
                "\n">
        maximized { false };
    ConfigEntry<bool,
                "{comment} When this profile is being *activated*, this flag decides\n"
                "{comment} whether or not to put the terminal's screen into fullscreen mode.\n"
                "{comment} It is activated during startup as well as when switching from another profile to "
                "this one.\n"
                "fullscreen: {}\n"
                "\n">
        fullscreen {
            false,
        };
    ConfigEntry<bool,
                "{comment} When this profile is *activated*, this flag decides\n"
                "{comment} whether or not the title bar will be shown\n"
                "show_title_bar: {}\n"
                "\n">
        showTitleBar { true };
    ConfigEntry<bool, "\n"> sizeIndicatorOnResize { true };
    ConfigEntry<bool,
                "{comment} whether or not to hide mouse when typing\n"
                "hide_while_typing: {}\n"
                "\n">
        mouseHideWhileTyping { true };
    ConfigEntry<vtbackend::LineOffset,
                "{comment} Advanced value that is useful when CopyPreviousMarkRange is used \n"
                "{comment} with multiline-prompts. This offset value is being added to the \n"
                "{comment} current cursor's line number minus 1 (i.e. the line above the current cursor). \n"
                "copy_last_mark_range_offset: {}\n"
                "\n">
        copyLastMarkRangeOffset = { 0 };
    ConfigEntry<std::string, "{comment} Defines the class part of the WM_CLASS property of the window.\n">
        wmClass = { "contour" };
    ConfigEntry<WindowMargins,
                "{comment} Window margins\n"
                "{comment}\n"
                "{comment} The margin values are applied on both sides and are given in pixels\n"
                "{comment} with DPI yet to be applied to these values.\n"
                "margins:\n"
                "    {comment} Horizontal (left/right) margins.\n"
                "    horizontal: {}\n"
                "    {comment} Vertical (top/bottom) margins.\n"
                "    vertical: {}\n"
                "\n">
        margins = { { 0, 0 } };
    ConfigEntry<vtbackend::PageSize,
                "{comment}Determines the initial terminal size in  characters\n"
                "terminal_size:\n"
                "    columns: {} \n"
                "    lines: {} \n"
                "\n">
        terminalSize { { vtbackend::LineCount(25), vtbackend::ColumnCount(80) } };
    ConfigEntry<vtbackend::VTType,
                "{comment} Determines the terminal type that is being advertised.\n"
                "{comment} Possible values are:\n"
                "{comment}   - VT100\n"
                "{comment}   - VT220\n"
                "{comment}   - VT240\n"
                "{comment}   - VT330\n"
                "{comment}   - VT340\n"
                "{comment}   - VT320\n"
                "{comment}   - VT420\n"
                "{comment}   - VT510\n"
                "{comment}   - VT520\n"
                "{comment}   - VT525\n"
                "terminal_id: {}\n"
                "\n">
        terminalId { vtbackend::VTType::VT525 };
    ConfigEntry<vtbackend::MaxHistoryLineCount,
                "{comment} Number of lines to preserve (-1 for infinite).\n"
                "limit: {}\n"
                "\n">
        maxHistoryLineCount { vtbackend::LineCount(1000) };
    ConfigEntry<vtbackend::LineCount,
                "{comment} Number of lines to scroll on ScrollUp & ScrollDown events.\n"
                "scroll_multiplier: {}\n"
                "\n">
        historyScrollMultiplier { vtbackend::LineCount(3) };
    ConfigEntry<ScrollBarPosition,
                "{comment} scroll bar position: Left, Right, Hidden (ignore-case)\n"
                "position: {}\n"
                "\n">
        scrollbarPosition { ScrollBarPosition::Right };
    ConfigEntry<vtbackend::StatusDisplayPosition,
                "{comment} Position to place the status line to, if it is to be shown.\n"
                "{comment} This can be either value `top` or value `bottom`.\n"
                "position: {}\n"
                "\n">
        statusDisplayPosition { vtbackend::StatusDisplayPosition::Bottom };
    ConfigEntry<bool,
                "{comment} Synchronize the window title with the Host Writable status_line if\n"
                "{comment} and only if the host writable status line was denied to be shown.\n"
                "sync_to_window_title: {}\n"
                "\n">
        syncWindowTitleWithHostWritableStatusDisplay { false };
    ConfigEntry<bool,
                "{comment} whether or not to hide the scrollbar when in alt-screen.\n"
                "hide_in_alt_screen: {}\n"
                "\n">
        hideScrollbarInAltScreen { true };
    ConfigEntry<bool, "{comment} fmt formatted doc {} \n"> optionKeyAsAlt { false }; // TODO(pr)
    ConfigEntry<
        bool,
        "{comment} Boolean indicating whether or not to scroll down to the bottom on screen updates.\n"
        "auto_scroll_on_update: {}\n"
        "\n">
        autoScrollOnUpdate { true };
    ConfigEntry<
        vtrasterizer::FontDescriptions,
        "{comment} Font related configuration (font face, styles, size, rendering mode).\n"
        "font:\n"
        "    {comment} Initial font size in pixels.\n"
        "    size: {}\n"
        "\n"
        "    {comment} Font Locator API\n"
        "    {comment} Selects an engine to use for locating font files on the system.\n"
        "    {comment} This is implicitly also responsible for font fallback\n"
        "    {comment} Possible values are:\n"
        "    {comment} - native          : automatically choose the best available on the current platform\n"
        "    {comment} - fontconfig      : uses fontconfig to select fonts\n"
        "    {comment} - CoreText        : uses OS/X CoreText to select fonts.\n"
        "    {comment} - DirectWrite     : selects DirectWrite engine (Windows only)\n"
        "    locator: {}\n"
        "\n"
        "    {comment} Text shaping related settings\n"
        "    text_shaping:\n"
        "        {comment} Selects which text shaping and font rendering engine to use.\n"
        "        {comment} Supported values are:\n"
        "        {comment} - native      : automatically choose the best available on the current platform.\n"
        "        {comment} - DirectWrite : selects DirectWrite engine (Windows only)\n"
        "        {comment} - CoreText    : selects CoreText engine (Mac OS/X only) (currently not "
        "implemented)\n"
        "        {comment} - OpenShaper  : selects OpenShaper (harfbuzz/freetype/fontconfig, available on "
        "all\n"
        "        {comment}                 platforms)\n"
        "        engine: {}\n"
        "\n"
        "    {comment} Uses builtin textures for pixel-perfect box drawing.\n"
        "    {comment} If disabled, the font's provided box drawing characters\n"
        "    {comment} will be used (Default: true).\n"
        "    builtin_box_drawing: {}\n"
        "\n"
        "    {comment} Font render modes tell the font rasterizer engine what rendering technique to use.\n"
        "    {comment}\n"
        "    {comment} Modes available are:\n"
        "    {comment} - lcd          Uses a subpixel rendering technique optimized for LCD displays.\n"
        "    {comment} - light        Uses a subpixel rendering technique in gray-scale.\n"
        "    {comment} - gray         Uses standard gray-scaled anti-aliasing.\n"
        "    {comment} - monochrome   Uses pixel-perfect bitmap rendering.\n"
        "    render_mode: {}\n"
        "\n"
        "    {comment} Indicates whether or not to include *only* monospace fonts in the font and\n"
        "    {comment} font-fallback list (Default: true).\n"
        "    strict_spacing: {}\n"
        "\n"
        "    {comment} Font family to use for displaying text.\n"
        "    {comment}\n"
        "    {comment} A font can be either described in detail as below or as a\n"
        "    {comment} simple string value (e.g. \"monospace\" with the appropriate\n"
        "    {comment} weight/slant applied automatically).\n"
        "    regular:\n"
        "        {comment} Font family defines the font family name, such as:\n"
        "        {comment} \"\"Fira Code\", \"Courier New\", or \"monospace\" (default).\n"
        "        family: {}\n"
        "\n"
        "        {comment} Font weight can be one of:\n"
        "        {comment}   thin, extra_light, light, demilight, book, normal,\n"
        "        {comment}   medium, demibold, bold, extra_bold, black, extra_black.\n"
        "        weight: {}\n"
        "\n"
        "        {comment} Font slant can be one of: normal, italic, oblique.\n"
        "        slant: {}\n"
        "\n"
        "        {comment} Set of optional font features to be enabled. This\n"
        "        {comment} is usually a 4-letter code, such as ss01 or ss02 etc.\n"
        "        {comment}\n"
        "        {comment} Please see your font's documentation to find out what it\n"
        "        {comment} supports.\n"
        "        {comment}\n"
        "        features: {}\n"
        "\n"
        "    {comment} If bold/italic/bold_italic are not explicitly specified, the regular font with\n"
        "    {comment} the respective weight and slant will be used.\n"
        "    {comment}bold: \"monospace\"\n"
        "    {comment}italic: \"monospace\"\n"
        "    {comment}bold_italic: \"monospace\"\n"
        "\n"
        "    {comment} This is a special font to be used for displaying unicode symbols\n"
        "    {comment} that are to be rendered in emoji presentation.\n"
        "    emoji: {}\n"
        "\n">
        fonts { defaultFont };
    ConfigEntry<Permission,
                "{comment} Allows capturing the screen buffer via `CSI > Pm ; Ps ; Pc ST`.\n"
                "{comment} The response can be read from stdin as sequence `OSC 314 ; <screen capture> ST`\n"
                "capture_buffer: {}\n"
                "\n">
        captureBuffer { Permission::Ask };
    ConfigEntry<Permission,
                "{comment} Allows changing the font via `OSC 50 ; Pt ST`.\n"
                "change_font: {}\n"
                "\n">
        changeFont { Permission::Ask };
    ConfigEntry<
        Permission,
        "{comment} Allows displaying the \" Host Writable Statusline \" programmatically using `DECSSDT 2`.\n"
        "display_host_writable_statusline: {}\n"
        "\n">
        displayHostWritableStatusLine { Permission::Ask };
    ConfigEntry<bool,
                "{comment} Indicates whether or not bold text should be rendered in bright colors,\n"
                "{comment} for indexed colors.\n"
                "{comment} If disabled, normal color will be used instead.\n"
                "draw_bold_text_with_bright_colors: {}\n"
                "\n">
        drawBoldTextWithBrightColors { false };
    ConfigEntry<
        ColorConfig,
        "{comment} Specifies a colorscheme to use (alternatively the colors can be inlined).\n"
        "{comment}\n"
        "{comment} This can be either the name to a single colorscheme to always use,\n"
        "{comment} or a map with two keys (dark and light) to determine the color scheme to use for each.\n"
        "{comment}\n"
        "{comment} The dark color scheme is used when the system is configured to prefer dark mode and light "
        "theme otherwise.\n"
        "\n">
        colors { SimpleColorConfig {} };
    ConfigEntry<
        vtbackend::LineCount,
        "{comment} Configures a `scrolloff` for cursor movements in normal and visual (block) modes.\n"
        "{comment}\n"
        "vi_mode_scrolloff: {}\n"
        "\n">
        modalCursorScrollOff { vtbackend::LineCount { 8 } };
    ConfigEntry<InputModeConfig,
                "{comment} Terminal cursor display configuration\n"
                "cursor:\n"
                "    {comment} Supported shapes are:\n"
                "    {comment}\n"
                "    {comment} - block         a filled rectangle\n"
                "    {comment} - rectangle     just the outline of a block\n"
                "    {comment} - underscore    a line under the text\n"
                "    {comment} - bar:          the well known i-Beam\n"
                "    shape: {}\n"
                "    {comment} Determines whether or not the cursor will be blinking over time.\n"
                "    blinking: {}\n"
                "    {comment} Blinking interval (in milliseconds) to use when cursor is blinking.\n"
                "    blinking_interval: {}\n"
                "\n">
        modeInsert { CursorConfig { vtbackend::CursorShape::Bar,
                                    vtbackend::CursorDisplay::Steady,
                                    std::chrono::milliseconds { 500 } } };
    ConfigEntry<InputModeConfig,
                "{comment} vi-like normal-mode specific settings.\n"
                "{comment} Note, currently only the cursor can be customized.\n"
                "normal_mode:\n"
                "    cursor:\n"
                "        shape: {}\n"
                "        blinking: {}\n"
                "        blinking_interval: {}\n"
                "\n">
        modeNormal { CursorConfig { vtbackend::CursorShape::Block,

                                    vtbackend::CursorDisplay::Steady,
                                    std::chrono::milliseconds { 500 } } };
    ConfigEntry<InputModeConfig,
                "{comment} vi-like normal-mode specific settings.\n"
                "{comment} Note, currently only the cursor can be customized.\n"
                "visual_mode:\n"
                "    cursor:\n"
                "        shape: {}\n"
                "        blinking: {}\n"
                "        blinking_interval: {}\n"
                "\n">
        modeVisual { CursorConfig { vtbackend::CursorShape::Block,
                                    vtbackend::CursorDisplay::Steady,
                                    std::chrono::milliseconds { 500 } } };
    ConfigEntry<std::chrono::milliseconds,
                "{comment} Defines the number of milliseconds to wait before\n"
                "{comment} actually executing the LF (linefeed) control code\n"
                "{comment} in case DEC mode `DECSCLM` is enabled.\n"
                "slow_scrolling_time: {}\n"
                "\n">
        smoothLineScrolling { 100 };
    ConfigEntry<std::chrono::milliseconds,
                "{comment} Time duration in milliseconds for which yank highlight is shown.\n"
                "vi_mode_highlight_timeout: {}\n"
                "\n">
        highlightTimeout { 100 };
    ConfigEntry<bool,
                "{comment} If enabled, and you double-click on a word in the primary screen,\n"
                "{comment} all other words matching this word will be highlighted as well.\n"
                "{comment} So the double-clicked word will be selected as well as highlighted, along with\n"
                "{comment} all other words being simply highlighted.\n"
                "{comment}\n"
                "{comment} This is currently implemented by initiating a search on the double-clicked word.\n"
                "{comment} Therefore one can even use FocusNextSearchMatch and FocusPreviousSearchMatch to\n"
                "{comment} jump to the next/previous same word, also outside of the current viewport.\n"
                "{comment}\n"
                "highlight_word_and_matches_on_double_click: {}\n"
                "\n">
        highlightDoubleClickedWord { true };
    ConfigEntry<vtbackend::StatusDisplayType,
                "{comment} Either none or indicator.\n"
                "{comment} This only reflects the initial state of the status line, as it can\n"
                "{comment} be changed at any time during runtime by the user or by an application.\n"
                "display: {}\n"
                "\n">
        initialStatusDisplayType { vtbackend::StatusDisplayType::None };
    ConfigEntry<
        vtbackend::Opacity,
        "{comment} Background opacity to use. A value of 1.0 means fully opaque whereas 0.0 means fully\n"
        "{comment} transparent. Only values between 0.0 and 1.0 are allowed.\n"
        "opacity: {}\n"
        "\n">
        backgroundOpacity { vtbackend::Opacity(0xFF) };
    ConfigEntry<bool,
                "{comment} Some platforms can blur the transparent background (currently only Windows 10 is "
                "supported).\n"
                "blur: {}\n"
                "\n">
        backgroundBlur { false };
    ConfigEntry<std::optional<contour::display::ShaderConfig>, "fmt formatted doc {} \n"> backgroundShader {

    }; // TODO(pr)
    ConfigEntry<std::optional<contour::display::ShaderConfig>, "fmt formatted doc {} \n"> textShader {

    }; // TODO(pr)
    ConfigEntry<vtrasterizer::Decorator, "normal: {} \n"> hyperlinkDecorationNormal {
        vtrasterizer::Decorator::DottedUnderline
    };
    ConfigEntry<vtrasterizer::Decorator, "hover: {} \n"> hyperlinkDecorationHover {
        vtrasterizer::Decorator::Underline
    };
    ConfigEntry<Bell,
                "bell:\n"
                "    {comment} There is no sound for BEL character if set to \"off\".\n"
                "    {comment} If set to \" default \" BEL character sound will be default sound.\n"
                "    {comment} If set to path to a file then BEL sound will "
                "use that file. Example\n"
                "    {comment}   sound: \"/home/user/Music/bell.wav\"\n"
                "    sound: {}\n"
                "\n"
                "    {comment} Bell volume, a normalized value between 0.0 (silent) and 1.0 (loudest).\n"
                "    {comment} Default: 1.0\n"
                "    volume: {}\n"
                "\n"
                "    {comment} If this boolean is true, a window alert will "
                "be raised with each bell\n"

                "    alert: true\n"
                "\n">
        bell { { .sound = "default", .alert = true, .volume = 1.0f } };
    ConfigEntry<std::map<vtbackend::DECMode, bool>, "fmt formatted doc {} \n"> frozenModes {};
};

const inline TerminalProfile defaultProfile = TerminalProfile {};
const inline InputMappings defaultInputMappings = InputMappings {
    .keyMappings {
        KeyInputMapping { .modes = []() -> vtbackend::MatchModes {
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
            .binding = { { actions::NewTerminal {} } } },
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
        CharInputMapping { .modes = []() -> vtbackend::MatchModes {
                              auto mods = vtbackend::MatchModes();
                              mods.enable(vtbackend::MatchModes::Select);
                              mods.enable(vtbackend::MatchModes::Insert);
                              return mods;
                          }(),
                           .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control } },
                           .input = 'C',
                           .binding = { { actions::CancelSelection {} } } },
        CharInputMapping { .modes = []() -> vtbackend::MatchModes {
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
    ConfigEntry<bool,
                "{comment} Determines whether the instance is reloading the configuration files "
                "whenever it is changing or not. \n"
                "live_config: {} \n"
                "\n">
        live { false };
    ConfigEntry<std::string,
                "{comment} Overrides the auto-detected platform plugin to be loaded. \n"
                "{comment} \n"
                "{comment} Possible (incomplete list of) values are:\n"
                "{comment} - auto        The platform will be auto-detected.\n"
                "{comment} - xcb         Uses XCB plugin (for X11 environment).\n"
                "{comment} - cocoa       Used to be run on Mac OS/X.\n"
                "{comment} - direct2d    Windows platform plugin using Direct2D.\n"
                "{comment} - winrt       Windows platform plugin using WinRT.\n"
                "platform_plugin: {} \n"
                "\n">
        platformPlugin { "xcb" };
    ConfigEntry<RenderingBackend,
                "{comment} Backend to use for rendering the terminal onto the screen \n"
                "{comment} Possible values are: \n"
                "{comment} - default     Uses the default rendering option as decided by the terminal. \n"
                "{comment} - software    Uses software-based rendering. \n"
                "{comment} - OpenGL      Use (possibly) hardware accelerated OpenGL \n"
                "backend: {} \n"
                "\n">
        renderingBackend { RenderingBackend::Default };
    ConfigEntry<bool,
                "{comment} Enables/disables the use of direct-mapped texture atlas tiles for \n"
                "{comment} the most often used ones (US-ASCII, cursor shapes, underline styles) \n"
                "{comment} You most likely do not want to touch this. \n"
                "{comment} \n"
                "tile_direct_mapping: {} \n"
                "\n">
        textureAtlasDirectMapping { false };
    ConfigEntry<crispy::strong_hashtable_size,
                "{comment} Number of hashtable slots to map to the texture tiles. \n"
                "{comment} Larger values may increase performance, but too large may also decrease. \n"
                "{comment} This value is rounded up to a value equal to the power of two. \n"
                "{comment} \n"
                "tile_hashtable_slots: {} \n"
                "\n">
        textureAtlasHashtableSlots { 4096 };
    ConfigEntry<crispy::lru_capacity,
                "{comment} Number of tiles that must fit at lest into the texture atlas. \n"
                "{comment} \n"
                "{comment} This does not include direct mapped tiles (US-ASCII glyphs, \n"
                "{comment} cursor shapes and decorations), if tile_direct_mapping is set to true). \n"
                "{comment} \n"
                "{comment} Value must be at least as large as grid cells available in the terminal view. \n"
                "{comment} This value is automatically adjusted if too small. \n"
                "{comment} \n"
                "tile_cache_count: {} \n"
                "\n">
        textureAtlasTileCount { 4000 };
    ConfigEntry<int,
                "{comment} Default PTY read buffer size. \n"
                "{comment} \n"
                "{comment} This is an advance option. Use with care! \n"
                "read_buffer_size: {} \n"
                "\n">
        ptyReadBufferSize { 16384 };
    ConfigEntry<int,
                "{comment} Size in bytes per PTY Buffer Object. \n "
                "{comment} \n"
                "{comment} This is an advanced option of an internal storage. Only change with care! \n"
                "pty_buffer_size: {} \n"
                "\n">
        ptyBufferObjectSize { 1024 * 1024 };
    ConfigEntry<bool,
                "{comment} Whether or not to reflow the lines on terminal resize events. \n"
                "reflow_on_resize: {} \n  \n">
        reflowOnResize { true };
    ConfigEntry<std::unordered_map<std::string, vtbackend::ColorPalette>,
                "{comment} Color Profiles\n"
                "{comment} --------------\n"
                "{comment}\n"
                "{comment} Here you can configure your color profiles, whereas a color can be expressed in "
                "standard web "
                "format,\n"
                "{comment} with a leading # followed by red/green/blue values, 7 characters in total.\n"
                "{comment} You may alternatively use 0x as prefix instead of #.\n"
                "{comment} For example 0x102030 is equal to '#102030'.\n"
                "color_schemes:\n">
        colorschemes { { { "default", defaultColorSchemes } } };
    ConfigEntry<
        std::unordered_map<std::string, TerminalProfile>,
        "\n"
        "{comment} Terminal Profiles\n"
        "{comment} -----------------\n"
        "{comment}\n"
        "{comment} Dominates how your terminal visually looks like. You will need at least one terminal "
        "profile.\n"
        "profiles:\n"
        "\n">
        profiles { { { "main", defaultProfile } } };
    ConfigEntry<std::string, "default_profile: {} \n"> defaultProfileName { "main" };
    ConfigEntry<std::string,
                "{comment} Word delimiters when selecting word-wise. \n"
                "word_delimiters: \"{}\" \n"
                "\n">
        wordDelimiters { " /\\\\()\\\"'-.,:;<>~!@#$%^&*+=[]{}~?|│" };
    ConfigEntry<vtbackend::Modifiers,
                "{comment} This keyboard modifier can be used to bypass the terminal's mouse protocol, \n"
                "{comment} which can be used to select screen content even if the an application \n"
                "{comment} mouse protocol has been activated (Default: Shift). \n"
                "{comment} \n"
                "{comment} The same modifier values apply as with input modifiers (see below). \n"
                "bypass_mouse_protocol_modifier: {} \n"
                "\n">
        bypassMouseProtocolModifiers { vtbackend::Modifier::Shift };
    ConfigEntry<contour::config::SelectionAction,
                "{comment} Selects an action to perform when a text selection has been made. \n"
                "{comment} \n"
                "{comment} Possible values are: \n"
                "{comment} \n"
                "{comment} - None                        Does nothing \n"
                "{comment} - CopyToClipboard             Copies the selection to the primary clipboard. \n"
                "{comment} - CopyToSelectionClipboard    Copies the selection to the selection clipboard. \n"
                "{comment}This is not supported on all platforms. \n"
                "{comment} \n"
                "on_mouse_select: {} \n"
                "\n">
        onMouseSelection { contour::config::SelectionAction::CopyToSelectionClipboard };
    ConfigEntry<vtbackend::Modifiers,
                "{comment} Modifier to be pressed in order to initiate block-selection \n"
                "{comment} using the left mouse button. \n"
                "{comment} \n"
                "{comment} This is usually the Control modifier, but on OS/X that is not possible, \n"
                "{comment} so Alt or Meta would be recommended instead. \n"
                "{comment} \n"
                "{comment} Supported modifiers: \n"
                "{comment} - Alt \n"
                "{comment} - Control \n"
                "{comment} - Shift \n"
                "{comment} - Meta \n"
                "{comment} \n"
                "mouse_block_selection_modifier: {} \n"
                "\n">
        mouseBlockSelectionModifiers { vtbackend::Modifier::Control };
    ConfigEntry<
        InputMappings,
        "{comment} Key Bindings\n"
        "{comment} ------------\n"
        "{comment}\n"
        "{comment} In this section you can customize key bindings.\n"
        "{comment} Each array element in `input_mapping` represents one key binding,\n"
        "{comment} whereas `mods` represents an array of keyboard modifiers that must be pressed - as well "
        "as\n"
        "{comment} the `key` or `mouse` -  in order to activate the corresponding action,\n"
        "{comment}\n"
        "{comment} Additionally one can filter input mappings based on special terminal modes using the "
        "`modes` "
        "option:\n"
        "{comment} - Alt       : The terminal is currently in alternate screen buffer, otherwise it is in "
        "primary "
        "screen buffer.\n"
        "{comment} - AppCursor : The application key cursor mode is enabled (otherwise it's normal cursor "
        "mode).\n"
        "{comment} - AppKeypad : The application keypad mode is enabled (otherwise it's the numeric keypad "
        "mode).\n"
        "{comment} - Select    : The terminal has currently an active grid cell selection (such as selected "
        "text).\n"
        "{comment} - Insert    : The Insert input mode is active, that is the default and one way to test\n"
        "{comment}               that the input mode is not in normal mode or any of the visual select "
        "modes.\n"
        "{comment} - Search    : There is a search term currently being edited or already present.\n"
        "{comment} - Trace     : The terminal is currently in trace-mode, i.e., each VT sequence can be "
        "interactively\n"
        "{comment}               single-step executed using custom actions. See "
        "TraceEnter/TraceStep/TraceLeave "
        "actions.\n"
        "{comment}\n"
        "{comment} You can combine these modes by concatenating them via | and negate a single one\n"
        "{comment} by prefixing with ~.\n"
        "{comment}\n"
        "{comment} The `modes` option defaults to not filter at all (the input mappings always\n"
        "{comment} match based on modifier and key press / mouse event).\n"
        "{comment}\n"
        "{comment} `key` represents keys on your keyboard, and `mouse` represents buttons\n"
        "{comment} as well as the scroll wheel.\n"
        "{comment}\n"
        "{comment} Modifiers:\n"
        "{comment} - Alt\n"
        "{comment} - Control\n"
        "{comment} - Shift\n"
        "{comment} - Meta (this is the Windows key on Windows OS, and the Command key on OS/X, and Meta on "
        "anything "
        "else)\n"
        "{comment}\n"
        "{comment} Keys can be expressed case-insensitively symbolic:\n"
        "{comment}   APOSTROPHE, ADD, BACKSLASH, COMMA, DECIMAL, DIVIDE, EQUAL, LEFT_BRACKET,\n"
        "{comment}   MINUS, MULTIPLY, PERIOD, RIGHT_BRACKET, SEMICOLON, SLASH, SUBTRACT, SPACE\n"
        "{comment}   Enter, Backspace, Tab, Escape, F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,\n"
        "{comment}   DownArrow, LeftArrow, RightArrow, UpArrow, Insert, Delete, Home, End, PageUp, "
        "PageDown,\n"
        "{comment}   Numpad_NumLock, Numpad_Divide, Numpad_Multiply, Numpad_Subtract, Numpad_CapsLock,\n"
        "{comment}   Numpad_Add, Numpad_Decimal, Numpad_Enter, Numpad_Equal,\n"
        "{comment}   Numpad_0, Numpad_1, Numpad_2, Numpad_3, Numpad_4,\n"
        "{comment}   Numpad_5, Numpad_6, Numpad_7, Numpad_8, Numpad_9\n"
        "{comment} or in case of standard characters, just the character.\n"
        "{comment}\n"
        "{comment} Mouse buttons can be one of the following self-explanatory ones:\n"
        "{comment}   Left, Middle, Right, WheelUp, WheelDown\n"
        "{comment}\n"
        "{comment} Actions:\n"
        "{comment} - CancelSelection   Cancels currently active selection, if any.\n"
        "{comment} - ChangeProfile     Changes the profile to the given profile `name`.\n"
        "{comment} - ClearHistoryAndReset    Clears the history, performs a terminal hard reset and attempts "
        "to "
        "force a redraw of the currently running application.\n"
        "{comment} - CopyPreviousMarkRange   Copies the most recent range that is delimited by vertical line "
        "marks "
        "into clipboard.\n"
        "{comment} - CopySelection     Copies the current selection into the clipboard buffer.\n"
        "{comment} - DecreaseFontSize  Decreases the font size by 1 pixel.\n"
        "{comment} - DecreaseOpacity   Decreases the default-background opacity by 5%.\n"
        "{comment} - FocusNextSearchMatch     Focuses the next search match (if any).\n"
        "{comment} - FocusPreviousSearchMatch Focuses the next previous match (if any).\n"
        "{comment} - FollowHyperlink   Follows the hyperlink that is exposed via OSC 8 under the current "
        "cursor "
        "position.\n"
        "{comment} - IncreaseFontSize  Increases the font size by 1 pixel.\n"
        "{comment} - IncreaseOpacity   Increases the default-background opacity by 5%.\n"
        "{comment} - NewTerminal       Spawns a new terminal at the current terminals current working "
        "directory.\n"
        "{comment} - NoSearchHighlight Disables current search highlighting, if anything is still "
        "highlighted "
        "due to "
        "a prior search.\n"
        "{comment} - OpenConfiguration Opens the configuration file.\n"
        "{comment} - OpenFileManager   Opens the current working directory in a system file manager.\n"
        "{comment} - OpenSelection     Open the current terminal selection with the default system "
        "application "
        "(eg; "
        "xdg-open)\n"
        "{comment} - PasteClipboard    Pastes clipboard to standard input. Pass boolean parameter 'strip' to "
        "indicate whether or not to strip repetitive whitespaces down to one and newlines to "
        "whitespaces.\n"
        "{comment} - PasteSelection    Pastes current selection to standard input.\n"
        "{comment} - Quit              Quits the application.\n"
        "{comment} - ReloadConfig      Forces a configuration reload.\n"
        "{comment} - ResetConfig       Overwrites current configuration with builtin default configuration "
        "and "
        "loads "
        "it. Attention, all your current configuration will be lost due to overwrite!\n"
        "{comment} - ResetFontSize     Resets font size to what is configured in the config file.\n"
        "{comment} - ScreenshotVT      Takes a screenshot in form of VT escape sequences.\n"
        "{comment} - ScrollDown        Scrolls down by the multiplier factor.\n"
        "{comment} - ScrollMarkDown    Scrolls one mark down (if none present, bottom of the screen)\n"
        "{comment} - ScrollMarkUp      Scrolls one mark up\n"
        "{comment} - ScrollOneDown     Scrolls down by exactly one line.\n"
        "{comment} - ScrollOneUp       Scrolls up by exactly one line.\n"
        "{comment} - ScrollPageDown    Scrolls a page down.\n"
        "{comment} - ScrollPageUp      Scrolls a page up.\n"
        "{comment} - ScrollToBottom    Scrolls to the bottom of the screen buffer.\n"
        "{comment} - ScrollToTop       Scrolls to the top of the screen buffer.\n"
        "{comment} - ScrollUp          Scrolls up by the multiplier factor.\n"
        "{comment} - SearchReverse     Initiates search mode (starting to search at current cursor position, "
        "moving "
        "upwards).\n"
        "{comment} - SendChars         Writes given characters in `chars` member to the applications input.\n"
        "{comment} - ToggleAllKeyMaps  Disables/enables responding to all keybinds (this keybind will be "
        "preserved "
        "when disabling all others).\n"
        "{comment} - ToggleFullScreen  Enables/disables full screen mode.\n"
        "{comment} - ToggleInputProtection Enables/disables terminal input protection.\n"
        "{comment} - ToggleStatusLine  Shows/hides the VT320 compatible Indicator status line.\n"
        "{comment} - ToggleTitleBar    Shows/Hides titlebar\n"
        "{comment} - TraceBreakAtEmptyQueue Executes any pending VT sequence from the VT sequence buffer in "
        "trace "
        "mode, then waits.\n"
        "{comment} - TraceEnter        Enables trace mode, suspending execution until explicitly requested "
        "to "
        "continue (See TraceLeave and TraceStep).\n"
        "{comment} - TraceLeave        Disables trace mode. Any pending VT sequence will be flushed out and "
        "normal "
        "execution will be resumed.\n"
        "{comment} - TraceStep         Executes a single VT sequence that is to be executed next.\n"
        "{comment} - ViNormalMode      Enters/Leaves Vi-like normal mode. The cursor can then be moved via "
        "h/j/k/l "
        "movements in normal mode and text can be selected via v, yanked via y, and clipboard pasted via "
        "p.\n"
        "{comment} - WriteScreen       Writes VT sequence in `chars` member to the screen (bypassing the "
        "application).\n"
        "input_mapping:\n">
        inputMappings { defaultInputMappings };
    ConfigEntry<
        bool,
        "{comment} Flag to determine whether to spawn new process or not when creating new terminal \n"
        "spawn_new_process: {} \n  \n ">
        spawnNewProcess { false };
    ConfigEntry<bool,
                "{comment} Enable or disable sixel scrolling (SM/RM ?80 default) \n"
                "sixel_scrolling: {} \n">
        sixelScrolling { true };
    ConfigEntry<
        vtbackend::ImageSize,
        "\n"
        "{comment} maximum width in pixels of an image to be accepted (0 defaults to system screen pixel "
        "width) "
        "\n"
        "max_width: {} \n"
        "{comment} maximum height in pixels of an image to be accepted (0 defaults to system screen pixel "
        "height) \n"
        "max_height: {} \n">
        maxImageSize { { vtpty::Width { 0 }, vtpty::Height { 0 } } };
    ConfigEntry<int,
                "\n"
                "{comment} Configures the maximum number of color registers available when rendering Sixel "
                "graphics. \n"
                "sixel_register_count: {} \n">
        maxImageColorRegisters { 4096 };
    ConfigEntry<
        std::set<std::string>,
        "\n"
        "{comment} Section of experimental features.\n"
        "{comment} All experimental features are disabled by default and must be explicitly enabled here.\n"
        "{comment} NOTE: Contour currently has no experimental features behind this configuration wall.\n"
        "{comment} experimental:\n"
        "{comment}     {comment} Enables experimental support for feature X/Y/Z\n"
        "{comment}     feature_xyz: true\n">
        experimentalFeatures {};

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

    template <typename T, StringLiteral D>
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

    template <typename T, StringLiteral D>
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

template <typename T, contour::config::StringLiteral D>
struct fmt::formatter<contour::config::ConfigEntry<T, D>>
{
    auto format(contour::config::ConfigEntry<T, D> const& c, fmt::format_context& ctx)
    {
        return fmt::format_to(ctx.out(), "{}", c.get());
    };
};
// }}}

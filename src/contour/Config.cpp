// SPDX-License-Identifier: Apache-2.0
#include <contour/Actions.h>
#include <contour/Config.h>

#include <vtbackend/ColorPalette.h>
#include <vtbackend/ControlCode.h>
#include <vtbackend/InputGenerator.h>
#include <vtbackend/TerminalState.h>
#include <vtbackend/primitives.h>

#include <vtpty/Process.h>

#include <text_shaper/mock_font_locator.h>

#include <crispy/StrongHash.h>
#include <crispy/escape.h>
#include <crispy/logstore.h>
#include <crispy/overloaded.h>
#include <crispy/utils.h>

#include <yaml-cpp/node/detail/iterator_fwd.h>
#include <yaml-cpp/ostream_wrapper.h>
#include <yaml-cpp/yaml.h>

#include <QtCore/QFile>
#include <QtGui/QOpenGLContext>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
    #include <Windows.h>
#elif defined(__APPLE__)
    #include <unistd.h>

    #include <mach-o/dyld.h>
#else
    #include <unistd.h>
#endif

auto constexpr MinimumFontSize = text::font_size { 8.0 };

using namespace std;
using crispy::escape;
using crispy::homeResolvedPath;
using crispy::replaceVariables;
using crispy::toLower;
using crispy::toUpper;
using crispy::unescape;

using vtpty::Process;

using vtbackend::Height;
using vtbackend::ImageSize;
using vtbackend::Width;

using vtbackend::CellRGBColorAndAlphaPair;
using vtbackend::ColumnCount;
using vtbackend::Infinite;
using vtbackend::LineCount;
using vtbackend::PageSize;

using contour::actions::Action;

using UsedKeys = set<string>;

namespace fs = std::filesystem;

namespace contour::config
{

int YAMLConfigWriter::Offset::levels { 0 };

namespace
{

    auto const configLog = logstore::category("config", "Logs configuration file loading.");
    optional<std::string> readFile(fs::path const& path)
    {
        if (!fs::exists(path))
            return nullopt;

        auto ifs = ifstream(path.string());
        if (!ifs.good())
            return nullopt;

        auto const size = fs::file_size(path);
        auto text = string {};
        text.resize(size);
        ifs.read(text.data(), static_cast<std::streamsize>(size));
        return { text };
    }

    std::vector<fs::path> configHomes(string const& programName)
    {
        std::vector<fs::path> paths;

#if defined(CONTOUR_PROJECT_SOURCE_DIR) && !defined(NDEBUG)
        paths.emplace_back(fs::path(CONTOUR_PROJECT_SOURCE_DIR) / "src" / "contour" / "display" / "shaders");
#endif

        paths.emplace_back(configHome(programName));

#if defined(__unix__) || defined(__APPLE__)
        paths.emplace_back(fs::path("/etc") / programName);
#endif

        return paths;
    }

    void createFileIfNotExists(fs::path const& path)
    {
        if (!fs::is_regular_file(path))
            if (auto const ec = createDefaultConfig(path); ec)
                throw runtime_error { fmt::format(
                    "Could not create directory {}. {}", path.parent_path().string(), ec.message()) };
    }

} // namespace

fs::path configHome(string const& programName)
{
#if defined(__unix__) || defined(__APPLE__)
    if (auto const* value = getenv("XDG_CONFIG_HOME"); value && *value)
        return fs::path { value } / programName;
    else
        return Process::homeDirectory() / ".config" / programName;
#endif

#if defined(_WIN32)
    DWORD size = GetEnvironmentVariableA("LOCALAPPDATA", nullptr, 0);
    if (size)
    {
        std::vector<char> buf;
        buf.resize(size);
        GetEnvironmentVariableA("LOCALAPPDATA", &buf[0], size);
        return fs::path { &buf[0] } / programName;
    }
    throw runtime_error { "Could not find config home folder." };
#endif
}

fs::path configHome()
{
    return configHome("contour");
}

std::string defaultConfigString()
{
    Config config {};
    auto configString = YAMLConfigWriter().createString(config);
    std::cout << configString;
    return configString;
}

error_code createDefaultConfig(fs::path const& path)
{
    std::error_code ec;
    if (!path.parent_path().empty())
    {
        fs::create_directories(path.parent_path(), ec);
        if (ec)
            return ec;
    }

    ofstream { path.string(), ios::binary | ios::trunc } << defaultConfigString();

    return error_code {};
}

std::string defaultConfigFilePath()
{
    return (configHome() / "contour.yml").string();
}

Config loadConfig()
{
    return loadConfigFromFile(defaultConfigFilePath());
}

Config loadConfigFromFile(fs::path const& fileName)
{
    Config config {};

    loadConfigFromFile(config, fileName);

    return config;
}

/**
 * @return success or failure of loading the config file.
 */
void loadConfigFromFile(Config& config, fs::path const& fileName)
{
    auto logger = configLog;
    logger()("Loading configuration from file: {} ", fileName.string());
    config.backingFilePath = fileName;
    createFileIfNotExists(config.backingFilePath);

    auto yamlVisitor = YAMLVisitor(config.backingFilePath, configLog);
    yamlVisitor.load(config);
}

optional<std::string> readConfigFile(std::string const& filename)
{
    for (fs::path const& prefix: configHomes("contour"))
        if (auto text = readFile(prefix / filename); text.has_value())
            return text;

    return nullopt;
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, std::filesystem::path& where)
{
    auto const child = node[entry];
    if (child)
    {
        where = crispy::homeResolvedPath(std::filesystem::path(child.as<std::string>()),
                                         vtpty::Process::homeDirectory());
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, RenderingBackend& where)
{
    auto const child = node[entry];
    if (child)
    {
        auto renderBackendStr = crispy::toUpper(child.as<std::string>());
        if (renderBackendStr == "OPENGL")
            where = RenderingBackend::OpenGL;
        else if (renderBackendStr == "SOFTWARE")
            where = RenderingBackend::Software;

        logger()("Loading entry: {}, value {}", entry, where);
    }
}

void YAMLVisitor::load(Config& c)
{
    try
    {

        loadFromEntry("platform_plugin", c.platformPlugin);
        if (c.platformPlugin.get() == "auto")
        {
            c.platformPlugin.get() = "";
        }
        loadFromEntry("default_profile", c.defaultProfileName);
        loadFromEntry("word_delimiters", c.wordDelimiters);
        loadFromEntry("read_buffer_size", c.ptyReadBufferSize);
        loadFromEntry("pty_buffer_size", c.ptyBufferObjectSize);
        loadFromEntry("images.sixel_register_count", c.maxImageColorRegisters);
        loadFromEntry("live_config", c.live);
        loadFromEntry("spawn_new_process", c.spawnNewProcess);
        loadFromEntry("images.sixe_scrolling", c.sixelScrolling);
        loadFromEntry("reflow_on_resize", c.reflowOnResize);
        loadFromEntry("renderer.tile_direct_mapping", c.textureAtlasDirectMapping);
        loadFromEntry("renderer.tile_hastable_slots", c.textureAtlasHashtableSlots);
        loadFromEntry("renderer.tile_cache_count", c.textureAtlasTileCount);
        loadFromEntry("bypass_mouse_protocol_modifier", c.bypassMouseProtocolModifiers);
        loadFromEntry("on_mouse_select", c.onMouseSelection);
        loadFromEntry("mouse_block_selection_modifier", c.mouseBlockSelectionModifiers);
        loadFromEntry("images", c.maxImageSize);
        loadFromEntry("", c.experimentalFeatures);
        loadFromEntry("profiles", c.profiles);
        loadFromEntry("color_schemes", c.colorschemes);
        loadFromEntry("input_mapping", c.inputMappings);
    }
    catch (std::exception const& e)
    {
        errorLog()("Something went wrong during config file loading, check `contour debug config` output "
                   "for more info");
    }
};

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, TerminalProfile& where)
{
    logger()("loading profile {}\n", entry);
    auto const child = node[entry];
    if (child)
    {
        if (child["shell"])
        {
            loadFromEntry(child, "shell", where.shell);
        }
        else if (child["ssh"])
        {
            loadFromEntry(child, "ssh", where.ssh);
        }
        else
        {
            // will create default shell if nor shell nor ssh config is provided
            loadFromEntry(child, "shell", where.shell);
        }
        loadFromEntry(child, "escape_sandbox", where.shell.get().escapeSandbox);
        loadFromEntry(child, "copy_last_mark_range_offset", where.copyLastMarkRangeOffset);
        loadFromEntry(child, "initial_working_directory", where.shell.get().workingDirectory);
        loadFromEntry(child, "show_title_bar", where.showTitleBar);
        loadFromEntry(child, "size_indicator_on_resize", where.sizeIndicatorOnResize);
        loadFromEntry(child, "fullscreen", where.fullscreen);
        loadFromEntry(child, "maximized", where.maximized);
        loadFromEntry(child, "bell", where.bell);
        loadFromEntry(child, "wm_class", where.wmClass);
        loadFromEntry(child, "margins", where.margins);
        loadFromEntry(child, "terminal_id", where.terminalId);
        loadFromEntry(child, "frozen_dec_modes", where.frozenModes);
        loadFromEntry(child, "slow_scrolling_time", where.smoothLineScrolling);
        loadFromEntry(child, "terminal_size", where.terminalSize);
        if (child["history"])
        {
            loadFromEntry(child["history"], "limit", where.maxHistoryLineCount);
            loadFromEntry(child["history"], "scroll_multiplier", where.historyScrollMultiplier);
            loadFromEntry(child["history"], "auto_scroll_on_update", where.autoScrollOnUpdate);
        }
        if (child["scrollbar"])
        {
            loadFromEntry(child["scrollbar"], "position", where.scrollbarPosition);
            loadFromEntry(child["scrollbar"], "hide_in_alt_screen", where.hideScrollbarInAltScreen);
        }
        if (child["mouse"])
            loadFromEntry(child["mouse"], "hide_while_typing", where.mouseHideWhileTyping);
        if (child["permissions"])
        {

            loadFromEntry(child["permissions"], "capture_buffer", where.captureBuffer);
            loadFromEntry(child["permissions"], "change_font", where.changeFont);
            loadFromEntry(child["permissions"],
                          "display_host_writable_statusline",
                          where.displayHostWritableStatusLine);
        }
        loadFromEntry(child, "highlight_word_and_matches_on_double_click", where.highlightDoubleClickedWord);
        loadFromEntry(child, "font", where.fonts);
        loadFromEntry(child, "draw_bold_text_with_bright_colors", where.drawBoldTextWithBrightColors);
        if (child["cursor"])
        {
            loadFromEntry(child["cursor"], "shape", where.modeInsert.get().cursor.cursorShape);
            loadFromEntry(child["cursor"], "blinking", where.modeInsert.get().cursor.cursorDisplay);
            loadFromEntry(
                child["cursor"], "blinking_interval", where.modeInsert.get().cursor.cursorBlinkInterval);
        }
        if (child["normal_mode"])
            if (child["normal_mode"]["cursor"])
            {
                loadFromEntry(
                    child["normal_mode"]["cursor"], "shape", where.modeNormal.get().cursor.cursorShape);
                loadFromEntry(
                    child["normal_mode"]["cursor"], "blinking", where.modeNormal.get().cursor.cursorDisplay);
                loadFromEntry(child["normal_mode"]["cursor"],
                              "blinking_interval",
                              where.modeNormal.get().cursor.cursorBlinkInterval);
            }
        if (child["visual_mode"])
            if (child["visual_mode"]["cursor"])
            {
                loadFromEntry(
                    child["visual_mode"]["cursor"], "shape", where.modeVisual.get().cursor.cursorShape);
                loadFromEntry(
                    child["visual_mode"]["cursor"], "blinking", where.modeVisual.get().cursor.cursorDisplay);
                loadFromEntry(child["visual_mode"]["cursor"],
                              "blinking_interval",
                              where.modeVisual.get().cursor.cursorBlinkInterval);
            }
        loadFromEntry(child, "vi_mode_highlight_timeout", where.highlightTimeout);
        loadFromEntry(child, "vi_mode_scrolloff", where.modalCursorScrollOff);
        if (child["status_line"])
        {
            loadFromEntry(child["status_line"], "position", where.statusDisplayPosition);
            loadFromEntry(child["status_line"],
                          "sync_to_window_title",
                          where.syncWindowTitleWithHostWritableStatusDisplay);
            loadFromEntry(child["status_line"], "display", where.initialStatusDisplayType);
        }
        if (child["background"])
        {
            loadFromEntry(child["background"], "opacity", where.backgroundOpacity);
            loadFromEntry(child["background"], "blur", where.backgroundBlur);
        }

        loadFromEntry(child, "colors", where.colors);

        if (auto* simple = get_if<SimpleColorConfig>(&(where.colors.get())))
            simple->colors.useBrightColors = where.drawBoldTextWithBrightColors.get();
        else if (auto* dual = get_if<DualColorConfig>(&(where.colors.get())))
        {
            dual->darkMode.useBrightColors = where.drawBoldTextWithBrightColors.get();
            dual->lightMode.useBrightColors = where.drawBoldTextWithBrightColors.get();
        }

        loadFromEntry(child, "hyperlink_decoration.normal", where.hyperlinkDecorationNormal);
        loadFromEntry(child, "hyperlink_decoration.hover", where.hyperlinkDecorationHover);
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtbackend::ColorPalette& where)
{
    logger()("color palette loading {} \n ", entry);
    auto const child = node[entry];
    if (child)
    {
        if (child["default"])
        {
            loadFromEntry(child["default"], "background", where.defaultBackground);
            loadFromEntry(child["default"], "foreground", where.defaultForeground);
        }

        if (child["background_image"])
            if (child["background_image"]["path"]) // ensure that path exist
            {
                where.backgroundImage = std::make_shared<vtbackend::BackgroundImage>();
                loadFromEntry(child, "background_image", where.backgroundImage);
            }

        loadFromEntry(child, "cursor", where.cursor);
        if (child["hyperlink_decoration"])
        {
            loadFromEntry(child["hyperlink_decoration"], "normal", where.hyperlinkDecoration.normal);
            loadFromEntry(child["hyperlink_decoration"], "hover", where.hyperlinkDecoration.hover);
        }
        loadFromEntry(child, "vi_mode_highlight", where.yankHighlight);
        loadFromEntry(child, "vi_mode_cursosrline", where.indicatorStatusLine);
        loadFromEntry(child, "selection", where.selection);
        loadFromEntry(child, "search_highlight", where.searchHighlight);
        loadFromEntry(child, "search_highlight_focused", where.searchHighlightFocused);
        loadFromEntry(child, "word_highlight_current", where.wordHighlightCurrent);
        loadFromEntry(child, "word_highlight_other", where.wordHighlight);
        loadFromEntry(child, "indicator_statusline", where.indicatorStatusLine);
        loadFromEntry(child, "indicator_statusline_inactive", where.indicatorStatusLineInactive);
        loadFromEntry(child, "input_method_editor", where.inputMethodEditor);
        loadFromEntry(child, "", where.palette);
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtbackend::ColorPalette::Palette& colors)
{
    auto const loadColorMap = [&](YAML::Node const& parent, std::string const& key, size_t offset) -> bool {
        auto node = parent[key];
        if (!node)
            return false;

        if (node.IsMap())
        {
            auto const assignColor = [&](size_t index, std::string const& name) {
                if (auto nodeValue = node[name]; nodeValue)
                {
                    if (auto const value = nodeValue.as<std::string>(); !value.empty())
                    {
                        if (value[0] == '#')
                            colors[offset + index] = value;
                        else if (value.size() > 2 && value[0] == '0' && value[1] == 'x')
                            colors[offset + index] = vtbackend::RGBColor { nodeValue.as<uint32_t>() };
                    }
                }
            };
            assignColor(0, "black");
            assignColor(1, "red");
            assignColor(2, "green");
            assignColor(3, "yellow");
            assignColor(4, "blue");
            assignColor(5, "magenta");
            assignColor(6, "cyan");
            assignColor(7, "white");
            return true;
        }
        else if (node.IsSequence())
        {
            for (size_t i = 0; i < node.size() && i < 8; ++i)
                if (node[i].IsScalar())
                    colors[i] = vtbackend::RGBColor { node[i].as<uint32_t>() };
                else
                    colors[i] = vtbackend::RGBColor { node[i].as<std::string>() };
            return true;
        }
        return false;
    };

    loadColorMap(node, "normal", 0);
    loadColorMap(node, "bright", 8);
    if (!loadColorMap(node, "dim", 256))
    {
        // calculate dim colors based on normal colors
        for (unsigned i = 0; i < 8; ++i)
            colors[256 + i] = colors[i] * 0.5f;
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node,
                                std::string entry,
                                vtbackend::CellRGBColorAndAlphaPair& where)
{
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "foreground", where.foreground);
        loadFromEntry(child, "foreground_alpha", where.foregroundAlpha);
        loadFromEntry(child, "background", where.background);
        loadFromEntry(child, "background_alpha", where.backgroundAlpha);
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtbackend::RGBColorPair& where)
{
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "foreground", where.foreground);
        loadFromEntry(child, "background", where.background);
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtbackend::RGBColor& where)
{
    auto const child = node[entry];
    if (child)
        where = child.as<std::string>();
    logger()("Loading entry: {}, value {}", entry, where);
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtbackend::CursorColor& where)
{
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "default", where.color);
        loadFromEntry(child, "text", where.textOverrideColor);
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtbackend::CellRGBColor& where)
{
    auto parseModifierKey = [&](std::string const& key) -> std::optional<vtbackend::CellRGBColor> {
        auto const literal = crispy::toUpper(key);
        logger()("Loading entry: {}, value {}", entry, where);
        if (literal == "CELLBACKGROUND")
            return vtbackend::CellBackgroundColor {};
        if (literal == "CELLFOREGROUND")
            return vtbackend::CellForegroundColor {};
        return vtbackend::RGBColor(key);
    };

    auto const child = node[entry];
    if (child)
    {
        auto opt = parseModifierKey(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node,
                                std::string entry,
                                std::shared_ptr<vtbackend::BackgroundImage> where)
{
    logger()("Loading background_image");

    auto const child = node[entry];

    if (child)
    {
        std::string filename;
        loadFromEntry(child, "path", filename);
        loadFromEntry(child, "opacity", where->opacity);
        loadFromEntry(child, "blur", where->blur);
        auto resolvedPath = crispy::homeResolvedPath(filename, vtpty::Process::homeDirectory());
        where->location = resolvedPath;
        where->hash = crispy::strong_hash::compute(resolvedPath.string());
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, Permission& where)
{
    auto parseModifierKey = [](std::string const& key) -> std::optional<Permission> {
        auto const literal = crispy::toLower(key);
        if (literal == "allow")
            return Permission::Allow;
        if (literal == "deny")
            return Permission::Deny;
        if (literal == "ask")
            return Permission::Ask;
        return std::nullopt;
    };

    auto const child = node[entry];
    if (child)
    {
        auto opt = parseModifierKey(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtbackend::StatusDisplayType& where)
{
    auto parseModifierKey = [&](std::string const& key) -> std::optional<vtbackend::StatusDisplayType> {
        auto const literal = crispy::toLower(key);
        logger()("Loading entry: {}, value {}", entry, literal);
        if (literal == "indicator")
            return vtbackend::StatusDisplayType::Indicator;
        if (literal == "none")
            return vtbackend::StatusDisplayType::None;
        return std::nullopt;
    };

    auto const child = node[entry];
    if (child)
    {
        auto opt = parseModifierKey(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtbackend::Opacity& where)
{
    auto const child = node[entry];
    if (child)
    {

        where = vtbackend::Opacity(static_cast<unsigned>(255 * std::clamp(child.as<float>(), 0.0f, 1.0f)));
    }
    logger()("Loading entry: {}, value {}", entry, static_cast<unsigned>(where));
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtrasterizer::Decorator& where)
{
    auto parseModifierKey = [](std::string const& key) -> std::optional<vtrasterizer::Decorator> {
        auto const literal = crispy::toLower(key);

        using std::pair;
        auto constexpr Mappings = std::array {
            pair { "underline", vtrasterizer::Decorator::Underline },
            pair { "dotted-underline", vtrasterizer::Decorator::DottedUnderline },
            pair { "double-underline", vtrasterizer::Decorator::DoubleUnderline },
            pair { "curly-underline", vtrasterizer::Decorator::CurlyUnderline },
            pair { "dashed-underline", vtrasterizer::Decorator::DashedUnderline },
            pair { "overline", vtrasterizer::Decorator::Overline },
            pair { "crossed-out", vtrasterizer::Decorator::CrossedOut },
            pair { "framed", vtrasterizer::Decorator::Framed },
            pair { "encircle", vtrasterizer::Decorator::Encircle },
        };
        for (auto const& mapping: Mappings)
            if (mapping.first == literal)
                return { mapping.second };
        return std::nullopt;
    };

    auto const child = node[entry];
    if (child)
    {
        auto opt = parseModifierKey(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, Bell& where)
{

    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "alert", where.alert);
        loadFromEntry(child, "sound", where.sound);
        loadFromEntry(child, "volume", where.volume);
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtpty::SshHostConfig& where)
{
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "host", where.hostname);
        loadFromEntry(child, "port", where.port);
        loadFromEntry(child, "user", where.username);
        loadFromEntry(child, "private_key", where.privateKeyFile);
        loadFromEntry(child, "public_key", where.publicKeyFile);
        loadFromEntry(child, "known_hosts", where.publicKeyFile);
        loadFromEntry(child, "forward_agent", where.forwardAgent);
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtpty::Process::ExecInfo& where)
{
    // TODO(pr)
    YAML::Emitter em;
    auto const child = node[entry];
    em << child;
    std::cout << em.c_str() << std::endl;
    where.program = child.as<std::string>();
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtbackend::LineOffset& where)
{
    auto const child = node[entry];
    if (child)
        where = vtbackend::LineOffset(child.as<int>());
    logger()("Loading entry: {}, value {}", entry, where.template as<int>());
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, WindowMargins& where)
{
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "horizontal", where.horizontal);
        loadFromEntry(child, "vertical", where.vertical);
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtpty::PageSize& where)
{
    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "lines", where.lines);
        loadFromEntry(child, "columns", where.columns);
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtbackend::VTType& where)
{
    auto parseModifierKey = [](std::string const& key) -> std::optional<vtbackend::VTType> {
        auto const literal = crispy::toLower(key);

        using Type = vtbackend::VTType;
        auto constexpr static Mappings = std::array<std::pair<std::string_view, Type>, 10> {
            std::pair { "VT100", Type::VT100 }, std::pair { "VT220", Type::VT220 },
            std::pair { "VT240", Type::VT240 }, std::pair { "VT330", Type::VT330 },
            std::pair { "VT340", Type::VT340 }, std::pair { "VT320", Type::VT320 },
            std::pair { "VT420", Type::VT420 }, std::pair { "VT510", Type::VT510 },
            std::pair { "VT520", Type::VT520 }, std::pair { "VT525", Type::VT525 }
        };
        for (auto const& mapping: Mappings)
            if (mapping.first == literal)
                return mapping.second;
        return std::nullopt;
    };

    auto const child = node[entry];
    if (child)
    {
        auto opt = parseModifierKey(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtbackend::LineCount& where)
{
    auto const child = node[entry];
    if (child)
        where = vtbackend::LineCount(child.as<int>());
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, text::font_size& where)
{
    auto const child = node[entry];
    if (child)
        where = text::font_size(child.as<double>());
    logger()("Loading entry: {}, value {}", entry, where.pt);
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, text::font_slant& where)
{
    auto const child = node[entry];
    if (child)
    {
        auto opt = text::make_font_slant(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, text::font_weight& where)
{
    auto const child = node[entry];
    if (child)
    {
        auto opt = text::make_font_weight(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, text::font_description& where)
{
    auto const child = node[entry];
    if (child)
    {
        if (child.IsMap())
        {
            loadFromEntry(child, "family", where.familyName);
            loadFromEntry(child, "weight", where.weight);
            loadFromEntry(child, "slant", where.slant);
            loadFromEntry(child, "features", where.features);
        }
        else // entries like emoji: "emoji"
        {
            where.familyName = child.as<std::string>();
        }
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, ColorConfig& where)
{
    auto const child = node[entry];
    logger()("Loading entry: {}", entry);
    if (child)
    {
        if (child.IsMap())
        {
            where = DualColorConfig {};
            loadFromEntry(doc["color_schemes"],
                          child["dark"].as<std::string>(),
                          std::get<DualColorConfig>(where).darkMode);
            loadFromEntry(doc["color_schemes"],
                          child["light"].as<std::string>(),
                          std::get<DualColorConfig>(where).lightMode);
        }
        else
        {
            where = SimpleColorConfig {};
            loadFromEntry(
                doc["color_schemes"], child.as<std::string>(), std::get<SimpleColorConfig>(where).colors);
        }
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtrasterizer::TextShapingEngine& where)
{
    auto constexpr NativeTextShapingEngine =
#if defined(_WIN32)
        vtrasterizer::TextShapingEngine::DWrite;
#elif defined(__APPLE__)
        vtrasterizer::TextShapingEngine::CoreText;
#else
        vtrasterizer::TextShapingEngine::OpenShaper;
#endif
    auto parseModifierKey = [&](std::string const& key) -> std::optional<vtrasterizer::TextShapingEngine> {
        auto const literal = crispy::toLower(key);
        logger()("Loading entry: {}, value {}", entry, literal);
        if (literal == "dwrite" || literal == "directwrite")
            return vtrasterizer::TextShapingEngine::DWrite;
        if (literal == "core" || literal == "coretext")
            return vtrasterizer::TextShapingEngine::CoreText;
        if (literal == "open" || literal == "openshaper")
            return vtrasterizer::TextShapingEngine::OpenShaper;
        if (literal == "native")
            return NativeTextShapingEngine;
        return std::nullopt;
    };

    auto const child = node[entry];
    if (child)
    {
        auto opt = parseModifierKey(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtrasterizer::FontLocatorEngine& where)
{
    auto constexpr NativeFontLocator =
#if defined(_WIN32)
        vtrasterizer::FontLocatorEngine::DWrite;
#elif defined(__APPLE__)
        vtrasterizer::FontLocatorEngine::CoreText;
#else
        vtrasterizer::FontLocatorEngine::FontConfig;
#endif
    auto parseModifierKey = [&](std::string const& key) -> std::optional<vtrasterizer::FontLocatorEngine> {
        auto const literal = crispy::toLower(key);
        logger()("Loading entry: {}, value {}", entry, literal);
        if (literal == "fontconfig")
            return vtrasterizer::FontLocatorEngine::FontConfig;
        if (literal == "coretext")
            return vtrasterizer::FontLocatorEngine::CoreText;
        if (literal == "dwrite" || literal == "directwrite")
            return vtrasterizer::FontLocatorEngine::DWrite;
        if (literal == "native")
            return NativeFontLocator;
        if (literal == "mock")
            return vtrasterizer::FontLocatorEngine::Mock;
        return std::nullopt;
    };

    auto const child = node[entry];
    if (child)
    {
        auto opt = parseModifierKey(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, text::render_mode& where)
{
    auto parseModifierKey = [&](std::string const& key) -> std::optional<text::render_mode> {
        auto const literal = crispy::toLower(key);

        using Type = vtbackend::VTType;
        auto constexpr static Mappings = std::array {
            std::pair { "lcd", text::render_mode::lcd },
            std::pair { "light", text::render_mode::light },
            std::pair { "gray", text::render_mode::gray },
            std::pair { "", text::render_mode::gray },
            std::pair { "monochrome", text::render_mode::bitmap },
        };
        for (auto const& mapping: Mappings)
            if (mapping.first == literal)
            {
                logger()("Loading entry: {}, value {}", entry, literal);
                return mapping.second;
            }
        return std::nullopt;
    };

    auto const child = node[entry];
    if (child)
    {
        auto opt = parseModifierKey(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtrasterizer::FontDescriptions& where)
{
    auto const child = node[entry];
    if (child)
    {

        loadFromEntry(child, "size", where.size);
        loadFromEntry(child, "locator", where.fontLocator);
        loadFromEntry(child, "text_shaping.engine", where.textShapingEngine);
        loadFromEntry(child, "builtin_box_drawing", where.builtinBoxDrawing);
        loadFromEntry(child, "render_mode", where.renderMode);
        loadFromEntry(child, "regular", where.regular);

        // inherit fonts from regular
        where.bold = where.regular;
        where.bold.weight = text::font_weight::bold;
        where.italic = where.regular;
        where.italic.slant = text::font_slant::italic;
        where.boldItalic = where.regular;
        where.boldItalic.slant = text::font_slant::italic;
        where.boldItalic.weight = text::font_weight::bold;

        loadFromEntry(child, "bold", where.bold);
        loadFromEntry(child, "italic", where.italic);
        loadFromEntry(child, "bold_italic", where.boldItalic);
        loadFromEntry(child, "emoji", where.emoji);

        // need separate loading since we need to save into font itself
        // TODO : must adhere to default behaviour from test_shaper/font
        bool strictSpacing = false;
        loadFromEntry(child, "strict_spacing", strictSpacing);
        where.regular.strictSpacing = strictSpacing;
        where.bold.strictSpacing = strictSpacing;
        where.italic.strictSpacing = strictSpacing;
        where.boldItalic.strictSpacing = strictSpacing;
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, ScrollBarPosition& where)
{
    auto parseModifierKey = [&](std::string const& key) -> std::optional<ScrollBarPosition> {
        auto const literal = crispy::toLower(key);
        logger()("Loading entry: {}, value {}", entry, literal);
        if (literal == "left")
            return ScrollBarPosition::Left;
        if (literal == "right")
            return ScrollBarPosition::Right;
        if (literal == "hidden")
            return ScrollBarPosition::Hidden;
        return std::nullopt;
    };

    auto const child = node[entry];
    if (child)
    {
        auto opt = parseModifierKey(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtbackend::StatusDisplayPosition& where)
{
    auto parseModifierKey = [&](std::string const& key) -> std::optional<vtbackend::StatusDisplayPosition> {
        auto const literal = crispy::toLower(key);
        logger()("Loading entry: {}, value {}", entry, literal);
        if (literal == "bottom")
            return vtbackend::StatusDisplayPosition::Bottom;
        if (literal == "top")
            return vtbackend::StatusDisplayPosition::Top;
        return std::nullopt;
    };

    auto const child = node[entry];
    if (child)
    {
        auto opt = parseModifierKey(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, std::string& where)
{
    auto const child = node[entry];
    if (child)
    {
        where = child.as<std::string>();
        logger()("Loading entry: {}, value {}", entry, where);
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtbackend::ImageSize& where)
{

    auto const child = node[entry];
    if (child)
    {
        loadFromEntry(child, "max_width", where.width);
        loadFromEntry(child, "max_height", where.height);
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, InputMappings& where)
{
    auto const child = node[entry];
    if (child)
    {
        // Clear default mappings if we are loading it
        where = InputMappings {};
        if (child.IsSequence())
        {
            for (auto&& mapping: child)
            {
                auto action = parseAction(mapping);
                auto mods = parseModifier(mapping);
                auto mode = parseMatchModes(mapping);
                if (action && mods && mode)
                {
                    if (tryAddKey(where, *mode, *mods, mapping["key"], *action))
                    {
                    }
                    else if (tryAddMouse(where.mouseMappings, *mode, *mods, mapping["mouse"], *action))
                    {
                    }
                    else
                    {
                        logger()("Could not add some input mapping.");
                    }
                }
            }
        }
    }
}

bool YAMLVisitor::tryAddMouse(std::vector<MouseInputMapping>& bindings,
                              vtbackend::MatchModes modes,
                              vtbackend::Modifiers modifier,
                              YAML::Node const& node,
                              actions::Action action)
{
    auto mouseButton = parseMouseButton(node);
    if (!mouseButton)
        return false;

    appendOrCreateBinding(bindings, modes, modifier, *mouseButton, std::move(action));
    return true;
}

std::optional<vtbackend::MouseButton> YAMLVisitor::parseMouseButton(YAML::Node const& node)
{
    using namespace std::literals::string_view_literals;
    if (!node)
        return std::nullopt;

    if (!node.IsScalar())
        return std::nullopt;

    auto constexpr static Mappings = std::array {
        std::pair { "WHEELUP"sv, vtbackend::MouseButton::WheelUp },
        std::pair { "WHEELDOWN"sv, vtbackend::MouseButton::WheelDown },
        std::pair { "LEFT"sv, vtbackend::MouseButton::Left },
        std::pair { "MIDDLE"sv, vtbackend::MouseButton::Middle },
        std::pair { "RIGHT"sv, vtbackend::MouseButton::Right },
    };
    auto const upperName = crispy::toUpper(node.as<std::string>());
    for (auto const& mapping: Mappings)
        if (upperName == mapping.first)
            return mapping.second;
    return std::nullopt;
}

bool YAMLVisitor::tryAddKey(InputMappings& inputMappings,
                            vtbackend::MatchModes modes,
                            vtbackend::Modifiers modifier,
                            YAML::Node const& node,
                            actions::Action action)
{
    if (!node)
        return false;

    if (!node.IsScalar())
        return false;

    auto const input = parseKeyOrChar(node.as<std::string>());
    if (!input.has_value())
        return false;

    if (holds_alternative<vtbackend::Key>(*input))
    {
        appendOrCreateBinding(
            inputMappings.keyMappings, modes, modifier, get<vtbackend::Key>(*input), std::move(action));
    }
    else if (holds_alternative<char32_t>(*input))
    {
        appendOrCreateBinding(
            inputMappings.charMappings, modes, modifier, get<char32_t>(*input), std::move(action));
    }
    else
        assert(false && "The impossible happened.");

    return true;
}

std::optional<std::variant<vtbackend::Key, char32_t>> YAMLVisitor::parseKeyOrChar(std::string const& name)
{
    using namespace vtbackend::ControlCode;
    using namespace std::literals::string_view_literals;

    if (auto const key = parseKey(name); key.has_value())
        return key.value();

    auto const text = QString::fromUtf8(name.c_str()).toUcs4();
    if (text.size() == 1)
        return static_cast<char32_t>(text[0]);

    auto constexpr NamedChars =
        std::array { std::pair { "LESS"sv, '<' },          std::pair { "GREATER"sv, '>' },
                     std::pair { "PLUS"sv, '+' },          std::pair { "APOSTROPHE"sv, '\'' },
                     std::pair { "ADD"sv, '+' },           std::pair { "BACKSLASH"sv, 'x' },
                     std::pair { "COMMA"sv, ',' },         std::pair { "DECIMAL"sv, '.' },
                     std::pair { "DIVIDE"sv, '/' },        std::pair { "EQUAL"sv, '=' },
                     std::pair { "LEFT_BRACKET"sv, '[' },  std::pair { "MINUS"sv, '-' },
                     std::pair { "MULTIPLY"sv, '*' },      std::pair { "PERIOD"sv, '.' },
                     std::pair { "RIGHT_BRACKET"sv, ']' }, std::pair { "SEMICOLON"sv, ';' },
                     std::pair { "SLASH"sv, '/' },         std::pair { "SUBTRACT"sv, '-' },
                     std::pair { "SPACE"sv, ' ' } };

    auto const lowerName = crispy::toUpper(name);
    for (auto const& mapping: NamedChars)
        if (lowerName == mapping.first)
            return static_cast<char32_t>(mapping.second);

    return std::nullopt;
}

std::optional<vtbackend::Key> YAMLVisitor::parseKey(std::string const& name)
{
    using vtbackend::Key;
    using namespace std::literals::string_view_literals;
    auto static constexpr Mappings = std::array {
        std::pair { "F1"sv, Key::F1 },
        std::pair { "F2"sv, Key::F2 },
        std::pair { "F3"sv, Key::F3 },
        std::pair { "F4"sv, Key::F4 },
        std::pair { "F5"sv, Key::F5 },
        std::pair { "F6"sv, Key::F6 },
        std::pair { "F7"sv, Key::F7 },
        std::pair { "F8"sv, Key::F8 },
        std::pair { "F9"sv, Key::F9 },
        std::pair { "F10"sv, Key::F10 },
        std::pair { "F11"sv, Key::F11 },
        std::pair { "F12"sv, Key::F12 },
        std::pair { "F13"sv, Key::F13 },
        std::pair { "F14"sv, Key::F14 },
        std::pair { "F15"sv, Key::F15 },
        std::pair { "F16"sv, Key::F16 },
        std::pair { "F17"sv, Key::F17 },
        std::pair { "F18"sv, Key::F18 },
        std::pair { "F19"sv, Key::F19 },
        std::pair { "F20"sv, Key::F20 },
        std::pair { "F21"sv, Key::F21 },
        std::pair { "F22"sv, Key::F22 },
        std::pair { "F23"sv, Key::F23 },
        std::pair { "F24"sv, Key::F24 },
        std::pair { "F25"sv, Key::F25 },
        std::pair { "F26"sv, Key::F26 },
        std::pair { "F27"sv, Key::F27 },
        std::pair { "F28"sv, Key::F28 },
        std::pair { "F29"sv, Key::F29 },
        std::pair { "F30"sv, Key::F30 },
        std::pair { "F31"sv, Key::F31 },
        std::pair { "F32"sv, Key::F32 },
        std::pair { "F33"sv, Key::F33 },
        std::pair { "F34"sv, Key::F34 },
        std::pair { "F35"sv, Key::F35 },
        std::pair { "Escape"sv, Key::Escape },
        std::pair { "Enter"sv, Key::Enter },
        std::pair { "Tab"sv, Key::Tab },
        std::pair { "Backspace"sv, Key::Backspace },
        std::pair { "DownArrow"sv, Key::DownArrow },
        std::pair { "LeftArrow"sv, Key::LeftArrow },
        std::pair { "RightArrow"sv, Key::RightArrow },
        std::pair { "UpArrow"sv, Key::UpArrow },
        std::pair { "Insert"sv, Key::Insert },
        std::pair { "Delete"sv, Key::Delete },
        std::pair { "Home"sv, Key::Home },
        std::pair { "End"sv, Key::End },
        std::pair { "PageUp"sv, Key::PageUp },
        std::pair { "PageDown"sv, Key::PageDown },
        std::pair { "MediaPlay"sv, Key::MediaPlay },
        std::pair { "MediaStop"sv, Key::MediaStop },
        std::pair { "MediaPrevious"sv, Key::MediaPrevious },
        std::pair { "MediaNext"sv, Key::MediaNext },
        std::pair { "MediaPause"sv, Key::MediaPause },
        std::pair { "MediaTogglePlayPause"sv, Key::MediaTogglePlayPause },
        std::pair { "VolumeUp"sv, Key::VolumeUp },
        std::pair { "VolumeDown"sv, Key::VolumeDown },
        std::pair { "VolumeMute"sv, Key::VolumeMute },
        std::pair { "PrintScreen"sv, Key::PrintScreen },
        std::pair { "Pause"sv, Key::Pause },
        std::pair { "Menu"sv, Key::Menu },
    };

    auto const lowerName = crispy::toLower(name);

    for (auto const& mapping: Mappings)
        if (lowerName == crispy::toLower(mapping.first))
            return mapping.second;

    return std::nullopt;
}

std::optional<vtbackend::MatchModes> YAMLVisitor::parseMatchModes(YAML::Node nodeYAML)
{
    auto node = nodeYAML["mode"];
    using vtbackend::MatchModes;
    if (!node)
        return vtbackend::MatchModes {};
    if (!node.IsScalar())
        return std::nullopt;

    auto matchModes = MatchModes {};

    auto const modeStr = node.as<std::string>();
    auto const args = crispy::split(modeStr, '|');
    for (std::string_view arg: args)
    {
        if (arg.empty())
            continue;
        bool negate = false;
        if (arg.front() == '~')
        {
            negate = true;
            arg.remove_prefix(1);
        }

        MatchModes::Flag flag = MatchModes::Flag::Default;
        std::string const upperArg = crispy::toUpper(arg);
        if (upperArg == "ALT")
            flag = MatchModes::AlternateScreen;
        else if (upperArg == "APPCURSOR")
            flag = MatchModes::AppCursor;
        else if (upperArg == "APPKEYPAD")
            flag = MatchModes::AppKeypad;
        else if (upperArg == "INSERT")
            flag = MatchModes::Insert;
        else if (upperArg == "SELECT")
            flag = MatchModes::Select;
        else if (upperArg == "SEARCH")
            flag = MatchModes::Search;
        else if (upperArg == "TRACE")
            flag = MatchModes::Trace;
        else
        {
            errorLog()("Unknown input_mapping mode: {}", arg);
            continue;
        }

        if (negate)
            matchModes.disable(flag);
        else
            matchModes.enable(flag);
    }

    return matchModes;
}

std::optional<vtbackend::Modifiers> YAMLVisitor::parseModifier(YAML::Node nodeYAML)
{
    using vtbackend::Modifier;
    auto node = nodeYAML["mods"];
    if (!node)
        return std::nullopt;
    if (node.IsScalar())
        return parseModifierKey(node.as<std::string>());
    if (!node.IsSequence())
        return std::nullopt;

    vtbackend::Modifiers mods;
    for (const auto& i: node)
    {
        if (!i.IsScalar())
            return std::nullopt;

        auto const mod = parseModifierKey(i.as<std::string>());
        if (!mod)
            return std::nullopt;

        mods |= *mod;
    }
    return mods;
}

std::optional<vtbackend::Modifiers> YAMLVisitor::parseModifierKey(std::string const& key)
{
    using vtbackend::Modifier;
    auto const upperKey = crispy::toUpper(key);
    if (upperKey == "ALT")
        return Modifier::Alt;
    if (upperKey == "CONTROL")
        return Modifier::Control;
    if (upperKey == "SHIFT")
        return Modifier::Shift;
    if (upperKey == "SUPER")
        return Modifier::Super;
    if (upperKey == "META")
        // TODO: This is technically not correct, but we used the term Meta up until now,
        // to refer to the Windows/Cmd key. But Qt also exposes another modifier called
        // Meta, which rarely exists on modern keyboards (?), but it we need to support it
        // as well, especially since extended CSIu protocol exposes it as well.
        return Modifier::Super; // Return Modifier::Meta in the future.
    return std::nullopt;
}

std::optional<actions::Action> YAMLVisitor::parseAction(YAML::Node node)
{
    auto actionNode = node["action"];
    if (actionNode)
    {
        auto actionName = actionNode.as<std::string>();
        auto actionOpt = actions::fromString(actionName);
        if (!actionOpt)
        {
            logger()("Unknown action '{}'.", actionNode["action"].as<std::string>());
            return std::nullopt;
        }
        auto action = actionOpt.value();
        if (holds_alternative<actions::ChangeProfile>(action))
        {
            if (auto name = node["name"]; name.IsScalar())
            {
                return actions::ChangeProfile { actionNode.as<std::string>() };
            }
            else
                return std::nullopt;
        }

        if (holds_alternative<actions::NewTerminal>(action))
        {
            if (auto profile = node["profile"]; profile && profile.IsScalar())
            {
                return actions::NewTerminal { profile.as<std::string>() };
            }
            else
                return action;
        }

        if (holds_alternative<actions::ReloadConfig>(action))
        {
            if (auto profileName = node["profile"]; profileName.IsScalar())
            {
                return actions::ReloadConfig { profileName.as<std::string>() };
            }
            else
                return action;
        }

        if (holds_alternative<actions::SendChars>(action))
        {
            if (auto chars = node["chars"]; chars.IsScalar())
            {
                return actions::SendChars { crispy::unescape(chars.as<std::string>()) };
            }
            else
                return std::nullopt;
        }

        if (holds_alternative<actions::CopySelection>(action))
        {
            if (auto nodeFormat = node["format"]; nodeFormat && nodeFormat.IsScalar())
            {
                auto const formatString = crispy::toUpper(nodeFormat.as<std::string>());
                static auto constexpr Mappings =
                    std::array<std::pair<std::string_view, actions::CopyFormat>, 4> { {
                        { "TEXT", actions::CopyFormat::Text },
                        { "HTML", actions::CopyFormat::HTML },
                        { "PNG", actions::CopyFormat::PNG },
                        { "VT", actions::CopyFormat::VT },
                    } };
                // NOLINTNEXTLINE(readability-qualified-auto)
                if (auto const p = std::find_if(Mappings.begin(),
                                                Mappings.end(),
                                                [&](auto const& t) { return t.first == formatString; });
                    p != Mappings.end())
                {
                    return actions::CopySelection { p->second };
                }
                logger()("Invalid format '{}' in CopySelection action. Defaulting to 'text'.",
                         nodeFormat.as<std::string>());
                return actions::CopySelection { actions::CopyFormat::Text };
            }
        }

        if (holds_alternative<actions::PasteClipboard>(action))
        {
            if (auto nodeStrip = node["strip"]; nodeStrip && nodeStrip.IsScalar())
            {
                return actions::PasteClipboard { nodeStrip.as<bool>() };
            }
        }

        if (holds_alternative<actions::WriteScreen>(action))
        {
            if (auto chars = node["chars"]; chars.IsScalar())
            {
                return actions::WriteScreen { chars.as<std::string>() };
            }
            else
                return std::nullopt;
        }

        return action;
    }
    return std::nullopt;
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, contour::config::SelectionAction& where)
{
    auto const child = node[entry];
    if (child)
    {

        auto const value = crispy::toUpper(child.as<std::string>());
        auto constexpr Mappings = std::array {
            std::pair { "COPYTOCLIPBOARD", contour::config::SelectionAction::CopyToClipboard },
            std::pair { "COPYTOSELECTIONCLIPBOARD",
                        contour::config::SelectionAction::CopyToSelectionClipboard },
            std::pair { "NOTHING", contour::config::SelectionAction::Nothing },
        };
        logger()("Loading entry: {}, value {}", entry, value);
        bool found = false;
        for (auto const& mapping: Mappings)
            if (mapping.first == value)
            {
                where = mapping.second;
                found = true;
            }
        if (!found)
            where = contour::config::SelectionAction::Nothing;
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtbackend::CursorShape& where)
{

    auto parseModifierKey = [&](std::string const& key) -> std::optional<vtbackend::CursorShape> {
        auto const upperKey = crispy::toUpper(key);
        logger()("Loading entry: {}, value {}", entry, upperKey);
        if (upperKey == "BLOCK")
            return vtbackend::CursorShape::Block;
        if (upperKey == "RECTANGLE")
        {
            return vtbackend::CursorShape::Rectangle;
        }
        if (upperKey == "UNDERSCORE")
            return vtbackend::CursorShape::Underscore;
        if (upperKey == "BAR")
            return vtbackend::CursorShape::Bar;
        return std::nullopt;
    };

    auto const child = node[entry];
    if (child)
    {
        auto opt = parseModifierKey(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtbackend::Modifiers& where)
{

    auto const child = node[entry];
    if (child)
    {
        auto opt = parseModifierKey(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtbackend::CursorDisplay& where)
{

    auto parseModifierKey = [&](std::string const& key) -> std::optional<vtbackend::CursorDisplay> {
        auto const upperKey = crispy::toUpper(key);
        logger()("Loading entry: {}, value {}", entry, upperKey);
        if (upperKey == "TRUE")
            return vtbackend::CursorDisplay::Blink;
        if (upperKey == "FALSE")
            return vtbackend::CursorDisplay::Steady;
        return std::nullopt;
    };

    auto const child = node[entry];
    if (child)
    {
        auto opt = parseModifierKey(child.as<std::string>());
        if (opt.has_value())
            where = opt.value();
    }
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, crispy::lru_capacity& where)
{
    auto const child = node[entry];
    if (child)
        where = crispy::lru_capacity(child.as<uint32_t>());
    logger()("Loading entry: {}, value {}", entry, where.value);
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, vtbackend::MaxHistoryLineCount& where)
{
    auto const child = node[entry];
    if (child)
    {
        auto value = child.as<int>();
        if (value == -1)
            where = vtbackend::Infinite {};
        else
            where = vtbackend::LineCount(value);
    }
    if (std::holds_alternative<vtbackend::Infinite>(where))
        logger()("Loading entry: {}, value {}", entry, "Infinity");
    else
        logger()("Loading entry: {}, value {}", entry, std::get<vtbackend::LineCount>(where));
}

void YAMLVisitor::loadFromEntry(YAML::Node node, std::string entry, crispy::strong_hashtable_size& where)
{
    auto const child = node[entry];
    if (child)
        where = crispy::strong_hashtable_size(child.as<uint32_t>());
    logger()("Loading entry: {}, value {}", entry, where.value);
}

std::string YAMLConfigWriter::createString(Config const& c)
{
    std::string doc {};
    auto process = [&](auto v) {
        doc.append(format(addOffset(v.documentation, Offset::levels * 4), v.get()));
    };

    auto processWithDoc = [&](auto docString, auto... val) {
        doc.append(fmt::format(fmt::runtime(format(addOffset(docString, Offset::levels * 4), val...)),
                               fmt::arg("comment", "#")));
    };

    process(c.platformPlugin);

    // inside renderer:
    {
        Offset _;
        doc.append("renderer: \n");
        process(c.renderingBackend);
        process(c.textureAtlasDirectMapping);
        process(c.textureAtlasHashtableSlots);
        process(c.textureAtlasTileCount);
    }

    process(c.wordDelimiters);
    process(c.ptyReadBufferSize);
    process(c.ptyBufferObjectSize);
    process(c.defaultProfileName);
    process(c.spawnNewProcess);
    process(c.reflowOnResize);
    process(c.bypassMouseProtocolModifiers);
    process(c.mouseBlockSelectionModifiers);
    process(c.onMouseSelection);
    process(c.live);
    process(c.experimentalFeatures);

    // inside images:
    doc.append("\nimages: \n");

    {
        Offset _;
        process(c.sixelScrolling);
        process(c.maxImageColorRegisters);
        process(c.maxImageSize);
    }

    // inside profiles:
    doc.append(fmt::format(fmt::runtime(c.profiles.documentation), fmt::arg("comment", "#")));
    {
        Offset _;
        for (auto&& [name, entry]: c.profiles.get())
        {
            doc.append(fmt::format("    {}: \n", name));
            {
                Offset _;
                process(entry.shell);
                process(entry.maximized);
                process(entry.fullscreen);
                process(entry.bell);
                process(entry.showTitleBar);
                process(entry.sizeIndicatorOnResize);
                process(entry.copyLastMarkRangeOffset);
                process(entry.wmClass);
                process(entry.terminalSize);
                process(entry.terminalId);
                process(entry.smoothLineScrolling);

                process(entry.margins);
                // history: section
                doc.append(addOffset("history:\n", Offset::levels * 4));
                {
                    Offset _;
                    process(entry.maxHistoryLineCount);
                    process(entry.historyScrollMultiplier);
                    process(entry.autoScrollOnUpdate);
                }

                // scrollbar: section
                doc.append(addOffset("scrollbar:\n", Offset::levels * 4));
                {
                    Offset _;
                    process(entry.scrollbarPosition);
                    process(entry.hideScrollbarInAltScreen);
                }

                // mouse: section
                doc.append(addOffset("mouse:\n", Offset::levels * 4));
                {
                    Offset _;
                    process(entry.mouseHideWhileTyping);
                }

                //  permissions: section
                doc.append(addOffset("\n"
                                     "permissions:\n",
                                     Offset::levels * 4));
                {
                    Offset _;
                    process(entry.changeFont);
                    process(entry.captureBuffer);
                    process(entry.displayHostWritableStatusLine);
                }

                process(entry.highlightDoubleClickedWord);
                process(entry.fonts);
                process(entry.drawBoldTextWithBrightColors);
                process(entry.modeInsert);
                process(entry.modeNormal);
                process(entry.modeVisual);
                process(entry.highlightTimeout);
                process(entry.modalCursorScrollOff);

                // status_line
                doc.append(addOffset("\n"
                                     "status_line:\n",
                                     Offset::levels * 4));
                {
                    Offset _;
                    process(entry.initialStatusDisplayType);
                    process(entry.statusDisplayPosition);
                    process(entry.syncWindowTitleWithHostWritableStatusDisplay);
                }

                doc.append(addOffset("\n"
                                     "background:\n",
                                     Offset::levels * 4));
                {
                    Offset _;
                    process(entry.backgroundOpacity);
                    process(entry.backgroundBlur);
                }

                // process(entry.colors,8); // TODO(pr)

                doc.append(addOffset("\n"
                                     "hyperlink_decoration:\n",
                                     Offset::levels * 4));
                {
                    Offset _;
                    process(entry.hyperlinkDecorationNormal);
                    process(entry.hyperlinkDecorationHover);
                }
            }
        }
    }

    doc.append(fmt::format(fmt::runtime(c.colorschemes.documentation), fmt::arg("comment", "#")));
    {
        Offset _;
        for (auto&& [name, entry]: c.colorschemes.get())
        {
            doc.append(fmt::format("    {}: \n", name));

            {
                Offset _;

                doc.append(fmt::format(fmt::runtime(addOffset("{comment} Default colors\n"
                                                              "default:\n",
                                                              Offset::levels * 4)),
                                       fmt::arg("comment", "#")));
                {
                    Offset _;
                    processWithDoc("{comment} Default colors\n"
                                   "default:\n"
                                   "    {comment} Default background color (this can be made "
                                   "transparent, see above).\n"
                                   "    background: {}\n"
                                   "    {comment} Default foreground text color.\n"
                                   "    foreground: {}\n",
                                   entry.defaultBackground,
                                   entry.defaultForeground);

                    // processWithDoc("# Background image support.\n"
                    //                "background_image:\n"
                    //                "    # Full path to the image to use as background.\n"
                    //                "    #\n"
                    //                "    # Default: empty string (disabled)\n"
                    //                "    # path: '/Users/trapni/Pictures/bg.png'\n"
                    //                "\n"
                    //                "    # Image opacity to be applied to make the image not look to
                    //                intense\n" "    # and not get too distracted by the background
                    //                image.\n" "    opacity: {}\n"
                    //                "\n"
                    //                "    # Optionally blurs background image to make it less
                    //                distracting\n" "    # and keep the focus on the actual terminal
                    //                contents.\n" "    #\n" "    blur: {}\n",
                    //                entry.backgroundImage->opacity,
                    //                entry.backgroundImage->blur);

                    // processWithDoc("# Mandates the color of the cursor and potentially overridden
                    // text.\n"
                    //                "#\n"
                    //                "# The color can be specified in RGB as usual, plus\n"
                    //                "# - CellForeground: Selects the cell's foreground color.\n"
                    //                "# - CellBackground: Selects the cell's background color.\n"
                    //                "cursor:\n"
                    //                "    # Specifies the color to be used for the actual cursor
                    //                shape.\n" "    #\n" "    default: {}\n" "    # Specifies the
                    //                color to be used for the characters that would\n" "    # be
                    //                covered otherwise.\n" "    #\n" "    text: {}\n",
                    //                entry.cursor.color, entry.cursor.textOverrideColor);

                    processWithDoc("\n"
                                   "{comment} color to pick for hyperlinks decoration, when hovering\n"
                                   "hyperlink_decoration:\n"
                                   "    normal: {}\n"
                                   "    hover: {}\n",
                                   entry.hyperlinkDecoration.normal,
                                   entry.hyperlinkDecoration.hover);

                    processWithDoc("\n"
                                   "{comment} Color to pick for vi_mode highlights.\n"
                                   "{comment} The value format is equivalent to how selection colors and "
                                   "alpha contribution "
                                   "is defined.\n"
                                   "vi_mode_highlight:\n"
                                   "    foreground: {}\n"
                                   "    foreground_alpha: {}\n"
                                   "    background: {}\n"
                                   "    background_alpha: {}\n",
                                   entry.yankHighlight.foreground,
                                   entry.yankHighlight.foregroundAlpha,
                                   entry.yankHighlight.background,
                                   entry.yankHighlight.backgroundAlpha);

                    processWithDoc(
                        "\n"
                        "{comment} Color override for the current cursor's line when in vi_mode:\n"
                        "{comment} The value format is equivalent to how selection colors and alpha "
                        "contribution "
                        "is defined.\n"
                        "{comment} To disable cursorline in vi_mode, set foreground to CellForeground "
                        "and "
                        "background to CellBackground.\n"
                        "vi_mode_cursorline:\n"
                        "    foreground: {}\n"
                        "    foreground_alpha: {}\n"
                        "    background: {}\n"
                        "    background_alpha: {}\n",
                        entry.normalModeCursorline.foreground,
                        entry.normalModeCursorline.foregroundAlpha,
                        entry.normalModeCursorline.background,
                        entry.normalModeCursorline.backgroundAlpha);

                    processWithDoc(
                        "\n"
                        "{comment} The text selection color can be customized here.\n"
                        "{comment} Leaving a value empty will default to the inverse of the content's "
                        "color "
                        "values.\n"
                        "{comment}\n"
                        "{comment} The color can be specified in RGB as usual, plus\n"
                        "{comment} - CellForeground: Selects the cell's foreground color.\n"
                        "{comment} - CellBackground: Selects the cell's background color.\n"
                        "selection:\n"
                        "    {comment} Specifies the color to be used for the selected text.\n"
                        "    {comment}\n"
                        "    foreground: {}\n"
                        "    {comment} Specifies the alpha value (between 0.0 and 1.0) the configured "
                        "foreground "
                        "color\n"
                        "    {comment} will contribute to the original color.\n"
                        "    {comment}\n"
                        "    {comment} A value of 1.0 will paint over, whereas a value of 0.5 will give\n"
                        "    {comment} a look of a half-transparently painted grid cell.\n"
                        "    foreground_alpha: {}\n"
                        "    {comment} Specifies the color to be used for the selected background.\n"
                        "    {comment}\n"
                        "    background: {}\n"
                        "    {comment} Specifies the alpha value (between 0.0 and 1.0) the configured "
                        "background "
                        "color\n"
                        "    {comment} will contribute to the original color.\n"
                        "    {comment}\n"
                        "    {comment} A value of 1.0 will paint over, whereas a value of 0.5 will give\n"
                        "    {comment} a look of a half-transparently painted grid cell.\n"
                        "    background_alpha: {}\n",
                        entry.selection.foreground,
                        entry.selection.foregroundAlpha,
                        entry.selection.background,
                        entry.selection.backgroundAlpha);

                    processWithDoc("\n"
                                   "{comment} Search match highlighting. Similar to selection highlighting.\n"
                                   "search_highlight:\n"
                                   "    foreground: {}\n"
                                   "    foreground_alpha: {}\n"
                                   "    background: {}\n"
                                   "    background_alpha: {}\n",
                                   entry.searchHighlight.foreground,
                                   entry.searchHighlight.foregroundAlpha,
                                   entry.searchHighlight.background,
                                   entry.searchHighlight.backgroundAlpha);

                    processWithDoc("\n"
                                   "{comment} Search match highlighting (focused term). Similar to selection "
                                   "highlighting.\n"
                                   "search_highlight_focused:\n"
                                   "    foreground: {}\n"
                                   "    foreground_alpha: {}\n"
                                   "    background: {}\n"
                                   "    background_alpha: {}\n",
                                   entry.searchHighlightFocused.foreground,
                                   entry.searchHighlightFocused.foregroundAlpha,
                                   entry.searchHighlightFocused.background,
                                   entry.searchHighlightFocused.backgroundAlpha);

                    processWithDoc(
                        "\n"
                        "{comment} Coloring for the word that is highlighted due to double-clicking it.\n"
                        "{comment}\n"
                        "{comment} The format is similar to selection highlighting.\n"
                        "word_highlight_current:\n"
                        "    foreground: {}\n"
                        "    foreground_alpha: {}\n"
                        "    background: {}\n"
                        "    background_alpha: {}\n",
                        entry.wordHighlightCurrent.foreground,
                        entry.wordHighlightCurrent.foregroundAlpha,
                        entry.wordHighlightCurrent.background,
                        entry.wordHighlightCurrent.backgroundAlpha);

                    processWithDoc(
                        "\n"
                        "{comment} Coloring for the word that is highlighted due to double-clicking\n"
                        "{comment} another word that matches this word.\n"
                        "{comment}\n"
                        "{comment} The format is similar to selection highlighting.\n"
                        "word_highlight_other:\n"
                        "    foreground: {}\n"
                        "    foreground_alpha: {}\n"
                        "    background: {}\n"
                        "    background_alpha: {}\n",
                        entry.wordHighlight.foreground,
                        entry.wordHighlight.foregroundAlpha,
                        entry.wordHighlight.background,
                        entry.wordHighlight.backgroundAlpha);

                    processWithDoc("\n"
                                   "{comment} Defines the colors to be used for the Indicator status line.\n"
                                   "{comment} Values must be in RGB form.\n"
                                   "indicator_statusline:\n"
                                   "    foreground: {}\n"
                                   "    background: {}\n",
                                   entry.indicatorStatusLine.foreground,
                                   entry.indicatorStatusLine.background);

                    processWithDoc(
                        "\n"
                        "{comment} Alternate colors to be used for the indicator status line when\n"
                        "{comment} this terminal is currently not in focus.\n"
                        "indicator_statusline_inactive:\n"
                        "    foreground: {}\n"
                        "    background: {}\n",
                        entry.indicatorStatusLineInactive.foreground,
                        entry.indicatorStatusLineInactive.background);

                    processWithDoc("\n"
                                   "{comment} Colors for the IME (Input Method Editor) area.\n"
                                   "input_method_editor:\n"
                                   "    foreground: {}\n"
                                   "    background: {}\n",
                                   entry.inputMethodEditor.foreground,
                                   entry.inputMethodEditor.background);

                    processWithDoc("\n"
                                   "{comment} Normal colors\n"
                                   "normal:\n"
                                   "    black:   {}\n"
                                   "    red:     {}\n"
                                   "    green:   {}\n"
                                   "    yellow:  {}\n"
                                   "    blue:    {}\n"
                                   "    magenta: {}\n"
                                   "    cyan:    {}\n"
                                   "    white:   {}\n",
                                   entry.normalColor(0),
                                   entry.normalColor(1),
                                   entry.normalColor(2),
                                   entry.normalColor(3),
                                   entry.normalColor(4),
                                   entry.normalColor(5),
                                   entry.normalColor(6),
                                   entry.normalColor(7));

                    processWithDoc("\n"
                                   "{comment} Bright colors\n"
                                   "bright:\n"
                                   "    black:   {}\n"
                                   "    red:     {}\n"
                                   "    green:   {}\n"
                                   "    yellow:  {}\n"
                                   "    blue:    {}\n"
                                   "    magenta: {}\n"
                                   "    cyan:    {}\n"
                                   "    white:   {}\n",
                                   entry.brightColor(0),
                                   entry.brightColor(1),
                                   entry.brightColor(2),
                                   entry.brightColor(3),
                                   entry.brightColor(4),
                                   entry.brightColor(5),
                                   entry.brightColor(6),
                                   entry.brightColor(7));

                    processWithDoc("\n"
                                   "{comment} Dim (faint) colors, if not set, they're automatically computed "
                                   "based on normal colors.\n"
                                   "{comment} dim:\n"
                                   "{comment}     black:   {}\n"
                                   "{comment}     red:     {}\n"
                                   "{comment}     green:   {}\n"
                                   "{comment}     yellow:  {}\n"
                                   "{comment}     blue:    {}\n"
                                   "{comment}     magenta: {}\n"
                                   "{comment}     cyan:    {}\n"
                                   "{comment}     white:   {}\n",
                                   entry.dimColor(0),
                                   entry.dimColor(1),
                                   entry.dimColor(2),
                                   entry.dimColor(3),
                                   entry.dimColor(4),
                                   entry.dimColor(5),
                                   entry.dimColor(6),
                                   entry.dimColor(7));
                }

                doc.append(addOffset("", Offset::levels * 4));
            }
        }
    }

    doc.append(fmt::format(fmt::runtime(c.inputMappings.documentation), fmt::arg("comment", "#")));
    {
        Offset _;
        for (auto&& entry: c.inputMappings.get().keyMappings)
        {
            doc.append(addOffset(format(entry), Offset::levels * 4));
        }

        for (auto&& entry: c.inputMappings.get().charMappings)
        {
            doc.append(addOffset(format(entry), Offset::levels * 4));
        }

        for (auto&& entry: c.inputMappings.get().mouseMappings)
        {
            doc.append(addOffset(format(entry), Offset::levels * 4));
        }
    }
    return doc;
}

} // namespace contour::config

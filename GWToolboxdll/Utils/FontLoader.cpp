#include "stdafx.h"

#include "FontLoader.h"
#include <Modules/Resources.h>
#include "toolbox_default_font.h"

#include "fonts/fontawesome5.h"

#include <atomic>
#include <fstream>
#include <memory>

namespace {
    ImFont* loaded_font = nullptr;
    ImFont* full_font = nullptr;

    bool fonts_loading = false;
    bool fonts_loaded = false;

    struct FontData {
        std::vector<ImWchar> glyph_ranges;
        std::filesystem::path path;
        void* data = nullptr;
        size_t data_size = 0;
    };

    const std::vector<ImWchar> fontawesome5_glyph_ranges = {ICON_MIN_FA, ICON_MAX_FA, 0};

    constexpr std::vector<ImWchar> ConstGetGlyphRangesLatin()
    {
        return {
            0x0020, 0x00FF, 0 // Basic Latin + Latin Supplement
        };
    }

    constexpr std::vector<ImWchar> ConstGetGlyphRangesJapanese()
    {
        return {
            0x0020, 0x00FF,   // Basic Latin + Latin Supplement
            0x3000, 0x30FF,   // CJK Symbols and Punctuations, Hiragana, Katakana
            0x31F0, 0x31FF,   // Katakana Phonetic Extensions
            0xFF00, 0xFFEF,   // Half-width characters
            0xFFFD, 0xFFFD, 0 // Invalid
        };
    }

    constexpr std::vector<ImWchar> ConstGetGlyphRangesCyrillic()
    {
        return {
            0x0020, 0x00FF,   // Basic Latin + Latin Supplement
            0x0400, 0x052F,   // Cyrillic + Cyrillic Supplement
            0x2DE0, 0x2DFF,   // Cyrillic Extended-A
            0xA640, 0xA69F, 0 // Cyrillic Extended-B
        };
    }

    constexpr std::vector<ImWchar> ConstGetGlyphRangesChinese()
    {
        return {
            0x0020, 0x00FF,   // Basic Latin + Latin Supplement
            0x2000, 0x206F,   // General Punctuation
            0x3000, 0x30FF,   // CJK Symbols and Punctuations, Hiragana, Katakana
            0x31F0, 0x31FF,   // Katakana Phonetic Extensions
            0xFF00, 0xFFEF,   // Half-width characters
            0xFFFD, 0xFFFD,   // Invalid
            0x4e00, 0x9FAF, 0 // CJK Ideograms
        };
    }

    constexpr std::vector<ImWchar> ConstGetGlyphRangesKorean()
    {
        return {
            0x0020, 0x00FF,   // Basic Latin + Latin Supplement
            0x3131, 0x3163,   // Korean alphabets
            0xAC00, 0xD7A3,   // Korean characters
            0xFFFD, 0xFFFD, 0 // Invalid
        };
    }

    constexpr std::vector<ImWchar> ConstGetGlyphRangesGW()
    {
        return {
            0x20, 0x7f,
            0xa1, 0xff,
            0x100, 0x180,
            0x0391, 0x0460,
            0x2010, 0x266b,
            0x3000, 0x3020,
            0x3041, 0x3100,
            0x3105, 0x312a,
            0x3131, 0x318f,
            0xac00, 0xd7a4,
            0x4e00, 0x9fa6,
            0xf900, 0xfa6b,
            0xff01, 0xffe7, 0
        };
    }

    constexpr std::vector<ImWchar> find_glyph_range_intersection(const std::vector<ImWchar>& range1, const std::vector<ImWchar>& range2)
    {
        if (range1.empty() || range2.empty()) return {};

        auto in_range = [](const wchar_t c, const std::vector<ImWchar>& range) {
            for (size_t i = 0; i < range.size() - 1; i += 2) {
                if (c >= range[i] && c <= range[i + 1]) return true;
            }
            return false;
        };

        wchar_t start = 0;
        bool in_intersection = false;

        std::vector<ImWchar> intersection;
        for (ImWchar c = 0; c <= 0xffe7; ++c) {
            const bool in_both = in_range(c, range1) && in_range(c, range2);

            if (in_both && !in_intersection) {
                start = c;
                in_intersection = true;
            }
            else if (!in_both && in_intersection) {
                intersection.push_back(start);
                intersection.push_back(c - 1);
                in_intersection = false;
            }
        }

        if (in_intersection) {
            intersection.push_back(start);
            intersection.push_back(0xffe7);
        }

        intersection.push_back(0); // Null-terminate the range
        return intersection;
    }

    std::vector<FontData>& GetFontData()
    {
        static std::vector<FontData> font_data;
        if (!font_data.empty())
            return font_data;

        const auto fonts_on_disk = std::to_array<std::pair<std::wstring_view, std::vector<ImWchar>>>({
            {L"Font.ttf", ConstGetGlyphRangesLatin()},
            {L"Font_Japanese.ttf", ConstGetGlyphRangesJapanese()},
            {L"Font_Cyrillic.ttf", ConstGetGlyphRangesCyrillic()},
            {L"Font_ChineseTraditional.ttf", ConstGetGlyphRangesChinese()},
            {L"Font_Korean.ttf", ConstGetGlyphRangesKorean()}
        });

        for (const auto& [font_name, glyph_range] : fonts_on_disk) {
            const auto path = Resources::GetPath(font_name);
            if (!std::filesystem::exists(path))
                continue;
            const auto font_glyphs = find_glyph_range_intersection(glyph_range, ConstGetGlyphRangesGW());
            font_data.emplace_back(font_glyphs, path);
        }

        return font_data;
    }

    ImFontConfig GetBaseFontConfig()
    {
        ImFontConfig cfg;
        cfg.PixelSnapH = true;
        cfg.OversampleH = 1;
        cfg.OversampleV = 1;
        cfg.MergeMode = false;
        // imgui 1.92's new backend resolves merged glyphs from the first
        // source, exclude font awesome's glyph ranges when adding regular fonts
        cfg.GlyphExcludeRanges = fontawesome5_glyph_ranges.data();
        return cfg;
    }

    ImFont* BuildDefaultFont(const float size)
    {
        ImFontAtlas* atlas = ImGui::GetIO().Fonts;

        auto cfg = GetBaseFontConfig();

        auto* font = atlas->AddFontFromMemoryCompressedTTF(toolbox_default_font_compressed_data, toolbox_default_font_compressed_size, size, &cfg, toolbox_default_font_glyph_ranges);
        cfg.MergeMode = true;
        cfg.GlyphExcludeRanges = nullptr;
        atlas->AddFontFromMemoryCompressedTTF(fontawesome5_compressed_data, fontawesome5_compressed_size, size, &cfg, fontawesome5_glyph_ranges.data());
        return font;
    }

    // 尝试加载系统中文字体（微软雅黑）。成功返回字体指针，失败返回 nullptr。
    ImFont* TryLoadSystemFont(const float size, ImFontAtlas* atlas)
    {
        const char* system_font_paths[] = {
            "C:\\Windows\\Fonts\\msyh.ttc",   // Windows 10 微软雅黑
            "C:\\Windows\\Fonts\\msyh.ttf",   // Windows 7 微软雅黑
            "C:\\Windows\\Fonts\\msyhbd.ttc", // Windows 10 微软雅黑加粗
            "C:\\Windows\\Fonts\\msyhbd.ttf", // Windows 7 微软雅黑加粗
        };
        static const ImWchar glyph_ranges[] = {
            0x0020, 0x00FF,   // Basic Latin + Latin Supplement
            0x2000, 0x206F,   // General Punctuation
            0x3000, 0x30FF,   // CJK Symbols, Hiragana, Katakana
            0x4e00, 0x9FAF,   // CJK Ideograms（简体中文在此范围内）
            0xFF00, 0xFFEF,   // Half-width
            0
        };
        for (const char* path : system_font_paths) {
            size_t data_size = 0;
            void* data = ImFileLoadToMemory(path, "rb", &data_size, 0);
            if (!data) {
                continue;
            }
            auto cfg = GetBaseFontConfig();
            cfg.MergeMode = false;
            auto* font = atlas->AddFontFromMemoryTTF(data, static_cast<int>(data_size), size, &cfg, glyph_ranges);
            if (font) {
                printf("[FontLoader] Loaded system font: %s\n", path);
                return font;
            }
        }
        return nullptr;
    }

} // namespace

namespace FontLoader {
    bool FontsLoaded()
    {
        return fonts_loaded;
    }

    // Loads fonts asynchronously. CJK font files can be over 20mb in size!
    void LoadFonts([[maybe_unused]] const bool force)
    {
        if (fonts_loading) {
            return;
        }
        if (fonts_loaded) {
            return;
        }
        if (!GImGui)
            return;
        fonts_loading = true;
        fonts_loaded = false;

        constexpr float base_size = static_cast<float>(FontSize::text);

        // 用于在异步任务间共享“系统字体是否已加载”的状态
        auto system_font_loaded = std::make_shared<std::atomic<bool>>(false);

        // 第一阶段：在主线程尝试加载系统字体；成功则合并 FontAwesome 并结束；失败则加载默认字体
        Resources::EnqueueDxTask([base_size, system_font_loaded](IDirect3DDevice9*) {
            ImFontAtlas* atlas = ImGui::GetIO().Fonts;
            if (auto* sysfont = TryLoadSystemFont(base_size, atlas)) {
                loaded_font = sysfont;
                ImGui::GetIO().FontDefault = sysfont;

                // 合并 FontAwesome 图标
                auto cfg = GetBaseFontConfig();
                cfg.MergeMode = true;
                cfg.GlyphExcludeRanges = nullptr;
                atlas->AddFontFromMemoryCompressedTTF(fontawesome5_compressed_data, fontawesome5_compressed_size, base_size, &cfg, fontawesome5_glyph_ranges.data());

                system_font_loaded->store(true);
                fonts_loaded = true;
                fonts_loading = false;
                printf("[FontLoader] System font loaded, skipping disk fonts.\n");
                return;
            }
            loaded_font = BuildDefaultFont(base_size);
            printf("Loaded default font\n");
        });

        // 第二阶段：在工作线程读取磁盘字体（仅当系统字体未加载时）
        Resources::EnqueueWorkerTask([base_size, system_font_loaded] {
            if (system_font_loaded->load()) {
                return; // 系统字体已处理完毕，跳过所有磁盘字体
            }
            for (auto& font : GetFontData()) {
                std::ifstream file(font.path, std::ios::binary);
                if (!file.is_open())
                    continue;
                file.seekg(0, std::ios::end);
                const auto file_end = file.tellg();
                if (file_end <= 0)
                    continue;
                const auto file_size = static_cast<size_t>(file_end);
                font.data = ImGui::MemAlloc(file_size);
                font.data_size = file_size;
                file.seekg(0);
                if (!file.read(static_cast<char*>(font.data), font.data_size)) {
                    ImGui::MemFree(font.data);
                    font.data = nullptr;
                    continue;
                }
                Resources::EnqueueDxTask([base_size, &font](IDirect3DDevice9*) {
                    auto cfg = GetBaseFontConfig();
                    cfg.MergeMode = full_font != nullptr;
                    full_font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(font.data, static_cast<int>(font.data_size), base_size, &cfg, font.glyph_ranges.data());
                    font.data = nullptr;
                });
            }
            Resources::EnqueueDxTask([base_size](IDirect3DDevice9*) {
                ImFontAtlas* atlas = ImGui::GetIO().Fonts;
                auto cfg = GetBaseFontConfig();
                cfg.MergeMode = true;
                cfg.GlyphExcludeRanges = nullptr;
                atlas->AddFontFromMemoryCompressedTTF(fontawesome5_compressed_data, fontawesome5_compressed_size, base_size, &cfg, fontawesome5_glyph_ranges.data());
                if (full_font) {
                    loaded_font = full_font;
                    ImGui::GetIO().FontDefault = loaded_font;
                }
                full_font = nullptr;
                printf("Loaded all fonts\n");
                fonts_loaded = true;
                fonts_loading = false;
            });
        });
    }

    void Terminate()
    {
        loaded_font = nullptr;
    }

    ImFont* GetFont()
    {
        if (loaded_font && loaded_font->IsLoaded()) {
            return loaded_font;
        }

        const auto& io = ImGui::GetIO();
        return io.Fonts->Fonts[0];
    }
}

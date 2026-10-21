#pragma once

#include <cassert>
#include <fstream>
#include <iterator>
#include <vector>

// The asset has 32-bit pointers; relocate its real maps/bitmaps for the 64-bit host.
// Kerning is disabled here; glyph metrics and bitmap offsets remain from the asset.
class CbinHostFont {
public:
    explicit CbinHostFont(const char* path) {
        std::ifstream input(path, std::ios::binary);
        data_ = std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
        assert(input && data_.size() >= 60);
        const auto descriptor = U32(24);
        const auto bitmap = descriptor + U32(descriptor);
        const auto glyphs = descriptor + U32(descriptor + 4);
        const auto maps = descriptor + U32(descriptor + 8);
        const auto flags = U16(descriptor + 18);
        assert(bitmap < data_.size() && glyphs < maps && maps < data_.size());
        for (size_t at = glyphs; at + 16 <= maps; at += 16) {
            lv_font_fmt_txt_glyph_dsc_t glyph{};
            glyph.bitmap_index = U32(at);
            glyph.adv_w = U32(at + 4);
            glyph.box_w = U16(at + 8);
            glyph.box_h = U16(at + 10);
            glyph.ofs_x = static_cast<int16_t>(U16(at + 12));
            glyph.ofs_y = static_cast<int16_t>(U16(at + 14));
            glyphs_.push_back(glyph);
        }
        for (unsigned i = 0; i < (flags & 511); ++i) {
            const auto at = maps + i * 20;
            lv_font_fmt_txt_cmap_t map{};
            map.range_start = U32(at);
            map.range_length = U16(at + 4);
            map.glyph_id_start = U16(at + 6);
            map.unicode_list =
                U32(at + 8) ? reinterpret_cast<const uint16_t*>(&data_.at(maps + U32(at + 8)))
                            : nullptr;
            map.glyph_id_ofs_list = U32(at + 12) ? &data_.at(maps + U32(at + 12)) : nullptr;
            map.list_length = U16(at + 16);
            map.type = static_cast<lv_font_fmt_txt_cmap_type_t>(data_.at(at + 18));
            maps_.push_back(map);
        }
        descriptor_.glyph_bitmap = &data_.at(bitmap);
        descriptor_.glyph_dsc = glyphs_.data();
        descriptor_.cmaps = maps_.data();
        descriptor_.cmap_num = maps_.size();
        descriptor_.bpp = (flags >> 9) & 15;
        descriptor_.bitmap_format = (flags >> 14) & 3;
        descriptor_.stride = data_.at(descriptor + 20);
        font.get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt;
        font.get_glyph_bitmap = lv_font_get_bitmap_fmt_txt;
        font.line_height = U32(12);
        font.base_line = U32(16);
        font.dsc = &descriptor_;
    }
    lv_font_t font{};

private:
    std::vector<uint8_t> data_;
    std::vector<lv_font_fmt_txt_glyph_dsc_t> glyphs_;
    std::vector<lv_font_fmt_txt_cmap_t> maps_;
    lv_font_fmt_txt_dsc_t descriptor_{};
    uint16_t U16(size_t at) const { return data_.at(at) | (data_.at(at + 1) << 8); }
    uint32_t U32(size_t at) const { return U16(at) | (static_cast<uint32_t>(U16(at + 2)) << 16); }
};

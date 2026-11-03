// SPDX-License-Identifier: Apache-2.0
#include "SleepText.h"
#include "Picture.h"
#include <cstdio>
#include <cstring>

namespace pw3 {
namespace {
void number(std::vector<std::string>* out, int value, int digits) {
    char text[16];
    snprintf(text, sizeof(text), "%0*d", digits, value);
    for (const char* c = text; *c; ++c) out->push_back(std::string(1, *c));
}
}  // namespace

std::vector<std::string> sleepLine(bool chinese, const std::tm* when, int battery) {
    static const char* const months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const std::string dot = " · ";
    std::vector<std::string> out;
    // A clock that was never set (no network yet) shows 1970: say nothing about the time then.
    if (when && when->tm_year + 1900 >= 2020 && when->tm_mon >= 0 && when->tm_mon < 12) {
        out.push_back(chinese ? "休眠于" : "Asleep at");
        out.push_back(" ");
        number(&out, when->tm_hour, 2);
        out.push_back(":");
        number(&out, when->tm_min, 2);
        out.push_back(dot);
        if (chinese) {
            number(&out, when->tm_mon + 1, 1);
            out.push_back("月");
            number(&out, when->tm_mday, 1);
            out.push_back("日");
        } else {
            out.push_back(months[when->tm_mon]);
            out.push_back(" ");
            number(&out, when->tm_mday, 1);
        }
    }
    if (battery >= 0 && battery <= 100) {
        if (!out.empty()) out.push_back(dot);
        out.push_back(chinese ? "电量" : "Battery");
        out.push_back(" ");
        number(&out, battery, 1);
        out.push_back("%");
    }
    return out;
}

bool SleepText::load(const char* strip, const char* index) {
    int w = 0, h = 0;
    FILE* f = openPicture(strip, &w, &h);
    if (!f) return false;
    if (h > 256 || w > 16384) { fclose(f); return false; }
    strip_.resize(size_t(w) * h);
    const bool read = fread(strip_.data(), 1, strip_.size(), f) == strip_.size();
    fclose(f);
    if (!read) return false;
    stripWidth_ = w;
    height_ = h;
    index_.clear();
    f = fopen(index, "re");
    if (!f) return false;
    char line[256];
    bool valid = fgets(line, sizeof(line), f) && !strcmp(line, "pw3-sleep-glyphs 1\n");
    while (valid && fgets(line, sizeof(line), f)) {
        // "x width text": the text is the rest of the line after the second
        // space, and may itself begin with spaces (" · ").
        int x = 0, width = 0;
        const size_t length = strlen(line);
        const char* first = strchr(line, ' ');
        const char* second = first ? strchr(first + 1, ' ') : nullptr;
        valid = length > 0 && line[length - 1] == '\n' && second && sscanf(line, "%d %d", &x, &width) == 2 &&
                x >= 0 && width > 0 && x + width <= stripWidth_;
        if (valid) index_.push_back({std::string(second + 1, size_t(line + length - 1 - (second + 1))), x, width});
    }
    fclose(f);
    return valid && !index_.empty();
}

bool SleepText::place(const std::vector<std::string>& pieces, int center, int top, int width, int height) {
    line_.clear();
    lineWidth_ = 0;
    std::vector<const Piece*> found;
    for (const auto& text : pieces) {
        const Piece* piece = nullptr;
        for (const auto& p : index_) if (p.text == text) { piece = &p; break; }
        if (!piece) return false;
        found.push_back(piece);
        lineWidth_ += piece->width;
    }
    const int left = center - lineWidth_ / 2;
    if (found.empty() || left < 0 || left + lineWidth_ > width || top < 0 || top + height_ > height) {
        lineWidth_ = 0;
        return false;
    }
    line_.assign(size_t(lineWidth_) * height_, 0);
    int x = 0;
    for (const Piece* p : found) {
        for (int y = 0; y < height_; ++y)
            std::memcpy(&line_[size_t(y) * lineWidth_ + x], &strip_[size_t(y) * stripWidth_ + p->x], size_t(p->width));
        x += p->width;
    }
    left_ = left;
    top_ = top;
    return true;
}

void SleepText::row(int y, uint8_t* luma, int width, uint8_t ink) const {
    if (!lineWidth_ || y < top_ || y >= top_ + height_ || left_ + lineWidth_ > width) return;
    const uint8_t* cover = &line_[size_t(y - top_) * lineWidth_];
    uint8_t* out = luma + left_;
    for (int x = 0; x < lineWidth_; ++x) {
        const unsigned a = cover[x];
        out[x] = uint8_t((out[x] * (255 - a) + ink * a + 127) / 255);
    }
}
}  // namespace pw3

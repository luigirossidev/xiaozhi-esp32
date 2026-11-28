#include "clock_face.h"

#include "config.h"
#include "tiny_font.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

namespace clock_face {

namespace {

constexpr int kWidth = DISPLAY_WIDTH;
constexpr int kHeight = DISPLAY_HEIGHT;

constexpr const char* kWeekdays[] = {"DOMENICA", "LUNEDI",  "MARTEDI", "MERCOLEDI",
                                     "GIOVEDI",  "VENERDI", "SABATO"};
constexpr const char* kMonths[] = {"GENNAIO", "FEBBRAIO", "MARZO",     "APRILE",
                                   "MAGGIO",  "GIUGNO",   "LUGLIO",    "AGOSTO",
                                   "SETTEMBRE", "OTTOBRE", "NOVEMBRE", "DICEMBRE"};

struct Rgb {
    uint8_t r, g, b;
};

// RGB565 with the two bytes swapped, the order of the frame buffer.
constexpr uint16_t Pack(Rgb c)
{
    uint16_t v = static_cast<uint16_t>(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
    return static_cast<uint16_t>((v >> 8) | (v << 8));
}

std::string Short(const char* name)
{
    return std::string(name, 3);
}

// One band of the frame buffer, with drawing clipped to it.
class Canvas {
public:
    Canvas(uint16_t* pixels, int width, int y_start, int y_end)
        : pixels_(pixels), width_(width), y_start_(y_start), y_end_(y_end)
    {
    }

    void Rect(int x, int y, int w, int h, uint16_t color)
    {
        int x0 = std::max(x, 0);
        int x1 = std::min(x + w, width_);
        int y0 = std::max(y, y_start_);
        int y1 = std::min(y + h, y_end_);
        for (int yy = y0; yy < y1; yy++) {
            uint16_t* row = pixels_ + (yy - y_start_) * width_;
            for (int xx = x0; xx < x1; xx++) {
                row[xx] = color;
            }
        }
    }

    void Fill(uint16_t color) { Rect(0, 0, width_, kHeight, color); }

    void VerticalGradient(Rgb top, Rgb bottom)
    {
        for (int y = y_start_; y < y_end_; y++) {
            Rgb c = {Mix(top.r, bottom.r, y), Mix(top.g, bottom.g, y), Mix(top.b, bottom.b, y)};
            Rect(0, y, width_, 1, Pack(c));
        }
    }

    void RoundRect(int x, int y, int w, int h, int r, uint16_t color)
    {
        for (int yy = std::max(y, y_start_); yy < std::min(y + h, y_end_); yy++) {
            int dy = std::max({y + r - yy, yy - (y + h - 1 - r), 0});
            int inset = 0;
            while (inset < r) {
                int dx = r - inset;
                if (dx * dx + dy * dy <= r * r) {
                    break;
                }
                inset++;
            }
            Rect(x + inset, yy, w - 2 * inset, 1, color);
        }
    }

    // Draws text from x; returns the x where it ends. Digits and '-' all take a
    // full 5-column cell so that the time does not shift as it changes.
    int Text(std::string_view text, int x, int y, int scale, uint16_t color)
    {
        for (size_t i = 0; i < text.size(); i++) {
            char ch = text[i];
            int first, count;
            tiny_font::GlyphSpan(ch, first, count);
            if (IsFixed(ch)) {
                first = 0;
                count = tiny_font::kGlyphColumns;
            }
            if (i > 0) {
                x += scale;
            }
            if (ch != ' ') {
                const uint8_t* cols = tiny_font::GlyphFor(ch);
                for (int c = 0; c < count; c++) {
                    uint8_t bits = cols[first + c];
                    for (int row = 0; bits != 0 && row < tiny_font::kGlyphRows; row++) {
                        if (bits & (1 << row)) {
                            Rect(x + c * scale, y + row * scale, scale, scale, color);
                        }
                    }
                }
            }
            x += count * scale;
        }
        return x;
    }

    static int TextWidth(std::string_view text, int scale)
    {
        int width = 0;
        for (size_t i = 0; i < text.size(); i++) {
            int first, count;
            tiny_font::GlyphSpan(text[i], first, count);
            if (IsFixed(text[i])) {
                count = tiny_font::kGlyphColumns;
            }
            width += count * scale + (i > 0 ? scale : 0);
        }
        return width;
    }

    void TextCentered(std::string_view text, int center_x, int y, int scale, uint16_t color)
    {
        Text(text, center_x - TextWidth(text, scale) / 2, y, scale, color);
    }

    // A 7-segment digit; '-' lights the middle segment only.
    void Segments(char digit, int x, int y, int w, int h, int t, uint16_t on, uint16_t off)
    {
        static const char* const kLit[] = {"abcdef", "bc",     "abdeg", "abcdg",   "bcfg",
                                           "acdfg",  "acdefg", "abc",   "abcdefg", "abcdfg"};
        const char* lit = (digit >= '0' && digit <= '9') ? kLit[digit - '0'] : "g";
        int half = h / 2;
        int upper = half - t - t / 2;
        int lower = half - t - (t + 1) / 2;
        auto segment = [&](char name, int sx, int sy, int sw, int sh, bool vertical) {
            if (vertical) {  // 1px gap at both ends, so segments read as separate LEDs
                sy += 1;
                sh -= 2;
            }
            bool is_lit = std::string_view(lit).find(name) != std::string_view::npos;
            Rect(sx, sy, sw, sh, is_lit ? on : off);
        };
        segment('a', x + t, y, w - 2 * t, t, false);
        segment('b', x + w - t, y + t, t, upper, true);
        segment('c', x + w - t, y + half + (t + 1) / 2, t, lower, true);
        segment('d', x + t, y + h - t, w - 2 * t, t, false);
        segment('e', x, y + half + (t + 1) / 2, t, lower, true);
        segment('f', x, y + t, t, upper, true);
        segment('g', x + t, y + half - t / 2, w - 2 * t, t, false);
    }

private:
    static bool IsFixed(char ch) { return (ch >= '0' && ch <= '9') || ch == '-'; }

    uint8_t Mix(uint8_t a, uint8_t b, int y) const
    {
        return static_cast<uint8_t>(a + (b - a) * y / (kHeight - 1));
    }

    uint16_t* pixels_;
    int width_;
    int y_start_;
    int y_end_;
};

void TwoDigits(char* out, int value, bool valid)
{
    if (valid) {
        snprintf(out, 3, "%02d", value);
    } else {
        out[0] = out[1] = '-';
        out[2] = '\0';
    }
}

void PaintClassic(Canvas& c, const Time& t)
{
    c.Fill(Pack({0, 0, 0}));
    char hh[3], mm[3];
    TwoDigits(hh, t.hour, t.valid);
    TwoDigits(mm, t.minute, t.valid);
    std::string time = std::string(hh) + ":" + mm;
    int x = (kWidth - Canvas::TextWidth(time, 5)) / 2;
    x = c.Text(hh, x, 6, 5, Pack({255, 255, 255}));
    // The colon blinks once a second.
    uint16_t colon = (t.second % 2 == 0) ? Pack({255, 255, 255}) : Pack({0, 0, 0});
    x = c.Text(":", x + 5, 6, 5, colon);
    c.Text(mm, x + 5, 6, 5, Pack({255, 255, 255}));
    if (t.valid) {
        char date[24];
        snprintf(date, sizeof(date), "%s %d %s", Short(kWeekdays[t.weekday]).c_str(), t.day,
                 Short(kMonths[t.month - 1]).c_str());
        c.TextCentered(date, kWidth / 2, 57, 2, Pack({150, 150, 150}));
    }
}

void PaintLed(Canvas& c, const Time& t)
{
    const uint16_t on = Pack({255, 30, 20});
    const uint16_t off = Pack({32, 3, 3});
    c.Fill(Pack({0, 0, 0}));
    char digits[5];
    TwoDigits(digits, t.hour, t.valid);
    TwoDigits(digits + 2, t.minute, t.valid);
    constexpr int kDigitW = 26, kDigitH = 50, kThick = 5, kGap = 5, kColon = 12;
    constexpr int kTotal = 4 * kDigitW + 4 * kGap + kColon;
    int x = (kWidth - kTotal) / 2;
    constexpr int y = 5;
    for (int i = 0; i < 4; i++) {
        c.Segments(digits[i], x, y, kDigitW, kDigitH, kThick, on, off);
        x += kDigitW + kGap;
        if (i == 1) {
            uint16_t colon = (t.second % 2 == 0) ? on : off;
            int cx = x + (kColon - kThick) / 2 - kGap / 2;
            c.Rect(cx, y + kDigitH / 3 - 2, kThick, kThick, colon);
            c.Rect(cx, y + 2 * kDigitH / 3 - 2, kThick, kThick, colon);
            x += kColon;
        }
    }
    if (t.valid) {
        char date[40];
        snprintf(date, sizeof(date), "%s %d %s %d", kWeekdays[t.weekday], t.day,
                 kMonths[t.month - 1], t.year);
        c.TextCentered(date, kWidth / 2, 65, 1, Pack({200, 30, 20}));
    }
}

void PaintColor(Canvas& c, const Time& t)
{
    c.VerticalGradient({12, 24, 70}, {55, 12, 75});
    char hh[3], mm[3];
    TwoDigits(hh, t.hour, t.valid);
    TwoDigits(mm, t.minute, t.valid);
    int hw = Canvas::TextWidth(hh, 5);
    int cw = Canvas::TextWidth(":", 5);
    int mw = Canvas::TextWidth(mm, 5);
    int x = (kWidth - (hw + cw + mw + 10)) / 2;
    c.Text(hh, x, 5, 5, Pack({255, 150, 30}));
    x += hw + 5;
    if (t.second % 2 == 0) {
        c.Text(":", x, 5, 5, Pack({255, 255, 255}));
    }
    x += cw + 5;
    c.Text(mm, x, 5, 5, Pack({40, 220, 255}));
    if (t.valid) {
        std::string weekday = Short(kWeekdays[t.weekday]);
        char date[16];
        snprintf(date, sizeof(date), "%d %s", t.day, Short(kMonths[t.month - 1]).c_str());
        int chip_w = Canvas::TextWidth(weekday, 2) + 8;
        int date_w = Canvas::TextWidth(date, 2);
        int x0 = (kWidth - (chip_w + 6 + date_w)) / 2;
        c.RoundRect(x0, 54, chip_w, 20, 4, Pack({255, 205, 40}));
        c.Text(weekday, x0 + 4, 56, 2, Pack({30, 20, 40}));
        c.Text(date, x0 + chip_w + 6, 56, 2, Pack({255, 255, 255}));
    }
}

void PaintCalendar(Canvas& c, const Time& t)
{
    const uint16_t red = Pack({215, 40, 40});
    c.Fill(Pack({18, 18, 24}));
    // Tear-off calendar sheet on the left.
    c.RoundRect(4, 4, 60, 72, 4, Pack({245, 245, 240}));
    c.RoundRect(4, 4, 60, 20, 4, red);
    c.Rect(4, 16, 60, 8, red);
    char day[3];
    TwoDigits(day, t.day, t.valid);
    if (t.valid) {
        c.TextCentered(Short(kMonths[t.month - 1]), 34, 7, 2, Pack({255, 255, 255}));
        snprintf(day, sizeof(day), "%d", t.day);
        c.TextCentered(kWeekdays[t.weekday], 34, 66, 1, red);
    }
    c.TextCentered(day, 34, 30, 4, Pack({25, 25, 30}));
    // Time and year on the right.
    char hh[3], mm[3];
    TwoDigits(hh, t.hour, t.valid);
    TwoDigits(mm, t.minute, t.valid);
    const uint16_t green = Pack({90, 230, 130});
    std::string time = std::string(hh) + ":" + mm;
    int x = 112 - Canvas::TextWidth(time, 3) / 2;
    x = c.Text(hh, x, 18, 3, green);
    x = c.Text(":", x + 3, 18, 3, (t.second % 2 == 0) ? green : Pack({18, 18, 24}));
    c.Text(mm, x + 3, 18, 3, green);
    if (t.valid) {
        char year[8];
        snprintf(year, sizeof(year), "%d", t.year);
        c.TextCentered(year, 112, 52, 2, Pack({140, 140, 150}));
    }
}

}  // namespace

void Paint(Style style, const Time& time, uint16_t* pixels, int width, int y_start, int y_end)
{
    Canvas canvas(pixels, width, y_start, y_end);
    Time t = time;
    if (t.valid && (t.weekday < 0 || t.weekday > 6 || t.month < 1 || t.month > 12)) {
        t.valid = false;
    }
    switch (style) {
    case Style::kClassic:
        PaintClassic(canvas, t);
        break;
    case Style::kLed:
        PaintLed(canvas, t);
        break;
    case Style::kCalendar:
        PaintCalendar(canvas, t);
        break;
    case Style::kColor:
    default:
        PaintColor(canvas, t);
        break;
    }
}

}  // namespace clock_face

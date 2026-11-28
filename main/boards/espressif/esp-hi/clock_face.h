#pragma once

#include <cstdint>

// Peppe: clock screen drawn over the 160x80 display, in four styles.
namespace clock_face {

enum class Style : int {
    kClassic = 1,   // white time, grey date, black background
    kLed = 2,       // red 7-segment digits with unlit segments, full date
    kColor = 3,     // orange hours, cyan minutes, yellow weekday chip, gradient
    kCalendar = 4,  // tear-off calendar sheet with time and year beside it
};
constexpr Style kDefaultStyle = Style::kColor;
constexpr int kFirstStyle = 1;
constexpr int kLastStyle = 4;

struct Time {
    bool valid = false;  // false until the clock has been synchronised
    int hour = 0;
    int minute = 0;
    int second = 0;
    int weekday = 0;  // 0 = Sunday
    int day = 1;
    int month = 1;  // 1..12
    int year = 2000;
};

// Draws rows [y_start, y_end) of the face into pixels, a band of width columns
// starting at row y_start. Colours are RGB565 with the bytes swapped, as the
// display frame buffer holds them.
void Paint(Style style, const Time& time, uint16_t* pixels, int width, int y_start, int y_end);

}  // namespace clock_face

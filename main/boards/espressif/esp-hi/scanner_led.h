#pragma once

#include "led/led.h"

#include <esp_timer.h>
#include <led_strip.h>

#include <array>
#include <cstdint>
#include <mutex>

// Peppe: the 4-LED WS2812 strip above the screen.
//
// While the assistant listens, a red light sweeps back and forth with a fading
// trail (a "scanner"); while it speaks, the same in green. In every other
// state the strip shows the colour last asked for by voice (MCP light tools),
// or stays off. The scanner can be switched off; the colour then never changes
// with the state.
class ScannerLed : public Led {
public:
    struct Color {
        uint8_t r = 0, g = 0, b = 0;
    };

    explicit ScannerLed(led_strip_handle_t strip);
    ~ScannerLed() override;

    void OnStateChanged() override;

    // Colour shown outside the scanner; {0,0,0} is off.
    void SetManualColor(Color color);
    Color GetManualColor();

    void SetScannerEnabled(bool enabled);
    bool IsScannerEnabled();

private:
    static constexpr int kLedCount = 4;
    static constexpr int kStepMs = 100;           // time the head stays on each LED
    static constexpr uint8_t kBrightness = 160;   // head brightness, out of 255
    static constexpr int kTrailPercent = 35;      // what is left of a LED at each step

    std::mutex mutex_;
    led_strip_handle_t strip_;
    esp_timer_handle_t timer_ = nullptr;
    Color manual_;
    bool scanner_enabled_ = true;
    bool scanning_ = false;
    Color scan_color_;
    std::array<int, kLedCount> levels_{};  // 0..255 per LED
    int head_ = 0;
    int direction_ = 1;

    void StartScanLocked(Color color);
    void StopScanLocked();
    void StepLocked();
    void ShowLocked(Color color);
};

#include "scanner_led.h"

#include "application.h"
#include "device_state.h"

#include <esp_log.h>

static const char* TAG = "ScannerLed";

ScannerLed::ScannerLed(led_strip_handle_t strip) : strip_(strip)
{
    esp_timer_create_args_t args = {};
    args.callback = [](void* arg) {
        auto* self = static_cast<ScannerLed*>(arg);
        std::lock_guard<std::mutex> lock(self->mutex_);
        if (self->scanning_) {
            self->StepLocked();
        }
    };
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "scanner_led";
    if (esp_timer_create(&args, &timer_) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create the scanner timer");
        timer_ = nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ShowLocked({});
}

ScannerLed::~ScannerLed()
{
    if (timer_ != nullptr) {
        esp_timer_stop(timer_);
        esp_timer_delete(timer_);
    }
}

void ScannerLed::OnStateChanged()
{
    auto state = Application::GetInstance().GetDeviceState();
    std::lock_guard<std::mutex> lock(mutex_);
    if (scanner_enabled_ && state == kDeviceStateListening) {
        StartScanLocked({kBrightness, 0, 0});
    } else if (scanner_enabled_ && state == kDeviceStateSpeaking) {
        StartScanLocked({0, kBrightness, 0});
    } else {
        StopScanLocked();
    }
}

void ScannerLed::SetManualColor(Color color)
{
    std::lock_guard<std::mutex> lock(mutex_);
    manual_ = color;
    if (!scanning_) {
        ShowLocked(manual_);
    }
}

ScannerLed::Color ScannerLed::GetManualColor()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return manual_;
}

void ScannerLed::SetScannerEnabled(bool enabled)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scanner_enabled_ = enabled;
    }
    OnStateChanged();  // start or stop right away if the robot is talking
}

bool ScannerLed::IsScannerEnabled()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return scanner_enabled_;
}

void ScannerLed::StartScanLocked(Color color)
{
    if (timer_ == nullptr) {
        return;
    }
    scan_color_ = color;
    if (scanning_) {
        return;  // keep sweeping from where it is, only the colour changes
    }
    scanning_ = true;
    levels_.fill(0);
    head_ = 0;
    direction_ = 1;
    StepLocked();
    esp_timer_start_periodic(timer_, kStepMs * 1000);
}

void ScannerLed::StopScanLocked()
{
    if (scanning_) {
        esp_timer_stop(timer_);
        scanning_ = false;
    }
    ShowLocked(manual_);
}

void ScannerLed::StepLocked()
{
    // Fade every LED, then light the head at full brightness.
    for (auto& level : levels_) {
        level = level * kTrailPercent / 100;
    }
    levels_[head_] = 255;
    for (int i = 0; i < kLedCount; i++) {
        led_strip_set_pixel(strip_, i, scan_color_.r * levels_[i] / 255,
                            scan_color_.g * levels_[i] / 255, scan_color_.b * levels_[i] / 255);
    }
    led_strip_refresh(strip_);

    // Bounce at both ends: 0 1 2 3 2 1 0 1 ...
    if (head_ + direction_ < 0 || head_ + direction_ >= kLedCount) {
        direction_ = -direction_;
    }
    head_ += direction_;
}

void ScannerLed::ShowLocked(Color color)
{
    for (int i = 0; i < kLedCount; i++) {
        led_strip_set_pixel(strip_, i, color.r, color.g, color.b);
    }
    led_strip_refresh(strip_);
}

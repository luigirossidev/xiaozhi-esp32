#pragma once

#include "display/lcd_display.h"
#include <atomic>
#include <memory>
#include <functional>
#include <mutex>
#include <string>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include "anim_player.h"
#include "assets.h"

namespace anim {

class EmojiPlayer;

using FlushIoReadyCallback = std::function<bool(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*)>;
using FlushCallback = std::function<void(anim_player_handle_t, int, int, int, int, const void*)>;

class EmojiPlayer {
public:
    EmojiPlayer(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io);
    ~EmojiPlayer();

    bool StartPlayer(const std::string& asset_name, bool repeat, int fps);
    void StopPlayer();

    // Peppe: text drawn in white over every frame, one line above the eyes and
    // one below. An empty string leaves that line out. Each line is drawn as
    // large as fits, up to max_scale times the 5x8 font.
    void SetOverlay(const std::string& top, const std::string& bottom, int bottom_max_scale = 2);
    void ClearOverlay();

private:
    // One line of overlay text, laid out once when it is set.
    struct OverlayLine {
        std::string text;
        int x = 0;
        int y = 0;
        int scale = 1;
    };

    static bool OnFlushIoReady(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx);
    static void OnFlush(anim_player_handle_t handle, int x_start, int y_start, int x_end, int y_end, const void *color_data);

    static OverlayLine LayoutLine(const std::string& text, int band_top, int band_bottom,
                                  int max_scale);
    void DrawOverlay(uint16_t* pixels, int width, int y_start, int y_end);

    esp_lcd_panel_handle_t panel_;
    anim_player_handle_t player_handle_;

    // Written by the main task, read by the player task on every flush.
    std::mutex overlay_mutex_;
    OverlayLine overlay_top_;
    OverlayLine overlay_bottom_;
};

class EmojiWidget : public Display {
public:
    EmojiWidget(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io);
    virtual ~EmojiWidget();

    virtual bool SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
    virtual void SetChatMessage(const char* role, const char* content) override {}

    // Peppe: activation code, written large below the eyes with the site where
    // to enter it above them. It goes away with the next status change, once
    // the device is activated.
    virtual void ShowActivationCode(const std::string& code, const std::string& message) override;

    // Peppe: Wi-Fi configuration screen. The eyes stay on screen and blink, with
    // the title above them and the hotspot name to join below.
    void ShowWifiConfig(const std::string& title, const std::string& ssid);
    void HideWifiConfig();

    anim::EmojiPlayer* GetPlayer()
    {
        return player_.get();
    }

private:
    // Screens that write text over the eyes. While one is shown, emotion
    // changes are ignored so nothing moves the eyes under the text.
    enum class TextScreen { kNone, kWifiConfig, kActivation };

    void ShowTextScreen(TextScreen screen, const std::string& top, const std::string& bottom,
                        int bottom_max_scale);
    void HideTextScreen();
    void InitializePlayer(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io);
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

    std::unique_ptr<anim::EmojiPlayer> player_;
    // Set from the main task and from the activation task.
    std::atomic<TextScreen> text_screen_{TextScreen::kNone};
};

} // namespace anim

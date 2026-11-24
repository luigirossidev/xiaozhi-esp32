#include "emoji_display.h"
#include <esp_log.h>
#include <algorithm>
#include <cstring>
#include <initializer_list>
#include "assets.h"
#include "assets/lang_config.h"
#include "config.h"
#include "display/lcd_display.h"
#include "tiny_font.h"

#include <esp_lcd_panel_io.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/task.h>

static const char* TAG = "emoji";

namespace anim {

// Emoji asset name mapping based on usage pattern
static const std::unordered_map<std::string, std::string> emoji_asset_name_map = {
    {"connecting", "connecting.aaf"}, {"wake", "wake.aaf"},
    {"asking", "asking.aaf"},         {"happy_loop", "happy_loop.aaf"},
    {"sad_loop", "sad_loop.aaf"},
    // Upstream image_player asset uses capital A in the filename.
    {"anger_loop", "Anger_loop.aaf"},
    {"panic_loop", "panic_loop.aaf"}, {"blink_quick", "blink_quick.aaf"},
    {"scorn_loop", "scorn_loop.aaf"}};

// Peppe: rows the eyes of blink_quick.aaf cover (measured on every frame: 27..52).
// The text screens (Wi-Fi configuration, activation) write in the free bands above and below.
static constexpr int kEyesTop = 27;
static constexpr int kEyesBottom = 53;
static constexpr int kOverlayMargin = 2;
static constexpr uint16_t kOverlayColor = 0xFFFF;  // white, same with or without byte swap

static const uint8_t* GlyphFor(char ch) {
    if (ch < tiny_font::kFirstChar || ch > tiny_font::kLastChar) {
        ch = '?';
    }
    return &tiny_font::kGlyphs[(ch - tiny_font::kFirstChar) * tiny_font::kGlyphColumns];
}

// Glyphs are drawn proportionally: empty columns at their sides are dropped, so
// that narrow glyphs such as "." or "i" do not waste a whole cell. A space keeps 3 columns.
static void GlyphSpan(char ch, int& first, int& count) {
    if (ch == ' ') {
        first = 0;
        count = 3;
        return;
    }
    const uint8_t* cols = GlyphFor(ch);
    int left = 0;
    int right = tiny_font::kGlyphColumns;
    while (left < right && cols[left] == 0) {
        left++;
    }
    while (right > left && cols[right - 1] == 0) {
        right--;
    }
    first = left;
    count = right - left;
}

static int TextWidth(const std::string& text, int scale) {
    int width = 0;
    for (size_t i = 0; i < text.size(); i++) {
        int first, count;
        GlyphSpan(text[i], first, count);
        width += count * scale;
        if (i > 0) {
            width += scale;  // one column between glyphs
        }
    }
    return width;
}

bool EmojiPlayer::OnFlushIoReady(esp_lcd_panel_io_handle_t panel_io,
                                 esp_lcd_panel_io_event_data_t* edata, void* user_ctx) {
    anim_player_flush_ready(static_cast<anim_player_handle_t>(user_ctx));
    return true;
}

void EmojiPlayer::OnFlush(anim_player_handle_t handle, int x_start, int y_start, int x_end,
                          int y_end, const void* color_data) {
    auto* player = static_cast<EmojiPlayer*>(anim_player_get_user_data(handle));
    // color_data is the player's own frame buffer, decoded again for every
    // split of every frame, so the text can be written straight into it.
    player->DrawOverlay(static_cast<uint16_t*>(const_cast<void*>(color_data)), x_end - x_start,
                        y_start, y_end);
    esp_lcd_panel_draw_bitmap(player->panel_, x_start, y_start, x_end, y_end, color_data);
}

EmojiPlayer::OverlayLine EmojiPlayer::LayoutLine(const std::string& text, int band_top,
                                                 int band_bottom, int max_scale) {
    OverlayLine line;
    line.text = text;
    if (text.empty()) {
        return line;
    }
    // As large as fits the band, so the text can be read on the 0.96" screen.
    for (line.scale = max_scale; line.scale > 1; line.scale--) {
        if (TextWidth(text, line.scale) <= DISPLAY_WIDTH - 2 * kOverlayMargin &&
            tiny_font::kGlyphRows * line.scale <= band_bottom - band_top) {
            break;
        }
    }
    int width = TextWidth(text, line.scale);
    int height = tiny_font::kGlyphRows * line.scale;
    line.x = std::max(0, (DISPLAY_WIDTH - width) / 2);
    line.y = band_top + std::max(0, (band_bottom - band_top - height) / 2);
    return line;
}

void EmojiPlayer::SetOverlay(const std::string& top, const std::string& bottom,
                             int bottom_max_scale) {
    OverlayLine top_line = LayoutLine(top, 0, kEyesTop, 2);
    OverlayLine bottom_line = LayoutLine(bottom, kEyesBottom, DISPLAY_HEIGHT, bottom_max_scale);
    std::lock_guard<std::mutex> lock(overlay_mutex_);
    overlay_top_ = std::move(top_line);
    overlay_bottom_ = std::move(bottom_line);
}

void EmojiPlayer::ClearOverlay() {
    std::lock_guard<std::mutex> lock(overlay_mutex_);
    overlay_top_ = OverlayLine();
    overlay_bottom_ = OverlayLine();
}

void EmojiPlayer::DrawOverlay(uint16_t* pixels, int width, int y_start, int y_end) {
    std::lock_guard<std::mutex> lock(overlay_mutex_);
    for (const OverlayLine* line : {&overlay_top_, &overlay_bottom_}) {
        int height = tiny_font::kGlyphRows * line->scale;
        if (line->text.empty() || line->y >= y_end || line->y + height <= y_start) {
            continue;
        }
        int x = line->x;
        for (char ch : line->text) {
            int first, count;
            GlyphSpan(ch, first, count);
            const uint8_t* cols = GlyphFor(ch);
            for (int c = 0; c < count; c++) {
                uint8_t bits = (ch == ' ') ? 0 : cols[first + c];
                for (int r = 0; bits != 0 && r < tiny_font::kGlyphRows; r++) {
                    if (!(bits & (1 << r))) {
                        continue;
                    }
                    for (int dy = 0; dy < line->scale; dy++) {
                        int py = line->y + r * line->scale + dy;
                        if (py < y_start || py >= y_end) {
                            continue;
                        }
                        for (int dx = 0; dx < line->scale; dx++) {
                            int px = x + c * line->scale + dx;
                            if (px >= 0 && px < width) {
                                pixels[(py - y_start) * width + px] = kOverlayColor;
                            }
                        }
                    }
                }
            }
            x += (count + 1) * line->scale;
        }
    }
}

EmojiPlayer::EmojiPlayer(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io)
    : panel_(panel) {
    ESP_LOGI(TAG, "Create EmojiPlayer, panel: %p, panel_io: %p", panel, panel_io);

    anim_player_config_t player_cfg = {.flush_cb = OnFlush,
                                       .update_cb = NULL,
                                       .user_data = this,
                                       .flags = {.swap = true},
                                       .task = ANIM_PLAYER_INIT_CONFIG()};

    player_cfg.task.task_priority = 1;
    player_cfg.task.task_stack = 4096;
    player_handle_ = anim_player_init(&player_cfg);

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = OnFlushIoReady,
    };
    esp_lcd_panel_io_register_event_callbacks(panel_io, &cbs, player_handle_);
    StartPlayer("connecting", true, 15);
}

EmojiPlayer::~EmojiPlayer() {
    if (player_handle_) {
        anim_player_update(player_handle_, PLAYER_ACTION_STOP);
        anim_player_deinit(player_handle_);
        player_handle_ = nullptr;
    }
}

bool EmojiPlayer::StartPlayer(const std::string& asset_name, bool repeat, int fps) {
    if (player_handle_) {
        uint32_t start, end;
        void* src_data = nullptr;
        size_t src_len = 0;

        auto& assets = Assets::GetInstance();
        auto filename_iter = emoji_asset_name_map.find(asset_name);
        if (filename_iter == emoji_asset_name_map.end()) {
            ESP_LOGE(TAG, "Unknown emoji asset: %s", asset_name.c_str());
            return false;
        }
        const std::string& filename = filename_iter->second;
        if (!assets.GetAssetData(filename, src_data, src_len)) {
            ESP_LOGE(TAG, "Failed to get asset data for %s", asset_name.c_str());
            return false;
        }

        esp_err_t err = anim_player_set_src_data(player_handle_, src_data, src_len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set asset data for %s: %s", asset_name.c_str(),
                     esp_err_to_name(err));
            return false;
        }
        anim_player_get_segment(player_handle_, &start, &end);
        if (asset_name == "wake") {
            start = 7;
        }
        anim_player_set_segment(player_handle_, start, end, fps, true);
        anim_player_update(player_handle_, PLAYER_ACTION_START);
        return true;
    }
    return false;
}

void EmojiPlayer::StopPlayer() {
    if (player_handle_) {
        anim_player_update(player_handle_, PLAYER_ACTION_STOP);
    }
}

EmojiWidget::EmojiWidget(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io) {
    InitializePlayer(panel, panel_io);
}

EmojiWidget::~EmojiWidget() {}

bool EmojiWidget::SetEmotion(const char* emotion) {
    if (!player_ || emotion == nullptr) {
        return false;
    }
    if (text_screen_ != TextScreen::kNone) {
        // Keep the centred eyes of the text screen under its text.
        return true;
    }

    using Param = std::tuple<std::string, bool, int>;
    static const std::unordered_map<std::string, Param> emotion_map = {
        {"happy", {"happy_loop", true, 25}},       {"laughing", {"happy_loop", true, 25}},
        {"funny", {"happy_loop", true, 25}},       {"loving", {"happy_loop", true, 25}},
        {"embarrassed", {"happy_loop", true, 25}}, {"confident", {"happy_loop", true, 25}},
        {"delicious", {"happy_loop", true, 25}},   {"sad", {"sad_loop", true, 25}},
        {"crying", {"sad_loop", true, 25}},        {"sleepy", {"sad_loop", true, 25}},
        {"silly", {"sad_loop", true, 25}},         {"angry", {"anger_loop", true, 25}},
        {"surprised", {"panic_loop", true, 25}},   {"shocked", {"panic_loop", true, 25}},
        {"thinking", {"happy_loop", true, 25}},    {"winking", {"blink_quick", true, 5}},
        {"relaxed", {"scorn_loop", true, 25}},     {"confused", {"scorn_loop", true, 25}},
    };

    auto it = emotion_map.find(emotion);
    if (it != emotion_map.end()) {
        const auto& [aaf, repeat, fps] = it->second;
        return player_->StartPlayer(aaf, repeat, fps);
    }
    // "neutral" is a request this widget deliberately has nothing to play for, which is still
    // an accepted request rather than a missing resource.
    return strcmp(emotion, "neutral") == 0;
}

void EmojiWidget::SetStatus(const char* status) {
    if (status == nullptr) {
        return;
    }
    // The activation code stays up while the device waits for it to be entered;
    // any other status means activation is over.
    if (text_screen_ == TextScreen::kActivation &&
        strcmp(status, Lang::Strings::ACTIVATION) != 0) {
        HideTextScreen();
    }
    if (player_ && text_screen_ == TextScreen::kNone) {
        if (strcmp(status, Lang::Strings::LISTENING) == 0) {
            player_->StartPlayer("asking", true, 15);
        } else if (strcmp(status, Lang::Strings::STANDBY) == 0) {
            player_->StartPlayer("wake", true, 15);
        }
    }
}

void EmojiWidget::ShowActivationCode(const std::string& code, const std::string& message) {
    // The server message is normally the site followed by the code on its own
    // line ("xiaozhi.me\n123456"). Its first line goes on top, unless it is
    // empty or already holds the code, in which case the status title is used.
    std::string site = message.substr(0, message.find('\n'));
    site.erase(0, site.find_first_not_of(" \t\r"));
    site.erase(site.find_last_not_of(" \t\r") + 1);
    if (site.empty() || site.find(code) != std::string::npos) {
        site = Lang::Strings::ACTIVATION;
    }
    ShowTextScreen(TextScreen::kActivation, site, code, 3);
}

void EmojiWidget::ShowWifiConfig(const std::string& title, const std::string& ssid) {
    ShowTextScreen(TextScreen::kWifiConfig, title, ssid, 2);
}

void EmojiWidget::HideWifiConfig() {
    if (text_screen_ == TextScreen::kWifiConfig) {
        HideTextScreen();
    }
}

void EmojiWidget::ShowTextScreen(TextScreen screen, const std::string& top,
                                 const std::string& bottom, int bottom_max_scale) {
    if (!player_) {
        return;
    }
    text_screen_ = screen;
    player_->SetOverlay(top, bottom, bottom_max_scale);
    player_->StartPlayer("blink_quick", true, 5);
}

void EmojiWidget::HideTextScreen() {
    if (!player_ || text_screen_ == TextScreen::kNone) {
        return;
    }
    text_screen_ = TextScreen::kNone;
    player_->ClearOverlay();
    player_->StartPlayer("connecting", true, 15);
}

void EmojiWidget::InitializePlayer(esp_lcd_panel_handle_t panel,
                                   esp_lcd_panel_io_handle_t panel_io) {
    player_ = std::make_unique<EmojiPlayer>(panel, panel_io);
}

bool EmojiWidget::Lock(int timeout_ms) { return true; }

void EmojiWidget::Unlock() {}

}  // namespace anim

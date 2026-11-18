#include "wifi_board.h"
#include "adc_pdm_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "system_info.h"
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <vector>

#include "display/lcd_display.h"
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include "esp_lcd_ili9341.h"

#include "assets/lang_config.h"
#include "anim_player.h"
#include "emoji_display.h"
#include "servo_dog_ctrl.h"
#include "led_strip.h"
#include "driver/rmt_tx.h"
#include "device_state.h"

#include "sdkconfig.h"

#ifdef CONFIG_ESP_HI_WEB_CONTROL_ENABLED
#include "esp_hi_web_control.h"
#include <esp_http_server.h>
#include <esp_app_desc.h>
#include <wifi_manager.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#endif //CONFIG_ESP_HI_WEB_CONTROL_ENABLED

#define TAG "ESP_HI"

// Named static payloads: C++ compound-literal pointers must outlive panel init.
static const uint8_t st7735_cmd_b1[] = {0x05, 0x3A, 0x3A};
static const uint8_t st7735_cmd_b2[] = {0x05, 0x3A, 0x3A};
static const uint8_t st7735_cmd_b3[] = {0x05, 0x3A, 0x3A, 0x05, 0x3A, 0x3A};
static const uint8_t st7735_cmd_b4[] = {0x03};
static const uint8_t st7735_cmd_c0[] = {0x44, 0x04, 0x04};
static const uint8_t st7735_cmd_c1[] = {0xC0};
static const uint8_t st7735_cmd_c2[] = {0x0D, 0x00};
static const uint8_t st7735_cmd_c3[] = {0x8D, 0x6A};
static const uint8_t st7735_cmd_c4[] = {0x8D, 0xEE};
static const uint8_t st7735_cmd_c5[] = {0x08};
static const uint8_t st7735_cmd_e0[] = {0x0F, 0x10, 0x03, 0x03, 0x07, 0x02, 0x00, 0x02,
                                        0x07, 0x0C, 0x13, 0x38, 0x0A, 0x0E, 0x03, 0x10};
static const uint8_t st7735_cmd_e1[] = {0x10, 0x0B, 0x04, 0x04, 0x10, 0x03, 0x00, 0x03,
                                        0x03, 0x09, 0x17, 0x33, 0x0B, 0x0C, 0x06, 0x10};
static const uint8_t st7735_cmd_35[] = {0x00};
static const uint8_t st7735_cmd_3a[] = {0x05};
static const uint8_t st7735_cmd_36[] = {0xC8};

static const ili9341_lcd_init_cmd_t vendor_specific_init[] = {
    {0x11, NULL, 0, 120},  // Sleep out, Delay 120ms
    {0xB1, st7735_cmd_b1, sizeof(st7735_cmd_b1), 0},
    {0xB2, st7735_cmd_b2, sizeof(st7735_cmd_b2), 0},
    {0xB3, st7735_cmd_b3, sizeof(st7735_cmd_b3), 0},
    {0xB4, st7735_cmd_b4, sizeof(st7735_cmd_b4), 0},  // Dot inversion
    {0xC0, st7735_cmd_c0, sizeof(st7735_cmd_c0), 0},
    {0xC1, st7735_cmd_c1, sizeof(st7735_cmd_c1), 0},
    {0xC2, st7735_cmd_c2, sizeof(st7735_cmd_c2), 0},
    {0xC3, st7735_cmd_c3, sizeof(st7735_cmd_c3), 0},
    {0xC4, st7735_cmd_c4, sizeof(st7735_cmd_c4), 0},
    {0xC5, st7735_cmd_c5, sizeof(st7735_cmd_c5), 0},
    {0xE0, st7735_cmd_e0, sizeof(st7735_cmd_e0), 0},
    {0xE1, st7735_cmd_e1, sizeof(st7735_cmd_e1), 0},
    {0x35, st7735_cmd_35, sizeof(st7735_cmd_35), 0},
    {0x3A, st7735_cmd_3a, sizeof(st7735_cmd_3a), 0},
    {0x36, st7735_cmd_36, sizeof(st7735_cmd_36), 0},
    {0x29, NULL, 0, 0},  // Display on
    {0x2C, NULL, 0, 0},  // Memory write
};

static const led_strip_config_t bsp_strip_config = {
    .strip_gpio_num = GPIO_NUM_8,
    .max_leds = 4,
    .led_model = LED_MODEL_WS2812,
    .flags = {
        .invert_out = false
    }
};

static const led_strip_rmt_config_t bsp_rmt_config = {
    .clk_src = RMT_CLK_SRC_DEFAULT,
    .resolution_hz = 10 * 1000 * 1000,
    .flags = {
        .with_dma = false
    }
};

class EspHi : public WifiBoard {
private:
    Button boot_button_;
    Button audio_wake_button_;
    Button move_wake_button_;
    anim::EmojiWidget* display_ = nullptr;
    bool web_server_initialized_ = false;
    led_strip_handle_t led_strip_;
    bool led_on_ = false;

#ifdef CONFIG_ESP_HI_WEB_CONTROL_ENABLED
    // Peppe: in-memory log, readable with GET http://<ip>:8080/log.
    //
    // This board has no usable console: CONFIG_ESP_CONSOLE_NONE is set and the
    // USB-C connector is taken by the servos once the body is attached. The
    // last few kB of log are kept in RAM so that problems can be diagnosed over
    // Wi-Fi instead of taking the robot apart.
    static constexpr size_t kLogSize = 4096;
    static inline char log_buffer_[kLogSize];
    static inline size_t log_head_ = 0;      // next write position
    static inline bool log_wrapped_ = false;
    static inline portMUX_TYPE log_lock_ = portMUX_INITIALIZER_UNLOCKED;

    static int LogVprintf(const char* format, va_list args)
    {
        char line[160];
        int len = vsnprintf(line, sizeof(line), format, args);
        if (len <= 0) {
            return len;
        }
        size_t count = std::min(static_cast<size_t>(len), sizeof(line) - 1);
        portENTER_CRITICAL(&log_lock_);
        for (size_t i = 0; i < count; i++) {
            log_buffer_[log_head_] = line[i];
            if (++log_head_ == kLogSize) {
                log_head_ = 0;
                log_wrapped_ = true;
            }
        }
        portEXIT_CRITICAL(&log_lock_);
        return len;
    }

    static esp_err_t LogHandler(httpd_req_t* req)
    {
        // Snapshot under the lock, then send without it.
        std::string snapshot;
        snapshot.resize(kLogSize);
        size_t size = 0;
        portENTER_CRITICAL(&log_lock_);
        if (log_wrapped_) {
            memcpy(snapshot.data(), log_buffer_ + log_head_, kLogSize - log_head_);
            memcpy(snapshot.data() + kLogSize - log_head_, log_buffer_, log_head_);
            size = kLogSize;
        } else {
            memcpy(snapshot.data(), log_buffer_, log_head_);
            size = log_head_;
        }
        portEXIT_CRITICAL(&log_lock_);
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_send(req, snapshot.data(), size);
    }

    // Peppe: firmware version and MAC address, GET http://<ip>:8080/version.
    //
    // The mobile app compares the version with the latest release published on
    // the portal, to tell the user when an update is available. It is
    // CONFIG_APP_PROJECT_VER from this board's config.json. The MAC is the
    // Wi-Fi station one, the same the device sends to the server as Device-Id,
    // e.g. {"version":"1.0.0","mac":"a0:b1:c2:d3:e4:f5"}.
    static esp_err_t VersionHandler(httpd_req_t* req)
    {
        char reply[96];
        snprintf(reply, sizeof(reply), "{\"version\":\"%s\",\"mac\":\"%s\"}",
                 esp_app_get_description()->version, SystemInfo::GetMacAddress().c_str());
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, reply);
    }

    // Peppe: "speak through the assistant" endpoint.
    //
    // POST http://<ip>:8080/speak with a body made of Opus packets, each one
    // preceded by its length as a big-endian uint16. The packets are sent to
    // the assistant in place of the microphone, as one push-to-talk utterance,
    // so the reply is spoken with the usual voice. The protocol also has a
    // text message ("listen"/"detect"), but the server only accepts it for
    // wake words and rejects anything longer than about 30 characters.
    //
    //
    // A client should first call POST /speak/begin (empty body), which opens
    // the audio channel and answers only once the server is listening, and
    // send the audio after that. Posting to /speak directly also works, but
    // then the audio travels while the channel is still being opened: unread
    // data sitting in the socket holds the few Wi-Fi RX buffers this board
    // has, and the server hello may no longer get in.
    //
    // Query parameters, all optional, meant for tuning without reflashing:
    //   fd    frame duration of each packet in ms (default 60)
    //   sr    sample rate in Hz (default 16000)
    //   pace  delay between packets in ms (default fd / 2)
    static int QueryInt(httpd_req_t* req, const char* key, int fallback)
    {
        char query[64];
        char value[12];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
            httpd_query_key_value(query, key, value, sizeof(value)) == ESP_OK) {
            return atoi(value);
        }
        return fallback;
    }

    static bool RecvAll(httpd_req_t* req, uint8_t* data, size_t size)
    {
        size_t received = 0;
        while (received < size) {
            int ret = httpd_req_recv(req, reinterpret_cast<char*>(data) + received, size - received);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            if (ret <= 0) {
                return false;
            }
            received += ret;
        }
        return true;
    }

    // Opens the conversation for an injected utterance. On failure the error
    // response has already been sent and ESP_ERR_INVALID_STATE is returned.
    static esp_err_t BeginSpeech(httpd_req_t* req)
    {
        auto& app = Application::GetInstance();
        httpd_resp_set_type(req, "application/json");
        if (!app.BeginInjectedSpeech()) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"busy\"}");
            return ESP_ERR_INVALID_STATE;
        }
        if (!app.WaitInjectedSpeechReady(15000)) {
            app.EndInjectedSpeech();
            httpd_resp_set_status(req, "504 Gateway Timeout");
            httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"server\"}");
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }

    static esp_err_t SpeakBeginHandler(httpd_req_t* req)
    {
        if (BeginSpeech(req) != ESP_OK) {
            return ESP_OK;
        }
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }

    static esp_err_t SpeakHandler(httpd_req_t* req)
    {
        static constexpr size_t kMaxBody = 256 * 1024;
        static constexpr size_t kMaxPacket = 1275 * 3 + 16;  // three Opus frames

        httpd_resp_set_type(req, "application/json");
        if (req->content_len < 3 || req->content_len > kMaxBody) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"length\"}");
        }

        int frame_duration = QueryInt(req, "fd", 60);
        int sample_rate = QueryInt(req, "sr", 16000);
        int pace = QueryInt(req, "pace", frame_duration / 2);

        auto& app = Application::GetInstance();
        // Already begun through /speak/begin? Then go straight to the audio.
        if (!app.IsInjectedSpeechReady()) {
            esp_err_t err = BeginSpeech(req);
            if (err != ESP_OK) {
                return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
            }
        }

        size_t remaining = req->content_len;
        int packets = 0;
        bool ok = true;
        while (remaining > 2) {
            uint8_t header[2];
            if (!RecvAll(req, header, sizeof(header))) {
                ok = false;
                break;
            }
            remaining -= 2;
            size_t length = (header[0] << 8) | header[1];
            if (length == 0 || length > kMaxPacket || length > remaining) {
                ok = false;
                break;
            }
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            packet->frame_duration = frame_duration;
            packet->payload.resize(length);
            if (!RecvAll(req, packet->payload.data(), length)) {
                ok = false;
                break;
            }
            remaining -= length;
            if (!app.SendInjectedAudio(std::move(packet))) {
                ok = false;
                break;
            }
            packets++;
            if (pace > 0) {
                vTaskDelay(pdMS_TO_TICKS(pace));
            }
        }
        app.EndInjectedSpeech();
        ESP_LOGI(TAG, "Injected speech: %d packets, ok=%d", packets, ok);

        if (!ok) {
            // The connection is in an unknown state: let httpd close it.
            return ESP_FAIL;
        }
        char reply[48];
        snprintf(reply, sizeof(reply), "{\"ok\":true,\"packets\":%d}", packets);
        return httpd_resp_sendstr(req, reply);
    }

    // Peppe's own HTTP server, on port 8080: POST /speak/begin, POST /speak,
    // GET /log and GET /version.
    //
    // It is separate from the web control server on port 80 because that one
    // keeps its handle private inside the servo_dog_ctrl managed component, so
    // no handler can be added to it from here.
    static void StartPeppeServer()
    {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = 8080;
        config.ctrl_port = 32769;  // the web control server already owns the default one
        config.stack_size = 6144;
        config.max_open_sockets = 2;
        config.max_uri_handlers = 4;
        config.lru_purge_enable = true;

        httpd_handle_t server = nullptr;
        esp_err_t err = httpd_start(&server, &config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start Peppe server: %d", err);
            return;
        }
        static const httpd_uri_t log_uri = {
            .uri = "/log",
            .method = HTTP_GET,
            .handler = LogHandler,
            .user_ctx = nullptr,
        };
        httpd_register_uri_handler(server, &log_uri);
        static const httpd_uri_t version_uri = {
            .uri = "/version",
            .method = HTTP_GET,
            .handler = VersionHandler,
            .user_ctx = nullptr,
        };
        httpd_register_uri_handler(server, &version_uri);
        static const httpd_uri_t speak_uri = {
            .uri = "/speak",
            .method = HTTP_POST,
            .handler = SpeakHandler,
            .user_ctx = nullptr,
        };
        httpd_register_uri_handler(server, &speak_uri);
        static const httpd_uri_t speak_begin_uri = {
            .uri = "/speak/begin",
            .method = HTTP_POST,
            .handler = SpeakBeginHandler,
            .user_ctx = nullptr,
        };
        httpd_register_uri_handler(server, &speak_begin_uri);
        ESP_LOGI(TAG, "Peppe server listening on port 8080");
    }

    static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                 int32_t event_id, void* event_data)
    {
        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {

            xTaskCreate(
                [](void* arg) {
                    EspHi* instance = static_cast<EspHi*>(arg);
                    
                    vTaskDelay(5000 / portTICK_PERIOD_MS);

                    if (!instance->web_server_initialized_) {
                        ESP_LOGI(TAG, "WiFi connected, init web control server");
                        esp_err_t err = esp_hi_web_control_server_init();
                        if (err != ESP_OK) {
                            ESP_LOGE(TAG, "Failed to initialize web control server: %d", err);
                        } else {
                            ESP_LOGI(TAG, "Web control server initialized");
                            instance->web_server_initialized_ = true;
                            StartPeppeServer();
                        }
                    }

                    vTaskDelete(NULL);
                },
                "web_server_init",
                1024 * 10, arg, 5, nullptr);
        }
    }
#endif //CONFIG_ESP_HI_WEB_CONTROL_ENABLED

    void HandleMoveWakePressDown(int64_t current_time, int64_t &last_trigger_time, int &gesture_state)
    {
        int64_t interval = last_trigger_time == 0 ? 0 : current_time - last_trigger_time;
        last_trigger_time = current_time;

        if (interval > 1000) {
            gesture_state = 0;
        } else {
            switch (gesture_state) {
            case 0:
                break;
            case 1:
                if (interval > 300) {
                    gesture_state = 2;
                }
                break;
            case 2:
                if (interval > 100) {
                    gesture_state = 0;
                }
                break;
            }
        }
    }

    void HandleMoveWakePressUp(int64_t current_time, int64_t &last_trigger_time, int &gesture_state)
    {
        int64_t interval = current_time - last_trigger_time;

        if (interval > 1000) {
            gesture_state = 0;
        } else {
            switch (gesture_state) {
            case 0:
                if (interval > 300) {
                    gesture_state = 1;
                }
                break;
            case 1:
                break;
            case 2:
                if (interval < 100) {
                    ESP_LOGI(TAG, "gesture detected");
                    gesture_state = 0;
                    auto &app = Application::GetInstance();
                    app.ToggleChatState();
                }
                break;
            }
        }
    }

    void InitializeButtons()
    {
        static int64_t last_trigger_time = 0;
        static int gesture_state = 0;  // 0: init, 1: wait second long interval, 2: wait oscillation

        boot_button_.OnClick([this]() {
            auto &app = Application::GetInstance();
            // During startup (before connected), pressing BOOT button enters Wi-Fi config mode without reboot
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        audio_wake_button_.OnPressDown([this]() {
        });

        audio_wake_button_.OnPressUp([this]() {
        });

        move_wake_button_.OnPressDown([this]() {
            int64_t current_time = esp_timer_get_time() / 1000;
            HandleMoveWakePressDown(current_time, last_trigger_time, gesture_state);
        });

        move_wake_button_.OnPressUp([this]() {
            int64_t current_time = esp_timer_get_time() / 1000;
            HandleMoveWakePressUp(current_time, last_trigger_time, gesture_state);
        });
    }
    
    void InitializeLed() {
        ESP_LOGI(TAG, "BLINK_GPIO setting %d", bsp_strip_config.strip_gpio_num);

        ESP_ERROR_CHECK(led_strip_new_rmt_device(&bsp_strip_config, &bsp_rmt_config, &led_strip_));
        led_strip_set_pixel(led_strip_, 0, 0x00, 0x00, 0x00);
        led_strip_set_pixel(led_strip_, 1, 0x00, 0x00, 0x00);
        led_strip_set_pixel(led_strip_, 2, 0x00, 0x00, 0x00);
        led_strip_set_pixel(led_strip_, 3, 0x00, 0x00, 0x00);
        led_strip_refresh(led_strip_);
    }

    esp_err_t SetLedColor(uint8_t r, uint8_t g, uint8_t b) {
        esp_err_t ret = ESP_OK;

        ret |= led_strip_set_pixel(led_strip_, 0, r, g, b);
        ret |= led_strip_set_pixel(led_strip_, 1, r, g, b);
        ret |= led_strip_set_pixel(led_strip_, 2, r, g, b);
        ret |= led_strip_set_pixel(led_strip_, 3, r, g, b);
        ret |= led_strip_refresh(led_strip_);
        return ret;
    }

    void InitializeIot()
    {
        ESP_LOGI(TAG, "Initialize Iot");
        InitializeLed();
        SetLedColor(0x00, 0x00, 0x00);

#ifdef CONFIG_ESP_HI_WEB_CONTROL_ENABLED
        esp_event_loop_create_default();
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED,
                                                 &wifi_event_handler, this));
#endif //CONFIG_ESP_HI_WEB_CONTROL_ENABLED
    }

    void InitializeSpi()
    {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        // Full-frame clear + emoji strips need room for one landscape row band.
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay()
    {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        static const ili9341_vendor_config_t vendor_config = {
            .init_cmds = vendor_specific_init,
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(ili9341_lcd_init_cmd_t),
        };

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void*)&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        // Software reset (RST is NC) needs extra settle time before init commands.
        vTaskDelay(pdMS_TO_TICKS(120));
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_set_gap(panel, 0, 24);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        ESP_LOGI(TAG, "LCD panel create success, %p", panel);

        esp_lcd_panel_disp_on_off(panel, true);

        // Clear residual GRAM so a failed assets load does not leave snow on screen.
        std::vector<uint16_t> black(DISPLAY_WIDTH * DISPLAY_HEIGHT, 0);
        esp_lcd_panel_draw_bitmap(panel, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, black.data());

        ESP_LOGI(TAG, "Create emoji widget, panel: %p, panel_io: %p", panel, panel_io);
        display_ = new anim::EmojiWidget(panel, panel_io);

#if CONFIG_ESP_CONSOLE_NONE
        servo_dog_ctrl_config_t config = {
            .fl_gpio_num = FL_GPIO_NUM,
            .fr_gpio_num = FR_GPIO_NUM,
            .bl_gpio_num = BL_GPIO_NUM,
            .br_gpio_num = BR_GPIO_NUM,
        };

        servo_dog_ctrl_init(&config);
#endif
    }

    void InitializeTools()
    {
        auto& mcp_server = McpServer::GetInstance();
        
        // 基础动作控制
        mcp_server.AddTool("self.dog.basic_control", "机器人的基础动作。机器人可以做以下基础动作：\n"
            "forward: 向前移动\nbackward: 向后移动\nturn_left: 向左转\nturn_right: 向右转\nstop: 立即停止当前动作", 
            PropertyList({
                Property("action", kPropertyTypeString),
            }), [this](const PropertyList& properties) -> ReturnValue {
                const std::string& action = properties["action"].value<std::string>();
                if (action == "forward") {
                    servo_dog_ctrl_send(DOG_STATE_FORWARD, NULL);
                } else if (action == "backward") {
                    servo_dog_ctrl_send(DOG_STATE_BACKWARD, NULL);
                } else if (action == "turn_left") {
                    servo_dog_ctrl_send(DOG_STATE_TURN_LEFT, NULL);
                } else if (action == "turn_right") {
                    servo_dog_ctrl_send(DOG_STATE_TURN_RIGHT, NULL);
                } else if (action == "stop") {
                    servo_dog_ctrl_send(DOG_STATE_IDLE, NULL);
                } else {
                    return false;
                }
                return true;
            });
        
        // 扩展动作控制
        mcp_server.AddTool("self.dog.advanced_control", "机器人的扩展动作。机器人可以做以下扩展动作：\n"
            "sway_back_forth: 前后摇摆\nlay_down: 趴下\nsway: 左右摇摆\nretract_legs: 收回腿部\n"
            "shake_hand: 握手\nshake_back_legs: 伸懒腰\njump_forward: 向前跳跃", 
            PropertyList({
                Property("action", kPropertyTypeString),
            }), [this](const PropertyList& properties) -> ReturnValue {
                const std::string& action = properties["action"].value<std::string>();
                if (action == "sway_back_forth") {
                    servo_dog_ctrl_send(DOG_STATE_SWAY_BACK_FORTH, NULL);
                } else if (action == "lay_down") {
                    servo_dog_ctrl_send(DOG_STATE_LAY_DOWN, NULL);
                } else if (action == "sway") {
                    dog_action_args_t args = {
                        .repeat_count = 4,
                    };
                    servo_dog_ctrl_send(DOG_STATE_SWAY, &args);
                } else if (action == "retract_legs") {
                    servo_dog_ctrl_send(DOG_STATE_RETRACT_LEGS, NULL);
                } else if (action == "shake_hand") {
                    servo_dog_ctrl_send(DOG_STATE_SHAKE_HAND, NULL);
                } else if (action == "shake_back_legs") {
                    servo_dog_ctrl_send(DOG_STATE_SHAKE_BACK_LEGS, NULL);
                } else if (action == "jump_forward") {
                    servo_dog_ctrl_send(DOG_STATE_JUMP_FORWARD, NULL);
                } else {
                    return false;
                }
                return true;
            });

        // 灯光控制
        mcp_server.AddTool("self.light.get_power", "获取灯是否打开", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            return led_on_;
        });

        mcp_server.AddTool("self.light.turn_on", "打开灯", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            SetLedColor(0xFF, 0xFF, 0xFF);
            led_on_ = true;
            return true;
        });

        mcp_server.AddTool("self.light.turn_off", "关闭灯", PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
            SetLedColor(0x00, 0x00, 0x00);
            led_on_ = false;
            return true;
        });

        mcp_server.AddTool("self.light.set_rgb", "设置RGB颜色", PropertyList({
            Property("r", kPropertyTypeInteger, 0, 255),
            Property("g", kPropertyTypeInteger, 0, 255),
            Property("b", kPropertyTypeInteger, 0, 255)
        }), [this](const PropertyList& properties) -> ReturnValue {
            int r = properties["r"].value<int>();
            int g = properties["g"].value<int>();
            int b = properties["b"].value<int>();

            led_on_ = true;
            SetLedColor(r, g, b);
            return true;
        });
    }

public:
    EspHi() : boot_button_(BOOT_BUTTON_GPIO),
        audio_wake_button_(AUDIO_WAKE_BUTTON_GPIO),
        move_wake_button_(MOVE_WAKE_BUTTON_GPIO)
    {
#ifdef CONFIG_ESP_HI_WEB_CONTROL_ENABLED
        esp_log_set_vprintf(LogVprintf);
#endif
        InitializeButtons();
        InitializeIot();
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeTools();
    }

    virtual AudioCodec* GetAudioCodec() override
    {
        static AdcPdmAudioCodec audio_codec(
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_ADC_MIC_CHANNEL,
            AUDIO_PDM_SPEAK_P_GPIO,
            AUDIO_PDM_SPEAK_N_GPIO,
            AUDIO_PA_CTL_GPIO);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override
    {
        return display_;
    }

    // Peppe: in Wi-Fi configuration mode, show "Peppe v<version>" above the
    // eyes and the hotspot to join below them, next to the spoken prompt.
    // The application's own handling of every event is kept unchanged.
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override
    {
        WifiBoard::SetNetworkEventCallback(
            [this, callback = std::move(callback)](NetworkEvent event, const std::string& data) {
                if (event == NetworkEvent::WifiConfigModeEnter) {
                    // Events come from the Wi-Fi tasks; draw from the main task.
                    Application::GetInstance().Schedule([this]() {
                        std::string ssid = WifiManager::GetInstance().GetApSsid();
                        if (display_ != nullptr && !ssid.empty()) {
                            std::string title = "Peppe v";
                            title += esp_app_get_description()->version;
                            display_->ShowWifiConfig(title, ssid);
                        }
                    });
                } else if (event == NetworkEvent::WifiConfigModeExit ||
                           event == NetworkEvent::Connected) {
                    Application::GetInstance().Schedule([this]() {
                        if (display_ != nullptr) {
                            display_->HideWifiConfig();
                        }
                    });
                }
                if (callback) {
                    callback(event, data);
                }
            });
    }
};

DECLARE_BOARD(EspHi);

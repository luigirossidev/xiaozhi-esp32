#include "rome_time.h"

#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include <atomic>
#include <cstdint>
#include <ctime>

namespace rome_time {

namespace {

const char* TAG = "RomeTime";

std::atomic<bool> started{false};
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
bool synced = false;
int64_t utc_minus_timer_us = 0;  // UTC microseconds minus esp_timer microseconds

void OnSync(struct timeval* tv)
{
    int64_t utc_us = static_cast<int64_t>(tv->tv_sec) * 1000000 + tv->tv_usec;
    int64_t offset = utc_us - esp_timer_get_time();
    portENTER_CRITICAL(&lock);
    utc_minus_timer_us = offset;
    synced = true;
    portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "Time synchronised");
}

// Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm).
int64_t DaysFromCivil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int yoe = static_cast<int>(y - era * 400);
    const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// UTC seconds of 01:00 UTC on the last Sunday of the month: when EU clocks change.
int64_t LastSundayAt1Utc(int year, int month)
{
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int last = kDays[month - 1];  // only March and October are used: no leap day
    int64_t days = DaysFromCivil(year, month, last);
    int weekday = static_cast<int>((days + 4) % 7);  // 1970-01-01 was a Thursday; 0 = Sunday
    return (days - weekday) * 86400 + 3600;
}

}  // namespace

void StartSync()
{
    if (started.exchange(true)) {
        return;
    }
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.sync_cb = OnSync;
    config.wait_for_sync = false;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start SNTP: %s", esp_err_to_name(err));
        started = false;
    }
}

void Now(clock_face::Time& time)
{
    portENTER_CRITICAL(&lock);
    bool have = synced;
    int64_t offset = utc_minus_timer_us;
    portEXIT_CRITICAL(&lock);
    time.valid = have;
    if (!have) {
        return;
    }
    int64_t utc = (esp_timer_get_time() + offset) / 1000000;

    struct tm utc_tm;
    time_t utc_t = static_cast<time_t>(utc);
    gmtime_r(&utc_t, &utc_tm);
    int year = utc_tm.tm_year + 1900;
    bool summer = utc >= LastSundayAt1Utc(year, 3) && utc < LastSundayAt1Utc(year, 10);

    time_t local_t = static_cast<time_t>(utc + (summer ? 2 : 1) * 3600);
    struct tm local;
    gmtime_r(&local_t, &local);
    time.hour = local.tm_hour;
    time.minute = local.tm_min;
    time.second = local.tm_sec;
    time.weekday = local.tm_wday;
    time.day = local.tm_mday;
    time.month = local.tm_mon + 1;
    time.year = local.tm_year + 1900;
}

}  // namespace rome_time

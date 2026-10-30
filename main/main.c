/*
 * C6 SDIO OTA — CrowPanel ESP32-P4
 *
 * Upgrades ESP32-C6 co-processor firmware via SDIO OTA.
 * Host starts no WiFi — isolates SDIO transport from WiFi contention.
 *
 * Strategic logging tags:
 *   [PHASE]  — phase transitions (grep for experiment flow)
 *   [DIAG]   — diagnostic measurements (timing, versions, sizes)
 *   [PASS]   — success indicators
 *   [FAIL]   — failure indicators with error codes
 *   [WARN]   — non-fatal anomalies
 */

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_hosted.h"

/* These headers exist in the stock OTA example but may not be public API.
 * If compilation fails here, check the actual esp_hosted v2.9.7 header layout:
 *   find managed_components -name "*.h" | grep hosted
 * Then update includes accordingly. */
#if __has_include("esp_hosted_ota.h")
#include "esp_hosted_ota.h"
#else
/* OTA functions may be declared in esp_hosted.h directly */
#endif

#if __has_include("esp_hosted_api_types.h")
#include "esp_hosted_api_types.h"
#else
/* Version struct may be in esp_hosted.h */
#endif

#include "esp_timer.h"
#include "esp_app_desc.h"
#include "ota_littlefs.h"

static const char *TAG = "c6-sdio-ota";

/* Millisecond timestamp since boot */
static int64_t ms_since_boot(void) {
    return esp_timer_get_time() / 1000;
}

/* Phase 1: Query C6 firmware version
 * Returns true if C6 already runs v2.9.7 (OTA not needed), false otherwise. */
static bool phase_query_version(void) {
    ESP_LOGW(TAG, "[PHASE] 1/5 VERSION-QUERY start t=%" PRId64 "ms", ms_since_boot());

    esp_hosted_coprocessor_fwver_t ver = {0};
    esp_err_t ret = esp_hosted_get_coprocessor_fwversion(&ver);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "[DIAG] C6 firmware: %" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 ver.major1, ver.minor1, ver.patch1);
        if (ver.major1 == 2 && ver.minor1 == 9 && ver.patch1 == 7) {
            ESP_LOGI(TAG, "[PASS] C6 already at v2.9.7 — OTA not needed");
            return true;
        }
        ESP_LOGI(TAG, "[DIAG] C6 needs upgrade from %" PRIu32 ".%" PRIu32 ".%" PRIu32 " to 2.9.7",
                 ver.major1, ver.minor1, ver.patch1);
    } else {
        ESP_LOGW(TAG, "[WARN] Version query failed: %s (0x%x) — expected for v2.3.0 (RPC timeout)",
                 esp_err_to_name(ret), ret);
        ESP_LOGI(TAG, "[DIAG] Proceeding with OTA despite version query failure");
    }
    return false;
}

/* Phase 2: Perform OTA via LittleFS */
static int phase_ota_transfer(void) {
    ESP_LOGW(TAG, "[PHASE] 2/5 OTA-TRANSFER start t=%" PRId64 "ms", ms_since_boot());

    int64_t ota_start = ms_since_boot();
    uint8_t delete_after = 0;  /* keep binary for retry */
    int ret = ota_littlefs_perform(delete_after);
    int64_t ota_end_time = ms_since_boot();

    int64_t duration = ota_end_time - ota_start;

    if (ret == ESP_HOSTED_SLAVE_OTA_COMPLETED) {
        ESP_LOGI(TAG, "[PASS] OTA transfer completed in %" PRId64 "ms", duration);
    } else if (ret == ESP_HOSTED_SLAVE_OTA_NOT_REQUIRED) {
        ESP_LOGI(TAG, "[PASS] OTA not required (slave firmware matches)");
    } else {
        ESP_LOGE(TAG, "[FAIL] OTA transfer failed: %s (0x%x) after %" PRId64 "ms",
                 esp_err_to_name(ret), ret, duration);
        ESP_LOGE(TAG, "[FAIL] SDIO transport may have died during transfer");
        ESP_LOGE(TAG, "[DIAG] If duration < 5000ms: transport init or begin failed");
        ESP_LOGE(TAG, "[DIAG] If duration 5000-30000ms: transport died mid-transfer (v2.3.0 bug)");
        ESP_LOGE(TAG, "[DIAG] If duration > 30000ms: timeout waiting for slave response");
    }

    return ret;
}

/* Phase 3: Activate new firmware on C6 */
static void phase_activate(void) {
    ESP_LOGW(TAG, "[PHASE] 3/5 OTA-ACTIVATE start t=%" PRId64 "ms", ms_since_boot());

    esp_err_t ret = esp_hosted_slave_ota_activate();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "[PASS] Activate succeeded — C6 will boot new firmware after reset");
    } else {
        ESP_LOGE(TAG, "[FAIL] Activate failed: %s (0x%x)", esp_err_to_name(ret), ret);
        ESP_LOGE(TAG, "[DIAG] Activate failure means OTA image was written but not marked bootable");
        ESP_LOGE(TAG, "[DIAG] C6 will continue booting old firmware on next reset");
    }
}

/* Phase 4: Verify — reset C6 and query version again */
static void phase_verify(void) {
    ESP_LOGW(TAG, "[PHASE] 4/5 VERIFY start t=%" PRId64 "ms", ms_since_boot());
    ESP_LOGI(TAG, "[DIAG] Waiting 3s for C6 reboot...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    esp_hosted_coprocessor_fwver_t ver = {0};
    esp_err_t ret = esp_hosted_get_coprocessor_fwversion(&ver);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "[DIAG] C6 version after OTA: %" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 ver.major1, ver.minor1, ver.patch1);
        if (ver.major1 == 2 && ver.minor1 == 9 && ver.patch1 == 7) {
            ESP_LOGI(TAG, "[PASS] *** C6 UPGRADED TO v2.9.7 — SUCCESS ***");
        } else {
            ESP_LOGW(TAG, "[WARN] Version changed but not to 2.9.7 — unexpected");
        }
    } else {
        ESP_LOGW(TAG, "[WARN] Post-OTA version query failed: %s (0x%x)", esp_err_to_name(ret), ret);
        ESP_LOGI(TAG, "[DIAG] This may be normal if C6 is still rebooting");
        ESP_LOGI(TAG, "[DIAG] Reflash normal firmware and check WiFi behavior to confirm upgrade");
    }
}

void app_main(void)
{
    ESP_LOGW(TAG, "==========================================================");
    ESP_LOGW(TAG, "  C6 SDIO OTA — CrowPanel ESP32-P4");
    ESP_LOGW(TAG, "  Target: ESP32-C6 upgrade via SDIO");
    ESP_LOGW(TAG, "  Host WiFi: DISABLED (SDIO transport only)");
    ESP_LOGW(TAG, "==========================================================");

    /* Phase 0: Init */
    ESP_LOGW(TAG, "[PHASE] 0/5 INIT start t=%" PRId64 "ms", ms_since_boot());

    esp_err_t ret;

    ret = nvs_flash_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] NVS init failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "[DIAG] NVS initialized");

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] Event loop creation failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "[DIAG] Initializing esp_hosted SDIO transport (no WiFi)...");
    int64_t hosted_start = ms_since_boot();
    ret = esp_hosted_init();
    int64_t hosted_init_time = ms_since_boot() - hosted_start;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] esp_hosted_init failed: %s (0x%x) after %" PRId64 "ms",
                 esp_err_to_name(ret), ret, hosted_init_time);
        ESP_LOGE(TAG, "[FAIL] SDIO transport cannot initialize — all OTA paths blocked");
        ESP_LOGE(TAG, "[DIAG] Check: C6 powered? Reset pin GPIO%d toggling? SDIO pins correct?",
                 CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE);
        return;
    }
    ESP_LOGI(TAG, "[PASS] esp_hosted_init OK in %" PRId64 "ms", hosted_init_time);

    /* esp_hosted_connect_to_slave() is in the stock OTA example.
     * If it doesn't exist in v2.9.7, esp_hosted_init() may handle
     * the SDIO handshake. Remove this call if build fails. */
    ESP_LOGI(TAG, "[DIAG] Connecting to C6 slave...");
    int64_t connect_start = ms_since_boot();
    ret = esp_hosted_connect_to_slave();
    int64_t connect_time = ms_since_boot() - connect_start;
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[FAIL] esp_hosted_connect_to_slave failed: %s (0x%x) after %" PRId64 "ms",
                 esp_err_to_name(ret), ret, connect_time);
        ESP_LOGE(TAG, "[FAIL] SDIO handshake with C6 failed — transport broken");
        return;
    }
    ESP_LOGI(TAG, "[PASS] Connected to C6 slave in %" PRId64 "ms", connect_time);
    ESP_LOGW(TAG, "[PHASE] 0/5 INIT complete t=%" PRId64 "ms", ms_since_boot());

    /* Phase 1: Version query */
    bool already_upgraded = phase_query_version();
    if (already_upgraded) {
        ESP_LOGW(TAG, "[PHASE] 5/5 SUMMARY t=%" PRId64 "ms", ms_since_boot());
        ESP_LOGW(TAG, "  RESULT: C6 already at v2.9.7 — no OTA needed");
        goto halt;
    }

    /* Phase 2: OTA transfer */
    int ota_result = phase_ota_transfer();

    if (ota_result == ESP_HOSTED_SLAVE_OTA_COMPLETED) {
        /* Phase 3: Activate */
        phase_activate();

        /* Phase 4: Verify */
        phase_verify();
    } else if (ota_result != ESP_HOSTED_SLAVE_OTA_NOT_REQUIRED) {
        ESP_LOGE(TAG, "[FAIL] Skipping activate/verify due to OTA transfer failure");
    }

    /* Phase 5: Summary */
    ESP_LOGW(TAG, "[PHASE] 5/5 SUMMARY t=%" PRId64 "ms", ms_since_boot());
    ESP_LOGW(TAG, "==========================================================");
    if (ota_result == ESP_HOSTED_SLAVE_OTA_COMPLETED) {
        ESP_LOGW(TAG, "  RESULT: OTA TRANSFER SUCCEEDED");
        ESP_LOGW(TAG, "  Next: Restore normal firmware and check WiFi stability");
    } else if (ota_result == ESP_HOSTED_SLAVE_OTA_NOT_REQUIRED) {
        ESP_LOGW(TAG, "  RESULT: OTA NOT REQUIRED (already up to date)");
    } else {
        ESP_LOGW(TAG, "  RESULT: OTA FAILED");
        ESP_LOGW(TAG, "  Next: Analyze [FAIL] and [DIAG] lines above");
    }
    ESP_LOGW(TAG, "==========================================================");
    ESP_LOGW(TAG, "  Halting. Reset board to run again.");

halt:
    /* Spin forever — don't restart, let user read logs */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/*
 * PCLK Deep Diagnostic
 * =====================
 * Tests whether GPIO13 is electrically dead or just not receiving PCLK.
 *
 * Test 1: GPIO13 output toggle — proves the pin's output driver works
 * Test 2: GPIO13 input from known source — proves input buffer works
 *         (connects GPIO44/D7 as output → read on GPIO13 with a jumper wire)
 * Test 3: OV3660 register dump — checks if camera is configured to output PCLK
 * Test 4: Multiple camera reinit with PCLK sampling — checks if init order matters
 * Test 5: Brute-force GPIO13 reset — clear all GPIO config and retry camera
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "driver/gpio.h"
#include "soc/gpio_reg.h"
#include "soc/io_mux_reg.h"
#include "helios.h"

static const char *TAG = "PCLK_DIAG";

// --- Test 1: GPIO13 Output Toggle ---
static void test_gpio13_output(void)
{
    printf("\n── TEST 1: GPIO13 as OUTPUT ──\n");
    printf("  Testing if GPIO13 can be driven by the ESP32...\n");

    // Configure as output
    gpio_reset_pin(13);
    gpio_set_direction(13, GPIO_MODE_OUTPUT);

    // Toggle and read back
    int pass = 1;
    for (int val = 0; val < 2; val++) {
        gpio_set_level(13, val);
        vTaskDelay(pdMS_TO_TICKS(10));
        // Read the output level via the register
        uint32_t out_reg = REG_READ(GPIO_OUT_REG);
        int read_val = (out_reg >> 13) & 1;
        printf("  Set %d → Read back %d %s\n", val, read_val, read_val == val ? "OK" : "FAIL");
        if (read_val != val) pass = 0;
    }

    gpio_reset_pin(13);
    printf("  RESULT: %s\n", pass ? "GPIO13 output driver WORKS" : "GPIO13 output BROKEN");
}

// --- Test 2: GPIO13 Input (requires jumper from GPIO44/D7 to GPIO13) ---
static void test_gpio13_input(void)
{
    printf("\n── TEST 2: GPIO13 as INPUT ──\n");
    printf("  NOTE: Connect a jumper wire from D7 (GPIO44) to the PCLK test point\n");
    printf("        or to the GPIO13 pin on the XIAO header.\n");
    printf("        If no jumper, this test will show 'no signal'.\n\n");

    // Configure GPIO44 as output (drive signal)
    gpio_reset_pin(44);
    gpio_set_direction(44, GPIO_MODE_OUTPUT);

    // Configure GPIO13 as input
    gpio_reset_pin(13);
    gpio_set_direction(13, GPIO_MODE_INPUT);
    gpio_set_pull_mode(13, GPIO_FLOATING);

    int match = 0, total = 0;
    for (int val = 0; val < 2; val++) {
        gpio_set_level(44, val);
        vTaskDelay(pdMS_TO_TICKS(50));
        int read = gpio_get_level(13);
        printf("  GPIO44 = %d → GPIO13 reads %d %s\n", val, read, read == val ? "MATCH" : "MISMATCH");
        total++;
        if (read == val) match++;
    }

    // Fast toggle test
    int transitions = 0;
    int last = gpio_get_level(13);
    for (int i = 0; i < 1000; i++) {
        gpio_set_level(44, i & 1);
        int cur = gpio_get_level(13);
        if (cur != last) { transitions++; last = cur; }
    }
    printf("  Fast toggle: %d transitions seen on GPIO13 (expect ~1000 with jumper)\n", transitions);

    gpio_reset_pin(44);
    gpio_reset_pin(13);

    if (match == total && transitions > 500) {
        printf("  RESULT: GPIO13 input WORKS — pin is electrically good!\n");
        printf("  → The PCLK failure is on the CAMERA side (B2B connector or sensor)\n");
    } else if (transitions > 100) {
        printf("  RESULT: GPIO13 input PARTIALLY works (%d transitions)\n", transitions);
    } else {
        printf("  RESULT: No signal on GPIO13 (no jumper connected, or pin is dead)\n");
    }
}

// --- Test 3: OV3660 Register Check ---
static void test_ov3660_registers(void)
{
    printf("\n── TEST 3: OV3660 Register State ──\n");

    camera_config_t cfg = {
        .pin_pwdn = -1, .pin_reset = -1, .pin_xclk = 10,
        .pin_sccb_sda = 40, .pin_sccb_scl = 39,
        .pin_d7 = 48, .pin_d6 = 11, .pin_d5 = 12, .pin_d4 = 14,
        .pin_d3 = 16, .pin_d2 = 18, .pin_d1 = 17, .pin_d0 = 15,
        .pin_vsync = 38, .pin_href = 47, .pin_pclk = 13,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = FRAMESIZE_QQVGA,
        .jpeg_quality = 12, .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&cfg);
    printf("  Camera init: %s\n", err == ESP_OK ? "OK" : "FAIL");

    if (err != ESP_OK) {
        printf("  RESULT: Cannot read registers (camera init failed)\n");
        return;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        printf("  RESULT: No sensor handle\n");
        esp_camera_deinit();
        return;
    }

    printf("  Sensor PID: 0x%04x\n", s->id.PID);

    // Read key OV3660 registers related to PCLK output
    // These are common OV registers — the exact addresses depend on the sensor
    // OV3660 System Control:
    //   0x3008 = System control (bit 7 = software reset, bit 6 = software power down)
    //   0x3017 = Pad output enable 01 (controls VSYNC, HREF, PCLK output enables)
    //   0x3018 = Pad output enable 02 (controls D0-D7 output enables)
    //   0x3034 = SC PLL Control 0 (PLL settings)
    //   0x3035 = SC PLL Control 1
    //   0x3036 = SC PLL Control 2
    //   0x3037 = SC PLL Control 3
    //   0x3108 = PCLK root divider, SCLK2x root divider

    // Use the sensor's SCCB (I2C) interface to read registers
    // The esp_camera driver exposes this through sensor->get_reg
    if (s->get_reg) {
        printf("\n  Key registers:\n");

        int reg_3008 = s->get_reg(s, 0x3008, 0xFF);
        printf("    0x3008 (sys ctrl):     0x%02x", reg_3008);
        if (reg_3008 >= 0) {
            printf(" [reset=%d, pwrdn=%d]", (reg_3008 >> 7) & 1, (reg_3008 >> 6) & 1);
            if (reg_3008 & 0x40) printf(" *** POWERED DOWN! ***");
        }
        printf("\n");

        int reg_3017 = s->get_reg(s, 0x3017, 0xFF);
        printf("    0x3017 (pad output 1): 0x%02x", reg_3017);
        if (reg_3017 >= 0) {
            printf(" [VSYNC_out=%d, HREF_out=%d, PCLK_out=%d]",
                   (reg_3017 >> 1) & 1, (reg_3017 >> 3) & 1, (reg_3017 >> 5) & 1);
            if (!((reg_3017 >> 5) & 1)) printf(" *** PCLK OUTPUT DISABLED! ***");
        }
        printf("\n");

        int reg_3018 = s->get_reg(s, 0x3018, 0xFF);
        printf("    0x3018 (pad output 2): 0x%02x", reg_3018);
        if (reg_3018 >= 0) {
            printf(" [data pins enabled: 0x%02x]", reg_3018);
        }
        printf("\n");

        int reg_3034 = s->get_reg(s, 0x3034, 0xFF);
        printf("    0x3034 (PLL ctrl 0):   0x%02x\n", reg_3034);

        int reg_3035 = s->get_reg(s, 0x3035, 0xFF);
        printf("    0x3035 (PLL ctrl 1):   0x%02x\n", reg_3035);

        int reg_3036 = s->get_reg(s, 0x3036, 0xFF);
        printf("    0x3036 (PLL ctrl 2):   0x%02x\n", reg_3036);

        int reg_3108 = s->get_reg(s, 0x3108, 0xFF);
        printf("    0x3108 (PCLK div):     0x%02x\n", reg_3108);

        // Try forcing PCLK output enable
        printf("\n  Attempting to force PCLK output enable...\n");
        if (s->set_reg && reg_3017 >= 0) {
            int new_3017 = reg_3017 | 0x20;  // set bit 5 = PCLK output enable
            s->set_reg(s, 0x3017, 0xFF, new_3017);
            vTaskDelay(pdMS_TO_TICKS(100));
            int verify = s->get_reg(s, 0x3017, 0xFF);
            printf("    Wrote 0x%02x, readback: 0x%02x\n", new_3017, verify);
        }
    } else {
        printf("  get_reg not available for this sensor\n");
    }

    // Sample PCLK after register force
    vTaskDelay(pdMS_TO_TICKS(500));
    int pclk_trans = 0;
    int last = gpio_get_level(13);
    uint32_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(200)) {
        int cur = gpio_get_level(13);
        if (cur != last) { pclk_trans++; last = cur; }
    }
    printf("  PCLK after register force: %d transitions %s\n",
           pclk_trans, pclk_trans > 0 ? "ALIVE!" : "still dead");

    // Try a capture
    if (pclk_trans > 0) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            printf("  CAPTURE: OK! %zu bytes %dx%d\n", fb->len, fb->width, fb->height);
            esp_camera_fb_return(fb);
        } else {
            printf("  CAPTURE: TIMEOUT\n");
        }
    }

    esp_camera_deinit();
}

// --- Test 4: GPIO13 Matrix Check ---
static void test_gpio_matrix(void)
{
    printf("\n── TEST 4: GPIO Matrix Configuration ──\n");

    // Check what GPIO13 is currently routed to in the GPIO matrix
    uint32_t func_sel = REG_READ(IO_MUX_GPIO13_REG);
    printf("  IO_MUX_GPIO13_REG: 0x%08lx\n", (unsigned long)func_sel);
    printf("    MCU_SEL (func): %ld\n", (func_sel >> 12) & 0x7);
    printf("    FUN_IE (input enable): %ld\n", (func_sel >> 9) & 0x1);
    printf("    FUN_PU (pull-up): %ld\n", (func_sel >> 8) & 0x1);
    printf("    FUN_PD (pull-down): %ld\n", (func_sel >> 7) & 0x1);

    // Check GPIO input signal routing
    // The camera peripheral's PCLK input should be routed from GPIO13
    // via the GPIO matrix. Signal 44 is typically CAM_PCLK.
    printf("\n  After camera init, GPIO13 should be routed to camera PCLK input\n");
    printf("  If the function select is wrong, the camera DMA won't see PCLK\n");
}

// --- Test 5: Full Power Cycle Reset ---
static void test_full_reset(void)
{
    printf("\n── TEST 5: Full GPIO Reset + Camera Reinit ──\n");

    // Reset GPIO13 completely
    gpio_reset_pin(13);
    // Clear any pull-up/down
    gpio_set_pull_mode(13, GPIO_FLOATING);
    // Reset to default function
    gpio_set_direction(13, GPIO_MODE_INPUT);

    vTaskDelay(pdMS_TO_TICKS(500));

    // Now reinit camera — the driver should reconfigure GPIO13 for PCLK
    camera_config_t cfg = {
        .pin_pwdn = -1, .pin_reset = -1, .pin_xclk = 10,
        .pin_sccb_sda = 40, .pin_sccb_scl = 39,
        .pin_d7 = 48, .pin_d6 = 11, .pin_d5 = 12, .pin_d4 = 14,
        .pin_d3 = 16, .pin_d2 = 18, .pin_d1 = 17, .pin_d0 = 15,
        .pin_vsync = 38, .pin_href = 47, .pin_pclk = 13,
        .xclk_freq_hz = 20000000,
        .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = FRAMESIZE_QQVGA,
        .jpeg_quality = 12, .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&cfg);
    printf("  Camera reinit: %s\n", err == ESP_OK ? "OK" : "FAIL");

    if (err == ESP_OK) {
        sensor_t *s = esp_camera_sensor_get();
        if (s && s->id.PID == 0x3660) {
            s->set_vflip(s, 1);
            s->set_brightness(s, 1);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));

        // Sample PCLK
        int pclk_trans = 0;
        int last = gpio_get_level(13);
        uint32_t start = xTaskGetTickCount();
        while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(200)) {
            int cur = gpio_get_level(13);
            if (cur != last) { pclk_trans++; last = cur; }
        }
        printf("  PCLK: %d transitions %s\n", pclk_trans, pclk_trans > 0 ? "ALIVE!" : "dead");

        if (pclk_trans > 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) {
                printf("  CAPTURE: OK! %zu bytes %dx%d\n", fb->len, fb->width, fb->height);
                esp_camera_fb_return(fb);
            } else {
                printf("  CAPTURE: TIMEOUT\n");
            }
        }
        esp_camera_deinit();
    }
}

void app_main(void)
{
    // Wait 5 seconds so serial monitor can connect
    printf("PCLK diagnostic starting in 5 seconds...\n");
    vTaskDelay(pdMS_TO_TICKS(5000));

    printf("\nPCLK Deep Diagnostic\n");
    printf("====================\n\n");
    fflush(stdout);

    test_gpio13_output();
    fflush(stdout);
    test_gpio13_input();
    fflush(stdout);
    test_ov3660_registers();
    fflush(stdout);
    test_gpio_matrix();
    fflush(stdout);
    test_full_reset();
    fflush(stdout);

    printf("\n====================\n");
    printf("DIAGNOSTIC COMPLETE\n");
    printf("====================\n");
    fflush(stdout);

    while (1) vTaskDelay(pdMS_TO_TICKS(10000));
}

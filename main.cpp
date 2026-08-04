#include <stdio.h>
#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "driver/ppa.h"

// ESP-IDF 및 Waveshare BSP 헤더
#include "bsp/esp-bsp.h"          // Waveshare ESP32-P4 BSP
#include "bsp/display.h"
#include "bsp/touch.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_hx8394.h"       // HX8394 MIPI-DSI Driver Component
#include "esp_lcd_touch_gt911.h"  // GT911 Touch Driver

#include "Jet.hpp"
using namespace Renderer;

#define LCD_H_RES          720
#define LCD_V_RES          1280
#define LCD_BIT_PER_PIXEL  16 // RGB565

#define LCD_BUFFER_SIZE    (LCD_H_RES * LCD_V_RES * sizeof(uint16_t))
#define DEPTH_BUFFER_SIZE  (LCD_BUFFER_SIZE)

static const char *TAG = "P4_Jet_3D";

// 글로벌 프레임버퍼 & 뎁스버퍼 (P4의 L2MEM 또는 Internal/External PSRAM)
uint16_t *fb[2] = {NULL, NULL};
uint16_t *depth = NULL;
int draw_idx = 0;

// LCD 드라이버 및 터치 핸들
esp_lcd_panel_handle_t panel_handle = NULL;
esp_lcd_touch_handle_t touch_handle = NULL;

// 더블 버퍼링 동기화용 세마포어
StaticSemaphore_t lcd_sem_buffer;
volatile SemaphoreHandle_t lcd_trans_done_sem = NULL;

// DMA/MIPI-DSI 전송 완료 콜백 함수
static bool on_vsync_event(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    BaseType_t need_yield = pdFALSE;
    if (lcd_trans_done_sem) {
        xSemaphoreGiveFromISR(lcd_trans_done_sem, &need_yield);
    }
    return need_yield == pdTRUE;
}

void jet_render_task(void *pvParameters);

#define PPA_BUFFER_ALIGNMENT 128
// PPA 클라이언트 핸들 (전역 변수)
static ppa_client_handle_t ppa_client = NULL;

esp_err_t init_ppa_engine(void)
{
    ppa_client_config_t ppa_cfg = {};
    ppa_cfg.oper_type = PPA_OPERATION_FILL;
    ppa_cfg.max_pending_trans_num = 2; // ★ 연속 2개의 Non-blocking Clear 허용!

    esp_err_t ret = ppa_register_client(&ppa_cfg, &ppa_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register PPA client: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "PPA Fast Fill Client initialized successfully.");
    return ESP_OK;
}

// 비동기(Non-blocking) 지원 ppa_clear_buffer
void ppa_clear_buffer(void *buf, uint16_t color_val, uint32_t width, uint32_t height, ppa_trans_mode_t mode)
{
    if (!ppa_client || !buf) return;

    size_t calc_buffer_size = width * height * sizeof(uint16_t);
    uint32_t fill_color32 = ((uint32_t)color_val << 16) | color_val;

    ppa_fill_oper_config_t fill_config = {};

    fill_config.out.buffer = buf;
    fill_config.out.buffer_size = calc_buffer_size;
    fill_config.out.pic_w = width;
    fill_config.out.pic_h = height;
    fill_config.out.fill_cm = PPA_FILL_COLOR_MODE_RGB565;

    fill_config.fill_block_w = width;
    fill_config.fill_block_h = height;
    fill_config.fill_color_val = fill_color32;
    fill_config.mode = mode; // ★ 매개변수로 전달된 blocking / non-blocking 설정

    esp_err_t err = ppa_do_fill(ppa_client, &fill_config);
    if (err != ESP_OK) {
        // 주소 및 크기 디버깅용
        ESP_LOGE(TAG, "PPA Fill Failed: %s (buf ptr: %p, size: %zu)", esp_err_to_name(err), buf, calc_buffer_size);
    }
}
#if CONFIG_BSP_LCD_TYPE_720_1280_5_INCH_A
enum mirrortype_t { MIRROR_NO, MIRROR_X, MIRROR_Y, MIRROR_XY };
static esp_err_t disp_mirror_hx8394(esp_lcd_panel_io_handle_t io, mirrortype_t mirrortype)
{
    uint8_t data;
    if      (mirrortype == MIRROR_NO) data = 0x00;
    else if (mirrortype == MIRROR_X ) data = 0x02;
    else if (mirrortype == MIRROR_Y ) data = 0x01;
    else if (mirrortype == MIRROR_XY) data = 0x03;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x36, (uint8_t[]){data,}, 1), TAG, "send command failed");
    return ESP_OK;
}
#endif

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Initializing via Waveshare BSP (bsp_display_new)...");


    // 1. DSI 전송 완료 대기용 세마포어 생성
    lcd_trans_done_sem = xSemaphoreCreateBinaryStatic(&lcd_sem_buffer);
    xSemaphoreGive(lcd_trans_done_sem);

    // 2. Waveshare BSP 함수로 LVGL 없이 순수 디스플레이 패널만 생성
    esp_lcd_panel_io_handle_t lcd_io = NULL;
    esp_err_t ret = bsp_display_new(NULL, &panel_handle, &lcd_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize display via BSP: %s", esp_err_to_name(ret));
        return;
    }

#if CONFIG_BSP_LCD_TYPE_720_1280_5_INCH_A
    ret = disp_mirror_hx8394(lcd_io, MIRROR_NO);
    //ret = disp_mirror_hx8394(lcd_io, MIRROR_X );
    //ret = disp_mirror_hx8394(lcd_io, MIRROR_Y );
    //ret = disp_mirror_hx8394(lcd_io, MIRROR_XY);
#endif

    bsp_display_backlight_on();

    // 3. VSYNC / DSI Frame Done 콜백 등록
    esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done = on_vsync_event,
        .on_refresh_done = NULL,
    };
    esp_lcd_dpi_panel_register_event_callbacks(panel_handle, &cbs, NULL);

    // 4. GT911 터치 드라이버 초기화 (BSP 터치 API 사용)
    // 4-1. I2C 버스 먼저 초기화
    ret = bsp_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C bus for touch: %s", esp_err_to_name(ret));
    } else {
        // 4-2. BSP 제공 기본 터치 설정 적용 (NULL 대신 기본 cfg 전송)
        //bsp_touch_config_t touch_cfg = {
            //.i2c_bus = bsp_i2c_get_handle(), // BSP에 생성된 I2C 버스 핸들 가져오기
        // 수정함:
        bsp_display_cfg_t lcd_cfg = {
            .touch_flags = {
            .swap_xy = 1,
            .mirror_x = 1,
            .mirror_y = 0
        }};

        // BSP 함수 버전에 따라 direct 호출 또는 bsp_touch_new(&touch_cfg, &touch_handle)
        ret = bsp_touch_new(&lcd_cfg, &touch_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Touch init failed: %s (Touch disabled)", esp_err_to_name(ret));
            touch_handle = NULL;
        } else {
            ESP_LOGI(TAG, "Touch controller (GT911) initialized successfully.");
        }
    }

    // 5. PSRAM 메모리에 프레임버퍼 및 Z-버퍼 할당
    fb[0] = (uint16_t *)heap_caps_aligned_alloc(PPA_BUFFER_ALIGNMENT, LCD_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    fb[1] = (uint16_t *)heap_caps_aligned_alloc(PPA_BUFFER_ALIGNMENT, LCD_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    depth = (uint16_t *)heap_caps_aligned_alloc(PPA_BUFFER_ALIGNMENT, DEPTH_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    //fb[0] = (uint16_t *)heap_caps_malloc(LCD_BUFFER_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    //fb[1] = (uint16_t *)heap_caps_malloc(LCD_BUFFER_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    //depth = (uint16_t *)heap_caps_malloc(DEPTH_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!fb[0] || !fb[1] || !depth) {
        ESP_LOGE(TAG, "Frame/Depth Buffer Allocation Failed!");
        return;
    }
    // 128의 배수인지 확인 (0이어야 정렬 성공)
    if (((uintptr_t)fb[0] % 128) != 0) {
        ESP_LOGE(TAG, "fb[0] address %p is NOT 128-byte aligned!", fb[draw_idx]);
    }
    if (((uintptr_t)fb[1] % 128) != 0) {
        ESP_LOGE(TAG, "fb[1] address %p is NOT 128-byte aligned!", fb[draw_idx]);
    }
    if (((uintptr_t)depth % 128) != 0) {
        ESP_LOGE(TAG, "depth address %p is NOT 128-byte aligned!", depth);
    }

    init_ppa_engine();

    // 6. Jet 3D 렌더링 태스크 생성 (Core 1)
    xTaskCreatePinnedToCore(
        jet_render_task,
        "jet_render_task",
        1024 * 16, // P4에서는 스택을 넉넉히 (16KB)
        NULL,
        5,
        NULL,
        1
    );

    ESP_LOGI(TAG, "App Main Complete. Task running on Core 1.");
}

void jet_render_task(void *pvParameters)
{
    uint32_t width = LCD_H_RES;
    uint32_t height = LCD_V_RES;

    uint32_t frame_count = 0;
    uint32_t last_time = (uint32_t)(esp_timer_get_time() / 1000);

    ESP_LOGI(TAG, "Jet Render Task Started on Core %d (%ldx%ld)", xPortGetCoreID(), width, height);

    // Jet Scene 설정
    Scene scene(fb[draw_idx], depth, (int32_t)width, (int32_t)height);
    scene.setBackcolor(0xFFFF); // White

    // ★ Jet 엔진의 소프트웨어(CPU) Clear 비활성화
    scene.setClearBuffer(false);
    //scene.setClearBuffer(true);

    Camera camera;
    camera.setPosition(0, 0, -800); // 720x1280에 맞게 거리 조정
    camera.setFOV((int32_t)75, (int32_t)width);
    camera.nearPlane = 16;
    camera.farPlane  = 8192;
    scene.setCamera(&camera);

    DirectionalLight sun(Vector3{45, 35, 0}, Color{255, 245, 220}, 220);
    AmbientLight     amb(Color{40, 48, 64});
    scene.setDirectionalLight(&sun);
    scene.setAmbientLight(&amb);

    Material red(0xF800);   // RGB565 Red
    Material green(0x07E0); // RGB565 Green
    Material blue(0x001F);  // RGB565 Blue
    //red.shadingMode = ShadingMode::GOURAUD;
    red.shadingMode = ShadingMode::FLAT;

    // 객체 크기를 720x1280 해상도 비례에 맞춰 확장
    Object* cube   = Primitives::createCube(200, 200, 200, &red);
    cube->setPosition(0, 0, 200);
    scene.addObject(cube);

    Object* plane  = Primitives::createPlane(1000, 1000, &green);
    plane->setPosition(0, 200, 0);
    scene.addObject(plane);

    Object* sphere = Primitives::createSphere(120, 12, &blue);
    sphere->setPosition(300, 0, 0);
    scene.addObject(sphere);

    // 터치 및 인터랙션 변수
    uint16_t touch_x[2], touch_y[2];
    uint8_t touch_cnt = 0;

    static int16_t prev_touch_x = 0, prev_touch_y = 0;
    static bool was_touched = false;
    static float prev_pinch_dist = 0.0f;
    static bool is_multi_touch = false;

    float cam_distance = 1000.0f;
    float cam_angle_x  = 0.0f;
    float cam_angle_y  = 0.0f;

    for (;;) {
        // ----------------------------------------------------
        // 1. GT911 터치 패드 읽기 (ESP-IDF Touch Component API)
        // ----------------------------------------------------
        esp_lcd_touch_read_data(touch_handle);
        touch_cnt = 0;
        bool pressed = esp_lcd_touch_get_coordinates(touch_handle, touch_x, touch_y, NULL, &touch_cnt, 2);

        if (pressed && touch_cnt > 0) {

            //for(int i = 0; i < touch_cnt; i++) {
            //    printf("id:%02d x:%4d y:%4d ", i, touch_x[i], touch_y[i]);
            //}
            //printf("\n");

            if (touch_cnt >= 2) { // 멀티터치 (Pinch-to-Zoom)
                float dx = (float)(touch_x[0] - touch_x[1]);
                float dy = (float)(touch_y[0] - touch_y[1]);
                float current_dist = sqrtf(dx * dx + dy * dy);

                if (is_multi_touch) {
                    float dist_diff = current_dist - prev_pinch_dist;
                    cam_distance -= dist_diff * 2.0f;
                    if (cam_distance < 500.0f)  cam_distance = 500.0f;
                    if (cam_distance > 4000.0f) cam_distance = 4000.0f;
                }
                prev_pinch_dist = current_dist;
                is_multi_touch = true;
                was_touched = false;
            } 
            else if (touch_cnt == 1) { // 싱글터치 (회전)
                if (is_multi_touch) { is_multi_touch = false; was_touched = false; }

                if (was_touched) {
                    int16_t dx = touch_x[0] - prev_touch_x;
                    int16_t dy = touch_y[0] - prev_touch_y;

                    cam_angle_x += dy * 0.008f;
                    cam_angle_y += dx * 0.008f;

                    if (cam_angle_y > 1.4f)  cam_angle_y = 1.4f;
                    if (cam_angle_y < -1.4f) cam_angle_y = -1.4f;
                }
                prev_touch_x = touch_x[0];
                prev_touch_y = touch_y[0];
                was_touched = true;
            }
        } else {
            was_touched = false;
            is_multi_touch = false;
        }

        // ----------------------------------------------------
        // 2. 3D 카메라 & 오브젝트 회전 계산
        // ----------------------------------------------------
        int32_t cam_x = (int32_t)(cam_distance * sinf(cam_angle_x) * cosf(cam_angle_y));
        int32_t cam_y = (int32_t)(cam_distance * sinf(cam_angle_y));
        int32_t cam_z = (int32_t)(-cam_distance * cosf(cam_angle_x) * cosf(cam_angle_y));
        camera.setPosition(cam_x, cam_y, cam_z);

        cube->rotate(2, 2, 0);
        sphere->rotate(0, 2, 2);
        plane->rotate(2, 0, 2);

        int64_t t0 = esp_timer_get_time();
        // PPA 하드웨어로 백 버퍼 및 Z-버퍼 Clear (CPU 사용률 0%)
        ppa_clear_buffer(fb[draw_idx], 0xFFFF, width, height, PPA_TRANS_MODE_NON_BLOCKING);
        ppa_clear_buffer(depth, 0x0000, width, height, PPA_TRANS_MODE_BLOCKING);
        int64_t t1 = esp_timer_get_time();

        // ----------------------------------------------------
        // 3. 백 버퍼에 3D 렌더링 (CPU 연산)
        // ----------------------------------------------------
        scene.render();
        int64_t t2 = esp_timer_get_time();

        // ----------------------------------------------------
        // 4. 더블 버퍼링 동기화 (비동기 DMA / MIPI DSI 전송)
        // ----------------------------------------------------
        // 이전 프레임의 VSYNC/전송 완료를 대기
        xSemaphoreTake(lcd_trans_done_sem, portMAX_DELAY);
        int64_t t3 = esp_timer_get_time();

        // 완성된 버퍼를 DSI panel로 전송
        esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, width, height, fb[draw_idx]);

        // 핑퐁 버퍼 교체
        draw_idx = 1 - draw_idx;
        scene.setFramebuffer(fb[draw_idx]);

        // ----------------------------------------------------
        // 5. FPS 측정
        // ----------------------------------------------------
        frame_count++;
        uint32_t current_time = (uint32_t)(esp_timer_get_time() / 1000);
        if (current_time - last_time >= 1000) {
            float clear_ms  = (t1 - t0) / 1000.0f;
            float render_ms = (t2 - t1) / 1000.0f;
            float vsync_ms  = (t3 - t2) / 1000.0f;
            float total_ms  = (t3 - t0) / 1000.0f;

            ESP_LOGI("P4_PERF", "FPS: %lu | Clear: %.1fms | Render: %.1fms | VSYNC Wait: %.1fms | Total: %.1fms",
                     frame_count, clear_ms, render_ms, vsync_ms, total_ms);
            frame_count = 0;
            last_time = current_time;
        }
    }
}

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event_loop.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cam.h"
#include "ov2640.h"
#include "lcd.h"
#include "jpeg.h"
#include "esp_lua.h"
#include "esp_lua_lib.h"

static const char *TAG = "main";

#define JPEG_MODE 0
#define DEBUG 0

#define CAM_WIDTH   (320)
#define CAM_HIGH    (240)

#define LCD_CLK   GPIO_NUM_15
#define LCD_MOSI  GPIO_NUM_9
#define LCD_DC    GPIO_NUM_13
#define LCD_RST   GPIO_NUM_16
#define LCD_CS    GPIO_NUM_11
#define LCD_BK    GPIO_NUM_6

#define CAM_XCLK  GPIO_NUM_1
#define CAM_PCLK  GPIO_NUM_0
#define CAM_VSYNC GPIO_NUM_2
#define CAM_HSYNC GPIO_NUM_3

#define CAM_D0    GPIO_NUM_46
#define CAM_D1    GPIO_NUM_45
#define CAM_D2    GPIO_NUM_41
#define CAM_D3    GPIO_NUM_42
#define CAM_D4    GPIO_NUM_39
#define CAM_D5    GPIO_NUM_40
#define CAM_D6    GPIO_NUM_21
#define CAM_D7    GPIO_NUM_38

static void cam_task(void *arg)
{
    lcd_config_t lcd_config = {
        .clk_fre = 80 * 1000 * 1000,
        .pin_clk = LCD_CLK,
        .pin_mosi = LCD_MOSI,
        .pin_dc = LCD_DC,
        .pin_cs = LCD_CS,
        .pin_rst = LCD_RST,
        .pin_bk = LCD_BK,
        .max_buffer_size = 2 * 1024,
        .horizontal = 2 // 2: UP, 3： DOWN
    };

    lcd_init(&lcd_config);

    cam_config_t cam_config = {
        .bit_width = 8,
        .mode.jpeg = JPEG_MODE,
        .xclk_fre = 16 * 1000 * 1000,
        .pin = {
            .xclk  = CAM_XCLK,
            .pclk  = CAM_PCLK,
            .vsync = CAM_VSYNC,
            .hsync = CAM_HSYNC,
        },
        .pin_data = {CAM_D0, CAM_D1, CAM_D2, CAM_D3, CAM_D4, CAM_D5, CAM_D6, CAM_D7},
        .size = {
            .width = CAM_WIDTH,
            .high  = CAM_HIGH,
        },
        .max_buffer_size = 8 * 1024,
        .task_stack = 1024,
        .task_pri = configMAX_PRIORITIES
    };

    // 使用PingPang buffer，帧率更高， 也可以单独使用一个buffer节省内存
    cam_config.frame1_buffer = (uint8_t *)heap_caps_malloc(CAM_WIDTH * CAM_HIGH * 2 * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    cam_config.frame2_buffer = (uint8_t *)heap_caps_malloc(CAM_WIDTH * CAM_HIGH * 2 * sizeof(uint8_t), MALLOC_CAP_SPIRAM);

    cam_init(&cam_config);
    if (OV2640_Init(0, 1) == 1) {
        vTaskDelete(NULL);
        return;
    }
    if (cam_config.mode.jpeg) {
        OV2640_JPEG_Mode();
    } else {
        OV2640_RGB565_Mode(false);	//RGB565模式
    }
    
    OV2640_ImageSize_Set(800, 600);
    OV2640_ImageWin_Set(0, 0, 800, 600);
  	OV2640_OutSize_Set(CAM_WIDTH, CAM_HIGH); 
    ESP_LOGI(TAG, "camera init done\n");
    while (1) {
        uint8_t *cam_buf = NULL;
        size_t recv_len = cam_take(&cam_buf);
#if JPEG_MODE
#if DEBUG
        printf("total_len: %d\n", recv_len);
        for (int x = 0; x < 10; x++) {
            ets_printf("%d ", cam_buf[x]);
        }
        ets_printf("\n");
#endif

        int w, h;
        uint8_t *img = jpeg_decode(cam_buf, &w, &h);
        if (img) {
            ESP_LOGI(TAG, "jpeg: w: %d, h: %d\n", w, h);
            lcd_set_index(0, 0, w - 1, h - 1);
            lcd_write_data(img, w * h * sizeof(uint16_t));
            free(img);
        }
#else
        lcd_set_index(0, 0, CAM_WIDTH - 1, CAM_HIGH - 1);
        lcd_write_data(cam_buf, CAM_WIDTH * CAM_HIGH * 2);
#endif
        cam_give(cam_buf);   
        // 使用逻辑分析仪观察帧率
        gpio_set_level(LCD_BK, 1);
        gpio_set_level(LCD_BK, 0);  
    }
    vTaskDelete(NULL);
}

static const luaL_Reg mylibs[] = {
    {"sys", esp_lib_sys},
    {"net", esp_lib_net},
    {"web", esp_lib_web},
    {"mqtt", esp_lib_mqtt},
    {"httpd", esp_lib_httpd},
    {NULL, NULL}
};

const char LUA_SCRIPT_INIT[] = " \
assert(sys.init()) \
dofile(\'/lua/init.lua\') \
";

void lua_task(void *arg)
{
    char *ESP_LUA_ARGV[5] = {"./lua", "-i", "-e", LUA_SCRIPT_INIT, NULL}; // enter interactive mode after executing 'script'

    esp_lua_init(NULL, NULL, mylibs);

    while (1) {
        esp_lua_main(4, ESP_LUA_ARGV);
        printf("lua exit\n");
        vTaskDelay(1000 / portTICK_RATE_MS);
    }

    vTaskDelete(NULL);
}

void app_main() 
{
    xTaskCreate(cam_task, "cam_task", 2048, NULL, 5, NULL);
    vTaskDelay(1000 / portTICK_RATE_MS);
    xTaskCreate(lua_task, "lua_task", 10240, NULL, 5, NULL);
}
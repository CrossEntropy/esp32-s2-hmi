#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"
#include "esp_system.h"
#include "esp_log.h"
#include "soc/i2s_struct.h"
#include "soc/apb_ctrl_reg.h"
#include "esp32s2/rom/lldesc.h"
#include "esp32s2/rom/cache.h"
#include "soc/dport_access.h"
#include "soc/dport_reg.h"
#include "driver/ledc.h"
#include "cam.h"

static const char *TAG = "cam";

#define CAM_DMA_MAX_SIZE     (4095)

typedef enum {
    CAM_IN_SUC_EOF_EVENT = 0,
    CAM_VSYNC_EVENT
} cam_event_t;

typedef struct {
    uint8_t *frame_buffer;
    size_t len;
} frame_buffer_event_t;

typedef struct {
    uint32_t buffer_size;
    uint32_t half_buffer_size;
    uint32_t node_cnt;
    uint32_t half_node_cnt;
    uint32_t dma_size;
    uint32_t cnt;
    uint32_t total_cnt;
    uint16_t width;
    uint16_t high;
    lldesc_t *dma;
    uint8_t *buffer;
    uint8_t *frame1_buffer;
    uint8_t *frame2_buffer;
    uint8_t frame1_buffer_en;
    uint8_t frame2_buffer_en;
    uint8_t jpeg_mode;
    uint8_t vsync_pin;
    QueueHandle_t event_queue;
    QueueHandle_t frame_buffer_queue;
} cam_obj_t;

static cam_obj_t *cam_obj = NULL;

void IRAM_ATTR cam_isr(void *arg)
{
    cam_event_t cam_event = {0};
    BaseType_t HPTaskAwoken = pdFALSE;
    typeof(I2S0.int_st) int_st = I2S0.int_st;
    I2S0.int_clr.val = int_st.val;
    if (int_st.in_suc_eof) {
        cam_event = CAM_IN_SUC_EOF_EVENT;
        xQueueSendFromISR(cam_obj->event_queue, (void *)&cam_event, &HPTaskAwoken);
    }

    if(HPTaskAwoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

void IRAM_ATTR cam_vsync_isr(void *arg)
{
    cam_event_t cam_event = {0};
    BaseType_t HPTaskAwoken = pdFALSE;
    cam_event = CAM_VSYNC_EVENT;
    xQueueSendFromISR(cam_obj->event_queue, (void *)&cam_event, &HPTaskAwoken);

    if(HPTaskAwoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void cam_config(cam_config_t *config)
{
    //Enable I2S periph
    periph_module_enable(PERIPH_I2S0_MODULE);

    // 配置时钟
    I2S0.clkm_conf.val = 0;
    I2S0.clkm_conf.clkm_div_num = 2;
    I2S0.clkm_conf.clkm_div_b = 0;
    I2S0.clkm_conf.clkm_div_a = 0;
    I2S0.clkm_conf.clk_sel = 2;
    I2S0.clkm_conf.clk_en = 1;

    // 配置采样率
    I2S0.sample_rate_conf.val = 0;
    I2S0.sample_rate_conf.tx_bck_div_num = 2;
    I2S0.sample_rate_conf.tx_bits_mod = 8;
    I2S0.sample_rate_conf.rx_bck_div_num = 1;
    I2S0.sample_rate_conf.rx_bits_mod = config->bit_width;

    // 配置数据格式
    I2S0.conf.val = 0;
    I2S0.conf.tx_right_first = 1;
    I2S0.conf.tx_msb_right = 1;
    I2S0.conf.tx_dma_equal = 1;
    I2S0.conf.rx_right_first = 1;
    I2S0.conf.rx_msb_right = 1;
    I2S0.conf.rx_dma_equal = 1;

    I2S0.conf1.val = 0;
    I2S0.conf1.tx_pcm_bypass = 1;
    I2S0.conf1.tx_stop_en = 1;
    I2S0.conf1.rx_pcm_bypass = 1;

    I2S0.conf2.val = 0;
    I2S0.conf2.cam_sync_fifo_reset = 1;
    I2S0.conf2.cam_sync_fifo_reset = 0;
    I2S0.conf2.lcd_en = 1;
    I2S0.conf2.camera_en = 1;
    I2S0.conf2.i_v_sync_filter_en = 0;
    I2S0.conf2.i_v_sync_filter_thres = 1;

    I2S0.conf_chan.val = 0;
    I2S0.conf_chan.tx_chan_mod = 1;
    I2S0.conf_chan.rx_chan_mod = 1;

    I2S0.fifo_conf.val = 0;
    I2S0.fifo_conf.rx_fifo_mod_force_en = 1;
    I2S0.fifo_conf.rx_data_num = 32;
    I2S0.fifo_conf.rx_fifo_mod = 2;
    I2S0.fifo_conf.tx_fifo_mod_force_en = 1;
    I2S0.fifo_conf.tx_data_num = 32;
    I2S0.fifo_conf.tx_fifo_mod = 2;
    I2S0.fifo_conf.dscr_en = 1;

    I2S0.lc_conf.out_rst  = 1;
    I2S0.lc_conf.out_rst  = 0;
    I2S0.lc_conf.in_rst  = 1;
    I2S0.lc_conf.in_rst  = 0;

    I2S0.timing.val = 0;

    I2S0.int_ena.val = 0;
    I2S0.int_clr.val = ~0;

    I2S0.lc_conf.check_owner = 0;

    esp_intr_alloc(ETS_I2S0_INTR_SOURCE, 0, cam_isr, NULL, NULL);
}

static void cam_set_pin(cam_config_t *config)
{
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_PIN_INTR_NEGEDGE;
    io_conf.pin_bit_mask = 1 << config->pin.vsync; 
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(config->pin.vsync, cam_vsync_isr, NULL);
    gpio_intr_disable(config->pin.vsync);

    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[config->pin.pclk], PIN_FUNC_GPIO);
    gpio_set_direction(config->pin.pclk, GPIO_MODE_INPUT);
    gpio_set_pull_mode(config->pin.pclk, GPIO_FLOATING);
    gpio_matrix_in(config->pin.pclk, I2S0I_WS_IN_IDX, false);

    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[config->pin.vsync], PIN_FUNC_GPIO);
    gpio_set_direction(config->pin.vsync, GPIO_MODE_INPUT);
    gpio_set_pull_mode(config->pin.vsync, GPIO_FLOATING);
    gpio_matrix_in(config->pin.vsync, I2S0I_V_SYNC_IDX, true);

    PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[config->pin.hsync], PIN_FUNC_GPIO);
    gpio_set_direction(config->pin.hsync, GPIO_MODE_INPUT);
    gpio_set_pull_mode(config->pin.hsync, GPIO_FLOATING);
    gpio_matrix_in(config->pin.hsync, I2S0I_H_SYNC_IDX, false);

    for(int i = 0; i < config->bit_width; i++) {
        PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[config->pin_data[i]], PIN_FUNC_GPIO);
        gpio_set_direction(config->pin_data[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(config->pin_data[i], GPIO_FLOATING);
        // 高位对齐，IN16总是最高位
        // fifo按bit来访问数据，rx_bits_mod为8时，数据需要按8位对齐
        gpio_matrix_in(config->pin_data[i], I2S0I_DATA_IN0_IDX + (16 - config->bit_width) + i, false);
    }

    ledc_timer_config_t ledc_timer = {
        .duty_resolution = LEDC_TIMER_1_BIT,
        .freq_hz = config->xclk_fre,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_1
    };
    ledc_timer_config(&ledc_timer);
    ledc_channel_config_t ledc_channel = {
        .channel    = LEDC_CHANNEL_2,
        .duty       = 1,
        .gpio_num   = config->pin.xclk,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel  = LEDC_TIMER_1,
        .hpoint     = 0
    };
    ledc_channel_config(&ledc_channel);

    gpio_matrix_in(0x38, I2S0I_H_ENABLE_IDX, false);
    ESP_LOGI(TAG, "cam_xclk_pin setup\n");
}

static void cam_stop(void)
{
    I2S0.conf2.cam_sync_fifo_reset = 1;
    I2S0.conf.rx_start = 0;
    I2S0.in_link.stop = 1;
    I2S0.conf.rx_reset = 1;
    I2S0.conf.rx_reset = 0;
    I2S0.lc_conf.in_rst = 1;
    I2S0.lc_conf.in_rst = 0;
    I2S0.conf.rx_fifo_reset = 1;
    I2S0.conf.rx_fifo_reset = 0;
    I2S0.int_ena.in_suc_eof = 0;
    I2S0.int_clr.in_suc_eof = 1;
}

static void cam_start(void)
{
    I2S0.int_clr.val = ~0;
    I2S0.conf2.cam_sync_fifo_reset = 0;
    I2S0.in_link.start = 1;
    ets_delay_us(1);
    I2S0.fifo_conf.dscr_en = 1;
    I2S0.fifo_conf.dscr_en = 1;
    I2S0.conf.rx_start = 1;
    I2S0.int_clr.in_suc_eof = 1;
    I2S0.int_ena.in_suc_eof = 1;
}

typedef enum {
    CAM_STATE_IDLE = 0,
    CAM_STATE_READ_BUF1 = 1,
    CAM_STATE_READ_BUF2 = 2,
} cam_state_t;

//Copy fram from DMA buffer to fram buffer
static void cam_task(void *arg)
{
    int state = CAM_STATE_IDLE;
    cam_event_t cam_event = {0};
    frame_buffer_event_t frame_buffer_event = {0};
    gpio_intr_enable(cam_obj->vsync_pin);
    if (cam_obj->jpeg_mode == 0) {
        cam_start();
    }
    while (1) {
        xQueueReceive(cam_obj->event_queue, (void *)&cam_event, portMAX_DELAY);
        if (cam_obj->jpeg_mode) {
            switch (state) {
                case CAM_STATE_IDLE: {
                    if (cam_event == CAM_VSYNC_EVENT) {
                        cam_obj->cnt = 0;
                        if (cam_obj->frame1_buffer_en) {
                            cam_start();
                            gpio_intr_disable(cam_obj->vsync_pin);
                            state = 1;
                        } else if (cam_obj->frame2_buffer_en) {
                            cam_start();
                            gpio_intr_disable(cam_obj->vsync_pin);
                            state = 2;
                        }
                    }
                }
                break;
                case CAM_STATE_READ_BUF1: {
                    if (cam_event == CAM_IN_SUC_EOF_EVENT) {
                        if (cam_obj->cnt == 0) {
                            gpio_intr_enable(cam_obj->vsync_pin); // 需要cam真正start接收到第一个buf数据再打开vsync中断
                        }
                        memcpy(&cam_obj->frame1_buffer[cam_obj->cnt * cam_obj->half_buffer_size], &cam_obj->buffer[(cam_obj->cnt % 2) * cam_obj->half_buffer_size], cam_obj->half_buffer_size);
                        if (cam_obj->frame1_buffer_en == 0) {
                            cam_stop();
                            frame_buffer_event.frame_buffer = cam_obj->frame1_buffer;
                            frame_buffer_event.len = (cam_obj->cnt + 1) * cam_obj->half_buffer_size;
                            xQueueSend(cam_obj->frame_buffer_queue, (void *)&frame_buffer_event, portMAX_DELAY);
                            state = 0;
                        } else {
                            cam_obj->cnt++;
                        }
                    } else if (cam_event == CAM_VSYNC_EVENT) {
                        cam_obj->frame1_buffer_en = 0;
                    }
                }
                break;

                case CAM_STATE_READ_BUF2: {
                    if (cam_event == CAM_IN_SUC_EOF_EVENT) {
                        if (cam_obj->cnt == 0) {
                            gpio_intr_enable(cam_obj->vsync_pin); // 需要cam真正start接收到第一个buf数据再打开vsync中断
                        }
                        memcpy(&cam_obj->frame2_buffer[cam_obj->cnt * cam_obj->half_buffer_size], &cam_obj->buffer[(cam_obj->cnt % 2) * cam_obj->half_buffer_size], cam_obj->half_buffer_size);
                        if (cam_obj->frame2_buffer_en == 0) {
                            cam_stop();
                            frame_buffer_event.frame_buffer = cam_obj->frame2_buffer;
                            frame_buffer_event.len = (cam_obj->cnt + 1) * cam_obj->half_buffer_size;
                            xQueueSend(cam_obj->frame_buffer_queue, (void *)&frame_buffer_event, portMAX_DELAY);
                            state = 0;
                        } else {
                            cam_obj->cnt++;
                        }
                    } else if (cam_event == CAM_VSYNC_EVENT) {
                        cam_obj->frame2_buffer_en = 0;
                    }
                }
                break;
            }
        } else {
            switch (state) {
                case CAM_STATE_IDLE: {
                    if (cam_event == CAM_VSYNC_EVENT) { 
                        cam_obj->cnt = 0;
                        if (cam_obj->frame1_buffer_en) {
                            state = 1;
                        } else if (cam_obj->frame2_buffer_en) {
                            state = 2;
                        }
                    }
                }
                break;

                case CAM_STATE_READ_BUF1: {
                    memcpy(&cam_obj->frame1_buffer[cam_obj->cnt * cam_obj->half_buffer_size], &cam_obj->buffer[(cam_obj->cnt % 2) * cam_obj->half_buffer_size], cam_obj->half_buffer_size);
                    if (cam_obj->cnt == cam_obj->total_cnt - 1) {
                        cam_obj->frame1_buffer_en = 0;
                        frame_buffer_event.frame_buffer = cam_obj->frame1_buffer;
                        frame_buffer_event.len = (cam_obj->cnt + 1) * cam_obj->half_buffer_size;
                        xQueueSend(cam_obj->frame_buffer_queue, (void *)&frame_buffer_event, portMAX_DELAY);
                        state = 0;
                    } else {
                        cam_obj->cnt++;
                    }
                }
                break;

                case CAM_STATE_READ_BUF2: {
                    memcpy(&cam_obj->frame2_buffer[cam_obj->cnt * cam_obj->half_buffer_size], &cam_obj->buffer[(cam_obj->cnt % 2) * cam_obj->half_buffer_size], cam_obj->half_buffer_size);
                    if (cam_obj->cnt == cam_obj->total_cnt - 1) {
                        cam_obj->frame2_buffer_en = 0;
                        frame_buffer_event.frame_buffer = cam_obj->frame2_buffer;
                        frame_buffer_event.len = (cam_obj->cnt + 1) * cam_obj->half_buffer_size;
                        xQueueSend(cam_obj->frame_buffer_queue, (void *)&frame_buffer_event, portMAX_DELAY);
                        state = 0;
                    } else {
                        cam_obj->cnt++;
                    }
                }
                break;
            }
        }
    }
}

size_t cam_take(uint8_t **buffer_p)
{
    frame_buffer_event_t frame_buffer_event;
    xQueueReceive(cam_obj->frame_buffer_queue, (void *)&frame_buffer_event, portMAX_DELAY);
    *buffer_p = frame_buffer_event.frame_buffer;
    return frame_buffer_event.len;
}

void cam_give(uint8_t *buffer)
{
    if (buffer == cam_obj->frame1_buffer) {
        cam_obj->frame1_buffer_en = 1;
    } else if (buffer == cam_obj->frame2_buffer){
        cam_obj->frame2_buffer_en = 1;
    }
}

void cam_dma_config(cam_config_t *config) 
{
    int cnt = 0;
    if (config->mode.jpeg) {
        cam_obj->buffer_size = 2048;
        cam_obj->half_buffer_size = cam_obj->buffer_size / 2;
        cam_obj->dma_size = 1024;
    } else {
        for (cnt = 0;;cnt++) { // 寻找可以整除的buffer大小
            if ((config->size.width * config->size.high * 2) % (config->max_buffer_size - cnt) == 0) {
                break;
            }
        }
        cam_obj->buffer_size = config->max_buffer_size - cnt;

        cam_obj->half_buffer_size = cam_obj->buffer_size / 2;
        for (cnt = 0;;cnt++) { // 寻找可以整除的dma大小
            if ((cam_obj->half_buffer_size) % (CAM_DMA_MAX_SIZE - cnt) == 0) {
                break;
            }
        }
        cam_obj->dma_size = CAM_DMA_MAX_SIZE - cnt;
    }

    cam_obj->node_cnt = (cam_obj->buffer_size) / cam_obj->dma_size; // DMA节点个数
    cam_obj->half_node_cnt = cam_obj->node_cnt / 2;
    cam_obj->total_cnt = (config->size.width * config->size.high * 2) / cam_obj->half_buffer_size; // 产生中断拷贝的次数, 乒乓拷贝

    ESP_LOGI(TAG, "cam_buffer_size: %d, cam_dma_size: %d, cam_dma_node_cnt: %d, cam_total_cnt: %d\n", cam_obj->buffer_size, cam_obj->dma_size, cam_obj->node_cnt, cam_obj->total_cnt);

    cam_obj->dma    = (lldesc_t *)heap_caps_malloc(cam_obj->node_cnt * sizeof(lldesc_t), MALLOC_CAP_DMA);
    cam_obj->buffer = (uint8_t *)heap_caps_malloc(cam_obj->buffer_size * sizeof(uint8_t), MALLOC_CAP_DMA);

    for (int x = 0; x < cam_obj->node_cnt; x++) {
        cam_obj->dma[x].size = cam_obj->dma_size;
        cam_obj->dma[x].length = cam_obj->dma_size;
        cam_obj->dma[x].eof = 0;
        cam_obj->dma[x].owner = 1;
        cam_obj->dma[x].buf = (cam_obj->buffer + cam_obj->dma_size * x);
        cam_obj->dma[x].empty = &cam_obj->dma[(x + 1) % cam_obj->node_cnt];
    }

    I2S0.in_link.addr = ((uint32_t)&cam_obj->dma[0]) & 0xfffff;
    I2S0.rx_eof_num = cam_obj->half_buffer_size; // 乒乓操作
}

int cam_init(const cam_config_t *config)
{
    cam_obj = (cam_obj_t *)heap_caps_calloc(1, sizeof(cam_obj_t), MALLOC_CAP_DMA);
    if (!cam_obj) {
        ESP_LOGI(TAG, "camera object malloc error\n");
        return -1;
    }
    cam_obj->width = config->size.width;
    cam_obj->high = config->size.high;
    cam_obj->frame1_buffer = config->frame1_buffer;
    cam_obj->frame2_buffer = config->frame2_buffer;
    cam_obj->jpeg_mode = config->mode.jpeg;
    cam_obj->vsync_pin = config->pin.vsync;
    cam_set_pin(config);
    cam_config(config);
    cam_dma_config(config);

    cam_obj->event_queue = xQueueCreate(2, sizeof(cam_event_t));
    cam_obj->frame_buffer_queue = xQueueCreate(2, sizeof(frame_buffer_event_t));

    if (cam_obj->frame1_buffer != NULL) {
        ESP_LOGI(TAG, "frame1_buffer_en\n");
        cam_obj->frame1_buffer_en = 1;
    } else {
        cam_obj->frame1_buffer_en = 0;
    }
    
    if (cam_obj->frame2_buffer != NULL) {
        ESP_LOGI(TAG, "frame2_buffer_en\n");
        cam_obj->frame2_buffer_en = 1;
    } else {
        cam_obj->frame2_buffer_en = 0;
    }
    xTaskCreate(cam_task, "cam_task", config->task_stack, NULL, config->task_pri, NULL);
    return 0;
}
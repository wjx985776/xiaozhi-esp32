#include <vector>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <freertos/FreeRTOS.h>
#include <esp_lcd_panel_io.h>
#include <esp_log.h>
#include <esp_err.h>
#include "custom_lcd_display.h"
#include "lcd_display.h"
#include "lvgl_theme.h"
#include "esp_lvgl_port.h"
#include "assets/lang_config.h"
#include "settings.h"
#include "config.h"
#include "board.h"
#include "application.h"
#include "avatar_img.h"

void CustomLcdDisplay::Lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p)
{
    assert(disp != NULL);
    CustomLcdDisplay *Disp = (CustomLcdDisplay *)lv_display_get_user_data(disp);
    uint16_t *buffer = (uint16_t *)color_p;
  	for(int y = area->y1; y <= area->y2; y++)
  	{
  	 	for(int x = area->x1; x <= area->x2; x++) 
  	 	{
  	 	   	uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
  	 	   	Disp->RLCD_SetPixel(x,y,color);
  	 	   	buffer++;
  	 	}
  	}
  	Disp->RLCD_Display();
	lv_disp_flush_ready(disp);
}

CustomLcdDisplay::CustomLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
esp_lcd_panel_handle_t panel,
int width, 
int height, 
int offset_x, 
int offset_y,
bool mirror_x, 
bool mirror_y, 
bool swap_xy,
spi_display_config_t spiconfig,
spi_host_device_t spi_host) : LcdDisplay(panel_io, panel, width, height),
mosi_(spiconfig.mosi),
scl_(spiconfig.scl), 
dc_(spiconfig.dc), 
cs_(spiconfig.cs), 
rst_(spiconfig.rst), 
width_(width), 
height_(height)
{
	ESP_LOGI(TAG, "Initialize SPI");
	esp_err_t        ret;
    spi_bus_config_t buscfg   = {};
    int              transfer = width_ * height_;
    buscfg.miso_io_num                   = -1;
    buscfg.mosi_io_num                   = mosi_;
    buscfg.sclk_io_num                   = scl_;
    buscfg.quadwp_io_num                 = -1;
    buscfg.quadhd_io_num                 = -1;
    buscfg.max_transfer_sz               = transfer;
    ret                                  = spi_bus_initialize(spi_host, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num = static_cast<gpio_num_t>(dc_);
    io_config.cs_gpio_num = static_cast<gpio_num_t>(cs_);
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.spi_mode = 0;
    io_config.trans_queue_depth = 7;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spi_host, &io_config, &io_handle));
    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type     = GPIO_INTR_DISABLE;
    gpio_conf.mode          = GPIO_MODE_OUTPUT;
    gpio_conf.pin_bit_mask  = (0x1ULL << rst_);
    gpio_conf.pull_down_en  = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en    = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
    Set_ResetIOLevel(1);

    DisplayLen                = transfer >> 3; //(1byte 8ipex)
    DispBuffer                = (uint8_t *) heap_caps_malloc(DisplayLen, MALLOC_CAP_SPIRAM);
    assert(DispBuffer);
	PixelIndexLUT = (uint16_t (*)[300])heap_caps_malloc(transfer * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
	PixelBitLUT   = (uint8_t (*)[300])heap_caps_malloc(transfer * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    assert(PixelIndexLUT);
    assert(PixelBitLUT);
    if(width_ == 400) {
        InitLandscapeLUT();
    } else {
        InitPortraitLUT();
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority   = 2;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);
    lvgl_port_lock(0);

    display_ = lv_display_create(width, height); /* 以水平和垂直分辨率（像素）进行基本初始化 */
    lv_display_set_flush_cb(display_, Lvgl_flush_cb);
    lv_display_set_user_data(display_, this);
	size_t lvgl_buffer_size = LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565) * transfer;
	uint8_t *lvgl_buffer1 = (uint8_t *) heap_caps_malloc(lvgl_buffer_size, MALLOC_CAP_SPIRAM);
    assert(lvgl_buffer1);
	lv_display_set_buffers(display_, lvgl_buffer1, NULL, lvgl_buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_LOGI(TAG, "RLCD init");
    RLCD_Init();

    lvgl_port_unlock();
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    // Note: SetupUI() should be called by Application::Initialize(), not in constructor
    // to ensure lvgl objects are created after the display is fully initialized.
}

CustomLcdDisplay::~CustomLcdDisplay() {
}

void CustomLcdDisplay::InitPortraitLUT() {
    uint16_t W4 = width_ >> 2;
    for (uint16_t y = 0; y < height_; y++)
    {
        uint16_t byte_y = y >> 1;
        uint8_t  local_y = y & 1;
        for (uint16_t x = 0; x < width_; x++)
        {
            uint16_t byte_x = x >> 2;
            uint8_t  local_x = x & 3;

            uint32_t index = byte_y * W4 + byte_x;
            uint8_t bit = 7 - ((local_x << 1) | local_y);

            PixelIndexLUT[x][y] = index;
            PixelBitLUT  [x][y] = (1 << bit);
        }
    }
}

void CustomLcdDisplay::InitLandscapeLUT() {
    uint16_t H4 = height_ >> 2;
    for (uint16_t y = 0; y < height_; y++)
    {
        uint16_t inv_y = height_ - 1 - y;
        uint16_t block_y = inv_y >> 2;
        uint8_t  local_y  = inv_y & 3;
        for (uint16_t x = 0; x < width_; x++)
        {
            uint16_t byte_x = x >> 1;
            uint8_t  local_x = x & 1;

            uint32_t index = byte_x * H4 + block_y;
            uint8_t bit = 7 - ((local_y << 1) | local_x);

            PixelIndexLUT[x][y] = index;
            PixelBitLUT  [x][y] = (1 << bit);
        }
    }
}

void CustomLcdDisplay::Set_ResetIOLevel(uint8_t level) {
    gpio_set_level((gpio_num_t) rst_, level ? 1 : 0);
}

void CustomLcdDisplay::RLCD_SendCommand(uint8_t Reg) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, Reg, NULL, 0));
}

void CustomLcdDisplay::RLCD_SendData(uint8_t Data) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, -1, &Data, 1));
}

void CustomLcdDisplay::RLCD_Sendbuffera(uint8_t *Data, int len) {
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(io_handle, -1, Data, len));
}

void CustomLcdDisplay::RLCD_Reset(void) {
    Set_ResetIOLevel(1);
    vTaskDelay(pdMS_TO_TICKS(50));
    Set_ResetIOLevel(0);
    vTaskDelay(pdMS_TO_TICKS(20));
    Set_ResetIOLevel(1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

void CustomLcdDisplay::RLCD_ColorClear(uint8_t color) {
    memset(DispBuffer, color, DisplayLen);
}

void CustomLcdDisplay::RLCD_Init() {
    RLCD_Reset();

    RLCD_SendCommand(0xD6);  // NVM Load Control
	RLCD_SendData(0x17);
	RLCD_SendData(0x02);

	RLCD_SendCommand(0xD1); //Booster Enable
	RLCD_SendData(0x01);

	RLCD_SendCommand(0xC0); //Gate Voltage Control
	RLCD_SendData(0x11);   
	RLCD_SendData(0x04);   

	RLCD_SendCommand(0xC1); //VSHP Setting
	RLCD_SendData(0x69);
	RLCD_SendData(0x69);
	RLCD_SendData(0x69);
	RLCD_SendData(0x69);

	RLCD_SendCommand(0xC2);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);

	RLCD_SendCommand(0xC4);
	RLCD_SendData(0x4B);
	RLCD_SendData(0x4B);
	RLCD_SendData(0x4B);
	RLCD_SendData(0x4B);

	RLCD_SendCommand(0xC5);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);
	RLCD_SendData(0x19);

	RLCD_SendCommand(0xD8);
	RLCD_SendData(0x80);
	RLCD_SendData(0xE9);

	RLCD_SendCommand(0xB2);
	RLCD_SendData(0x02);

	RLCD_SendCommand(0xB3);
	RLCD_SendData(0xE5);
	RLCD_SendData(0xF6);
	RLCD_SendData(0x05);
	RLCD_SendData(0x46);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x76);
	RLCD_SendData(0x45);

	RLCD_SendCommand(0xB4);
	RLCD_SendData(0x05);
	RLCD_SendData(0x46);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x77);
	RLCD_SendData(0x76);
	RLCD_SendData(0x45);

	RLCD_SendCommand(0x62);
	RLCD_SendData(0x32);
	RLCD_SendData(0x03);
	RLCD_SendData(0x1F);

	RLCD_SendCommand(0xB7);
	RLCD_SendData(0x13);

	RLCD_SendCommand(0xB0);
	RLCD_SendData(0x64);

	RLCD_SendCommand(0x11); 
	vTaskDelay(pdMS_TO_TICKS(200));     
	RLCD_SendCommand(0xC9);
	RLCD_SendData(0x00);

	RLCD_SendCommand(0x36);
	RLCD_SendData(0x48); 

	RLCD_SendCommand(0x3A);
	RLCD_SendData(0x11); 

	RLCD_SendCommand(0xB9);
	RLCD_SendData(0x20);

	RLCD_SendCommand(0xB8);
	RLCD_SendData(0x29);

	RLCD_SendCommand(0x21);

	RLCD_SendCommand(0x2A); 
	RLCD_SendData(0x12);
	RLCD_SendData(0x2A);

	RLCD_SendCommand(0x2B); 
	RLCD_SendData(0x00);
	RLCD_SendData(0xC7);

	RLCD_SendCommand(0x35);
	RLCD_SendData(0x00);

	RLCD_SendCommand(0xD0);
	RLCD_SendData(0xFF);

	RLCD_SendCommand(0x38);
	RLCD_SendCommand(0x29);

    RLCD_ColorClear(ColorWhite);
}

void CustomLcdDisplay::RLCD_SetPixel(uint16_t x, uint16_t y, uint8_t color) {
    uint32_t idx = PixelIndexLUT[x][y];
    uint8_t  mask = PixelBitLUT[x][y];

    uint8_t *p = &DispBuffer[idx];

    if (color)
        *p |= mask;
    else
        *p &= ~mask;
}

void CustomLcdDisplay::RLCD_Display() {
    RLCD_SendCommand(0x2A);     // Column Address Set
  	RLCD_SendData(0x12);
  	RLCD_SendData(0x2A);

  	RLCD_SendCommand(0x2B);     // Page Address Set
  	RLCD_SendData(0x00);
  	RLCD_SendData(0xC7);

  	RLCD_SendCommand(0x2c);     // Page Address Set

	RLCD_Sendbuffera(DispBuffer,DisplayLen);
}

/* ==========================================================================
 * [DSH] Dashboard layout for the 400x300 RLCD, modelled on the reference
 * desktop-gadget video: character on the left, rounded info cards on the right.
 *
 *   +--------------------------------------------------+
 *   |  (wifi)                                    (bat)  |
 *   |  +--------+   +--------------------------------+  |
 *   |  |        |   |            21:19               |  |
 *   |  | avatar |   |        周五 11月22日            |  |
 *   |  |        |   +--------------------------------+  |
 *   |  +--------+   +--------------------------------+  |
 *   |  +------+     |  多云  9°C  体感 8°C            |  |
 *   |  |######|     +--------------------------------+  |
 *   |  +------+                                        |
 *   +--------------------------------------------------+
 *
 * Objects we create are tagged with a magic user-data value so a theme rebuild
 * can be detected and the dashboard re-created rather than leaking dangling
 * pointers.
 * ========================================================================== */

static constexpr uint32_t kDashMagic = 0x44534831;  // "DSH1"

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

bool CustomLcdDisplay::BuildDashboard()
{
    // If a previous build survived, nothing to do.
    if (card_time_ != nullptr && lv_obj_is_valid(card_time_) &&
        lv_obj_get_user_data(card_time_) == (void *)(uintptr_t)kDashMagic) {
        return false;
    }
    card_time_ = nullptr;
    card_hello_ = nullptr;
    time_label_ = nullptr;
    date_label_ = nullptr;
    hello_label_ = nullptr;
    mood_bar_ = nullptr;
    avatar_obj_ = nullptr;

    auto lvgl_theme = static_cast<LvglTheme *>(current_theme_);
    if (lvgl_theme == nullptr) {
        return false;
    }
    auto text_font = lvgl_theme->text_font()->font();
    lv_obj_t *scr = lv_screen_active();
    if (scr == nullptr) {
        return false;
    }

    // Hide the stock widgets we are replacing.
    if (status_bar_ != nullptr) lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    if (bottom_bar_ != nullptr) lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_box_ != nullptr) lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);

    // --- left: character portrait -----------------------------------------
    lv_obj_t *frame = lv_obj_create(scr);
    lv_obj_remove_style_all(frame);
    lv_obj_set_pos(frame, 14, 52);
    lv_obj_set_size(frame, 150, 150);
    lv_obj_set_style_border_color(frame, lv_color_black(), 0);
    lv_obj_set_style_border_width(frame, 2, 0);
    lv_obj_set_style_radius(frame, 12, 0);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);

    avatar_obj_ = lv_image_create(frame);
    lv_image_set_src(avatar_obj_, &avatar_img);
    lv_obj_center(avatar_obj_);

    // --- left bottom: mood bar --------------------------------------------
    mood_bar_ = lv_bar_create(scr);
    lv_obj_set_pos(mood_bar_, 14, 262);
    lv_obj_set_size(mood_bar_, 150, 12);
    lv_bar_set_range(mood_bar_, 0, 100);
    lv_bar_set_value(mood_bar_, 72, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(mood_bar_, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mood_bar_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(mood_bar_, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(mood_bar_, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(mood_bar_, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(mood_bar_, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(mood_bar_, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(mood_bar_, 6, LV_PART_INDICATOR);

    // --- right top: clock card --------------------------------------------
    card_time_ = make_card(scr, 178, 52, 208, 96);
    lv_obj_set_user_data(card_time_, (void *)(uintptr_t)kDashMagic);

    time_label_ = lv_label_create(card_time_);
    lv_obj_set_style_text_font(time_label_, text_font, 0);
    lv_obj_set_style_text_color(time_label_, lv_color_black(), 0);
    lv_label_set_text(time_label_, "00:00");
    lv_obj_align(time_label_, LV_ALIGN_TOP_MID, 0, 2);
    // The builtin font tops out at 30px; scale it up so the clock reads across
    // a desk. Pivot must be the top-centre or the glyphs drift off the card.
    lv_obj_set_style_transform_pivot_x(time_label_, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(time_label_, 0, 0);
    lv_obj_set_style_transform_scale(time_label_, 620, 0);  // ~2.4x

    date_label_ = lv_label_create(card_time_);
    lv_obj_set_style_text_font(date_label_, text_font, 0);
    lv_obj_set_style_text_color(date_label_, lv_color_black(), 0);
    lv_label_set_text(date_label_, "");
    lv_obj_align(date_label_, LV_ALIGN_BOTTOM_MID, 0, -2);

    // --- right bottom: weather card ---------------------------------------
    card_hello_ = make_card(scr, 178, 158, 208, 96);
    lv_obj_set_user_data(card_hello_, (void *)(uintptr_t)kDashMagic);

    hello_label_ = lv_label_create(card_hello_);
    lv_obj_set_style_text_font(hello_label_, text_font, 0);
    lv_obj_set_style_text_color(hello_label_, lv_color_black(), 0);
    lv_label_set_long_mode(hello_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hello_label_, 190);
    lv_obj_set_style_text_align(hello_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(hello_label_, "你好阿哈");
    lv_obj_align(hello_label_, LV_ALIGN_CENTER, 0, 0);

    ESP_LOGI(TAG, "[DSH] dashboard built");
    return true;
}

void CustomLcdDisplay::SetupUI() {
    // [DSH] Stock UI only. Any widget tweak happens from ApplyIdleLayout(),
    // which runs once the runtime theme is ready (see UpdateStatusBar).
    LcdDisplay::SetupUI();
}

void CustomLcdDisplay::ApplyIdleLayout() {
    // Rebuild when the theme (re)created the screen underneath us.
    BuildDashboard();

    if (!lv_obj_is_valid(card_time_) || !lv_obj_is_valid(card_hello_)) {
        return;
    }

    // Idle <-> chatting: the chat page needs the whole panel, so hide the cards.
    bool chatting = false;
    {
        auto state = Application::GetInstance().GetDeviceState();
        chatting = (state == kDeviceStateListening || state == kDeviceStateSpeaking);
    }

    if (chatting) {
        lv_obj_add_flag(card_time_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(card_hello_, LV_OBJ_FLAG_HIDDEN);
        if (mood_bar_ != nullptr) lv_obj_add_flag(mood_bar_, LV_OBJ_FLAG_HIDDEN);
        if (bottom_bar_ != nullptr) lv_obj_clear_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(card_time_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(card_hello_, LV_OBJ_FLAG_HIDDEN);
        if (mood_bar_ != nullptr) lv_obj_clear_flag(mood_bar_, LV_OBJ_FLAG_HIDDEN);
        if (bottom_bar_ != nullptr) lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }

    refresh_clock_pending_ = true;
}

void CustomLcdDisplay::RefreshClock() {
    if (!lv_obj_is_valid(time_label_)) {
        return;
    }
    time_t now = time(nullptr);
    struct tm info;
    localtime_r(&now, &info);
    if (info.tm_min == last_clock_min_) {
        return;
    }
    last_clock_min_ = info.tm_min;

    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", info.tm_hour, info.tm_min);
    lv_label_set_text(time_label_, buf);

    static const char *kWeek[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    char date_buf[32];
    snprintf(date_buf, sizeof(date_buf), "%s %d月%d日", kWeek[info.tm_wday],
             info.tm_mon + 1, info.tm_mday);
    lv_label_set_text(date_label_, date_buf);
    refresh_clock_pending_ = false;
}

void CustomLcdDisplay::UpdateStatusBar(bool update_all) {
    LcdDisplay::UpdateStatusBar(update_all);
    DisplayLockGuard lock(this);
    ApplyIdleLayout();
    RefreshClock();
}

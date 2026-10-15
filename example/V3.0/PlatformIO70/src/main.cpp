/*---------------------------------------------------------------
 * CrowPanel 7.0-HMI ESP32 Display (800x480) - MP3 Player
 *
 * Features & Hardware Drivers:
 *  - 800x480 RGB LCD Panel (LovyanGFX RGB Bus)
 *  - GT911 Capacitive Touch Controller (I2C / PCA9557)
 *  - LVGL 9.1.0 Touch GUI Interface
 *  - SD Card File Browser over SPI (MOSI:11, MISO:13, SCK:12, CS:10)
 *  - I2S Audio MP3 Decoder (ESP32-audioI2S, BCLK:42, LRC:18, DOUT:17)
 *  - Backlight control (GPIO 2)
 *  - Relay control (GPIO 38)
 *  - DHT20 Sensor support (Wire1: SDA 19, SCL 20)
 *--------------------------------------------------------------*/

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <Audio.h>
#include <DHT20.h>
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lvgl.h>

#include <string>
#include <vector>

/*---------------------------------------------------------------
 * RGB display configuration
 * Bind the ESP32-S3 RGB signals and panel timing to LovyanGFX.
 *--------------------------------------------------------------*/
class LGFX : public lgfx::LGFX_Device
{
public:
    lgfx::Bus_RGB bus;
    lgfx::Panel_RGB panel;

    LGFX()
    {
        auto bus_config = bus.config();
        bus_config.panel = &panel;
        bus_config.pin_d0 = GPIO_NUM_15;
        bus_config.pin_d1 = GPIO_NUM_7;
        bus_config.pin_d2 = GPIO_NUM_6;
        bus_config.pin_d3 = GPIO_NUM_5;
        bus_config.pin_d4 = GPIO_NUM_4;
        bus_config.pin_d5 = GPIO_NUM_9;
        bus_config.pin_d6 = GPIO_NUM_46;
        bus_config.pin_d7 = GPIO_NUM_3;
        bus_config.pin_d8 = GPIO_NUM_8;
        bus_config.pin_d9 = GPIO_NUM_16;
        bus_config.pin_d10 = GPIO_NUM_1;
        bus_config.pin_d11 = GPIO_NUM_14;
        bus_config.pin_d12 = GPIO_NUM_21;
        bus_config.pin_d13 = GPIO_NUM_47;
        bus_config.pin_d14 = GPIO_NUM_48;
        bus_config.pin_d15 = GPIO_NUM_45;
        bus_config.pin_henable = GPIO_NUM_41;
        bus_config.pin_vsync = GPIO_NUM_40;
        bus_config.pin_hsync = GPIO_NUM_39;
        bus_config.pin_pclk = GPIO_NUM_0;
        bus_config.freq_write = 24000000;
        bus_config.hsync_polarity = 0;
        bus_config.hsync_front_porch = 40;
        bus_config.hsync_pulse_width = 48;
        bus_config.hsync_back_porch = 40;
        bus_config.vsync_polarity = 0;
        bus_config.vsync_front_porch = 1;
        bus_config.vsync_pulse_width = 31;
        bus_config.vsync_back_porch = 13;
        bus_config.pclk_active_neg = 1;
        bus_config.de_idle_high = 0;
        bus_config.pclk_idle_high = 0;
        bus.config(bus_config);

        auto panel_config = panel.config();
        panel_config.memory_width = 800;
        panel_config.memory_height = 480;
        panel_config.panel_width = 800;
        panel_config.panel_height = 480;
        panel_config.offset_x = 0;
        panel_config.offset_y = 0;
        panel.config(panel_config);

        panel.setBus(&bus);
        setPanel(&panel);
    }
};

LGFX lcd;

#include "touch.h"

namespace
{
// Hardware pin configurations
constexpr uint8_t   BACKLIGHT_PIN = 2;
constexpr uint8_t   RELAY_PIN     = 38;
constexpr uint32_t  SCREEN_WIDTH  = 800;
constexpr uint32_t  SCREEN_HEIGHT = 480;

// SD Card SPI Pins
constexpr int SD_MOSI = 11;
constexpr int SD_MISO = 13;
constexpr int SD_SCK  = 12;
constexpr int SD_CS   = 10;

// I2S Audio Pins
constexpr int I2S_DOUT = 17;
constexpr int I2S_BCLK = 42;
constexpr int I2S_LRC  = 18;

// Peripherals
Audio audio;
DHT20 dht20(&Wire1);
SemaphoreHandle_t audio_mutex = nullptr;
TaskHandle_t audio_task_handle = nullptr;

bool sd_mounted = false;
bool is_playing = false;
bool is_paused  = false;
uint8_t current_volume = 15; // 0..21

std::string current_folder = "/";
std::string selected_track = "";

// Directory cache for list clicks
std::vector<std::string> current_subdirs;
std::vector<std::string> current_audio_files;

// UI Elements
lv_obj_t *lbl_sd_badge      = nullptr;
lv_obj_t *lbl_track_title   = nullptr;
lv_obj_t *lbl_track_folder  = nullptr;
lv_obj_t *lbl_status        = nullptr;
lv_obj_t *lbl_time          = nullptr;
lv_obj_t *lbl_volume        = nullptr;
lv_obj_t *bar_progress      = nullptr;
lv_obj_t *btn_play_pause    = nullptr;
lv_obj_t *lbl_play_pause    = nullptr;

// File Browser Modal Elements
lv_obj_t *modal_browser     = nullptr;
lv_obj_t *lbl_browser_path  = nullptr;
lv_obj_t *file_list         = nullptr;

uint32_t tick_get()
{
    return millis();
}

void display_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixel_map)
{
    (void)area;
    if(!lcd.bus.presentFrameBuffer(pixel_map)) {
        Serial.println("LovyanGFX VSYNC frame switch timeout");
    }
    lv_display_flush_ready(display);
}

void touchpad_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->state = LV_INDEV_STATE_RELEASED;

    if(touch_touched()) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = touch_last_x;
        data->point.y = touch_last_y;
    }
}

bool is_audio_file(const String &name)
{
    String lower = name;
    lower.toLowerCase();
    return lower.endsWith(".mp3") || lower.endsWith(".wav") ||
           lower.endsWith(".aac") || lower.endsWith(".flac") ||
           lower.endsWith(".m4a");
}

void update_ui_state();
void populate_file_list(const char *dir_path);

void audio_info(Audio::msg_t m)
{
    if (m.e == Audio::evt_eof) {
        Serial.println("[Audio] End of file reached");
        is_playing = false;
        is_paused = false;
    }
    if (m.msg) {
        Serial.printf("[Audio %s] %s\n", m.s ? m.s : "info", m.msg);
    }
}

void audio_task_func(void *pvParameters)
{
    while (1) {
        if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            audio.loop();
            xSemaphoreGive(audio_mutex);
        }
        if (!audio.isRunning()) {
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

bool try_mount_sd()
{
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI);
    sd_mounted = SD.begin(SD_CS);

    if (sd_mounted) {
        uint64_t cardSize = SD.cardSize() / (1024 * 1024);
        Serial.printf("SD Card mounted successfully. Size: %llu MB\n", cardSize);
        if (lbl_sd_badge) {
            lv_label_set_text_fmt(lbl_sd_badge, LV_SYMBOL_SD_CARD " SD: %lluMB", cardSize);
            lv_obj_set_style_text_color(lbl_sd_badge, lv_color_hex(0x10B981), LV_PART_MAIN);
        }
    } else {
        Serial.println("SD Card mount failed or not inserted.");
        if (lbl_sd_badge) {
            lv_label_set_text(lbl_sd_badge, LV_SYMBOL_WARNING " No SD Card");
            lv_obj_set_style_text_color(lbl_sd_badge, lv_color_hex(0xEF4444), LV_PART_MAIN);
        }
    }
    return sd_mounted;
}

void play_track(const std::string &filepath)
{
    selected_track = filepath;
    Serial.printf("[Player] Starting playback: %s\n", filepath.c_str());

    if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        audio.stopSong();
        audio.connecttoFS(SD, filepath.c_str());
        is_playing = true;
        is_paused = false;
        xSemaphoreGive(audio_mutex);
    }
    update_ui_state();
}

void toggle_play_pause()
{
    if (selected_track.empty()) {
        // No track selected, open file chooser
        if (modal_browser) {
            lv_obj_remove_flag(modal_browser, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(modal_browser);
            populate_file_list(current_folder.c_str());
        }
        return;
    }

    if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (!is_playing && !is_paused) {
            audio.connecttoFS(SD, selected_track.c_str());
            is_playing = true;
            is_paused = false;
        } else if (is_playing) {
            audio.pauseResume();
            is_paused = !is_paused;
        }
        xSemaphoreGive(audio_mutex);
    }
    update_ui_state();
}

void stop_playback()
{
    if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        audio.stopSong();
        is_playing = false;
        is_paused = false;
        xSemaphoreGive(audio_mutex);
    }
    if (bar_progress) lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);
    if (lbl_time) lv_label_set_text(lbl_time, "00:00 / 00:00");
    update_ui_state();
}

void adjust_volume(int delta)
{
    int new_vol = static_cast<int>(current_volume) + delta;
    if (new_vol < 0) new_vol = 0;
    if (new_vol > 21) new_vol = 21;
    current_volume = static_cast<uint8_t>(new_vol);

    if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        audio.setVolume(current_volume);
        xSemaphoreGive(audio_mutex);
    }
    if (lbl_volume) {
        lv_label_set_text_fmt(lbl_volume, LV_SYMBOL_VOLUME_MAX " Vol: %u/21", current_volume);
    }
}

void update_ui_state()
{
    // Update play button text
    if (lbl_play_pause) {
        if (is_playing && !is_paused) {
            lv_label_set_text(lbl_play_pause, LV_SYMBOL_PAUSE " Pause");
        } else if (is_playing && is_paused) {
            lv_label_set_text(lbl_play_pause, LV_SYMBOL_PLAY " Resume");
        } else {
            lv_label_set_text(lbl_play_pause, LV_SYMBOL_PLAY " Play");
        }
    }

    // Update status badge
    if (lbl_status) {
        if (is_playing && !is_paused) {
            lv_label_set_text(lbl_status, "Status: PLAYING");
            lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x10B981), LV_PART_MAIN);
        } else if (is_playing && is_paused) {
            lv_label_set_text(lbl_status, "Status: PAUSED");
            lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xF59E0B), LV_PART_MAIN);
        } else {
            lv_label_set_text(lbl_status, "Status: STOPPED");
            lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x94A3B8), LV_PART_MAIN);
        }
    }

    // Update track title and directory
    if (lbl_track_title) {
        if (selected_track.empty()) {
            lv_label_set_text(lbl_track_title, "No track selected (Tap 'Choose File')");
            lv_obj_set_style_text_color(lbl_track_title, lv_color_hex(0x94A3B8), LV_PART_MAIN);
        } else {
            size_t slash = selected_track.find_last_of('/');
            std::string fname = (slash != std::string::npos) ? selected_track.substr(slash + 1) : selected_track;
            lv_label_set_text_fmt(lbl_track_title, "%s", fname.c_str());
            lv_obj_set_style_text_color(lbl_track_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        }
    }

    if (lbl_track_folder) {
        if (selected_track.empty()) {
            lv_label_set_text(lbl_track_folder, "Location: -");
        } else {
            size_t slash = selected_track.find_last_of('/');
            std::string fpath = (slash != std::string::npos && slash > 0) ? selected_track.substr(0, slash) : "/";
            lv_label_set_text_fmt(lbl_track_folder, "Folder: %s", fpath.c_str());
        }
    }
}

// -------------------------------------------------------------
// File Browser Handlers
// -------------------------------------------------------------
static void async_navigate_folder(void *user_data)
{
    populate_file_list(current_folder.c_str());
}

void populate_file_list(const char *dir_path)
{
    if (!file_list) return;
    lv_obj_clean(file_list);

    Serial.printf("[Browser] Scanning: %s\n", dir_path);

    if (lbl_browser_path) {
        lv_label_set_text_fmt(lbl_browser_path, "Directory: %s", dir_path);
    }

    if (!sd_mounted) {
        lv_list_add_text(file_list, "SD card not mounted! Tap 'Mount SD'.");
        return;
    }

    File root = SD.open(dir_path);
    if (!root || !root.isDirectory()) {
        lv_list_add_text(file_list, "Error opening directory!");
        Serial.println("[Browser] Failed to open directory.");
        return;
    }

    current_subdirs.clear();
    current_audio_files.clear();

    File entry = root.openNextFile();
    while (entry) {
        String full_entry_name = entry.name();
        int slash = full_entry_name.lastIndexOf('/');
        String base = (slash >= 0) ? full_entry_name.substring(slash + 1) : full_entry_name;

        if (!base.startsWith(".") && !base.startsWith("._")) {
            if (entry.isDirectory()) {
                current_subdirs.push_back(base.c_str());
            } else if (is_audio_file(base)) {
                current_audio_files.push_back(base.c_str());
            }
        }
        entry.close();
        entry = root.openNextFile();
    }
    root.close();

    Serial.printf("[Browser] Found %u folders, %u audio files\n",
                  (unsigned)current_subdirs.size(), (unsigned)current_audio_files.size());

    // Add Directories
    if (!current_subdirs.empty()) {
        lv_list_add_text(file_list, "--- FOLDERS ---");
        for (size_t i = 0; i < current_subdirs.size(); ++i) {
            lv_obj_t *btn = lv_list_add_button(file_list, LV_SYMBOL_DIRECTORY, current_subdirs[i].c_str());
            lv_obj_set_user_data(btn, reinterpret_cast<void *>(uintptr_t(i)));
            lv_obj_add_event_cb(btn, [](lv_event_t *e) {
                lv_obj_t *btn_target = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
                size_t idx = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(btn_target));
                if (idx < current_subdirs.size()) {
                    if (current_folder == "/" || current_folder.empty()) {
                        current_folder = "/" + current_subdirs[idx];
                    } else {
                        current_folder = current_folder + "/" + current_subdirs[idx];
                    }
                    lv_async_call(async_navigate_folder, NULL);
                }
            }, LV_EVENT_CLICKED, NULL);
        }
    }

    // Add Audio Tracks
    lv_list_add_text(file_list, "--- AUDIO TRACKS ---");
    if (current_audio_files.empty()) {
        lv_list_add_text(file_list, "No audio files (.mp3, .wav, .flac) found in this folder.");
    } else {
        for (size_t i = 0; i < current_audio_files.size(); ++i) {
            lv_obj_t *btn = lv_list_add_button(file_list, LV_SYMBOL_AUDIO, current_audio_files[i].c_str());
            lv_obj_set_user_data(btn, reinterpret_cast<void *>(uintptr_t(i)));
            lv_obj_add_event_cb(btn, [](lv_event_t *e) {
                lv_obj_t *btn_target = static_cast<lv_obj_t *>(lv_event_get_current_target(e));
                size_t idx = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(btn_target));
                if (idx < current_audio_files.size()) {
                    std::string full_path;
                    if (current_folder == "/" || current_folder.empty()) {
                        full_path = "/" + current_audio_files[idx];
                    } else {
                        full_path = current_folder + "/" + current_audio_files[idx];
                    }
                    // Hide file browser and play chosen file
                    if (modal_browser) {
                        lv_obj_add_flag(modal_browser, LV_OBJ_FLAG_HIDDEN);
                    }
                    play_track(full_path);
                }
            }, LV_EVENT_CLICKED, NULL);
        }
    }
}

// -------------------------------------------------------------
// UI Builder
// -------------------------------------------------------------
void create_player_ui()
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0C101A), LV_PART_MAIN);

    // --- Header ---
    lv_obj_t *lbl_title = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0x00E5FF), LV_PART_MAIN);
    lv_label_set_text(lbl_title, "CrowPanel MP3 Player");
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, 30, 20);

    lbl_sd_badge = lv_label_create(scr);
    lv_obj_align(lbl_sd_badge, LV_ALIGN_TOP_RIGHT, -30, 32);
    lv_label_set_text(lbl_sd_badge, "Checking SD...");
    lv_obj_set_style_text_color(lbl_sd_badge, lv_color_hex(0xF59E0B), LV_PART_MAIN);

    // --- Now Playing Main Card ---
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, 740, 220);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 85);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x161C2C), LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(0x28314A), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 20, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lbl_track_title = lv_label_create(card);
    lv_obj_set_style_text_font(lbl_track_title, &lv_font_montserrat_40, LV_PART_MAIN);
    lv_obj_align(lbl_track_title, LV_ALIGN_TOP_LEFT, 5, 5);
    lv_label_set_text(lbl_track_title, "No track selected");

    lbl_track_folder = lv_label_create(card);
    lv_obj_align(lbl_track_folder, LV_ALIGN_TOP_LEFT, 5, 55);
    lv_obj_set_style_text_color(lbl_track_folder, lv_color_hex(0x8E9AAF), LV_PART_MAIN);
    lv_label_set_text(lbl_track_folder, "Folder: /");

    lbl_status = lv_label_create(card);
    lv_obj_align(lbl_status, LV_ALIGN_TOP_LEFT, 5, 95);
    lv_label_set_text(lbl_status, "Status: STOPPED");
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x94A3B8), LV_PART_MAIN);

    lbl_time = lv_label_create(card);
    lv_obj_align(lbl_time, LV_ALIGN_TOP_RIGHT, -5, 95);
    lv_label_set_text(lbl_time, "00:00 / 00:00");
    lv_obj_set_style_text_color(lbl_time, lv_color_hex(0xCBD5E1), LV_PART_MAIN);

    // Progress bar
    bar_progress = lv_bar_create(card);
    lv_obj_set_size(bar_progress, 690, 14);
    lv_obj_align(bar_progress, LV_ALIGN_BOTTOM_MID, 0, -5);
    lv_bar_set_range(bar_progress, 0, 100);
    lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_progress, lv_color_hex(0x242D45), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_progress, lv_color_hex(0x3B82F6), LV_PART_INDICATOR);

    // --- Control Bar ---
    lv_obj_t *ctrl_bar = lv_obj_create(scr);
    lv_obj_set_size(ctrl_bar, 740, 105);
    lv_obj_align(ctrl_bar, LV_ALIGN_TOP_MID, 0, 325);
    lv_obj_set_style_bg_color(ctrl_bar, lv_color_hex(0x131826), LV_PART_MAIN);
    lv_obj_set_style_border_color(ctrl_bar, lv_color_hex(0x28314A), LV_PART_MAIN);
    lv_obj_set_style_border_width(ctrl_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(ctrl_bar, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ctrl_bar, 15, LV_PART_MAIN);
    lv_obj_remove_flag(ctrl_bar, LV_OBJ_FLAG_SCROLLABLE);

    // Button 1: Choose File / Open Folder
    lv_obj_t *btn_open = lv_button_create(ctrl_bar);
    lv_obj_set_size(btn_open, 200, 65);
    lv_obj_align(btn_open, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_open, lv_color_hex(0x2563EB), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_open, 10, LV_PART_MAIN);
    lv_obj_t *lbl_open = lv_label_create(btn_open);
    lv_label_set_text(lbl_open, LV_SYMBOL_DIRECTORY " Choose File");
    lv_obj_center(lbl_open);
    lv_obj_add_event_cb(btn_open, [](lv_event_t *e) {
        if (modal_browser) {
            lv_obj_remove_flag(modal_browser, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(modal_browser);
            populate_file_list(current_folder.c_str());
        }
    }, LV_EVENT_CLICKED, NULL);

    // Button 2: Play / Pause
    btn_play_pause = lv_button_create(ctrl_bar);
    lv_obj_set_size(btn_play_pause, 150, 65);
    lv_obj_align(btn_play_pause, LV_ALIGN_LEFT_MID, 215, 0);
    lv_obj_set_style_bg_color(btn_play_pause, lv_color_hex(0x16A34A), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_play_pause, 10, LV_PART_MAIN);
    lbl_play_pause = lv_label_create(btn_play_pause);
    lv_label_set_text(lbl_play_pause, LV_SYMBOL_PLAY " Play");
    lv_obj_center(lbl_play_pause);
    lv_obj_add_event_cb(btn_play_pause, [](lv_event_t *e) {
        toggle_play_pause();
    }, LV_EVENT_CLICKED, NULL);

    // Button 3: Stop
    lv_obj_t *btn_stop = lv_button_create(ctrl_bar);
    lv_obj_set_size(btn_stop, 105, 65);
    lv_obj_align(btn_stop, LV_ALIGN_LEFT_MID, 380, 0);
    lv_obj_set_style_bg_color(btn_stop, lv_color_hex(0xDC2626), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_stop, 10, LV_PART_MAIN);
    lv_obj_t *lbl_stop = lv_label_create(btn_stop);
    lv_label_set_text(lbl_stop, LV_SYMBOL_STOP " Stop");
    lv_obj_center(lbl_stop);
    lv_obj_add_event_cb(btn_stop, [](lv_event_t *e) {
        stop_playback();
    }, LV_EVENT_CLICKED, NULL);

    // Volume Down
    lv_obj_t *btn_voldown = lv_button_create(ctrl_bar);
    lv_obj_set_size(btn_voldown, 45, 65);
    lv_obj_align(btn_voldown, LV_ALIGN_LEFT_MID, 500, 0);
    lv_obj_set_style_bg_color(btn_voldown, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_voldown, 10, LV_PART_MAIN);
    lv_obj_t *lbl_vd = lv_label_create(btn_voldown);
    lv_label_set_text(lbl_vd, "-");
    lv_obj_center(lbl_vd);
    lv_obj_add_event_cb(btn_voldown, [](lv_event_t *e) {
        adjust_volume(-1);
    }, LV_EVENT_CLICKED, NULL);

    // Volume Label
    lbl_volume = lv_label_create(ctrl_bar);
    lv_obj_align(lbl_volume, LV_ALIGN_LEFT_MID, 555, 0);
    lv_label_set_text_fmt(lbl_volume, "Vol:%u", current_volume);
    lv_obj_set_style_text_color(lbl_volume, lv_color_hex(0xE2E8F0), LV_PART_MAIN);

    // Volume Up
    lv_obj_t *btn_volup = lv_button_create(ctrl_bar);
    lv_obj_set_size(btn_volup, 45, 65);
    lv_obj_align(btn_volup, LV_ALIGN_LEFT_MID, 620, 0);
    lv_obj_set_style_bg_color(btn_volup, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_volup, 10, LV_PART_MAIN);
    lv_obj_t *lbl_vu = lv_label_create(btn_volup);
    lv_label_set_text(lbl_vu, "+");
    lv_obj_center(lbl_vu);
    lv_obj_add_event_cb(btn_volup, [](lv_event_t *e) {
        adjust_volume(1);
    }, LV_EVENT_CLICKED, NULL);

    // Button: Mount/Refresh SD
    lv_obj_t *btn_mount = lv_button_create(scr);
    lv_obj_set_size(btn_mount, 130, 36);
    lv_obj_align(btn_mount, LV_ALIGN_BOTTOM_RIGHT, -30, -10);
    lv_obj_set_style_bg_color(btn_mount, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_mount, 8, LV_PART_MAIN);
    lv_obj_t *lbl_mount = lv_label_create(btn_mount);
    lv_label_set_text(lbl_mount, LV_SYMBOL_REFRESH " Mount SD");
    lv_obj_center(lbl_mount);
    lv_obj_add_event_cb(btn_mount, [](lv_event_t *e) {
        try_mount_sd();
    }, LV_EVENT_CLICKED, NULL);

    // --- File Browser Modal (Hidden by default) ---
    modal_browser = lv_obj_create(scr);
    lv_obj_set_size(modal_browser, 760, 440);
    lv_obj_center(modal_browser);
    lv_obj_set_style_bg_color(modal_browser, lv_color_hex(0x131A2B), LV_PART_MAIN);
    lv_obj_set_style_border_color(modal_browser, lv_color_hex(0x3B82F6), LV_PART_MAIN);
    lv_obj_set_style_border_width(modal_browser, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(modal_browser, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(modal_browser, 15, LV_PART_MAIN);
    lv_obj_remove_flag(modal_browser, LV_OBJ_FLAG_SCROLLABLE); // CRITICAL: Not scrollable so it doesn't steal touches
    lv_obj_add_flag(modal_browser, LV_OBJ_FLAG_HIDDEN); // Hidden initially

    // Browser Header
    lbl_browser_path = lv_label_create(modal_browser);
    lv_obj_align(lbl_browser_path, LV_ALIGN_TOP_LEFT, 5, 10);
    lv_label_set_text(lbl_browser_path, "Directory: /");
    lv_obj_set_style_text_color(lbl_browser_path, lv_color_hex(0x38BDF8), LV_PART_MAIN);

    // Up Button
    lv_obj_t *btn_up = lv_button_create(modal_browser);
    lv_obj_set_size(btn_up, 100, 40);
    lv_obj_align(btn_up, LV_ALIGN_TOP_RIGHT, -120, 0);
    lv_obj_set_style_bg_color(btn_up, lv_color_hex(0x2563EB), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_up, 8, LV_PART_MAIN);
    lv_obj_t *lbl_up = lv_label_create(btn_up);
    lv_label_set_text(lbl_up, LV_SYMBOL_UP " Up");
    lv_obj_center(lbl_up);
    lv_obj_add_event_cb(btn_up, [](lv_event_t *e) {
        if (current_folder != "/" && !current_folder.empty()) {
            size_t slash = current_folder.find_last_of('/');
            if (slash == 0) {
                current_folder = "/";
            } else if (slash != std::string::npos) {
                current_folder = current_folder.substr(0, slash);
            }
            lv_async_call(async_navigate_folder, NULL);
        }
    }, LV_EVENT_CLICKED, NULL);

    // Close Button
    lv_obj_t *btn_close = lv_button_create(modal_browser);
    lv_obj_set_size(btn_close, 100, 40);
    lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0xEF4444), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_close, 8, LV_PART_MAIN);
    lv_obj_t *lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, LV_SYMBOL_CLOSE " Close");
    lv_obj_center(lbl_close);
    lv_obj_add_event_cb(btn_close, [](lv_event_t *e) {
        lv_obj_add_flag(modal_browser, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_CLICKED, NULL);

    // List container
    file_list = lv_list_create(modal_browser);
    lv_obj_set_size(file_list, 720, 345);
    lv_obj_align(file_list, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(file_list, lv_color_hex(0x0F1523), LV_PART_MAIN);
    lv_obj_set_style_border_color(file_list, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_radius(file_list, 10, LV_PART_MAIN);
}

} // namespace

/*---------------------------------------------------------------
 * Setup & Initialization
 *--------------------------------------------------------------*/
void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println("\n==========================================");
    Serial.println("  CrowPanel 7.0-HMI ESP32 Display");
    Serial.println("  SD Card MP3 Player (Baud: 115200)");
    Serial.println("==========================================");

    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, LOW);

    Wire1.begin(19, 20);
    Wire1.setClock(400000);

    if(!lcd.begin()) {
        Serial.println("lcd.begin() failed!");
        return;
    }
    delay(200);

    Wire1.end();
    delay(10);
    Wire1.begin(19, 20);
    Wire1.setClock(400000);

    lv_init();
    lv_tick_set_cb(tick_get);
    touch_init();

    dht20.begin();

    lv_color_t *frame_buffer_0 = reinterpret_cast<lv_color_t *>(lcd.bus.getFrameBuffer(0));
    lv_color_t *frame_buffer_1 = reinterpret_cast<lv_color_t *>(lcd.bus.getFrameBuffer(1));
    if(frame_buffer_0 == nullptr || frame_buffer_1 == nullptr) {
        Serial.println("RGB double frame buffer allocation failed!");
        return;
    }

    lv_display_t *display = lv_display_create(lcd.width(), lcd.height());
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, display_flush);
    lv_display_set_buffers(display, frame_buffer_0, frame_buffer_1,
                           SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_FULL);

    lv_indev_t *touchpad = lv_indev_create();
    lv_indev_set_type(touchpad, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touchpad, touchpad_read);

    pinMode(BACKLIGHT_PIN, OUTPUT);
    digitalWrite(BACKLIGHT_PIN, HIGH);

    // Initialize I2S Audio
    audio_mutex = xSemaphoreCreateMutex();
    Audio::audio_info_callback = audio_info;
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(current_volume);

    // Create Audio FreeRTOS task pinned to Core 0
    xTaskCreatePinnedToCore(
        audio_task_func,
        "AudioTask",
        16384,
        NULL,
        5,
        &audio_task_handle,
        0
    );

    // Build GUI
    create_player_ui();

    // Mount SD Card
    try_mount_sd();

    Serial.println("MP3 Player initialization complete!");
}

/*---------------------------------------------------------------
 * Main Loop
 *--------------------------------------------------------------*/
void loop()
{
    static uint32_t last_playback_update = 0;
    const uint32_t now = millis();

    // Update track position and progress bar every 500ms
    if (now - last_playback_update >= 500) {
        last_playback_update = now;

        if (is_playing && !is_paused) {
            uint32_t cur = 0;
            uint32_t dur = 0;
            if (audio_mutex && xSemaphoreTake(audio_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                cur = audio.getAudioCurrentTime();
                dur = audio.getAudioFileDuration();
                xSemaphoreGive(audio_mutex);
            }

            char time_buf[32];
            snprintf(time_buf, sizeof(time_buf), "%02u:%02u / %02u:%02u", cur / 60, cur % 60, dur / 60, dur % 60);
            if (lbl_time) lv_label_set_text(lbl_time, time_buf);

            if (bar_progress && dur > 0) {
                int pct = static_cast<int>(cur * 100 / dur);
                if (pct > 100) pct = 100;
                lv_bar_set_value(bar_progress, pct, LV_ANIM_OFF);
            }
        }
        update_ui_state();
    }

    lv_timer_handler();
    delay(5);
}

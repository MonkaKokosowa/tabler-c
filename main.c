#define _GNU_SOURCE
#include "lvgl/lvgl.h"
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

// LOGICAL RESOLUTION (3x Scaling: 480x300)
#define HOR_RES 480
#define VER_RES 300

// --- Data Structures ---
// We now cache 10 buses so we always have backups ready to show
#define CACHE_SIZE 10

typedef struct {
    time_t timestamp;
    int hour;
    int min;
    int is_valid;
} BusDeparture;

typedef struct {
    time_t timestamp;
    float temperature;
    float rain;
    int precipitation_probability;
    float uv_index;
    float apparent_temperature;
} WeatherData;

WeatherData cached_weather[24];
BusDeparture cached_buses[CACHE_SIZE];
int is_fetching = 0;

// Location ID (can be overridden by .env LOCATION_ID)
char location_id[64] = "00000000-0000-0000-0000-000000000000";
char open_meteo_url[256] = "https://example.com";
// Trim whitespace in-place (both ends)
void trim_whitespace(char *s) {
    char *p = s;
    while(*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while(len > 0 && isspace((unsigned char)s[len-1])) s[--len] = '\0';
}

// Load LOCATION_ID from .env if present (format: LOCATION_ID=...)
void load_env_location(void) {
    FILE *f = fopen(".env", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        trim_whitespace(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim_whitespace(key);
        trim_whitespace(val);
        if (strcmp(key, "LOCATION_ID") == 0 && val[0] != '\0') {
            strncpy(location_id, val, sizeof(location_id)-1);
            location_id[sizeof(location_id)-1] = '\0';
            // break;
        }
        if (strcmp(key, "OPEN_METEO_URL") == 0 && val[0] != '\0') {
            fprintf(stderr, "OPEN_METEO_URL: %s\n", val);
            strncpy(open_meteo_url, val, sizeof(open_meteo_url)-1);
            open_meteo_url[sizeof(open_meteo_url)-1] = '\0';
            // break;
        }

    }
    fclose(f);
}

// --- Framebuffer Driver (3x Scaling) ---
int fbfd = 0;
struct fb_var_screeninfo vinfo;
struct fb_fix_screeninfo finfo;
char *fbp = 0;

void fbdev_flush(lv_disp_drv_t * drv, const lv_area_t * area, lv_color_t * color_p) {
    if(fbp == 0) { lv_disp_flush_ready(drv); return; }

    int32_t x, y;
    int bpp = vinfo.bits_per_pixel / 8;
    long int line_len = finfo.line_length;

    for(y = area->y1; y <= area->y2; y++) {
        for(x = area->x1; x <= area->x2; x++) {
            lv_color_t pixel_color = *color_p;
            color_p++;

            int phys_x = x * 3;
            int phys_y = y * 3;

            if (phys_x >= vinfo.xres - 2 || phys_y >= vinfo.yres - 2) continue;

            long int base = (phys_x + vinfo.xoffset) * bpp + (phys_y + vinfo.yoffset) * line_len;

            // Row 1
            memcpy(fbp + base,                 &pixel_color, bpp);
            memcpy(fbp + base + bpp,           &pixel_color, bpp);
            memcpy(fbp + base + 2*bpp,         &pixel_color, bpp);

            // Row 2
            long int r2 = base + line_len;
            memcpy(fbp + r2,                   &pixel_color, bpp);
            memcpy(fbp + r2 + bpp,             &pixel_color, bpp);
            memcpy(fbp + r2 + 2*bpp,           &pixel_color, bpp);

            // Row 3
            long int r3 = base + 2*line_len;
            memcpy(fbp + r3,                   &pixel_color, bpp);
            memcpy(fbp + r3 + bpp,             &pixel_color, bpp);
            memcpy(fbp + r3 + 2*bpp,           &pixel_color, bpp);
        }
    }
    lv_disp_flush_ready(drv);
}

void fbdev_init(int use_file) {
    long int screensize;

    if (use_file) {
        // Run in file mode for 'fbe' compatibility
        fbfd = open("/tmp/fbe_buffer", O_RDWR | O_CREAT, 0666);
        if (fbfd == -1) { perror("Error: cannot open /tmp/fbe_buffer"); return; }

        // Mock the values that ioctl() would normally retrieve
        vinfo.xres = HOR_RES * 3;
        vinfo.yres = VER_RES * 3;
        vinfo.bits_per_pixel = sizeof(lv_color_t) * 8;
        vinfo.xoffset = 0;
        vinfo.yoffset = 0;
        finfo.line_length = vinfo.xres * vinfo.bits_per_pixel / 8;

        screensize = vinfo.xres * vinfo.yres * vinfo.bits_per_pixel / 8;

        // Ensure the file is the proper size for mmap
        if (ftruncate(fbfd, screensize) == -1) {
            perror("Error: could not resize /tmp/fbe_buffer");
            return;
        }
    } else {
        // Standard hardware framebuffer setup
        fbfd = open("/dev/fb0", O_RDWR);
        if (fbfd == -1) { perror("Error: cannot open framebuffer"); return; }
        if (ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) == -1) { perror("Error reading fixed information"); return; }
        if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) == -1) { perror("Error reading variable information"); return; }

        screensize = vinfo.xres * vinfo.yres * vinfo.bits_per_pixel / 8;
    }

    fbp = (char *)mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fbp == (char *)-1) {
        perror("Error: failed to map framebuffer device to memory");
        fbp = 0;
    }
}

// --- Network & Parsing ---
struct string { char *ptr; size_t len; };
void init_string(struct string *s) { s->len = 0; s->ptr = malloc(s->len + 1); s->ptr[0] = '\0'; }
size_t writefunc(void *ptr, size_t size, size_t nmemb, struct string *s) {
    size_t new_len = s->len + size * nmemb;
    s->ptr = realloc(s->ptr, new_len + 1);
    memcpy(s->ptr + s->len, ptr, size * nmemb);
    s->ptr[new_len] = '\0';
    s->len = new_len;
    return size * nmemb;
}

lv_obj_t *clock_label;
lv_obj_t *bus_label;
lv_obj_t *weather_label;

void update_clock() {
    time_t now;
    struct tm *timeinfo;
    char buffer[32];
    time(&now);
    timeinfo = localtime(&now);
    strftime(buffer, 32, "%H:%M:%S", timeinfo);
    lv_label_set_text(clock_label, buffer);
}

// --- NEW LOGIC: Scan all 24 times, display top 3 valid ---
void refresh_weather_ui() {
    if (is_fetching) return;

    char display_buf[2048] = "";
    time_t now = time(NULL);
    int displayed_count = 0;

    // Iterate through ALL cached weather (up to 24)
    for(int i = 0; i < 24; i++) {

            double diff = difftime(cached_weather[i].timestamp, now);

            // Only show if weather is in the future
            if (diff > -3600) {
                int minutes = (int)(diff / 60);
                if (minutes < 0) minutes = 0;

                char line[64];
                sprintf(line, "#FFFF00 %.1fC##00CCFF %d%% ## %02d\n",
                    cached_weather[i].temperature,
                    cached_weather[i].precipitation_probability,
                    localtime(&cached_weather[i].timestamp)->tm_hour,
                    localtime(&cached_weather[i].timestamp)->tm_min);
                strcat(display_buf, line);
                displayed_count++;

                // Stop after we have filled the screen with 3 items
                if (displayed_count >= 10) break;
            }

    }

    if (displayed_count == 0) lv_label_set_text(weather_label, "Brak pogody.");
    else lv_label_set_text(weather_label, display_buf);
}

// --- NEW LOGIC: Scan all 10 buses, display top 3 valid ---
void refresh_bus_ui() {
    if (is_fetching) return;

    char display_buf[2048] = "";
    time_t now = time(NULL);
    int displayed_count = 0;

    // Iterate through ALL cached buses (up to 10)
    for(int i = 0; i < CACHE_SIZE; i++) {
        if (cached_buses[i].is_valid) {
            double diff = difftime(cached_buses[i].timestamp, now);

            // Only show if bus is in the future (or just left < 60s ago)
            if (diff > -60) {
                int minutes = (int)(diff / 60);
                if (minutes < 0) minutes = 0;

                char line[64];
                sprintf(line, "#FFFF00 %d min# %02d:%02d\n", minutes, cached_buses[i].hour, cached_buses[i].min);
                strcat(display_buf, line);
                displayed_count++;

                // Stop after we have filled the screen with 3 items
                if (displayed_count >= 5) break;
            }
        }
    }

    if (displayed_count == 0) lv_label_set_text(bus_label, "Brak kursow.");
    else lv_label_set_text(bus_label, display_buf);
}

time_t parse_iso_time(const char* iso_str) {
    struct tm tm = {0};
    sscanf(iso_str, "%d-%d-%dT%d:%d:%d",
           &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
           &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    // The API provides time in UTC.
    // timegm() treats the struct tm as UTC, returning the correct epoch time.
    return timegm(&tm);
}

void fetch_weather_data() {
    is_fetching = 1;

    if (open_meteo_url[0] == '\0') {
        is_fetching = 0;
        return;
    }

    CURL *curl;
    CURLcode res;
    curl = curl_easy_init();
    if (!curl) { is_fetching = 0; return; }

    struct string s;
    init_string(&s);

    time_t t = time(NULL);
    struct tm tm = *localtime(&t);

    char url[256];
    sprintf(url, "%s", open_meteo_url);

    fprintf(stdout, "Fetching weather data from URL: %s\n", url);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        cJSON *json = cJSON_Parse(s.ptr);
        if (json) {
            cJSON *hourly = cJSON_GetObjectItemCaseSensitive(json, "hourly");
            if (hourly) {
                // time
                cJSON *time = cJSON_GetObjectItemCaseSensitive(hourly, "time");
                if (time) {
                    int collected_count = 0;
                    cJSON *entry = NULL;
                    cJSON_ArrayForEach(entry, time) {
                        if (cJSON_IsString(entry) && (entry->valuestring != NULL)) {
                            time_t weather_time = parse_iso_time(entry->valuestring);
                            cached_weather[collected_count].timestamp = weather_time;
                            collected_count++;
                        }
                        if (collected_count >= 24) break;
                    }
                }
                // temperature_2m
                cJSON *temperature_2m = cJSON_GetObjectItemCaseSensitive(hourly, "temperature_2m");
                if (temperature_2m) {
                    int collected_count = 0;
                    cJSON *entry = NULL;
                    cJSON_ArrayForEach(entry, temperature_2m) {
                        if (cJSON_IsNumber(entry)) {
                            cached_weather[collected_count].temperature = entry->valuedouble;
                            collected_count++;
                        }
                        if (collected_count >= 24) break;
                    }
                }
                // rain
                cJSON *rain = cJSON_GetObjectItemCaseSensitive(hourly, "rain");
                if (rain) {
                    int collected_count = 0;
                    cJSON *entry = NULL;
                    cJSON_ArrayForEach(entry, rain) {
                        if (cJSON_IsNumber(entry)) {
                            cached_weather[collected_count].rain = entry->valuedouble;
                            collected_count++;
                        }
                        if (collected_count >= 24) break;
                    }
                }
                // precipitation_probability
                cJSON *precipitation_probability = cJSON_GetObjectItemCaseSensitive(hourly, "precipitation_probability");
                if (precipitation_probability) {
                    int collected_count = 0;
                    cJSON *entry = NULL;
                    cJSON_ArrayForEach(entry, precipitation_probability) {
                        if (cJSON_IsNumber(entry)) {
                            cached_weather[collected_count].precipitation_probability = entry->valueint;
                            collected_count++;
                        }
                        if (collected_count >= 24) break;
                    }
                }
                // uv_index
                cJSON *uv_index = cJSON_GetObjectItemCaseSensitive(hourly, "uv_index");
                if (uv_index) {
                    int collected_count = 0;
                    cJSON *entry = NULL;
                    cJSON_ArrayForEach(entry, uv_index) {
                        if (cJSON_IsNumber(entry)) {
                            cached_weather[collected_count].uv_index = entry->valuedouble;
                            collected_count++;
                        }
                        if (collected_count >= 24) break;
                    }
                }
                // apparent_temperature
                cJSON *apparent_temperature = cJSON_GetObjectItemCaseSensitive(hourly, "apparent_temperature");
                if (apparent_temperature) {
                    int collected_count = 0;
                    cJSON *entry = NULL;
                    cJSON_ArrayForEach(entry, apparent_temperature) {
                        if (cJSON_IsNumber(entry)) {
                            cached_weather[collected_count].apparent_temperature = entry->valuedouble;
                            collected_count++;
                        }
                        if (collected_count >= 24) break;
                    }
                }


            }
             cJSON_Delete(json);
        }
    }
    free(s.ptr);
    curl_easy_cleanup(curl);

    is_fetching = 0;
    refresh_weather_ui();

}
void fetch_mpk_data() {
    is_fetching = 1;
    // Clear old cache
    for(int i=0; i<CACHE_SIZE; i++) cached_buses[i].is_valid = 0;

    CURL *curl;
    CURLcode res;
    curl = curl_easy_init();
    if (!curl) { is_fetching = 0; return; }

    struct string s;
    init_string(&s);

    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    char url[256];
        sprintf(url, "https://live.mpk.czest.pl/api/locations/%s/timetables/%d/%d/%d",
            location_id, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    fprintf(stdout, "Fetching data for date: %d-%d-%d from URL: %s\n", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, url);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        cJSON *json = cJSON_Parse(s.ptr);
        if (json) {
             cJSON *item = cJSON_GetArrayItem(json, 0);
             if (item) {
                 cJSON *timetable = cJSON_GetObjectItemCaseSensitive(item, "timetable");
                 time_t now = time(NULL);
                 int collected_count = 0;
                 cJSON *entry = NULL;

                 cJSON_ArrayForEach(entry, timetable) {
                     cJSON *dt = cJSON_GetObjectItemCaseSensitive(entry, "dateTime");
                     if (cJSON_IsString(dt) && (dt->valuestring != NULL)) {
                         time_t bus_time = parse_iso_time(dt->valuestring);
                         double diff = difftime(bus_time, now);

                         // Store future buses in our Deep Cache
                         if (diff > 0) {
                             struct tm *bt = localtime(&bus_time);
                             cached_buses[collected_count].timestamp = bus_time;
                             cached_buses[collected_count].hour = bt->tm_hour;
                             cached_buses[collected_count].min = bt->tm_min;
                             cached_buses[collected_count].is_valid = 1;
                             fprintf(stdout, "  Departure time: %s (Parsed: %02d:%02d)\n", dt->valuestring, bt->tm_hour, bt->tm_min);
                             collected_count++;
                         }
                     }
                     // Keep gathering until we have 10 backup buses
                     if (collected_count >= CACHE_SIZE) break;
                 }
             }
             cJSON_Delete(json);
        }
    }
    free(s.ptr);
    curl_easy_cleanup(curl);

    is_fetching = 0;
    refresh_bus_ui();
}

int main(int argc, char **argv) {

    int use_file = 0;

    // Parse command line arguments for the '-d' flag
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            use_file = 1;
            break;
        }
    }

    lv_init();
    fbdev_init(use_file);

    // Load optional LOCATION_ID from .env (overrides built-in UUID)
    load_env_location();

    static lv_disp_draw_buf_t disp_buf;
    static lv_color_t buf[HOR_RES * 40];
    lv_disp_draw_buf_init(&disp_buf, buf, NULL, HOR_RES * 40);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &disp_buf;
    disp_drv.flush_cb = fbdev_flush;
    disp_drv.hor_res = HOR_RES;
    disp_drv.ver_res = VER_RES;
    lv_disp_drv_register(&disp_drv);

    // Styling
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_text_color(lv_scr_act(), lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    // --- Clock ---
    clock_label = lv_label_create(lv_scr_act());
    lv_obj_align(clock_label, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(clock_label, lv_color_hex(0x00FF00), 0);
    update_clock();

    // --- Bus List ---
    bus_label = lv_label_create(lv_scr_act());
    lv_obj_align(bus_label, LV_ALIGN_TOP_MID, -120, 70);
    lv_obj_set_style_text_font(bus_label, &lv_font_montserrat_36, 0);
    lv_label_set_recolor(bus_label, true);
    lv_label_set_text(bus_label, "Pobieranie...");

    // --- Weather List ---
    weather_label = lv_label_create(lv_scr_act());
    lv_obj_align(weather_label, LV_ALIGN_TOP_MID, 120, 70);
    lv_obj_set_style_text_font(weather_label, &lv_font_montserrat_18, 0);
    lv_label_set_recolor(weather_label, true);
    lv_label_set_text(weather_label, "Pobieranie...");

    fetch_mpk_data();
    fetch_weather_data();

    uint32_t last_net_update = 0;
    uint32_t last_ui_update = 0;

    while(1) {
        lv_tick_inc(5);
        lv_timer_handler();

        update_clock();
        if (lv_tick_get() - last_ui_update > 2000) {
            refresh_bus_ui();
            refresh_weather_ui();
            last_ui_update = lv_tick_get();
        }
        if (lv_tick_get() - last_net_update > 120000) {
            fetch_mpk_data();
            fetch_weather_data();
            last_net_update = lv_tick_get();
        }
        usleep(5000);
    }

    return 0;
}

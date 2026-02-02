#include "lvgl/lvgl.h"
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

BusDeparture cached_buses[CACHE_SIZE];
int is_fetching = 0; 

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

void fbdev_init(void) {
    fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd == -1) { perror("Error: cannot open framebuffer"); return; }
    if (ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) == -1) return;
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) == -1) return;
    long int screensize = vinfo.xres * vinfo.yres * vinfo.bits_per_pixel / 8;
    fbp = (char *)mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
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

void update_clock() {
    time_t now;
    struct tm *timeinfo;
    char buffer[32];
    time(&now);
    timeinfo = localtime(&now);
    strftime(buffer, 32, "%H:%M:%S", timeinfo);
    lv_label_set_text(clock_label, buffer);
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
                if (displayed_count >= 3) break;
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
    tm.tm_isdst = -1; 
    return mktime(&tm);
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
    sprintf(url, "https://live.mpk.czest.pl/api/locations/00000000-0000-0000-0000-000000000000/timetables/%d/%d/%d", 
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

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

int main(void) {
    lv_init();
    fbdev_init();

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
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(clock_label, lv_color_hex(0x00FF00), 0);
    update_clock(); 

    // --- Bus List ---
    bus_label = lv_label_create(lv_scr_act());
    lv_obj_align(bus_label, LV_ALIGN_TOP_MID, 0, 70); 
    lv_obj_set_style_text_font(bus_label, &lv_font_montserrat_48, 0);
    lv_label_set_recolor(bus_label, true); 
    lv_label_set_text(bus_label, "Pobieranie...");

    fetch_mpk_data();

    uint32_t last_net_update = 0;
    uint32_t last_ui_update = 0;
    
    while(1) {
        lv_tick_inc(5); 
        lv_timer_handler();
        
        update_clock();
        if (lv_tick_get() - last_ui_update > 2000) {
            refresh_bus_ui();
            last_ui_update = lv_tick_get();
        }
        if (lv_tick_get() - last_net_update > 120000) {
            fetch_mpk_data();
            last_net_update = lv_tick_get();
        }
        usleep(5000); 
    }

    return 0;
}

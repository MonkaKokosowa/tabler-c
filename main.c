#define _GNU_SOURCE
#include "lvgl/lvgl.h"
#include "logo_img.c"
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

// LOGICAL RESOLUTION (3x Scaling: 480x300 -> 1440x900 Framebuffer)
#define HOR_RES 480
#define VER_RES 300

// --- Data Structures ---
#define MAX_LOCATIONS 4096
#define MAX_LINES 128
#define MAX_VEHICLES 512
#define MAX_DEPARTURES 16
#define MAX_WEATHER 24

typedef struct {
    char location_id[64];
    char current_stop_name[128];
    char open_meteo_url[256];
} LocationConfig;

typedef struct {
    char line_id[37];
    char name[16];
} LineInfo;

typedef struct {
    char location_id[37];
    char name[96];
} LocationInfo;

typedef struct {
    char vehicle_id[37];
    char side_no[16];
    char model[32];
    int low_floor;
} VehicleInfo;

typedef struct {
    time_t timestamp;
    int hour;
    int min;
    char line_name[16];      // e.g. "26"
    char destination[64];   // e.g. "SROCKO"
    char vehicle_side_no[16];
    int low_floor;          // 1 if low floor, 0 otherwise
    int is_valid;
} DepartureItem;

typedef struct {
    time_t timestamp;
    float temperature;
    float rain;
    int precipitation_probability;
    float uv_index;
    float apparent_temperature;
    int is_valid;
} WeatherItem;

// --- Unified Global Data Structure ---
typedef struct {
    LocationConfig config;

    LineInfo lines[MAX_LINES];
    int lines_count;

    LocationInfo locations[MAX_LOCATIONS];
    int locations_count;

    VehicleInfo vehicles[MAX_VEHICLES];
    int vehicles_count;

    DepartureItem departures[MAX_DEPARTURES];
    int departures_count;

    WeatherItem weather[MAX_WEATHER];
    int weather_count;

    int is_fetching;
    int current_screen; // 0 = Timetable, 1 = Weather
    uint32_t last_screen_swap;
} AppState;

static AppState app_state;

// Convert Polish / non-ASCII characters to standard ASCII for LVGL Montserrat font
void sanitize_ascii(char *s) {
    if (!s) return;
    char *read = s;
    char *write = s;

    while (*read) {
        unsigned char c = (unsigned char)*read;
        if (c == 0xC4 || c == 0xC5 || c == 0xC3) { // UTF-8 2-byte prefix
            unsigned char c2 = (unsigned char)*(read + 1);
            if (c == 0xC4) {
                if (c2 == 0x84 || c2 == 0x85) *write++ = 'A'; // Ą / ą
                else if (c2 == 0x86 || c2 == 0x87) *write++ = 'C'; // Ć / ć
                else if (c2 == 0x98 || c2 == 0x99) *write++ = 'E'; // Ę / ę
                else *write++ = 'A';
            } else if (c == 0xC5) {
                if (c2 == 0x81 || c2 == 0x82) *write++ = 'L'; // Ł / ł
                else if (c2 == 0x83 || c2 == 0x84) *write++ = 'N'; // Ń / ń
                else if (c2 == 0x9a || c2 == 0x9b) *write++ = 'S'; // Ś / ś
                else if (c2 == 0xba || c2 == 0xbb) *write++ = 'Z'; // Ź / ź
                else if (c2 == 0xbc || c2 == 0xbd) *write++ = 'Z'; // Ż / ż
                else *write++ = 'S';
            } else if (c == 0xC3) {
                if (c2 == 0x93 || c2 == 0xB3) *write++ = 'O'; // Ó / ó
                else *write++ = 'O';
            }
            read += 2;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
}

void trim_whitespace(char *s) {
    char *p = s;
    while(*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while(len > 0 && isspace((unsigned char)s[len-1])) s[--len] = '\0';
}

void load_env_location(void) {
    // Default fallback values
    strcpy(app_state.config.location_id, "00000000-0000-0000-0000-000000000000");
    strcpy(app_state.config.open_meteo_url, "https://api.open-meteo.com/v1/forecast?latitude=0.0&longitude=0.0&hourly=temperature_2m,rain,precipitation_probability,uv_index,apparent_temperature&timezone=UTC&forecast_days=1");
    strcpy(app_state.config.current_stop_name, "Pobieranie nazwy przystanku...");

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
            strncpy(app_state.config.location_id, val, sizeof(app_state.config.location_id)-1);
        }
        if (strcmp(key, "OPEN_METEO_URL") == 0 && val[0] != '\0') {
            strncpy(app_state.config.open_meteo_url, val, sizeof(app_state.config.open_meteo_url)-1);
        }
    }
    fclose(f);
}

const char* get_line_name(const char* line_id) {
    if (!line_id || line_id[0] == '\0') return "?";
    for (int i = 0; i < app_state.lines_count; i++) {
        if (strcmp(app_state.lines[i].line_id, line_id) == 0) {
            return app_state.lines[i].name;
        }
    }
    return "?";
}

const char* get_location_name(const char* location_id) {
    if (!location_id || location_id[0] == '\0') return "Przystanek";
    for (int i = 0; i < app_state.locations_count; i++) {
        if (strcmp(app_state.locations[i].location_id, location_id) == 0) {
            return app_state.locations[i].name;
        }
    }
    return location_id;
}

int get_vehicle_low_floor(const char* vehicle_id, const char** side_no_out) {
    if (side_no_out) *side_no_out = "";
    if (!vehicle_id || vehicle_id[0] == '\0') return 0;
    for (int i = 0; i < app_state.vehicles_count; i++) {
        if (strcmp(app_state.vehicles[i].vehicle_id, vehicle_id) == 0) {
            if (side_no_out) *side_no_out = app_state.vehicles[i].side_no;
            return app_state.vehicles[i].low_floor;
        }
    }
    return 0;
}

time_t parse_iso_time(const char* iso_str) {
    struct tm tm = {0};
    sscanf(iso_str, "%d-%d-%dT%d:%d:%d",
           &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
           &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return timegm(&tm);
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
        fbfd = open("/tmp/fbe_buffer", O_RDWR | O_CREAT, 0666);
        if (fbfd == -1) { perror("Error: cannot open /tmp/fbe_buffer"); return; }

        vinfo.xres = HOR_RES * 3;
        vinfo.yres = VER_RES * 3;
        vinfo.bits_per_pixel = sizeof(lv_color_t) * 8;
        vinfo.xoffset = 0;
        vinfo.yoffset = 0;
        finfo.line_length = vinfo.xres * vinfo.bits_per_pixel / 8;

        screensize = vinfo.xres * vinfo.yres * vinfo.bits_per_pixel / 8;
        if (ftruncate(fbfd, screensize) == -1) {
            perror("Error: could not resize /tmp/fbe_buffer");
            return;
        }
    } else {
        fbfd = open("/dev/fb0", O_RDWR);
        if (fbfd == -1) { perror("Error: cannot open framebuffer"); return; }
        if (ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) == -1) { perror("Error reading fixed info"); return; }
        if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) == -1) { perror("Error reading variable info"); return; }

        screensize = vinfo.xres * vinfo.yres * vinfo.bits_per_pixel / 8;
    }

    fbp = (char *)mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fbp == (char *)-1) {
        perror("Error: failed to map framebuffer device to memory");
        fbp = 0;
    }
}

// --- Network & Data Fetching ---
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

// 1. Fetch Locations
void fetch_locations() {
    CURL *curl = curl_easy_init();
    if (!curl) return;
    struct string s;
    init_string(&s);
    curl_easy_setopt(curl, CURLOPT_URL, "https://live.mpk.czest.pl/api/locations");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        cJSON *json = cJSON_Parse(s.ptr);
        if (json && cJSON_IsArray(json)) {
            int count = 0;
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, json) {
                cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "locationID");
                cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
                cJSON *desc = cJSON_GetObjectItemCaseSensitive(item, "description");
                if (cJSON_IsString(id) && id->valuestring) {
                    strncpy(app_state.locations[count].location_id, id->valuestring, sizeof(app_state.locations[count].location_id)-1);

                    char full_name[96] = "";
                    if (cJSON_IsString(desc) && desc->valuestring && desc->valuestring[0] != '\0') {
                        snprintf(full_name, sizeof(full_name), "%s. %s", desc->valuestring, (cJSON_IsString(name) && name->valuestring) ? name->valuestring : "");
                    } else if (cJSON_IsString(name) && name->valuestring) {
                        snprintf(full_name, sizeof(full_name), "%s", name->valuestring);
                    }
                    trim_whitespace(full_name);
                    sanitize_ascii(full_name);
                    strncpy(app_state.locations[count].name, full_name, sizeof(app_state.locations[count].name)-1);

                    if (strcmp(id->valuestring, app_state.config.location_id) == 0) {
                        strncpy(app_state.config.current_stop_name, full_name, sizeof(app_state.config.current_stop_name)-1);
                    }

                    count++;
                    if (count >= MAX_LOCATIONS) break;
                }
            }
            app_state.locations_count = count;
            cJSON_Delete(json);
        }
    }
    free(s.ptr);
    curl_easy_cleanup(curl);
}

// 2. Fetch Lines
void fetch_lines() {
    CURL *curl = curl_easy_init();
    if (!curl) return;
    struct string s;
    init_string(&s);
    curl_easy_setopt(curl, CURLOPT_URL, "https://live.mpk.czest.pl/api/lines");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        cJSON *json = cJSON_Parse(s.ptr);
        if (json && cJSON_IsArray(json)) {
            int count = 0;
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, json) {
                cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "lineID");
                cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
                if (cJSON_IsString(id) && id->valuestring && cJSON_IsString(name) && name->valuestring) {
                    strncpy(app_state.lines[count].line_id, id->valuestring, sizeof(app_state.lines[count].line_id)-1);
                    strncpy(app_state.lines[count].name, name->valuestring, sizeof(app_state.lines[count].name)-1);
                    sanitize_ascii(app_state.lines[count].name);
                    count++;
                    if (count >= MAX_LINES) break;
                }
            }
            app_state.lines_count = count;
            cJSON_Delete(json);
        }
    }
    free(s.ptr);
    curl_easy_cleanup(curl);
}

// 3. Fetch Vehicles & Database
void fetch_vehicles() {
    CURL *curl = curl_easy_init();
    if (!curl) return;

    struct string db_s;
    init_string(&db_s);
    curl_easy_setopt(curl, CURLOPT_URL, "https://live.mpk.czest.pl/vehicles_base/database.json");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &db_s);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    cJSON *db_json = NULL;
    if (curl_easy_perform(curl) == CURLE_OK) {
        db_json = cJSON_Parse(db_s.ptr);
    }

    struct string v_s;
    init_string(&v_s);
    curl_easy_setopt(curl, CURLOPT_URL, "https://live.mpk.czest.pl/api/vehicles");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &v_s);
    if (curl_easy_perform(curl) == CURLE_OK) {
        cJSON *v_json = cJSON_Parse(v_s.ptr);
        if (v_json && cJSON_IsArray(v_json)) {
            int count = 0;
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, v_json) {
                cJSON *vid = cJSON_GetObjectItemCaseSensitive(item, "vehicleID");
                cJSON *side = cJSON_GetObjectItemCaseSensitive(item, "sideNo");
                if (cJSON_IsString(vid) && vid->valuestring) {
                    strncpy(app_state.vehicles[count].vehicle_id, vid->valuestring, sizeof(app_state.vehicles[count].vehicle_id)-1);
                    if (cJSON_IsString(side) && side->valuestring) {
                        strncpy(app_state.vehicles[count].side_no, side->valuestring, sizeof(app_state.vehicles[count].side_no)-1);
                    }

                    app_state.vehicles[count].low_floor = 1;
                    if (db_json && cJSON_IsArray(db_json) && cJSON_IsString(side) && side->valuestring) {
                        cJSON *db_item = NULL;
                        cJSON_ArrayForEach(db_item, db_json) {
                            cJSON *db_side = cJSON_GetObjectItemCaseSensitive(db_item, "sideNo");
                            cJSON *db_low = cJSON_GetObjectItemCaseSensitive(db_item, "lowFlor");
                            if (cJSON_IsString(db_side) && db_side->valuestring && strcmp(db_side->valuestring, side->valuestring) == 0) {
                                if (cJSON_IsBool(db_low)) {
                                    app_state.vehicles[count].low_floor = cJSON_IsTrue(db_low) ? 1 : 0;
                                }
                                break;
                            }
                        }
                    }

                    count++;
                    if (count >= MAX_VEHICLES) break;
                }
            }
            app_state.vehicles_count = count;
            cJSON_Delete(v_json);
        }
    }
    if (db_json) cJSON_Delete(db_json);
    free(db_s.ptr);
    free(v_s.ptr);
    curl_easy_cleanup(curl);
}

// 4. Fetch Timetable Departures
void fetch_timetable_data() {
    app_state.is_fetching = 1;
    app_state.departures_count = 0;
    for(int i = 0; i < MAX_DEPARTURES; i++) app_state.departures[i].is_valid = 0;

    CURL *curl = curl_easy_init();
    if (!curl) { app_state.is_fetching = 0; return; }

    struct string s;
    init_string(&s);

    time_t t = time(NULL);
    struct tm tm = *localtime(&t);

    char url[256];
    snprintf(url, sizeof(url), "https://live.mpk.czest.pl/api/locations/%s/timetables/%d/%d/%d",
             app_state.config.location_id, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        cJSON *json = cJSON_Parse(s.ptr);
        if (json && cJSON_IsArray(json)) {
            time_t now = time(NULL);
            int collected = 0;

            cJSON *item = NULL;
            cJSON_ArrayForEach(item, json) {
                cJSON *line_id_obj = cJSON_GetObjectItemCaseSensitive(item, "lineID");
                cJSON *timetable = cJSON_GetObjectItemCaseSensitive(item, "timetable");

                const char *line_name = get_line_name(cJSON_IsString(line_id_obj) ? line_id_obj->valuestring : "");

                if (timetable && cJSON_IsArray(timetable)) {
                    cJSON *entry = NULL;
                    cJSON_ArrayForEach(entry, timetable) {
                        cJSON *dt = cJSON_GetObjectItemCaseSensitive(entry, "dateTime");
                        cJSON *dir = cJSON_GetObjectItemCaseSensitive(entry, "direction");
                        cJSON *vid = cJSON_GetObjectItemCaseSensitive(entry, "vehicleID");

                        if (cJSON_IsString(dt) && dt->valuestring) {
                            time_t bus_time = parse_iso_time(dt->valuestring);
                            double diff = difftime(bus_time, now);

                            if (diff > -60) {
                                struct tm *bt = localtime(&bus_time);
                                app_state.departures[collected].timestamp = bus_time;
                                app_state.departures[collected].hour = bt->tm_hour;
                                app_state.departures[collected].min = bt->tm_min;
                                strncpy(app_state.departures[collected].line_name, line_name, sizeof(app_state.departures[collected].line_name)-1);
                                sanitize_ascii(app_state.departures[collected].line_name);

                                const char* dest_name = get_location_name(cJSON_IsString(dir) ? dir->valuestring : "");
                                strncpy(app_state.departures[collected].destination, dest_name, sizeof(app_state.departures[collected].destination)-1);
                                sanitize_ascii(app_state.departures[collected].destination);

                                const char* side_no = "";
                                app_state.departures[collected].low_floor = get_vehicle_low_floor(cJSON_IsString(vid) ? vid->valuestring : "", &side_no);
                                strncpy(app_state.departures[collected].vehicle_side_no, side_no, sizeof(app_state.departures[collected].vehicle_side_no)-1);

                                app_state.departures[collected].is_valid = 1;
                                collected++;
                                if (collected >= MAX_DEPARTURES) break;
                            }
                        }
                    }
                }
                if (collected >= MAX_DEPARTURES) break;
            }

            for (int i = 0; i < collected - 1; i++) {
                for (int j = 0; j < collected - i - 1; j++) {
                    if (app_state.departures[j].timestamp > app_state.departures[j+1].timestamp) {
                        DepartureItem temp = app_state.departures[j];
                        app_state.departures[j] = app_state.departures[j+1];
                        app_state.departures[j+1] = temp;
                    }
                }
            }

            app_state.departures_count = collected;
            cJSON_Delete(json);
        }
    }
    free(s.ptr);
    curl_easy_cleanup(curl);
    app_state.is_fetching = 0;
}

// 5. Fetch Weather
void fetch_weather_data() {
    if (app_state.config.open_meteo_url[0] == '\0') return;

    CURL *curl = curl_easy_init();
    if (!curl) return;

    struct string s;
    init_string(&s);

    curl_easy_setopt(curl, CURLOPT_URL, app_state.config.open_meteo_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        cJSON *json = cJSON_Parse(s.ptr);
        if (json) {
            cJSON *hourly = cJSON_GetObjectItemCaseSensitive(json, "hourly");
            if (hourly) {
                cJSON *time_arr = cJSON_GetObjectItemCaseSensitive(hourly, "time");
                cJSON *temp_arr = cJSON_GetObjectItemCaseSensitive(hourly, "temperature_2m");
                cJSON *precip_arr = cJSON_GetObjectItemCaseSensitive(hourly, "precipitation_probability");
                cJSON *app_temp_arr = cJSON_GetObjectItemCaseSensitive(hourly, "apparent_temperature");

                int count = 0;
                if (time_arr && cJSON_IsArray(time_arr)) {
                    cJSON *entry = NULL;
                    cJSON_ArrayForEach(entry, time_arr) {
                        if (cJSON_IsString(entry) && entry->valuestring) {
                            app_state.weather[count].timestamp = parse_iso_time(entry->valuestring);
                            app_state.weather[count].is_valid = 1;
                            count++;
                        }
                        if (count >= MAX_WEATHER) break;
                    }
                }

                if (temp_arr && cJSON_IsArray(temp_arr)) {
                    int i = 0; cJSON *e = NULL;
                    cJSON_ArrayForEach(e, temp_arr) {
                        if (i < count && cJSON_IsNumber(e)) app_state.weather[i].temperature = e->valuedouble;
                        i++;
                    }
                }

                if (precip_arr && cJSON_IsArray(precip_arr)) {
                    int i = 0; cJSON *e = NULL;
                    cJSON_ArrayForEach(e, precip_arr) {
                        if (i < count && cJSON_IsNumber(e)) app_state.weather[i].precipitation_probability = e->valueint;
                        i++;
                    }
                }

                if (app_temp_arr && cJSON_IsArray(app_temp_arr)) {
                    int i = 0; cJSON *e = NULL;
                    cJSON_ArrayForEach(e, app_temp_arr) {
                        if (i < count && cJSON_IsNumber(e)) app_state.weather[i].apparent_temperature = e->valuedouble;
                        i++;
                    }
                }
                app_state.weather_count = count;
            }
            cJSON_Delete(json);
        }
    }
    free(s.ptr);
    curl_easy_cleanup(curl);
}

// Combined API Refresher
void refresh_all_api_data() {
    fetch_locations();
    fetch_lines();
    fetch_vehicles();
    fetch_timetable_data();
    fetch_weather_data();
}

// --- UI Objects & Controller ---
static lv_obj_t *header_cont;
static lv_obj_t *logo_obj;
static lv_obj_t *brand_label;
static lv_obj_t *clock_label;
static lv_obj_t *date_label;

static lv_obj_t *stop_banner;
static lv_obj_t *next_dep_label;
static lv_obj_t *stop_name_label;

static lv_obj_t *cards_cont;
static lv_obj_t *card_btn[4];
static lv_obj_t *card_label[4];

static lv_obj_t *footer_cont;
static lv_obj_t *footer_label;
static lv_obj_t *screen_badge_label;

void update_clock_and_date() {
    time_t now;
    struct tm *timeinfo;
    char clock_buf[32];
    char date_buf[32];
    time(&now);
    timeinfo = localtime(&now);

    snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d:%02d",
             timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    snprintf(date_buf, sizeof(date_buf), "%02d-%02d-%04d",
             timeinfo->tm_mday, timeinfo->tm_mon + 1, timeinfo->tm_year + 1900);

    lv_label_set_text(clock_label, clock_buf);
    lv_label_set_text(date_label, date_buf);
}

void render_ui_screen() {
    time_t now = time(NULL);

    if (app_state.current_screen == 0) {
        // --- SCREEN 0: TIMETABLE DEPARTURES ---
        lv_label_set_text(screen_badge_label, "[ Odjazdy 1/2 ]");

        // Banner stop name & next departure
        lv_label_set_text(stop_name_label, app_state.config.current_stop_name);

        if (app_state.departures_count > 0 && app_state.departures[0].is_valid) {
            char next_buf[128];
            snprintf(next_buf, sizeof(next_buf), "%s -> %s",
                     app_state.departures[0].line_name,
                     app_state.departures[0].destination);
            lv_label_set_text(next_dep_label, next_buf);
        } else {
            lv_label_set_text(next_dep_label, "Brak najblizszych kursow");
        }

        // Render 4 Departure Cards (Clean non-recolored text layout for perfect spacing)
        int displayed = 0;
        for (int i = 0; i < app_state.departures_count && displayed < 4; i++) {
            if (app_state.departures[i].is_valid) {
                double diff = difftime(app_state.departures[i].timestamp, now);
                if (diff > -60) {
                    int mins = (int)(diff / 60);
                    if (mins < 0) mins = 0;

                    char min_str[32];
                    if (mins == 0) strcpy(min_str, "teraz");
                    else snprintf(min_str, sizeof(min_str), "za %d min", mins);

                    char card_text[256];
                    snprintf(card_text, sizeof(card_text),
                             "L %s -> %s   %s (%02d:%02d)%s",
                             app_state.departures[i].line_name,
                             app_state.departures[i].destination,
                             min_str,
                             app_state.departures[i].hour,
                             app_state.departures[i].min,
                             app_state.departures[i].low_floor ? " [N]" : "");

                    lv_label_set_text(card_label[displayed], card_text);
                    displayed++;
                }
            }
        }

        for (int i = displayed; i < 4; i++) {
            if (i == 0 && app_state.departures_count == 0) {
                lv_label_set_text(card_label[0], "Pobieranie / Brak kursow...");
            } else {
                lv_label_set_text(card_label[i], "");
            }
        }
    } else {
        // --- SCREEN 1: WEATHER FORECAST ---
        lv_label_set_text(screen_badge_label, "[ Pogoda 2/2 ]");

        lv_label_set_text(next_dep_label, "PROGNOZA POGODY");
        lv_label_set_text(stop_name_label, "Czestochowa Centrum");

        // Display up to 4 Weather Cards
        int displayed = 0;
        for (int i = 0; i < app_state.weather_count && displayed < 4; i++) {
            double diff = difftime(app_state.weather[i].timestamp, now);
            if (diff > -3600) {
                struct tm *wt = localtime(&app_state.weather[i].timestamp);
                char card_text[256];

                if (displayed == 0) {
                    snprintf(card_text, sizeof(card_text),
                             "Teraz (%02d:00): %.1fC | Deszcz %d%% | Odcz. %.1fC",
                             wt->tm_hour,
                             app_state.weather[i].temperature,
                             app_state.weather[i].precipitation_probability,
                             app_state.weather[i].apparent_temperature);
                } else {
                    snprintf(card_text, sizeof(card_text),
                             "+%dh (%02d:00): %.1fC | Deszcz %d%% | Odcz. %.1fC",
                             displayed,
                             wt->tm_hour,
                             app_state.weather[i].temperature,
                             app_state.weather[i].precipitation_probability,
                             app_state.weather[i].apparent_temperature);
                }

                lv_label_set_text(card_label[displayed], card_text);
                displayed++;
            }
        }

        for (int i = displayed; i < 4; i++) {
            lv_label_set_text(card_label[i], "");
        }
    }
}

void init_ui() {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xc8d7e3), LV_PART_MAIN);

    // --- 1. Top Header Container ---
    header_cont = lv_obj_create(scr);
    lv_obj_set_size(header_cont, 480, 46);
    lv_obj_set_pos(header_cont, 0, 0);
    lv_obj_set_style_bg_color(header_cont, lv_color_hex(0x233342), LV_PART_MAIN);
    lv_obj_set_style_border_color(header_cont, lv_color_hex(0x17232e), LV_PART_MAIN);
    lv_obj_set_style_border_width(header_cont, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(header_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header_cont, 4, LV_PART_MAIN);
    lv_obj_clear_flag(header_cont, LV_OBJ_FLAG_SCROLLABLE);

    // MPK Logo
    logo_obj = lv_img_create(header_cont);
    lv_img_set_src(logo_obj, &logo_img);
    lv_obj_align(logo_obj, LV_ALIGN_LEFT_MID, 4, 0);

    // Brand Label next to logo
    brand_label = lv_label_create(header_cont);
    lv_label_set_text(brand_label, "MPK");
    lv_obj_set_style_text_color(brand_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(brand_label, &lv_font_montserrat_14, 0);
    lv_obj_align(brand_label, LV_ALIGN_LEFT_MID, 80, 0);

    // Clock Label (Cyan/Light Blue)
    clock_label = lv_label_create(header_cont);
    lv_obj_set_style_text_color(clock_label, lv_color_hex(0x5adbff), 0);
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_22, 0);
    lv_obj_align(clock_label, LV_ALIGN_RIGHT_MID, -6, -8);

    // Date Label
    date_label = lv_label_create(header_cont);
    lv_obj_set_style_text_color(date_label, lv_color_hex(0x9ebfd8), 0);
    lv_obj_set_style_text_font(date_label, &lv_font_montserrat_14, 0);
    lv_obj_align(date_label, LV_ALIGN_RIGHT_MID, -6, 12);

    update_clock_and_date();

    // --- 2. Stop Banner Container (Clean dark blue box) ---
    stop_banner = lv_obj_create(scr);
    lv_obj_set_size(stop_banner, 468, 54);
    lv_obj_set_pos(stop_banner, 6, 48);
    lv_obj_set_style_bg_color(stop_banner, lv_color_hex(0x354859), LV_PART_MAIN);
    lv_obj_set_style_border_color(stop_banner, lv_color_hex(0x22313f), LV_PART_MAIN);
    lv_obj_set_style_border_width(stop_banner, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(stop_banner, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(stop_banner, 4, LV_PART_MAIN);
    lv_obj_clear_flag(stop_banner, LV_OBJ_FLAG_SCROLLABLE);

    // Next Departure Line & Direction
    next_dep_label = lv_label_create(stop_banner);
    lv_label_set_text(next_dep_label, "Pobieranie...");
    lv_obj_set_style_text_color(next_dep_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(next_dep_label, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(next_dep_label, 6, 2);

    // Stop Name Text
    stop_name_label = lv_label_create(stop_banner);
    lv_label_set_text(stop_name_label, app_state.config.current_stop_name);
    lv_obj_set_style_text_color(stop_name_label, lv_color_hex(0x5adbff), 0);
    lv_obj_set_style_text_font(stop_name_label, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(stop_name_label, 6, 24);

    // --- 3. Departure Cards Container (4 Rows) ---
    cards_cont = lv_obj_create(scr);
    lv_obj_set_size(cards_cont, 468, 166);
    lv_obj_set_pos(cards_cont, 6, 104);
    lv_obj_set_style_bg_color(cards_cont, lv_color_hex(0xc8d7e3), LV_PART_MAIN);
    lv_obj_set_style_border_width(cards_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cards_cont, 0, LV_PART_MAIN);
    lv_obj_clear_flag(cards_cont, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 4; i++) {
        card_btn[i] = lv_btn_create(cards_cont);
        lv_obj_set_size(card_btn[i], 468, 38);
        lv_obj_set_pos(card_btn[i], 0, i * 40);

        lv_obj_set_style_bg_color(card_btn[i], lv_color_hex(0xc4daed), LV_PART_MAIN);
        lv_obj_set_style_border_color(card_btn[i], lv_color_hex(0xbd472a), LV_PART_MAIN);
        lv_obj_set_style_border_width(card_btn[i], 2, LV_PART_MAIN);
        lv_obj_set_style_radius(card_btn[i], 4, LV_PART_MAIN);
        lv_obj_set_style_pad_all(card_btn[i], 2, LV_PART_MAIN);

        card_label[i] = lv_label_create(card_btn[i]);
        lv_label_set_recolor(card_label[i], false);
        lv_obj_set_style_text_font(card_label[i], &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(card_label[i], lv_color_hex(0x0a1c2a), 0);
        lv_obj_align(card_label[i], LV_ALIGN_LEFT_MID, 6, 0);
        lv_label_set_text(card_label[i], "Pobieranie...");
    }

    // --- 4. Footer Container ---
    footer_cont = lv_obj_create(scr);
    lv_obj_set_size(footer_cont, 480, 26);
    lv_obj_set_pos(footer_cont, 0, 274);
    lv_obj_set_style_bg_color(footer_cont, lv_color_hex(0x1d2936), LV_PART_MAIN);
    lv_obj_set_style_border_width(footer_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(footer_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(footer_cont, 2, LV_PART_MAIN);
    lv_obj_clear_flag(footer_cont, LV_OBJ_FLAG_SCROLLABLE);

    footer_label = lv_label_create(footer_cont);
    lv_label_set_text(footer_label, "Fundusze Europejskie  |  Slaskie. Pozytywna energia");
    lv_obj_set_style_text_color(footer_label, lv_color_hex(0x7692a8), 0);
    lv_obj_set_style_text_font(footer_label, &lv_font_montserrat_12, 0);
    lv_obj_align(footer_label, LV_ALIGN_LEFT_MID, 4, 0);

    screen_badge_label = lv_label_create(footer_cont);
    lv_label_set_text(screen_badge_label, "[ Odjazdy 1/2 ]");
    lv_obj_set_style_text_color(screen_badge_label, lv_color_hex(0x5adbff), 0);
    lv_obj_set_style_text_font(screen_badge_label, &lv_font_montserrat_12, 0);
    lv_obj_align(screen_badge_label, LV_ALIGN_RIGHT_MID, -4, 0);
}

int main(int argc, char **argv) {
    int use_file = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            use_file = 1;
            break;
        }
    }

    lv_init();
    fbdev_init(use_file);
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

    init_ui();

    refresh_all_api_data();
    render_ui_screen();

    uint32_t last_clock_tick = 0;
    uint32_t last_ui_tick = 0;
    uint32_t last_net_tick = 0;
    uint32_t last_swap_tick = 0;

    while(1) {
        lv_tick_inc(5);
        lv_timer_handler();

        uint32_t now_tick = lv_tick_get();

        if (now_tick - last_clock_tick >= 1000) {
            update_clock_and_date();
            last_clock_tick = now_tick;
        }

        if (now_tick - last_ui_tick >= 2000) {
            render_ui_screen();
            last_ui_tick = now_tick;
        }

        if (now_tick - last_swap_tick >= 8000) {
            app_state.current_screen = (app_state.current_screen == 0) ? 1 : 0;
            render_ui_screen();
            last_swap_tick = now_tick;
        }

        if (now_tick - last_net_tick >= 120000) {
            refresh_all_api_data();
            render_ui_screen();
            last_net_tick = now_tick;
        }

        usleep(5000);
    }

    return 0;
}

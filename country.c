/*
 * Country Search Map — C + SQLite + Raylib (single-file app logic: country.c)
 * Data: countries.csv (name, capital, lat, lon)
 *
 * Map basemap: assets/world_map.jpg
 *   Wikimedia Commons — "Equirectangular projection SW"
 *   https://upload.wikimedia.org/wikipedia/commons/thumb/8/83/Equirectangular_projection_SW.jpg/1280px-Equirectangular_projection_SW.jpg
 *   Projection in app: x=(lon+180)/360*W, y=(90-lat)/180*H
 *
 * Coordinates: Open-Meteo Geocoding API → countries.csv
 *   https://geocoding-api.open-meteo.com/v1/search
 *   (manual fixes for unmatched / disputed capitals)
 *
 * Stack: SQLite amalgamation, Raylib 5.5, Malgun Gothic (assets/malgun.ttf)
 *
 * Country flags: assets/flags/*.png (80x60)
 *   Primary: Flagcdn.com / Flagpedia.net — https://flagcdn.com/ https://flagpedia.net/
 *   Fallback (limited recognition): Wikimedia Commons SVG thumbnails
 *   See assets/flags/SOURCES.txt
 *
 * Top-3 dense major cities: cities table (FK -> countries), seed cities.csv
 *   See assets/cities/SOURCES.txt
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"
#include "raylib.h"

/* Avoid windows.h — conflicts with Raylib (Rectangle, CloseWindow, ...) */
__declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int wCodePageID);
__declspec(dllimport) int __stdcall SetConsoleCP(unsigned int wCodePageID);

#define MAX_LINE_LENGTH 512
#define MAX_NAME_LENGTH 128
#define MAX_INPUT_LENGTH 256
#define MAX_STATUS_LENGTH 256
#define MAX_LIST_ROWS 512
#define DB_PATH "countries.db"
#define CSV_PATH "countries.csv"
#define MAP_PATH "assets/world_map.jpg"
#define FONT_PATH "assets/malgun.ttf"
#define FLAG_DIR "assets/flags"
#define FLAG_MAP_PATH "assets/flags/iso_map.csv"
#define CITIES_CSV_PATH "cities.csv"
#define TOP_BAR_H 72.0f
#define LIST_ROW_H 28.0f
#define FLAG_DRAW_W 96.0f
#define FLAG_DRAW_H 72.0f
#define CITY_IMG_W 140.0f
#define CITY_IMG_H 100.0f
#define MAX_ISO_ENTRIES 512
#define MAX_ISO_CODE 16
#define MAX_TOP_CITIES 3
#define MAX_IMAGE_PATH 256

typedef struct {
    char name[MAX_NAME_LENGTH];
    char capital[MAX_NAME_LENGTH];
    double lat;
    double lon;
} Country;

typedef struct {
    char name[MAX_NAME_LENGTH];
    char iso[MAX_ISO_CODE];
} IsoEntry;

typedef struct {
    char name[MAX_NAME_LENGTH];
    double lat;
    double lon;
    double density;
    int rank;
    char image_path[MAX_IMAGE_PATH];
} CityInfo;

typedef enum {
    MODE_SEARCH = 0,
    MODE_CREATE,
    MODE_UPDATE,
    MODE_DELETE,
    MODE_LIST
} AppMode;

typedef enum {
    FIELD_NAME = 0,
    FIELD_CAPITAL,
    FIELD_LAT,
    FIELD_LON,
    FIELD_DONE
} FormField;

static int table_has_lat_lon(sqlite3 *db)
{
    sqlite3_stmt *stmt = NULL;
    int has_lat = 0;
    int has_lon = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(countries);", -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *col = (const char *)sqlite3_column_text(stmt, 1);
        if (col && strcmp(col, "lat") == 0) {
            has_lat = 1;
        }
        if (col && strcmp(col, "lon") == 0) {
            has_lon = 1;
        }
    }
    sqlite3_finalize(stmt);
    return has_lat && has_lon;
}

static int create_table(sqlite3 *db)
{
    sqlite3_exec(db, "PRAGMA foreign_keys = ON;", 0, 0, NULL);

    if (!table_has_lat_lon(db)) {
        sqlite3_exec(db, "DROP TABLE IF EXISTS cities;", 0, 0, NULL);
        sqlite3_exec(db, "DROP TABLE IF EXISTS countries;", 0, 0, NULL);
    }

    const char *sql =
        "CREATE TABLE IF NOT EXISTS countries ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "name TEXT UNIQUE NOT NULL, "
        "capital TEXT NOT NULL, "
        "lat REAL NOT NULL, "
        "lon REAL NOT NULL);";
    char *err = NULL;
    if (sqlite3_exec(db, sql, 0, 0, &err) != SQLITE_OK) {
        fprintf(stderr, "[ERROR] create table: %s\n", err);
        sqlite3_free(err);
        return 0;
    }

    const char *cities_sql =
        "CREATE TABLE IF NOT EXISTS cities ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "country_id INTEGER NOT NULL, "
        "name TEXT NOT NULL, "
        "lat REAL NOT NULL, "
        "lon REAL NOT NULL, "
        "density REAL NOT NULL, "
        "rank INTEGER NOT NULL CHECK(rank BETWEEN 1 AND 3), "
        "image_path TEXT NOT NULL, "
        "UNIQUE(country_id, rank), "
        "FOREIGN KEY(country_id) REFERENCES countries(id) ON DELETE CASCADE);";
    if (sqlite3_exec(db, cities_sql, 0, 0, &err) != SQLITE_OK) {
        fprintf(stderr, "[ERROR] create cities: %s\n", err);
        sqlite3_free(err);
        return 0;
    }
    return 1;
}

static int count_rows(sqlite3 *db)
{
    sqlite3_stmt *stmt = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM countries;", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            n = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return n;
}

static int count_city_rows(sqlite3 *db)
{
    sqlite3_stmt *stmt = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM cities;", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            n = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return n;
}

static int country_id_by_name(sqlite3 *db, const char *name)
{
    const char *sql = "SELECT id FROM countries WHERE name = ? LIMIT 1;";
    sqlite3_stmt *stmt = NULL;
    int id = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        id = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return id;
}

static void load_cities_csv(sqlite3 *db, const char *csv_filename)
{
    FILE *file = fopen(csv_filename, "r");
    if (!file) {
        return;
    }

    char line[MAX_LINE_LENGTH];
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return;
    }

    sqlite3_exec(db, "PRAGMA foreign_keys = ON;", 0, 0, NULL);
    sqlite3_exec(db, "BEGIN TRANSACTION;", 0, 0, NULL);

    const char *sql =
        "INSERT OR REPLACE INTO cities "
        "(country_id, name, lat, lon, density, rank, image_path) "
        "VALUES (?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[ERROR] prepare cities insert: %s\n", sqlite3_errmsg(db));
        fclose(file);
        return;
    }

    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 0) {
            continue;
        }

        char *country_name = strtok(line, ",");
        char *rank_s = strtok(NULL, ",");
        char *city_name = strtok(NULL, ",");
        char *lat_s = strtok(NULL, ",");
        char *lon_s = strtok(NULL, ",");
        char *dens_s = strtok(NULL, ",");
        char *image_path = strtok(NULL, ",");
        if (!country_name || !rank_s || !city_name || !lat_s || !lon_s || !dens_s || !image_path) {
            continue;
        }

        int cid = country_id_by_name(db, country_name);
        if (cid < 0) {
            continue;
        }

        sqlite3_bind_int(stmt, 1, cid);
        sqlite3_bind_text(stmt, 2, city_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, atof(lat_s));
        sqlite3_bind_double(stmt, 4, atof(lon_s));
        sqlite3_bind_double(stmt, 5, atof(dens_s));
        sqlite3_bind_int(stmt, 6, atoi(rank_s));
        sqlite3_bind_text(stmt, 7, image_path, -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }

    sqlite3_finalize(stmt);
    sqlite3_exec(db, "COMMIT;", 0, 0, NULL);
    fclose(file);
}

static int load_top_cities(sqlite3 *db, const char *country_name, CityInfo *out, int cap)
{
    const char *sql =
        "SELECT c.name, c.lat, c.lon, c.density, c.rank, c.image_path "
        "FROM cities c "
        "JOIN countries co ON co.id = c.country_id "
        "WHERE co.name = ? "
        "ORDER BY c.rank ASC "
        "LIMIT ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(stmt, 1, country_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, cap);
    int n = 0;
    while (n < cap && sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 0);
        const unsigned char *img = sqlite3_column_text(stmt, 5);
        if (!name) {
            continue;
        }
        strncpy(out[n].name, (const char *)name, MAX_NAME_LENGTH - 1);
        out[n].name[MAX_NAME_LENGTH - 1] = 0;
        out[n].lat = sqlite3_column_double(stmt, 1);
        out[n].lon = sqlite3_column_double(stmt, 2);
        out[n].density = sqlite3_column_double(stmt, 3);
        out[n].rank = sqlite3_column_int(stmt, 4);
        if (img) {
            strncpy(out[n].image_path, (const char *)img, MAX_IMAGE_PATH - 1);
            out[n].image_path[MAX_IMAGE_PATH - 1] = 0;
        } else {
            out[n].image_path[0] = 0;
        }
        n++;
    }
    sqlite3_finalize(stmt);
    return n;
}

static void load_csv_data(sqlite3 *db, const char *csv_filename)
{
    FILE *file = fopen(csv_filename, "r");
    if (!file) {
        return;
    }

    char line[MAX_LINE_LENGTH];
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return;
    }

    sqlite3_exec(db, "BEGIN TRANSACTION;", 0, 0, NULL);

    const char *sql =
        "INSERT OR IGNORE INTO countries (name, capital, lat, lon) VALUES (?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "[ERROR] prepare insert: %s\n", sqlite3_errmsg(db));
        fclose(file);
        return;
    }

    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 0) {
            continue;
        }

        char *country = strtok(line, ",");
        char *capital = strtok(NULL, ",");
        char *lat_s = strtok(NULL, ",");
        char *lon_s = strtok(NULL, ",");
        if (!country || !capital || !lat_s || !lon_s) {
            continue;
        }

        sqlite3_bind_text(stmt, 1, country, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, capital, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, atof(lat_s));
        sqlite3_bind_double(stmt, 4, atof(lon_s));
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }

    sqlite3_finalize(stmt);
    sqlite3_exec(db, "COMMIT;", 0, 0, NULL);
    fclose(file);
}

static int utf8_prev_char_len(const char *s, int len)
{
    if (len <= 0) {
        return 0;
    }
    int i = len - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) {
        i--;
    }
    return len - i;
}

static void utf8_append_codepoint(char *buf, int cap, int *len, int cp)
{
    unsigned char tmp[4];
    int n = 0;
    if (cp <= 0x7F) {
        tmp[0] = (unsigned char)cp;
        n = 1;
    } else if (cp <= 0x7FF) {
        tmp[0] = (unsigned char)(0xC0 | (cp >> 6));
        tmp[1] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp <= 0xFFFF) {
        tmp[0] = (unsigned char)(0xE0 | (cp >> 12));
        tmp[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        tmp[0] = (unsigned char)(0xF0 | (cp >> 18));
        tmp[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (*len + n >= cap) {
        return;
    }
    memcpy(buf + *len, tmp, (size_t)n);
    *len += n;
    buf[*len] = 0;
}

static void handle_text_input(char *buf, int cap, int *len)
{
    int key = GetCharPressed();
    while (key > 0) {
        if (key >= 32) {
            utf8_append_codepoint(buf, cap, len, key);
        }
        key = GetCharPressed();
    }
    if (IsKeyPressed(KEY_BACKSPACE) && *len > 0) {
        int cut = utf8_prev_char_len(buf, *len);
        *len -= cut;
        buf[*len] = 0;
    }
}

static Font load_korean_font(int font_size)
{
    /* ASCII + Hangul syllables — required for visible Korean UI */
    const int ascii_count = 95; /* 32..126 */
    const int hangul_first = 0xAC00;
    const int hangul_last = 0xD7A3;
    const int hangul_count = hangul_last - hangul_first + 1;
    const int total = ascii_count + hangul_count;

    int *codepoints = (int *)malloc(sizeof(int) * (size_t)total);
    if (!codepoints) {
        return GetFontDefault();
    }

    int n = 0;
    for (int i = 32; i <= 126; i++) {
        codepoints[n++] = i;
    }
    for (int cp = hangul_first; cp <= hangul_last; cp++) {
        codepoints[n++] = cp;
    }

    Font font = LoadFontEx(FONT_PATH, font_size, codepoints, n);
    free(codepoints);
    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
    return font;
}

static Vector2 latlon_to_pixel(double lat, double lon, float map_w, float map_h)
{
    float x = (float)((lon + 180.0) / 360.0 * map_w);
    float y = (float)((90.0 - lat) / 180.0 * map_h);
    return (Vector2){x, y};
}

static int fill_country_from_stmt(sqlite3_stmt *stmt, Country *out)
{
    const unsigned char *name = sqlite3_column_text(stmt, 0);
    const unsigned char *capital = sqlite3_column_text(stmt, 1);
    if (!name || !capital) {
        return 0;
    }
    strncpy(out->name, (const char *)name, MAX_NAME_LENGTH - 1);
    strncpy(out->capital, (const char *)capital, MAX_NAME_LENGTH - 1);
    out->name[MAX_NAME_LENGTH - 1] = 0;
    out->capital[MAX_NAME_LENGTH - 1] = 0;
    out->lat = sqlite3_column_double(stmt, 2);
    out->lon = sqlite3_column_double(stmt, 3);
    return 1;
}

static int find_country_exact(sqlite3 *db, const char *name, Country *out)
{
    const char *sql =
        "SELECT name, capital, lat, lon FROM countries WHERE name = ? LIMIT 1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    int ok = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ok = fill_country_from_stmt(stmt, out);
    }
    sqlite3_finalize(stmt);
    return ok;
}

static int find_capital_exact(sqlite3 *db, const char *capital, Country *out)
{
    const char *sql =
        "SELECT name, capital, lat, lon FROM countries WHERE capital = ? ORDER BY name LIMIT 1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(stmt, 1, capital, -1, SQLITE_TRANSIENT);
    int ok = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ok = fill_country_from_stmt(stmt, out);
    }
    sqlite3_finalize(stmt);
    return ok;
}

static int find_country_prefix(sqlite3 *db, const char *name, Country *out)
{
    const char *sql =
        "SELECT name, capital, lat, lon FROM countries WHERE name LIKE ? ORDER BY name LIMIT 1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    char pattern[MAX_NAME_LENGTH + 2];
    snprintf(pattern, sizeof(pattern), "%s%%", name);
    sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
    int ok = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ok = fill_country_from_stmt(stmt, out);
    }
    sqlite3_finalize(stmt);
    return ok;
}

static int find_capital_prefix(sqlite3 *db, const char *capital, Country *out)
{
    const char *sql =
        "SELECT name, capital, lat, lon FROM countries WHERE capital LIKE ? ORDER BY capital LIMIT 1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    char pattern[MAX_NAME_LENGTH + 2];
    snprintf(pattern, sizeof(pattern), "%s%%", capital);
    sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT);
    int ok = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ok = fill_country_from_stmt(stmt, out);
    }
    sqlite3_finalize(stmt);
    return ok;
}

static int search_country(sqlite3 *db, const char *query, Country *out)
{
    if (!query || query[0] == 0) {
        return 0;
    }
    /* Prefer exact name, then exact capital, then prefixes */
    if (find_country_exact(db, query, out)) {
        return 1;
    }
    if (find_capital_exact(db, query, out)) {
        return 1;
    }
    if (find_country_prefix(db, query, out)) {
        return 1;
    }
    return find_capital_prefix(db, query, out);
}

static int load_iso_map(IsoEntry *out, int cap)
{
    FILE *f = fopen(FLAG_MAP_PATH, "r");
    if (!f) {
        return 0;
    }
    char line[MAX_LINE_LENGTH];
    if (fgets(line, sizeof(line), f) == NULL) {
        fclose(f);
        return 0;
    }
    int n = 0;
    while (n < cap && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 0) {
            continue;
        }
        char *name = strtok(line, ",");
        char *iso = strtok(NULL, ",");
        if (!name || !iso) {
            continue;
        }
        strncpy(out[n].name, name, MAX_NAME_LENGTH - 1);
        strncpy(out[n].iso, iso, MAX_ISO_CODE - 1);
        out[n].name[MAX_NAME_LENGTH - 1] = 0;
        out[n].iso[MAX_ISO_CODE - 1] = 0;
        n++;
    }
    fclose(f);
    return n;
}

static const char *lookup_iso(const IsoEntry *map, int map_n, const char *country_name)
{
    for (int i = 0; i < map_n; i++) {
        if (strcmp(map[i].name, country_name) == 0) {
            return map[i].iso;
        }
    }
    return NULL;
}

static void unload_flag_tex(Texture2D *tex, int *has_flag)
{
    if (*has_flag && tex->id != 0) {
        UnloadTexture(*tex);
        *tex = (Texture2D){0};
    }
    *has_flag = 0;
}

static void load_flag_for_country(const IsoEntry *map, int map_n, const char *country_name,
                                  Texture2D *tex, int *has_flag)
{
    unload_flag_tex(tex, has_flag);
    const char *iso = lookup_iso(map, map_n, country_name);
    if (!iso) {
        return;
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/%s.png", FLAG_DIR, iso);
    if (!FileExists(path)) {
        return;
    }
    Image img = LoadImage(path);
    if (img.data == NULL) {
        return;
    }
    *tex = LoadTextureFromImage(img);
    UnloadImage(img);
    *has_flag = (tex->id != 0);
    if (*has_flag) {
        SetTextureFilter(*tex, TEXTURE_FILTER_BILINEAR);
    }
}

static int db_insert(sqlite3 *db, const Country *c)
{
    const char *sql =
        "INSERT INTO countries (name, capital, lat, lon) VALUES (?, ?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(stmt, 1, c->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, c->capital, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, c->lat);
    sqlite3_bind_double(stmt, 4, c->lon);
    int ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

static int db_update(sqlite3 *db, const Country *c)
{
    const char *sql =
        "UPDATE countries SET capital = ?, lat = ?, lon = ? WHERE name = ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(stmt, 1, c->capital, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, c->lat);
    sqlite3_bind_double(stmt, 3, c->lon);
    sqlite3_bind_text(stmt, 4, c->name, -1, SQLITE_TRANSIENT);
    int ok = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0);
    sqlite3_finalize(stmt);
    return ok;
}

static int db_delete(sqlite3 *db, const char *name)
{
    const char *sql = "DELETE FROM countries WHERE name = ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    int ok = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0);
    sqlite3_finalize(stmt);
    return ok;
}

static int load_country_list(sqlite3 *db, Country *out, int cap)
{
    const char *sql =
        "SELECT name, capital, lat, lon FROM countries ORDER BY name;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    int n = 0;
    while (n < cap && sqlite3_step(stmt) == SQLITE_ROW) {
        strncpy(out[n].name, (const char *)sqlite3_column_text(stmt, 0), MAX_NAME_LENGTH - 1);
        strncpy(out[n].capital, (const char *)sqlite3_column_text(stmt, 1), MAX_NAME_LENGTH - 1);
        out[n].name[MAX_NAME_LENGTH - 1] = 0;
        out[n].capital[MAX_NAME_LENGTH - 1] = 0;
        out[n].lat = sqlite3_column_double(stmt, 2);
        out[n].lon = sqlite3_column_double(stmt, 3);
        n++;
    }
    sqlite3_finalize(stmt);
    return n;
}

static void list_layout(float dw, float dh, float *panel_x, float *panel_y,
                        float *panel_w, float *panel_h, float *rows_y, int *visible)
{
    *panel_x = 40.0f;
    *panel_y = TOP_BAR_H + 20.0f;
    *panel_w = dw - 80.0f;
    *panel_h = dh - TOP_BAR_H - 40.0f;
    if (*panel_w < 200.0f) {
        *panel_w = 200.0f;
    }
    if (*panel_h < 120.0f) {
        *panel_h = 120.0f;
    }
    float header_h = 40.0f;
    float footer_h = 32.0f;
    float list_top = *panel_y + header_h;
    float list_h = *panel_h - header_h - footer_h;
    *visible = (int)(list_h / LIST_ROW_H);
    if (*visible < 1) {
        *visible = 1;
    }
    *rows_y = list_top + 22.0f;
}

static int list_row_at_pos(float dw, float dh, int scroll, int count, Vector2 mouse)
{
    float panel_x, panel_y, panel_w, panel_h, rows_y;
    int visible;
    list_layout(dw, dh, &panel_x, &panel_y, &panel_w, &panel_h, &rows_y, &visible);
    if (mouse.x < panel_x + 8 || mouse.x > panel_x + panel_w - 8) {
        return -1;
    }
    if (mouse.y < rows_y - 2 || mouse.y > panel_y + panel_h - 32) {
        return -1;
    }
    int row = (int)((mouse.y - (rows_y - 2)) / LIST_ROW_H);
    int idx = scroll + row;
    if (idx < 0 || idx >= count || row >= visible) {
        return -1;
    }
    return idx;
}

static void draw_db_list(Font font, int has_font, const Country *list, int count,
                         int scroll, float dw, float dh)
{
    float panel_x, panel_y, panel_w, panel_h, rows_y;
    int visible;
    list_layout(dw, dh, &panel_x, &panel_y, &panel_w, &panel_h, &rows_y, &visible);

    if (scroll < 0) {
        scroll = 0;
    }
    int max_scroll = count - visible;
    if (max_scroll < 0) {
        max_scroll = 0;
    }
    if (scroll > max_scroll) {
        scroll = max_scroll;
    }

    DrawRectangleRounded(
        (Rectangle){panel_x, panel_y, panel_w, panel_h},
        0.04f, 8, (Color){10, 14, 22, 230});
    DrawRectangleRoundedLines(
        (Rectangle){panel_x, panel_y, panel_w, panel_h},
        0.04f, 8, (Color){70, 120, 180, 255});

    float list_top = panel_y + 40.0f;
    char title[160];
    snprintf(title, sizeof(title), "DB 목록  (%d개)  —  클릭 선택 / ↑↓ 스크롤 / ESC 닫기", count);
    if (has_font) {
        DrawTextEx(font, title, (Vector2){panel_x + 16, panel_y + 10}, 22, 1, RAYWHITE);
        DrawTextEx(font, "국가", (Vector2){panel_x + 16, list_top - 2}, 16, 1, (Color){150, 180, 220, 255});
        DrawTextEx(font, "수도", (Vector2){panel_x + panel_w * 0.38f, list_top - 2}, 16, 1, (Color){150, 180, 220, 255});
        DrawTextEx(font, "위도", (Vector2){panel_x + panel_w * 0.62f, list_top - 2}, 16, 1, (Color){150, 180, 220, 255});
        DrawTextEx(font, "경도", (Vector2){panel_x + panel_w * 0.78f, list_top - 2}, 16, 1, (Color){150, 180, 220, 255});
    } else {
        DrawText(title, (int)panel_x + 16, (int)panel_y + 12, 18, RAYWHITE);
    }

    DrawLine((int)(panel_x + 12), (int)(list_top + 18),
             (int)(panel_x + panel_w - 12), (int)(list_top + 18),
             (Color){60, 90, 130, 255});

    int end = scroll + visible;
    if (end > count) {
        end = count;
    }
    for (int i = scroll; i < end; i++) {
        float y = rows_y + (float)(i - scroll) * LIST_ROW_H;
        if ((i - scroll) % 2 == 0) {
            DrawRectangle((int)(panel_x + 8), (int)y - 2,
                          (int)(panel_w - 16), (int)LIST_ROW_H,
                          (Color){255, 255, 255, 12});
        }
        char lat_s[32];
        char lon_s[32];
        snprintf(lat_s, sizeof(lat_s), "%.4f", list[i].lat);
        snprintf(lon_s, sizeof(lon_s), "%.4f", list[i].lon);
        if (has_font) {
            DrawTextEx(font, list[i].name, (Vector2){panel_x + 16, y}, 18, 1, RAYWHITE);
            DrawTextEx(font, list[i].capital, (Vector2){panel_x + panel_w * 0.38f, y}, 18, 1, (Color){220, 230, 245, 255});
            DrawTextEx(font, lat_s, (Vector2){panel_x + panel_w * 0.62f, y}, 18, 1, (Color){255, 230, 120, 255});
            DrawTextEx(font, lon_s, (Vector2){panel_x + panel_w * 0.78f, y}, 18, 1, (Color){255, 230, 120, 255});
        } else {
            DrawText(list[i].name, (int)panel_x + 16, (int)y, 16, RAYWHITE);
            DrawText(list[i].capital, (int)(panel_x + panel_w * 0.38f), (int)y, 16, LIGHTGRAY);
            DrawText(lat_s, (int)(panel_x + panel_w * 0.62f), (int)y, 16, YELLOW);
            DrawText(lon_s, (int)(panel_x + panel_w * 0.78f), (int)y, 16, YELLOW);
        }
    }

    char foot[64];
    if (count == 0) {
        snprintf(foot, sizeof(foot), "저장된 국가가 없습니다");
    } else {
        int from = scroll + 1;
        int to = end;
        snprintf(foot, sizeof(foot), "%d–%d / %d", from, to, count);
    }
    if (has_font) {
        DrawTextEx(font, foot, (Vector2){panel_x + 16, panel_y + panel_h - 26}, 16, 1, LIGHTGRAY);
    } else {
        DrawText(foot, (int)panel_x + 16, (int)(panel_y + panel_h - 24), 14, LIGHTGRAY);
    }
}

static void draw_country_card(Font font, int has_font, const Country *c,
                              Texture2D flag, int has_flag, float dw, float dh)
{
    float card_w = 300.0f;
    float card_h = 168.0f;
    float x = dw - card_w - 20.0f;
    float y = dh - card_h - 20.0f;
    if (x < 16.0f) {
        x = 16.0f;
    }
    if (y < TOP_BAR_H + 16.0f) {
        y = TOP_BAR_H + 16.0f;
    }

    DrawRectangleRounded((Rectangle){x, y, card_w, card_h}, 0.08f, 8, (Color){8, 12, 20, 230});
    DrawRectangleRoundedLines((Rectangle){x, y, card_w, card_h}, 0.08f, 8, (Color){90, 150, 220, 255});

    float img_x = x + 16.0f;
    float img_y = y + 16.0f;
    if (has_flag) {
        DrawRectangleLinesEx((Rectangle){img_x - 1, img_y - 1, FLAG_DRAW_W + 2, FLAG_DRAW_H + 2}, 1, LIGHTGRAY);
        DrawTexturePro(
            flag,
            (Rectangle){0, 0, (float)flag.width, (float)flag.height},
            (Rectangle){img_x, img_y, FLAG_DRAW_W, FLAG_DRAW_H},
            (Vector2){0, 0}, 0.0f, WHITE);
    } else {
        DrawRectangle((int)img_x, (int)img_y, (int)FLAG_DRAW_W, (int)FLAG_DRAW_H, (Color){40, 50, 70, 255});
        if (has_font) {
            DrawTextEx(font, "이미지 없음", (Vector2){img_x + 8, img_y + 26}, 16, 1, GRAY);
        } else {
            DrawText("no img", (int)img_x + 16, (int)img_y + 28, 16, GRAY);
        }
    }

    float tx = img_x + FLAG_DRAW_W + 14.0f;
    char line1[160];
    char line2[160];
    char line3[96];
    snprintf(line1, sizeof(line1), "%.40s", c->name);
    snprintf(line2, sizeof(line2), "수도: %.40s", c->capital);
    snprintf(line3, sizeof(line3), "위도 %.4f", c->lat);
    char line4[96];
    snprintf(line4, sizeof(line4), "경도 %.4f", c->lon);

    if (has_font) {
        DrawTextEx(font, line1, (Vector2){tx, img_y}, 22, 1, RAYWHITE);
        DrawTextEx(font, line2, (Vector2){tx, img_y + 30}, 18, 1, (Color){200, 220, 255, 255});
        DrawTextEx(font, line3, (Vector2){tx, img_y + 58}, 16, 1, (Color){255, 230, 120, 255});
        DrawTextEx(font, line4, (Vector2){tx, img_y + 80}, 16, 1, (Color){255, 230, 120, 255});
        DrawTextEx(font, "핀 클릭: 3대 도시", (Vector2){x + 16, y + card_h - 28}, 14, 1, LIGHTGRAY);
    } else {
        DrawText(line1, (int)tx, (int)img_y, 18, RAYWHITE);
        DrawText(line2, (int)tx, (int)img_y + 28, 14, SKYBLUE);
        DrawText(line3, (int)tx, (int)img_y + 52, 14, YELLOW);
        DrawText(line4, (int)tx, (int)img_y + 72, 14, YELLOW);
    }
}

static void unload_city_textures(Texture2D *tex, int *has_tex, int n)
{
    for (int i = 0; i < n; i++) {
        if (has_tex[i] && tex[i].id != 0) {
            UnloadTexture(tex[i]);
            tex[i] = (Texture2D){0};
        }
        has_tex[i] = 0;
    }
}

static void load_city_textures(const CityInfo *cities, int count,
                               Texture2D *tex, int *has_tex)
{
    unload_city_textures(tex, has_tex, MAX_TOP_CITIES);
    for (int i = 0; i < count && i < MAX_TOP_CITIES; i++) {
        if (cities[i].image_path[0] == 0 || !FileExists(cities[i].image_path)) {
            continue;
        }
        /* Skip broken/placeholder downloads (e.g. 1x1 PNG stubs). */
        if (GetFileLength(cities[i].image_path) < 800) {
            continue;
        }
        Image img = LoadImage(cities[i].image_path);
        if (img.data == NULL || img.width < 8 || img.height < 8) {
            if (img.data != NULL) {
                UnloadImage(img);
            }
            continue;
        }
        tex[i] = LoadTextureFromImage(img);
        UnloadImage(img);
        has_tex[i] = (tex[i].id != 0);
        if (has_tex[i]) {
            SetTextureFilter(tex[i], TEXTURE_FILTER_BILINEAR);
        }
    }
}

static void draw_cities_panel(Font font, int has_font, const char *country_name,
                              const CityInfo *cities, int count,
                              Texture2D *tex, int *has_tex,
                              float dw, float dh)
{
    float panel_w = 520.0f;
    float panel_h = 260.0f;
    float x = 20.0f;
    float y = dh - panel_h - 20.0f;
    if (y < TOP_BAR_H + 12.0f) {
        y = TOP_BAR_H + 12.0f;
    }
    if (panel_w > dw - 40.0f) {
        panel_w = dw - 40.0f;
    }

    DrawRectangleRounded((Rectangle){x, y, panel_w, panel_h}, 0.06f, 8, (Color){8, 12, 20, 235});
    DrawRectangleRoundedLines((Rectangle){x, y, panel_w, panel_h}, 0.06f, 8, (Color){100, 170, 230, 255});

    char title[192];
    snprintf(title, sizeof(title), "%.40s — 주요 3도시", country_name);
    if (has_font) {
        DrawTextEx(font, title, (Vector2){x + 14, y + 10}, 20, 1, RAYWHITE);
    } else {
        DrawText(title, (int)x + 14, (int)y + 12, 16, RAYWHITE);
    }

    if (count <= 0) {
        if (has_font) {
            DrawTextEx(font, "주요 도시 데이터 없음", (Vector2){x + 14, y + 80}, 20, 1, ORANGE);
        } else {
            DrawText("no city data", (int)x + 14, (int)y + 80, 18, ORANGE);
        }
        return;
    }

    float slot_w = (panel_w - 28.0f) / 3.0f;
    for (int i = 0; i < count && i < MAX_TOP_CITIES; i++) {
        float sx = x + 14.0f + slot_w * (float)i;
        float sy = y + 44.0f;
        float img_w = slot_w - 12.0f;
        if (img_w > CITY_IMG_W) {
            img_w = CITY_IMG_W;
        }
        float img_h = CITY_IMG_H;

        DrawRectangle((int)sx, (int)sy, (int)img_w, (int)img_h, (Color){30, 40, 55, 255});
        if (has_tex[i]) {
            DrawTexturePro(
                tex[i],
                (Rectangle){0, 0, (float)tex[i].width, (float)tex[i].height},
                (Rectangle){sx, sy, img_w, img_h},
                (Vector2){0, 0}, 0.0f, WHITE);
        } else if (has_font) {
            DrawTextEx(font, "이미지 없음", (Vector2){sx + 8, sy + 40}, 14, 1, GRAY);
        }

        char name_line[160];
        char coord_line[96];
        snprintf(name_line, sizeof(name_line), "%d. %.40s", cities[i].rank, cities[i].name);
        snprintf(coord_line, sizeof(coord_line), "(%.2f, %.2f)", cities[i].lat, cities[i].lon);

        if (has_font) {
            DrawTextEx(font, name_line, (Vector2){sx, sy + img_h + 8}, 16, 1, RAYWHITE);
            DrawTextEx(font, coord_line, (Vector2){sx, sy + img_h + 30}, 14, 1, (Color){255, 230, 120, 255});
        } else {
            DrawText(name_line, (int)sx, (int)(sy + img_h + 8), 14, RAYWHITE);
            DrawText(coord_line, (int)sx, (int)(sy + img_h + 28), 12, YELLOW);
        }
    }
}

static void draw_pin(Vector2 pin, Font font, const Country *c, int has_selection)
{
    if (!has_selection) {
        return;
    }

    DrawCircleV(pin, 11.0f, RED);
    DrawCircleLines((int)pin.x, (int)pin.y, 16, YELLOW);
    DrawLine((int)pin.x, (int)pin.y - 28, (int)pin.x, (int)pin.y - 11, YELLOW);
    DrawCircle((int)pin.x, (int)pin.y - 28, 4, YELLOW);

    char label[256];
    char coords[128];
    snprintf(label, sizeof(label), "%.40s / %.40s", c->name, c->capital);
    snprintf(coords, sizeof(coords), "(%.4f, %.4f)", c->lat, c->lon);

    Vector2 label_pos = {pin.x + 18.0f, pin.y - 34.0f};
    Vector2 size = MeasureTextEx(font, label, 22, 1);
    DrawRectangleRounded(
        (Rectangle){label_pos.x - 6, label_pos.y - 4, size.x + 12, 48},
        0.2f, 6, (Color){0, 0, 0, 170});
    DrawTextEx(font, label, label_pos, 22, 1, RAYWHITE);
    DrawTextEx(font, coords, (Vector2){label_pos.x, label_pos.y + 22}, 18, 1, (Color){255, 230, 120, 255});
}

static void reset_form(char fields[4][MAX_INPUT_LENGTH], int lens[4], FormField *field)
{
    for (int i = 0; i < 4; i++) {
        fields[i][0] = 0;
        lens[i] = 0;
    }
    *field = FIELD_NAME;
}

static void run_map_app(sqlite3 *db)
{
    const int screen_w = 1280;
    const int screen_h = 720;

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(screen_w, screen_h, "Country Search Map (C + SQLite + Raylib)");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL); /* ESC handled manually: cancel form / quit */

    Texture2D map_tex = {0};
    int has_map = 0;
    if (FileExists(MAP_PATH)) {
        Image img = LoadImage(MAP_PATH);
        if (img.data != NULL) {
            map_tex = LoadTextureFromImage(img);
            UnloadImage(img);
            has_map = (map_tex.id != 0);
        }
    }

    Font font = GetFontDefault();
    int has_font = 0;
    if (FileExists(FONT_PATH)) {
        font = load_korean_font(32);
        has_font = (font.glyphCount > 0);
    }

    char search[MAX_INPUT_LENGTH] = {0};
    int search_len = 0;
    char status[MAX_STATUS_LENGTH] = "국가/수도 Enter 검색, 핀 클릭 시 3대 도시";
    Country selected = {0};
    int has_selection = 0;
    int show_card = 0;
    int show_cities = 0;

    IsoEntry iso_map[MAX_ISO_ENTRIES];
    int iso_map_n = load_iso_map(iso_map, MAX_ISO_ENTRIES);
    Texture2D flag_tex = {0};
    int has_flag = 0;

    CityInfo top_cities[MAX_TOP_CITIES];
    int top_city_count = 0;
    Texture2D city_tex[MAX_TOP_CITIES] = {{0}};
    int has_city_tex[MAX_TOP_CITIES] = {0};

    AppMode mode = MODE_SEARCH;
    FormField form_field = FIELD_NAME;
    char form[4][MAX_INPUT_LENGTH];
    int form_len[4];
    reset_form(form, form_len, &form_field);

    Country list_items[MAX_LIST_ROWS];
    int list_count = 0;
    int list_scroll = 0;

    while (!WindowShouldClose()) {
        float dw = (float)GetScreenWidth();
        float dh = (float)GetScreenHeight();
        float map_h = dh - TOP_BAR_H;
        float map_w = dw;

        /* Hotkeys */
        if (mode == MODE_SEARCH) {
            if (IsKeyPressed(KEY_F2)) {
                mode = MODE_CREATE;
                reset_form(form, form_len, &form_field);
                snprintf(status, sizeof(status), "Create: 국가명 입력 후 Enter");
            } else if (IsKeyPressed(KEY_F3)) {
                mode = MODE_UPDATE;
                reset_form(form, form_len, &form_field);
                snprintf(status, sizeof(status), "Update: 국가명 입력 후 Enter");
            } else if (IsKeyPressed(KEY_F4)) {
                mode = MODE_DELETE;
                reset_form(form, form_len, &form_field);
                snprintf(status, sizeof(status), "Delete: 삭제할 국가명 입력 후 Enter");
            } else if (IsKeyPressed(KEY_F5)) {
                list_count = load_country_list(db, list_items, MAX_LIST_ROWS);
                list_scroll = 0;
                mode = MODE_LIST;
                show_card = 0;
                show_cities = 0;
                snprintf(status, sizeof(status), "DB 목록: %d개 (클릭 선택 / ESC 닫기)", list_count);
            } else if (IsKeyPressed(KEY_ESCAPE)) {
                break;
            }
        } else if (IsKeyPressed(KEY_ESCAPE)) {
            mode = MODE_SEARCH;
            snprintf(status, sizeof(status), "검색 모드");
        }

        if (mode == MODE_LIST) {
            float list_h = dh - TOP_BAR_H - 40.0f - 40.0f - 32.0f;
            int visible = (int)(list_h / LIST_ROW_H);
            if (visible < 1) {
                visible = 1;
            }
            int max_scroll = list_count - visible;
            if (max_scroll < 0) {
                max_scroll = 0;
            }
            if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) {
                list_scroll--;
            }
            if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) {
                list_scroll++;
            }
            if (IsKeyPressed(KEY_PAGE_UP)) {
                list_scroll -= visible;
            }
            if (IsKeyPressed(KEY_PAGE_DOWN)) {
                list_scroll += visible;
            }
            list_scroll -= (int)(GetMouseWheelMove() * 3.0f);
            if (list_scroll < 0) {
                list_scroll = 0;
            }
            if (list_scroll > max_scroll) {
                list_scroll = max_scroll;
            }
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                int hit = list_row_at_pos(dw, dh, list_scroll, list_count, GetMousePosition());
                if (hit >= 0) {
                    selected = list_items[hit];
                    has_selection = 1;
                    show_card = 1;
                    show_cities = 0;
                    unload_city_textures(city_tex, has_city_tex, MAX_TOP_CITIES);
                    top_city_count = 0;
                    load_flag_for_country(iso_map, iso_map_n, selected.name, &flag_tex, &has_flag);
                    mode = MODE_SEARCH;
                    snprintf(status, sizeof(status),
                             "선택: %s / %s (%.4f, %.4f)",
                             selected.name, selected.capital, selected.lat, selected.lon);
                }
            }
        } else if (mode == MODE_SEARCH) {
            handle_text_input(search, MAX_INPUT_LENGTH, &search_len);
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
                Country hit;
                if (search_country(db, search, &hit)) {
                    selected = hit;
                    has_selection = 1;
                    show_card = 1;
                    show_cities = 0;
                    unload_city_textures(city_tex, has_city_tex, MAX_TOP_CITIES);
                    top_city_count = 0;
                    load_flag_for_country(iso_map, iso_map_n, selected.name, &flag_tex, &has_flag);
                    snprintf(status, sizeof(status),
                             "결과: %s / %s (%.4f, %.4f) — 핀 클릭: 3대 도시",
                             hit.name, hit.capital, hit.lat, hit.lon);
                } else {
                    has_selection = 0;
                    show_card = 0;
                    show_cities = 0;
                    unload_flag_tex(&flag_tex, &has_flag);
                    unload_city_textures(city_tex, has_city_tex, MAX_TOP_CITIES);
                    top_city_count = 0;
                    snprintf(status, sizeof(status), "'%s' 를 찾을 수 없습니다", search);
                }
            }
            if (has_selection && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                Vector2 pin = latlon_to_pixel(selected.lat, selected.lon, map_w, map_h);
                pin.y += TOP_BAR_H;
                Vector2 mouse = GetMousePosition();
                float dx = mouse.x - pin.x;
                float dy = mouse.y - pin.y;
                if (dx * dx + dy * dy <= 22.0f * 22.0f) {
                    show_card = 0;
                    show_cities = 1;
                    top_city_count = load_top_cities(db, selected.name, top_cities, MAX_TOP_CITIES);
                    load_city_textures(top_cities, top_city_count, city_tex, has_city_tex);
                    if (top_city_count > 0) {
                        snprintf(status, sizeof(status),
                                 "%s 주요 %d도시", selected.name, top_city_count);
                    } else {
                        snprintf(status, sizeof(status),
                                 "%s 주요 도시 데이터 없음", selected.name);
                    }
                }
            }
        } else {
            handle_text_input(form[form_field], MAX_INPUT_LENGTH, &form_len[form_field]);
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
                if (mode == MODE_DELETE) {
                    if (db_delete(db, form[FIELD_NAME])) {
                        if (has_selection && strcmp(selected.name, form[FIELD_NAME]) == 0) {
                            has_selection = 0;
                            show_card = 0;
                            show_cities = 0;
                            unload_flag_tex(&flag_tex, &has_flag);
                            unload_city_textures(city_tex, has_city_tex, MAX_TOP_CITIES);
                            top_city_count = 0;
                        }
                        snprintf(status, sizeof(status), "삭제됨: %s", form[FIELD_NAME]);
                    } else {
                        snprintf(status, sizeof(status), "삭제 실패: %s", form[FIELD_NAME]);
                    }
                    mode = MODE_SEARCH;
                } else if (mode == MODE_CREATE) {
                    if (form_field == FIELD_NAME) {
                        form_field = FIELD_CAPITAL;
                        snprintf(status, sizeof(status), "Create: 수도 입력 후 Enter");
                    } else if (form_field == FIELD_CAPITAL) {
                        form_field = FIELD_LAT;
                        snprintf(status, sizeof(status), "Create: 위도 입력 후 Enter");
                    } else if (form_field == FIELD_LAT) {
                        form_field = FIELD_LON;
                        snprintf(status, sizeof(status), "Create: 경도 입력 후 Enter");
                    } else {
                        Country c;
                        strncpy(c.name, form[FIELD_NAME], MAX_NAME_LENGTH - 1);
                        strncpy(c.capital, form[FIELD_CAPITAL], MAX_NAME_LENGTH - 1);
                        c.name[MAX_NAME_LENGTH - 1] = 0;
                        c.capital[MAX_NAME_LENGTH - 1] = 0;
                        c.lat = atof(form[FIELD_LAT]);
                        c.lon = atof(form[FIELD_LON]);
                        if (db_insert(db, &c)) {
                            selected = c;
                            has_selection = 1;
                            show_card = 1;
                            load_flag_for_country(iso_map, iso_map_n, selected.name, &flag_tex, &has_flag);
                            snprintf(status, sizeof(status), "추가됨: %s", c.name);
                        } else {
                            snprintf(status, sizeof(status), "추가 실패 (중복/오류)");
                        }
                        mode = MODE_SEARCH;
                    }
                } else if (mode == MODE_UPDATE) {
                    if (form_field == FIELD_NAME) {
                        Country cur;
                        if (!find_country_exact(db, form[FIELD_NAME], &cur)) {
                            snprintf(status, sizeof(status), "없음: %s", form[FIELD_NAME]);
                            mode = MODE_SEARCH;
                        } else {
                            strncpy(form[FIELD_CAPITAL], cur.capital, MAX_INPUT_LENGTH - 1);
                            form_len[FIELD_CAPITAL] = (int)strlen(form[FIELD_CAPITAL]);
                            snprintf(form[FIELD_LAT], MAX_INPUT_LENGTH, "%.4f", cur.lat);
                            form_len[FIELD_LAT] = (int)strlen(form[FIELD_LAT]);
                            snprintf(form[FIELD_LON], MAX_INPUT_LENGTH, "%.4f", cur.lon);
                            form_len[FIELD_LON] = (int)strlen(form[FIELD_LON]);
                            form_field = FIELD_CAPITAL;
                            snprintf(status, sizeof(status), "Update: 수도 수정 후 Enter");
                        }
                    } else if (form_field == FIELD_CAPITAL) {
                        form_field = FIELD_LAT;
                        snprintf(status, sizeof(status), "Update: 위도 수정 후 Enter");
                    } else if (form_field == FIELD_LAT) {
                        form_field = FIELD_LON;
                        snprintf(status, sizeof(status), "Update: 경도 수정 후 Enter");
                    } else {
                        Country c;
                        strncpy(c.name, form[FIELD_NAME], MAX_NAME_LENGTH - 1);
                        strncpy(c.capital, form[FIELD_CAPITAL], MAX_NAME_LENGTH - 1);
                        c.name[MAX_NAME_LENGTH - 1] = 0;
                        c.capital[MAX_NAME_LENGTH - 1] = 0;
                        c.lat = atof(form[FIELD_LAT]);
                        c.lon = atof(form[FIELD_LON]);
                        if (db_update(db, &c)) {
                            selected = c;
                            has_selection = 1;
                            show_card = 1;
                            load_flag_for_country(iso_map, iso_map_n, selected.name, &flag_tex, &has_flag);
                            snprintf(status, sizeof(status), "수정됨: %s", c.name);
                        } else {
                            snprintf(status, sizeof(status), "수정 실패");
                        }
                        mode = MODE_SEARCH;
                    }
                }
            }
        }

        BeginDrawing();
        ClearBackground((Color){12, 18, 28, 255});

        /* Map */
        if (has_map) {
            Rectangle src = {0, 0, (float)map_tex.width, (float)map_tex.height};
            Rectangle dst = {0, TOP_BAR_H, map_w, map_h};
            DrawTexturePro(map_tex, src, dst, (Vector2){0, 0}, 0.0f, WHITE);
        } else {
            DrawRectangle(0, (int)TOP_BAR_H, (int)map_w, (int)map_h, (Color){30, 50, 70, 255});
            for (int i = 0; i <= 12; i++) {
                float x = map_w * i / 12.0f;
                DrawLine((int)x, (int)TOP_BAR_H, (int)x, (int)dh, (Color){50, 70, 90, 255});
            }
            for (int i = 0; i <= 6; i++) {
                float y = TOP_BAR_H + map_h * i / 6.0f;
                DrawLine(0, (int)y, (int)map_w, (int)y, (Color){50, 70, 90, 255});
            }
            if (has_font) {
                DrawTextEx(font, "assets/world_map.jpg 없음 — 격자 지도",
                           (Vector2){24, TOP_BAR_H + 24}, 24, 1, ORANGE);
            }
        }

        if (has_selection && mode != MODE_LIST) {
            Vector2 pin = latlon_to_pixel(selected.lat, selected.lon, map_w, map_h);
            pin.y += TOP_BAR_H;
            draw_pin(pin, has_font ? font : GetFontDefault(), &selected, 1);
            if (show_cities) {
                draw_cities_panel(has_font ? font : GetFontDefault(), has_font,
                                  selected.name, top_cities, top_city_count,
                                  city_tex, has_city_tex, dw, dh);
            } else if (show_card) {
                draw_country_card(has_font ? font : GetFontDefault(), has_font,
                                  &selected, flag_tex, has_flag, dw, dh);
            }
        }

        if (mode == MODE_LIST) {
            draw_db_list(has_font ? font : GetFontDefault(), has_font,
                         list_items, list_count, list_scroll, dw, dh);
        }

        /* Top bar */
        DrawRectangle(0, 0, (int)dw, (int)TOP_BAR_H, (Color){18, 24, 36, 245});
        DrawRectangle(0, (int)TOP_BAR_H - 2, (int)dw, 2, (Color){70, 120, 180, 255});

        const char *prompt = "검색";
        const char *active = search;
        if (mode == MODE_CREATE) {
            prompt = (form_field == FIELD_NAME) ? "추가/국가" :
                     (form_field == FIELD_CAPITAL) ? "추가/수도" :
                     (form_field == FIELD_LAT) ? "추가/위도" : "추가/경도";
            active = form[form_field];
        } else if (mode == MODE_UPDATE) {
            prompt = (form_field == FIELD_NAME) ? "수정/국가" :
                     (form_field == FIELD_CAPITAL) ? "수정/수도" :
                     (form_field == FIELD_LAT) ? "수정/위도" : "수정/경도";
            active = form[form_field];
        } else if (mode == MODE_DELETE) {
            prompt = "삭제/국가";
            active = form[FIELD_NAME];
        } else if (mode == MODE_LIST) {
            prompt = "목록";
            active = "";
        }

        char line_buf[512];
        if (mode == MODE_LIST) {
            snprintf(line_buf, sizeof(line_buf), "%s: %d개", prompt, list_count);
        } else {
            snprintf(line_buf, sizeof(line_buf), "%s: %s", prompt, active);
        }

        if (has_font) {
            DrawTextEx(font, line_buf, (Vector2){16, 10}, 28, 1, RAYWHITE);
            DrawTextEx(font, status, (Vector2){16, 42}, 18, 1, (Color){180, 210, 255, 255});
            DrawTextEx(font, "국가/수도 Enter | 핀클릭 3도시 | F2추가 F3수정 F4삭제 F5목록 | ESC",
                       (Vector2){dw - 640, 42}, 16, 1, LIGHTGRAY);
        } else {
            DrawText(line_buf, 16, 12, 24, RAYWHITE);
            DrawText(status, 16, 44, 16, SKYBLUE);
        }

        /* caret blink (not in list mode) */
        if (mode != MODE_LIST && ((int)(GetTime() * 2.0)) % 2 == 0) {
            Vector2 ts = MeasureTextEx(has_font ? font : GetFontDefault(), line_buf, has_font ? 28 : 24, 1);
            DrawRectangle(16 + (int)ts.x + 2, 14, 2, 24, YELLOW);
        }

        EndDrawing();
    }

    unload_flag_tex(&flag_tex, &has_flag);
    unload_city_textures(city_tex, has_city_tex, MAX_TOP_CITIES);
    if (has_font) {
        UnloadFont(font);
    }
    if (has_map) {
        UnloadTexture(map_tex);
    }
    CloseWindow();
}

int main(void)
{
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);

    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "[ERROR] open db: %s\n", sqlite3_errmsg(db));
        return 1;
    }
    sqlite3_exec(db, "PRAGMA foreign_keys = ON;", 0, 0, NULL);
    if (!create_table(db)) {
        sqlite3_close(db);
        return 1;
    }
    if (count_rows(db) == 0) {
        load_csv_data(db, CSV_PATH);
    }
    if (count_city_rows(db) == 0) {
        load_cities_csv(db, CITIES_CSV_PATH);
    }

    run_map_app(db);

    sqlite3_close(db);
    return 0;
}

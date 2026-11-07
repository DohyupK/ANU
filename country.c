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
#define DB_PATH "countries.db"
#define CSV_PATH "countries.csv"
#define MAP_PATH "assets/world_map.jpg"
#define FONT_PATH "assets/malgun.ttf"
#define TOP_BAR_H 72.0f

typedef struct {
    char name[MAX_NAME_LENGTH];
    char capital[MAX_NAME_LENGTH];
    double lat;
    double lon;
} Country;

typedef enum {
    MODE_SEARCH = 0,
    MODE_CREATE,
    MODE_UPDATE,
    MODE_DELETE
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
    if (!table_has_lat_lon(db)) {
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
        strncpy(out->name, (const char *)sqlite3_column_text(stmt, 0), MAX_NAME_LENGTH - 1);
        strncpy(out->capital, (const char *)sqlite3_column_text(stmt, 1), MAX_NAME_LENGTH - 1);
        out->name[MAX_NAME_LENGTH - 1] = 0;
        out->capital[MAX_NAME_LENGTH - 1] = 0;
        out->lat = sqlite3_column_double(stmt, 2);
        out->lon = sqlite3_column_double(stmt, 3);
        ok = 1;
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
        strncpy(out->name, (const char *)sqlite3_column_text(stmt, 0), MAX_NAME_LENGTH - 1);
        strncpy(out->capital, (const char *)sqlite3_column_text(stmt, 1), MAX_NAME_LENGTH - 1);
        out->name[MAX_NAME_LENGTH - 1] = 0;
        out->capital[MAX_NAME_LENGTH - 1] = 0;
        out->lat = sqlite3_column_double(stmt, 2);
        out->lon = sqlite3_column_double(stmt, 3);
        ok = 1;
    }
    sqlite3_finalize(stmt);
    return ok;
}

static int search_country(sqlite3 *db, const char *query, Country *out)
{
    if (!query || query[0] == 0) {
        return 0;
    }
    if (find_country_exact(db, query, out)) {
        return 1;
    }
    return find_country_prefix(db, query, out);
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
    char status[MAX_STATUS_LENGTH] = "지명을 입력하고 Enter로 검색하세요";
    Country selected = {0};
    int has_selection = 0;

    AppMode mode = MODE_SEARCH;
    FormField form_field = FIELD_NAME;
    char form[4][MAX_INPUT_LENGTH];
    int form_len[4];
    reset_form(form, form_len, &form_field);

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
                snprintf(status, sizeof(status), "DB rows: %d", count_rows(db));
            } else if (IsKeyPressed(KEY_ESCAPE)) {
                break;
            }
        } else if (IsKeyPressed(KEY_ESCAPE)) {
            mode = MODE_SEARCH;
            snprintf(status, sizeof(status), "검색 모드");
        }

        if (mode == MODE_SEARCH) {
            handle_text_input(search, MAX_INPUT_LENGTH, &search_len);
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
                Country hit;
                if (search_country(db, search, &hit)) {
                    selected = hit;
                    has_selection = 1;
                    snprintf(status, sizeof(status),
                             "결과: %s / %s (%.4f, %.4f)",
                             hit.name, hit.capital, hit.lat, hit.lon);
                } else {
                    has_selection = 0;
                    snprintf(status, sizeof(status), "'%s' 를 찾을 수 없습니다", search);
                }
            }
        } else {
            handle_text_input(form[form_field], MAX_INPUT_LENGTH, &form_len[form_field]);
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
                if (mode == MODE_DELETE) {
                    if (db_delete(db, form[FIELD_NAME])) {
                        if (has_selection && strcmp(selected.name, form[FIELD_NAME]) == 0) {
                            has_selection = 0;
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

        if (has_selection) {
            Vector2 pin = latlon_to_pixel(selected.lat, selected.lon, map_w, map_h);
            pin.y += TOP_BAR_H;
            draw_pin(pin, has_font ? font : GetFontDefault(), &selected, 1);
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
        }

        char line_buf[512];
        snprintf(line_buf, sizeof(line_buf), "%s: %s", prompt, active);

        if (has_font) {
            DrawTextEx(font, line_buf, (Vector2){16, 10}, 28, 1, RAYWHITE);
            DrawTextEx(font, status, (Vector2){16, 42}, 18, 1, (Color){180, 210, 255, 255});
            DrawTextEx(font, "Enter 검색 | F2추가 F3수정 F4삭제 F5개수 | ESC종료",
                       (Vector2){dw - 520, 42}, 16, 1, LIGHTGRAY);
        } else {
            DrawText(line_buf, 16, 12, 24, RAYWHITE);
            DrawText(status, 16, 44, 16, SKYBLUE);
        }

        /* caret blink */
        if (((int)(GetTime() * 2.0)) % 2 == 0) {
            Vector2 ts = MeasureTextEx(has_font ? font : GetFontDefault(), line_buf, has_font ? 28 : 24, 1);
            DrawRectangle(16 + (int)ts.x + 2, 14, 2, 24, YELLOW);
        }

        EndDrawing();
    }

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
    if (!create_table(db)) {
        sqlite3_close(db);
        return 1;
    }
    if (count_rows(db) == 0) {
        load_csv_data(db, CSV_PATH);
    }

    run_map_app(db);

    sqlite3_close(db);
    return 0;
}

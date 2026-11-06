# (C+DB) 나라 이름을 입력하면 그 나라의 수도를 알려주는 DB

- 개요
  
  나라 이름을 입력하면 그 나라의 수도를 알려주는 데이터베이스를 구현했습니다.
---
- 소스 코드
  
```c
#define CRT_NO_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "sqlite3.h"

#define MAX_LINE_LENGTH 256
#define MAX_NAME_LENGTH 100

// 문자열 내의 모든 공백을 제거하는 함수
void remove_spaces(char* str)
{
    char* i = str;
    char* j = str;
    while(*j != 0)
    {
        if(*j != ' ' && *j != '\t' && *j != '\r' && *j != '\n')
        {
            *i = *j;
            i++;
        }
        j++;
    }
    *i = 0;
}

// 1. 테이블 생성 함수
int create_table(sqlite3 *db)
{
    const char *sql_create = "CREATE TABLE IF NOT EXISTS countries ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "name TEXT UNIQUE NOT NULL, "
        "capital TEXT NOT NULL);";
    char *err_msg = NULL;
    int rc = sqlite3_exec(db, sql_create, 0, 0, &err_msg);

    if(rc != SQLITE_OK)
    {
        fprintf(stderr, "[ERROR] Failed to create SQL table: %s\n", err_msg);
        sqlite3_free(err_msg);
        return 0;
    }

    return 1;
}

// 2. CSV 파일에서 데이터를 읽어 DB에 적재하는 함수 (트랜잭션 사용)
void load_csv_data(sqlite3 *db, const char *csv_filename)
{
    FILE *file = fopen(csv_filename, "r");
    if(!file)
    {
        printf("[INFO] '%s' file not found. Skipping initial data loading.\n", csv_filename);
        return;
    }

    char line[MAX_LINE_LENGTH];
    char *err_msg = NULL;

    // 헤더 줄 넘기기 (국가,수도)
    if(fgets(line, sizeof(line), file) == NULL)
    {
        fclose(file);
        return ;
    }

    // 트랜잭션 시작 (대량 삽입 속도 극대화)
    sqlite3_exec(db, "BEGIN TRANSACTION;", 0, 0, &err_msg);

    const char *sql_insert = "INSERT OR IGNORE INTO countries (name, capital) VALUES (?, ?);";
    sqlite3_stmt *stmt;
    if(sqlite3_prepare_v2(db, sql_insert, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "[ERROR] Failed to prepare insert statement: %s\n", sqlite3_errmsg(db));
        fclose(file);
        return ;
    }

    int inserted_count = 0;
    while(fgets(line, sizeof(line), file))
    {
        // 개행 문자 제거
        line[strcspn(line, "\r\n")] = 0;

        // 콤마(,) 기준 파싱
        char *country = strtok(line, ",");
        char *capital = strtok(NULL, ",");

        if(country && capital)
        {
            remove_spaces(country);

            sqlite3_bind_text(stmt, 1, country, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, capital, -1, SQLITE_STATIC);

            if(sqlite3_step(stmt) == SQLITE_DONE)
            {
                if(sqlite3_changes(db) > 0)
                {
                    inserted_count++;
                }
            }
            sqlite3_reset(stmt); // 다음 레코드를 위해 Statement 리셋
        }
    }

    sqlite3_finalize(stmt);

    // 트랜잭션 커밋
    sqlite3_exec(db, "COMMIT;", 0, 0, &err_msg);
    fclose(file);

    printf("[INFO] CSV data loading completed (%d items added)\n", inserted_count);
}

// 3. 사용자 입력에 따라 수도를 조회하는 함수
void search_capital(sqlite3 *db)
{
    char input_country[MAX_NAME_LENGTH];

    const char *sql_select = "SELECT capital FROM countries WHERE name = ?;";
    sqlite3_stmt *stmt;

    if(sqlite3_prepare_v2(db, sql_select, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "[ERROR] Failed to prepare select statement: %s\n", sqlite3_errmsg(db));
        return ;
    }

    printf("\n===========================================\n");
    printf(" Capital Search Program (Type 'exit' to quit)\n");
    printf("===========================================\n");

    while(1)
    {
        printf("\nEnter a country name: ");
        if(fgets(input_country, sizeof(input_country), stdin) == NULL) break;

		input_country[strcspn(input_country, "\r\n")] = 0;

        if(strcmp(input_country, "exit") == 0)
        {
            printf("Exiting the program.\n");
            break;
        }

        if(strlen(input_country) == 0) continue;

        remove_spaces(input_country);

        sqlite3_bind_text(stmt, 1, input_country, -1, SQLITE_STATIC);

        int step = sqlite3_step(stmt);
        if(step == SQLITE_ROW)
        {
            const unsigned char *capital = sqlite3_column_text(stmt, 0);
            printf(" -> [Result] %s's capital: %s\n", input_country, capital);
        }
        else
        {
            printf(" -> [Result] '%s' is not found.\n", input_country);
        }

        sqlite3_reset(stmt); // 다음 검색을 위한 Statement 리셋
    }

    sqlite3_finalize(stmt);
}


int main()
{
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);

    sqlite3 *db;
    int rc = sqlite3_open("countries.db", &db);

    if(rc != SQLITE_OK)
    {
        fprintf(stderr, "[ERROR] Failed to open database: %s\n", sqlite3_errmsg(db));
        return rc;
    }

    printf("[INFO] Database('countries.db') connection successful.\n");

    // 1. 테이블 생성
    if(!create_table(db))
    {
        sqlite3_close(db);
        return 1;
    }

    // 2. CSV 파일에서 데이터 읽어와 적재
    load_csv_data(db, "countries.csv");

    // 3. 사용자 검색 실행
    search_capital(db);

    // 4. DB 연결 해제
    sqlite3_close(db);
    printf("[INFO] Database connection closed.\n");

    return 0;
}
```
---
  - 동작 원리

  1. 데이터베이스를 넣을 테이블을 생성한다.  
  2. 데이터가 저장되어있는 countries.csv 파일을 불러와 적재한다.  
  3. 사용자 검색 함수를 실행해 나라 이름을 입력 받는다.  
  4. 함수 종료 시 데이터베이스 연결을 해제하고 닫는다.
---

#pragma once
#include <time.h>
#include "Winsock2.h"
#include <windows.h>
static void sleep(int s) { Sleep(s * 1000); }
static void usleep(int us) { Sleep(us / 1000); }
#include "Shlwapi.h"
#define strcasestr StrStrI
#include "conio.h" // _kbhit, _getch
#define printf_(...) do{printf(__VA_ARGS__); fflush(stdout);} while(0)
#define fprintf_(f, ...) do{fprintf(f, __VA_ARGS__); fflush(f);} while(0)

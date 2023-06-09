#include <fstream>
#include <string>

#if defined(BASE_INTERNAL)
#include <cstdio>
#else
static void platform_win_console_write(char *format, ...);
#define printf(s, ...) platform_win_console_write(s, __VA_ARGS__)
#endif // BASE_INTERNAL

#define BASE_PLATFORM_USE_SOCKETS 1
#define BASE_PLATFORM_WINDOWS 1
#include "base.hpp"
#include "base_platform.hpp"
#include "game_server.hpp"

#define MODULE_FILE "login_server_module.dll"
#define MODULE_FILE_TEMP "login_server_module_temp.dll"
#define MODULE_LOCK_FILE ".reload-lock"

typedef struct App_Code App_Code;
struct App_Code {
  HMODULE module;
  FILETIME module_last_write_time;
  app_tick_t *tick_func;
  bool is_valid;
};

APP_TICK(app_tick_stub) {
  UNUSED(app_memory);
  // UNUSED(should_reload);
}

internal FILETIME win32_get_last_write_time(char *filename) {
  FILETIME result = {0};

  WIN32_FILE_ATTRIBUTE_DATA file_data;
  if (GetFileAttributesExA(filename, GetFileExInfoStandard, &file_data)) {
    result = file_data.ftLastWriteTime;
  }

  return result;
}

internal App_Code win32_app_code_load() {
  App_Code result = {0};

  result.module_last_write_time = win32_get_last_write_time(MODULE_FILE);
  CopyFileA(MODULE_FILE, MODULE_FILE_TEMP, FALSE);

  result.module = LoadLibraryA(MODULE_FILE_TEMP);
  if (result.module) {
    result.tick_func =
        (app_tick_t *)GetProcAddress(result.module, "server_tick");
    result.is_valid = !!result.tick_func;
  }

  if (!result.is_valid) {
    result.tick_func = app_tick_stub;
  }

  return result;
}

internal void win32_app_code_unload(App_Code *app_code) {
  if (app_code->module) {
    FreeLibrary(app_code->module);
    app_code->module = 0;
  }

  app_code->is_valid = false;
  app_code->tick_func = app_tick_stub;
}

#if defined(BASE_INTERNAL)
int main(void)
#else
int mainCRTStartup(void)
#endif // BASE_INTERNAL
{
  App_Memory app_memory;
  app_memory.platform_api = {
    reinterpret_cast<platform_folder_create *>(platform_win_folder_create),
    reinterpret_cast<platform_buffer_write_to_file *>(
        platform_win_buffer_write_to_file),
    reinterpret_cast<platform_buffer_load_from_file *>(
        platform_win_buffer_load_from_file),
    reinterpret_cast<platform_wall_clock *>(platform_win_wall_clock),
    reinterpret_cast<platform_elapsed_seconds *>(platform_win_elapsed_seconds),
#if defined(BASE_PLATFORM_USE_SOCKETS)
    reinterpret_cast<platform_socket_udp_create_and_bind *>(
        platform_win_socket_udp_create_and_bind),
    reinterpret_cast<platform_receive_from *>(platform_win_receive_from),
    reinterpret_cast<platform_send_to *>(platform_win_send_to),
#endif // BASE_PLATFORM_USE_SOCKETS
  };

  app_memory.backing_memory.size = MB(100);
  app_memory.backing_memory.data = reinterpret_cast<u8 *>(VirtualAlloc(
      NULL, app_memory.backing_memory.size, MEM_COMMIT, PAGE_READWRITE));

  LARGE_INTEGER local_performance_frequency;
  QueryPerformanceFrequency(&local_performance_frequency);
  global_performance_frequency = local_performance_frequency.QuadPart;
  bool is_sleep_granular = timeBeginPeriod(1) == TIMERR_NOERROR;
  float tick_rate = 60.0f;
  float target_seconds_per_tick = 1.0f / tick_rate;

#if defined(TERMINAL_UI)
  HANDLE console_handle = CreateConsoleScreenBuffer(
      GENERIC_READ | GENERIC_WRITE, 0, NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
  SMALL_RECT window_rect = {0, 0, 1, 1};
  SetConsoleWindowInfo(console_handle, TRUE, &window_rect);
  SetConsoleScreenBufferSize(console_handle,
                             (COORD){SCREEN_WIDTH, SCREEN_HEIGHT});
  SetConsoleActiveScreenBuffer(console_handle);

  window_rect = (SMALL_RECT){0, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1};
  SetConsoleWindowInfo(console_handle, TRUE, &window_rect);
#endif // TERMINAL_UI

  App_Code app_code = win32_app_code_load();
  unsigned long long previous_counter = platform_win_wall_clock();
  bool is_running = true;
  while (is_running) {
#if defined(BASE_INTERNAL)
    WIN32_FILE_ATTRIBUTE_DATA unused;
    bool is_code_locked =
        GetFileAttributesExA(MODULE_LOCK_FILE, GetFileExInfoStandard, &unused);
    FILETIME new_module_write_time = win32_get_last_write_time(MODULE_FILE);
    bool should_reload =
        !is_code_locked && CompareFileTime(&new_module_write_time,
                                           &app_code.module_last_write_time);
    if (should_reload) {
      printf("\n==============================================================="
             "========\n");
      printf("  Reloading...\n");
      printf("================================================================="
             "======\n");
      app_code.module_last_write_time = new_module_write_time;
      win32_app_code_unload(&app_code);
      app_code = win32_app_code_load();
    }
#endif

    app_code.tick_func(&app_memory
#if defined(BASE_INTERNAL)
                       ,
                       should_reload
#endif
    );

#if defined(TERMINAL_UI)
    DWORD bytes_written;
    WriteConsoleOutputCharacter(console_handle, (LPCSTR)app_memory.screen.data,
                                (DWORD)app_memory.screen.size, (COORD){0},
                                &bytes_written);
#endif // TERMINAL_UI

    u64 work_counter = platform_win_wall_clock();
    app_memory.work_ms =
        1000.0f * platform_win_elapsed_seconds(previous_counter, work_counter);

    float elapsed_tick_seconds = platform_win_elapsed_seconds(
        previous_counter, platform_win_wall_clock());
    if (elapsed_tick_seconds < target_seconds_per_tick) {
      if (is_sleep_granular) {
        int sleep_ms = (int)(target_seconds_per_tick * 1000.0f) -
                       (int)(elapsed_tick_seconds * 1000.0f) - 1;

        if (sleep_ms > 0) {
          Sleep(sleep_ms);
        }
      }

      while (elapsed_tick_seconds < target_seconds_per_tick) {
        elapsed_tick_seconds = platform_win_elapsed_seconds(
            previous_counter, platform_win_wall_clock());
      }
    }

    u64 end_counter = platform_win_wall_clock();
    app_memory.tick_ms =
        1000.0f * platform_win_elapsed_seconds(previous_counter, end_counter);
    previous_counter = end_counter;

    app_memory.tick_count++;
  }

  return 0;
}

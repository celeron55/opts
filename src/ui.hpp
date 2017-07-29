#pragma once
#include "types.hpp"
#include <mpv/client.h>

extern mpv_handle *mpv;
extern sv_<ss_> arduino_serial_paths;
extern int arduino_display_width;
extern bool minimize_display_updates;
extern time_t startup_timestamp;
extern bool do_main_loop;
extern ss_ current_collection_part;
extern bool queued_pause;
extern sv_<ss_> static_media_paths;

extern set_<ss_> enabled_log_sources;
#define LOG_MPV enabled_log_sources.count("mpv")
#define LOG_DEBUG enabled_log_sources.count("debug")

void save_stuff();
void check_mpv_error(int status);
void refresh_track();
void next_collection_part(int dir);
void force_start_at_cursor();

void on_ui_playpause();
void on_ui_next();
void on_ui_prev();
void on_ui_nextalbum();
void on_ui_prevalbum();
void on_ui_playmode();
void on_ui_track_number(int track_n);
void on_ui_album_number(int album_n);
void on_ui_search(const ss_ &searchstring);
void on_ui_random_album();
void on_ui_random_album_approx_num_tracks(size_t approx_num_tracks);
void on_ui_random_album_min_num_tracks(size_t min_num_tracks);
void on_ui_random_album_max_num_tracks(size_t max_num_tracks);
void on_ui_random_track();


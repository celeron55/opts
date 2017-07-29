#pragma once
#include "types.hpp"

void after_mpv_loadfile(double start_pos, const ss_ &track_name, const ss_ &album_name);
void check_mpv_error(int status);
void force_start_at_cursor();
bool mpv_is_idle();
void load_and_play_current_track_from_start();
void refresh_track();
void start_at_relative_track(int album_add, int track_add, bool force_show_album=false);
void eat_all_mpv_events();
void wait_mpv_event(int event_id, int max_ms);
void automated_start_play_next_track();
void do_something_instead_of_idle();
void handle_mpv();


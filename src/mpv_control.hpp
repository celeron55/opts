#pragma once
#include "types.hpp"

void check_mpv_error(int status);
void force_start_at_cursor();
bool mpv_is_idle();
void refresh_track();
void start_at_relative_track(int album_add, int track_add, bool force_show_album=false);
void wait_until_mpv_idle();
void handle_mpv();


#include "arduino_controls.hpp"
#include "stuff.hpp"
#include "c55_getopt.h"
#include "command_accumulator.hpp"
#include "string_util.hpp"
#include "file_watch.hpp"
#include "types.hpp"
#include "filesys.hpp"
#include "scope_end_trigger.hpp"
#include "arduino_firmware.hpp"
#include "stuff2.hpp"
#include "mkdir_p.hpp"
#include "terminal.hpp"
#include "../common/common.hpp"
#include <mpv/client.h>
#include <fstream>
#include <algorithm> // sort
#ifdef __WIN32__
#  include "windows_includes.hpp"
#else
#  include <sys/poll.h>
#  include <sys/types.h>
#  include <sys/stat.h>
#  include <sys/mount.h>
#  include <unistd.h>
#  include <signal.h>
#  include <fcntl.h>
#  include <termios.h>
#  define printf_(...) printf(__VA_ARGS__)
#  define fprintf_(f, ...) fprintf(f, __VA_ARGS__)
#endif
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

ss_ config_path = "__default__";
bool config_must_be_readable = false;

ss_ saved_state_path = "__default__";

sv_<ss_> arduino_serial_paths;
sv_<ss_> track_devices;
sv_<ss_> static_media_paths;
bool there_are_command_line_static_media_paths = false;
ss_ arduino_serial_debug_mode = "off"; // off / raw / fancy
int arduino_display_width = 8;
bool minimize_display_updates = false;

set_<ss_> enabled_log_sources;
#define LOG_MPV enabled_log_sources.count("mpv")
#define LOG_DEBUG enabled_log_sources.count("debug")

time_t startup_timestamp = 0;

bool do_main_loop = true;
mpv_handle *mpv = NULL;
CommandAccumulator<100> stdin_command_accu;

int arduino_serial_fd = -1;
ss_ arduino_serial_fd_path;
bool tried_to_update_arduino_firmware = false;
time_t arduino_last_incoming_message_timestamp = 0;
CommandAccumulator<100> arduino_message_accu;
set_<int> current_keys;

time_t display_update_timestamp = 0;
size_t display_next_startpos = 0;
ss_ display_last_shown_track_name;

up_<FileWatch> partitions_watch;

ss_ current_mount_device;
ss_ current_mount_path;
ss_ current_collection_part;

bool queued_pause = false;
sv_<size_t> queued_album_shuffled_track_order;

#include "library.hpp"

MediaContent current_media_content;

#include "play_cursor.hpp"

TrackProgressMode track_progress_mode = TPM_NORMAL;
PlayCursor current_cursor;
PlayCursor last_succesfully_playing_cursor;

bool track_was_loaded = false;

time_t mpv_last_not_idle_timestamp = 0;
time_t mpv_last_loadfile_timestamp = 0;

void after_mpv_loadfile(double start_pos, const ss_ &track_name, const ss_ &album_name)
{
	mpv_last_loadfile_timestamp = time(0);

	current_cursor.stream_end = 0; // Will be filled in at time-pos getter code or something

	if(current_cursor.track_name != track_name){
		printf_("WARNING: Changing track name at loadfile to \"%s\"\n",
				cs(track_name));
		current_cursor.track_name = track_name;
		current_cursor.album_name = album_name;
	}

	arduino_serial_write(">PROGRESS:0\r\n");
}

enum StatefulInputMode {
	SIM_NONE,
	SIM_TRACK_NUMBER,
	SIM_ALBUM_NUMBER,

	SIM_NUM_MODES,
};

StatefulInputMode stateful_input_mode = SIM_NONE;
time_t stateful_input_mode_active_timestamp = 0;
CommandAccumulator<10> stateful_input_accu;

ss_ last_searchstring;

ss_ mpv_get_string_property(mpv_handle *mpv, const char *name)
{
	char *cs = NULL;
	mpv_get_property(mpv, name, MPV_FORMAT_STRING, &cs);
	ss_ s = cs != NULL ? ss_(cs) : ss_();
	mpv_free(cs);
	return s;
}

time_t last_save_timestamp = 0;

void save_stuff()
{
	last_save_timestamp = time(0);

	if(LOG_DEBUG)
		printf_("Saving stuff to %s...\n", cs(saved_state_path));

	// If at <5s into the track, start from the beginning next time
	double save_time_pos = last_succesfully_playing_cursor.time_pos;
	int save_stream_pos = last_succesfully_playing_cursor.stream_pos;
	if(save_time_pos < 5){
		save_time_pos = 0;
		save_stream_pos = 0;
	}

	ss_ save_blob;
	save_blob += itos(last_succesfully_playing_cursor.album_seq_i) + ";";
	save_blob += itos(last_succesfully_playing_cursor.track_seq_i) + ";";
	save_blob += ftos(save_time_pos) + ";";
	save_blob += itos(save_stream_pos) + ";";
	save_blob += itos(last_succesfully_playing_cursor.current_pause_mode == PM_PAUSE) + ";";
	save_blob += itos(last_succesfully_playing_cursor.track_progress_mode) + ";";
	save_blob += "\n";
	save_blob += last_succesfully_playing_cursor.track_name + "\n";
	save_blob += last_succesfully_playing_cursor.album_name + "\n";

	// Save track order of current album
	auto &cursor = current_cursor;
	auto &mc = current_media_content;
	if(cursor.album_i(mc) < (int)mc.albums.size()){
		const Album &album = mc.albums[cursor.album_i(mc)];
		for(size_t i : album.shuffled_track_order)
			save_blob += itos(i) + ";";
	}
	save_blob += "\n";

	save_blob += current_collection_part + "\n";

	std::ofstream f(saved_state_path.c_str(), std::ios::binary);
	f<<save_blob;
	f.close();

	if(LOG_DEBUG)
		printf_("Saved.\n");
}

void load_stuff()
{
	ss_ data;
	{
		std::ifstream f(saved_state_path.c_str());
		if(!f.good()){
			printf_("No saved state at %s\n", cs(saved_state_path));
			return;
		}
		printf_("Loading saved state from %s\n", cs(saved_state_path));
		data = ss_((std::istreambuf_iterator<char>(f)),
				std::istreambuf_iterator<char>());
	}
	Strfnd f(data);
	Strfnd f1(f.next("\n"));
	last_succesfully_playing_cursor.album_seq_i = stoi(f1.next(";"), 0);
	last_succesfully_playing_cursor.track_seq_i = stoi(f1.next(";"), 0);
	last_succesfully_playing_cursor.time_pos = stof(f1.next(";"), 0.0);
	last_succesfully_playing_cursor.stream_pos = stoi(f1.next(";"), 0);
	queued_pause = stoi(f1.next(";"), 0);
	last_succesfully_playing_cursor.track_progress_mode = (TrackProgressMode)stoi(f1.next(";"), 0);
	last_succesfully_playing_cursor.track_name = f.next("\n");
	last_succesfully_playing_cursor.album_name = f.next("\n");

	// Load track order of current album
	queued_album_shuffled_track_order.clear();
	ss_ order_s = f.next("\n");
	Strfnd order_f(order_s);
	while(!order_f.atend()){
		int i = stoi(order_f.next(";"), 0);
		queued_album_shuffled_track_order.push_back(i);
	}

	current_collection_part = f.next("\n");

	current_cursor = last_succesfully_playing_cursor;

	if(queued_pause){
		if(LOG_DEBUG)
			printf_("Queuing pause\n");
	}
}

static inline void check_mpv_error(int status)
{
    if (status < 0) {
        printf_("mpv API error: %s\n", mpv_error_string(status));
        exit(1);
    }
}

void force_start_at_cursor()
{
	if(LOG_DEBUG)
		printf_("Force-start at cursor\n");
	printf_("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));

	Track track = get_track(current_media_content, current_cursor);
	if(track.display_name != "" && current_cursor.track_name == ""){
		printf_("Warning: Cursor has empty track name\n");
	} else if(track.display_name != current_cursor.track_name){
		printf_("Track name does not match cursor name\n");
		track_was_loaded = false;
		return;
	}

	if(current_cursor.time_pos >= 0.001){
		if(LOG_DEBUG)
			printf_("Force-starting at %fs\n", current_cursor.time_pos);
		mpv_set_option_string(mpv, "start", cs(ftos(current_cursor.time_pos)));
	} else {
		if(LOG_DEBUG)
			printf_("Force-starting at 0s\n");
		mpv_set_option_string(mpv, "start", "#1");
	}

	void eat_all_mpv_events();
	eat_all_mpv_events();

	const char *cmd[] = {"loadfile", track.path.c_str(), NULL};
	check_mpv_error(mpv_command(mpv, cmd));

	after_mpv_loadfile(current_cursor.time_pos, track.display_name,
			get_album_name(current_media_content, current_cursor));

	// Wait for the start-file event
	void wait_mpv_event(int event_id, int max_ms);
	wait_mpv_event(MPV_EVENT_START_FILE, 1000);

	void refresh_track();
	refresh_track();
}

bool mpv_is_idle()
{
	char *idle_cs = NULL;
	// For some reason the idle property always says "yes" on Windows, so don't
	// even read it on Windows
#ifndef __WIN32__
	mpv_get_property(mpv, "idle", MPV_FORMAT_STRING, &idle_cs);
#endif
	if(idle_cs == NULL){
		static bool warned = false;
		if(!warned){
			warned = true;
			printf_("WARNING: MPV property \"idle\" returns NULL; "
					"using the filename property instead.\n");
		}
		char *filename_cs = NULL;
		mpv_get_property(mpv, "filename", MPV_FORMAT_STRING, &filename_cs);
		bool is_idle = (filename_cs == NULL);
		mpv_free(filename_cs);
		return is_idle;
	}
	bool is_idle = (strcmp(idle_cs, "yes") == 0);
	mpv_free(idle_cs);
	return is_idle;
}

void handle_control_playpause()
{
	bool idle = mpv_is_idle();

	if(!idle){
		int was_pause = 0;
		mpv_get_property(mpv, "pause", MPV_FORMAT_FLAG, &was_pause);

		if(was_pause)
			printf_("Resume\n");
		else
			printf_("Pause\n");

		// Some kind of track is loaded; toggle playback
		check_mpv_error(mpv_command_string(mpv, "pause"));

		current_cursor.current_pause_mode = was_pause ? PM_PLAY : PM_PAUSE; // Invert

		if(!was_pause){
			arduino_set_temp_text("PAUSE");
		} else {
			arduino_set_temp_text("RESUME");
		}
		void arduino_set_extra_segments();
		arduino_set_extra_segments();
	} else {
		// No track is loaded; load from cursor
		force_start_at_cursor();
	}
}

void load_and_play_current_track_from_start()
{
	Track track = get_track(current_media_content, current_cursor);

	// Reset starting position
	mpv_set_option_string(mpv, "start", "#1");

	// Play the file
	const char *cmd[] = {"loadfile", track.path.c_str(), NULL};
	check_mpv_error(mpv_command(mpv, cmd));

	after_mpv_loadfile(0, track.display_name,
			get_album_name(current_media_content, current_cursor));

	void update_display();
	update_display();
}

void refresh_track()
{
	void update_display();
	update_display();

	if(current_media_content.albums.empty())
		return;

	char *playing_path = NULL;
	ScopeEndTrigger set([&](){ mpv_free(playing_path); });
	mpv_get_property(mpv, "path", MPV_FORMAT_STRING, &playing_path);
	//printf_("Currently playing: %s\n", playing_path);

	Track track = get_track(current_media_content, current_cursor);
	if(track.path != ""){
		current_cursor.track_name = track.display_name;
		current_cursor.album_name = get_album_name(current_media_content, current_cursor);

		if(playing_path == NULL || ss_(playing_path) != track.path){
			printf_("Playing path does not match current track; Switching track.\n");

			load_and_play_current_track_from_start();
		}
	}
}

void temp_display_album()
{
	if(current_media_content.albums.empty())
		return;

	arduino_set_temp_text(squeeze(get_album_name(current_media_content, current_cursor),
			arduino_display_width));

	// Delay track scroll for one second
	display_update_timestamp = time(0) + 1;
}

void arduino_set_extra_segments()
{
	uint8_t extra_segment_flags = 0;
	switch(current_cursor.track_progress_mode){
	case TPM_NORMAL:
		break;
	case TPM_ALBUM_REPEAT:
		extra_segment_flags |= (1<<DISPLAY_FLAG_REPEAT);
		break;
	case TPM_ALBUM_REPEAT_TRACK:
		extra_segment_flags |= (1<<DISPLAY_FLAG_REPEAT) | (1<<DISPLAY_FLAG_REPEAT_ONE);
		break;
	case TPM_SHUFFLE_ALL:
		extra_segment_flags |= (1<<DISPLAY_FLAG_SHUFFLE);
		break;
	case TPM_SHUFFLE_TRACKS:
		extra_segment_flags |= (1<<DISPLAY_FLAG_SHUFFLE) | (1<<DISPLAY_FLAG_REPEAT_ONE);
		break;
	case TPM_SMART_TRACK_SHUFFLE:
	case TPM_SMART_ALBUM_SHUFFLE:
	case TPM_MR_SHUFFLE:
		extra_segment_flags |= (1<<DISPLAY_FLAG_SHUFFLE) | (1<<DISPLAY_FLAG_REPEAT) |
				(1<<DISPLAY_FLAG_REPEAT_ONE);
		break;
	case TPM_NUM_MODES:
		break;
	}
	if(current_cursor.current_pause_mode == PM_PAUSE){
		extra_segment_flags |= (1<<DISPLAY_FLAG_PAUSE);
	}
	arduino_serial_write(">EXTRA_SEGMENTS:"+itos(extra_segment_flags)+"\r\n");
}

void start_at_relative_track(int album_add, int track_add, bool force_show_album=false)
{
	if(album_add != 0){
		current_cursor.album_seq_i += album_add;
		current_cursor.track_seq_i = 0;
	} else {
		current_cursor.track_seq_i += track_add;
	}
	current_cursor.time_pos = 0;
	current_cursor.stream_pos = 0;
	cursor_bound_wrap(current_media_content, current_cursor);
	current_cursor.track_name = get_track_name(current_media_content, current_cursor);
	current_cursor.album_name = get_album_name(current_media_content, current_cursor);
	if(album_add != 0 || force_show_album)
		temp_display_album();
	printf_("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
	load_and_play_current_track_from_start();
}

void handle_control_next()
{
	start_at_relative_track(0, 1);
}

void handle_control_prev()
{
	start_at_relative_track(0, -1);
}

void handle_control_nextalbum()
{
	start_at_relative_track(1, 0);
}

void handle_control_prevalbum()
{
	start_at_relative_track(-1, 0);
}

const char* tpm_to_string(TrackProgressMode m)
{
	switch(m){
	case TPM_NORMAL:              return "NORMAL";
	case TPM_ALBUM_REPEAT:        return "ALBUM REPEAT";
	case TPM_ALBUM_REPEAT_TRACK:  return "TRACK REPEAT";
	case TPM_SHUFFLE_ALL:         return "ALL SHUFFLE";
	case TPM_SHUFFLE_TRACKS:      return "TRACK SHUFFLE";
	case TPM_SMART_ALBUM_SHUFFLE: return "SMART ALBUM SHUFFLE";
	case TPM_SMART_TRACK_SHUFFLE: return "SMART TRACK SHUFFLE";
	case TPM_MR_SHUFFLE:          return "MR. SHUFFLE";
	case TPM_NUM_MODES:           return "INVALID";
	}
	return "INVALID";
}

void change_track_progress_mode(TrackProgressMode track_progress_mode)
{
	printf_("Track progress mode: %s\n", tpm_to_string(track_progress_mode));

	arduino_set_temp_text(tpm_to_string(track_progress_mode));

	current_cursor.set_track_progress_mode(current_media_content, track_progress_mode);

	// Seamless looping!
	if(track_progress_mode == TPM_ALBUM_REPEAT_TRACK){
		mpv_set_property_string(mpv, "loop", "inf");
	} else {
		mpv_set_property_string(mpv, "loop", "no");
	}

	void arduino_set_extra_segments();
	arduino_set_extra_segments();
}

void handle_control_playmode()
{
	TrackProgressMode track_progress_mode = current_cursor.track_progress_mode;

	if(track_progress_mode < TPM_NUM_MODES - 1)
		track_progress_mode = (TrackProgressMode)(track_progress_mode + 1);
	else
		track_progress_mode = (TrackProgressMode)0;

	change_track_progress_mode(track_progress_mode);
}

void handle_control_track_number(int track_n)
{
	if(track_n < 1){
		printf_("handle_control_track_number(): track_n = %i < 1\n", track_n);
		arduino_set_temp_text("PASS");
		return;
	}
	int track_media_index = track_n - 1;

	auto &cursor = current_cursor;
	auto &mc = current_media_content;
	if(cursor.album_seq_i >= (int)mc.albums.size()){
		printf_("handle_control_track_number(): album_seq_i %i doesn't exist\n", cursor.album_seq_i);
		arduino_set_temp_text("PASS A");
		return;
	}
	const Album &album = mc.albums[cursor.album_i(mc)];
	if(track_media_index >= (int)album.tracks.size()){
		printf_("handle_control_track_number(): track_media_index %i doesn't exist\n", track_media_index);
		arduino_set_temp_text("PASS T");
		return;
	}

	current_cursor.select_track_using_media_index(mc, track_media_index);
	start_at_relative_track(0, 0);
}

void handle_control_album_number(int album_n)
{
	if(album_n < 1){
		printf_("handle_control_album_number(): album_n = %i < 1\n", album_n);
		arduino_set_temp_text("PASS");
		return;
	}
	int album_media_index = album_n - 1;

	auto &mc = current_media_content;
	if(album_media_index >= (int)mc.albums.size()){
		printf_("handle_control_album_number(): album_media_index %i doesn't exist\n", album_media_index);
		arduino_set_temp_text("PASS");
		return;
	}

	current_cursor.select_album_using_media_index(mc, album_media_index);
	current_cursor.track_seq_i = 0;
	start_at_relative_track(0, 0, true);
}

void update_display();

void handle_control_stateful_input_mode()
{
	if(stateful_input_mode < SIM_NUM_MODES - 1)
		stateful_input_mode = (StatefulInputMode)(stateful_input_mode + 1);
	else
		stateful_input_mode = (StatefulInputMode)0;

	update_display();
}

void handle_control_stateful_input_mode_input(char input_char)
{
	if(stateful_input_accu.put_char(input_char)){
		ss_ command = stateful_input_accu.command();
		printf_("Stateful input command: %s\n", cs(command));
		if(command.size() == 0)
			return;
		int input_number = stoi(command, -1);
		if(input_number == -1)
			return;
		switch(stateful_input_mode){
		case SIM_TRACK_NUMBER:
			stateful_input_accu.reset();
			handle_control_track_number(input_number);
			break;
		case SIM_ALBUM_NUMBER:
			stateful_input_accu.reset();
			handle_control_album_number(input_number);
			break;
		case SIM_NONE:
		case SIM_NUM_MODES:
			break;
		}
	}
	update_display();
}

void handle_control_stateful_input_enter()
{
	handle_control_stateful_input_mode_input('\r');
}

void handle_control_stateful_input_cancel()
{
	stateful_input_accu.reset();
	stateful_input_mode = SIM_NONE;
	update_display();
}

void update_stateful_input()
{
}

sv_<ss_> get_collection_parts();
void scan_current_mount();

void set_collection_part(const ss_ &part)
{
	current_collection_part = part;

	// Show part
	if(current_collection_part == ""){
		arduino_set_temp_text("- All -");
	} else {
		arduino_set_temp_text(squeeze(current_collection_part, arduino_display_width));
	}
	// Delay track scroll for one second
	display_update_timestamp = time(0) + 1;

	printf_("Switched to part \"%s\"\n", cs(current_collection_part));

	if(part != ""){
		// Reset cursor (unless switching away from parts into full mode)
		last_succesfully_playing_cursor = PlayCursor();
	}

	// Re-scan
	scan_current_mount();
}

void next_collection_part(int dir)
{
	sv_<ss_> collection_parts = get_collection_parts();
	int current_collection_part_i = -1;
	for(size_t i=0; i<collection_parts.size(); i++){
		if(collection_parts[i] == current_collection_part){
			current_collection_part_i = i;
			break;
		}
	}

	current_collection_part_i += dir;
	if(current_collection_part_i < 0)
		current_collection_part_i = collection_parts.size() + current_collection_part_i;

	if((size_t)current_collection_part_i >= collection_parts.size()){
		set_collection_part("");
	} else {
		if((size_t)current_collection_part_i < collection_parts.size())
			current_collection_part = collection_parts[current_collection_part_i];
		set_collection_part(current_collection_part);
	}
}

void handle_control_input_digit(int input_digit)
{
	if(stateful_input_mode != SIM_NONE){
		handle_control_stateful_input_mode_input('0'+input_digit);
		return;
	}

	if(input_digit == 1){
		const int seconds = 30;
		mpv_command_string(mpv, cs("seek -"+itos(seconds)));
		return;
	}

	if(input_digit == 2){
		const int seconds = 30;
		mpv_command_string(mpv, cs("seek +"+itos(seconds)));
		return;
	}

	if(input_digit == 3){
		next_collection_part(-1);
		// TODO: Add a temp text display prioritization system
		sleep(1); // Wait while directory is shown on screen
		return;
	}

	if(input_digit == 4){
		next_collection_part(1);
		// TODO: Add a temp text display prioritization system
		sleep(1); // Wait while directory is shown on screen
		return;
	}

	// Album and track number will be displayed
	update_display();
}

void handle_control_search(const ss_ &searchstring)
{
	printf_("Searching for \"%s\"...\n", cs(searchstring));
	auto &mc = current_media_content;
	if(mc.albums.empty()){
		printf_("Cannot search: no media\n");
		return;
	}
	// Start at current cursor + 1 and loop from end to beginning
	PlayCursor cursor = current_cursor;
	cursor.track_seq_i++;
	cursor_bound_wrap(mc, cursor);
	for(;;){
		auto &album = mc.albums[cursor.album_i(mc)];
		for(;;){
			if(cursor.track_seq_i == current_cursor.track_seq_i &&
					cursor.album_seq_i == current_cursor.album_seq_i){
				printf_("Not found\n");
				return;
			}
			auto &track = album.tracks[cursor.track_i(mc)];
			//printf_("track.display_name: %s\n", cs(track.display_name));
			if(strcasestr(track.display_name.c_str(), searchstring.c_str())){
				printf_("Found track\n");
				current_cursor = cursor;
				start_at_relative_track(0, 0);
				return;
			}
			cursor.track_seq_i++;
			if(cursor.track_seq_i >= (int)album.tracks.size())
				break;
		}
		cursor.track_seq_i = 0;
		//printf_("album.name: %s\n", cs(album.name));
		if(strcasestr(album.name.c_str(), searchstring.c_str())){
			printf_("Found album\n");
			current_cursor = cursor;
			start_at_relative_track(0, 0, true);
			return;
		}
		cursor.album_seq_i++;
		if(cursor.album_seq_i >= (int)mc.albums.size())
			cursor.album_seq_i = 0;
	}
}

void handle_control_random_album()
{
	if(current_media_content.albums.empty()){
		printf_("Picking random album when there is no media -> recovery mode: "
				"resetting and saving play cursor\n");
		current_cursor = PlayCursor();
		last_succesfully_playing_cursor = PlayCursor();
		save_stuff();
		return;
	}
	int album_seq_i = rand() % current_media_content.albums.size();
	printf_("Picking random album #%i\n", album_seq_i+1);
	current_cursor.album_seq_i = album_seq_i;
	current_cursor.track_seq_i = 0;
	start_at_relative_track(0, 0, true);
}

void handle_control_random_album_approx_num_tracks(size_t approx_num_tracks)
{
	auto &mc = current_media_content;
	sv_<int> suitable_albums;
	for(size_t i=0; i<mc.albums.size(); i++){
		auto &album = mc.albums[i];
		if(album.tracks.size() >= approx_num_tracks * 0.60 &&
				album.tracks.size() <= approx_num_tracks * 1.6){
			suitable_albums.push_back(i);
		}
	}
	if(suitable_albums.empty()){
		printf_("No suitable albums\n");
		return;
	}
	int album_seq_i = suitable_albums[rand() % suitable_albums.size()];
	auto &album = mc.albums[album_seq_i];
	printf_("Picking random album #%i (%zu tracks) from %zu suitable albums\n",
			album_seq_i+1, album.tracks.size(), suitable_albums.size());
	current_cursor.album_seq_i = album_seq_i;
	current_cursor.track_seq_i = 0;
	start_at_relative_track(0, 0, true);
}

void handle_control_random_album_min_num_tracks(size_t min_num_tracks)
{
	auto &mc = current_media_content;
	sv_<int> suitable_albums;
	for(size_t i=0; i<mc.albums.size(); i++){
		auto &album = mc.albums[i];
		if(album.tracks.size() >= min_num_tracks)
			suitable_albums.push_back(i);
	}
	if(suitable_albums.empty()){
		printf_("No suitable albums\n");
		return;
	}
	int album_seq_i = suitable_albums[rand() % suitable_albums.size()];
	auto &album = mc.albums[album_seq_i];
	printf_("Picking random album #%i (%zu tracks) from %zu suitable albums\n",
			album_seq_i+1, album.tracks.size(), suitable_albums.size());
	current_cursor.album_seq_i = album_seq_i;
	current_cursor.track_seq_i = 0;
	start_at_relative_track(0, 0, true);
}

void handle_control_random_album_max_num_tracks(size_t max_num_tracks)
{
	auto &mc = current_media_content;
	sv_<int> suitable_albums;
	for(size_t i=0; i<mc.albums.size(); i++){
		auto &album = mc.albums[i];
		if(album.tracks.size() <= max_num_tracks)
			suitable_albums.push_back(i);
	}
	if(suitable_albums.empty()){
		printf_("No suitable albums\n");
		return;
	}
	int album_seq_i = suitable_albums[rand() % suitable_albums.size()];
	auto &album = mc.albums[album_seq_i];
	printf_("Picking random album #%i (%zu tracks) from %zu suitable albums\n",
			album_seq_i+1, album.tracks.size(), suitable_albums.size());
	current_cursor.album_seq_i = album_seq_i;
	current_cursor.track_seq_i = 0;
	start_at_relative_track(0, 0, true);
}

void handle_control_random_track()
{
	auto &mc = current_media_content;
	auto &cursor = current_cursor;
	if(cursor.album_seq_i < 0 || cursor.album_seq_i >= (int)mc.albums.size())
		return;
	auto &album = mc.albums[cursor.album_i(mc)];
	int track_seq_i = rand() % album.tracks.size();
	printf_("Picking random track #%i\n", track_seq_i+1);
	current_cursor.track_seq_i = track_seq_i;
	start_at_relative_track(0, 0, false);
}

void handle_stdin()
{
	ss_ stdin_stuff = read_any(0); // 0=stdin
	for(char c : stdin_stuff){
		if(stdin_command_accu.put_char(c)){
			ss_ command = stdin_command_accu.command();
			Strfnd f(command);
			ss_ w1 = f.next(" ");
			Strfnd fn(command);
			fn.while_any("abcdefghijklmnopqrstuvwxyz");
			ss_ w1n = command.substr(0, fn.where());
			fn.while_any(" ");
			if(command == "help" || command == "h" || command == "?"){
				printf_("Commands:\n");
				printf_("  next, n, +\n");
				printf_("  prev, p, -\n");
				printf_("  nextalbum, na, N, .\n");
				printf_("  prevalbum, pa, P, ,\n");
				printf_("  pause, [space][enter]\n");
				printf_("  fwd, f <seconds: optional>\n");
				printf_("  bwd, b <seconds: optional>\n");
				printf_("  playmode, m\n");
				printf_("  playmodeget, mg\n");
				printf_("  pos\n");
				printf_("  save\n");
				printf_("  /<string> (search) (alias: 1)\n");
				printf_("  album <n>\n");
				printf_("  track <n>\n");
				printf_("  randomalbum, ra, r <approx. #tracks (optional)>\n");
				printf_("  rg, g <min. #tracks> (greater)\n");
				printf_("  rl, l <max. #tracks> (lower)\n");
				printf_("  randomtrack, rt\n");
				printf_("  albumlist, al, la\n");
				printf_("  tracklist, tl, lt\n");
				printf_("  intro\n");
				printf_("  i, info (playmodeget + pos)\n");
				printf_("  path (show path of current track)\n");
				printf_("  np/pp/rp/lp/sp<n> (next/previous/reset/list/select collection part)\n");
				printf_("  reshuffle\n");
			} else if(command == "next" || command == "n" || command == "+"){
				handle_control_next();
			} else if(command == "prev" || command == "p" || command == "-"){
				handle_control_prev();
			} else if(command == "nextalbum" || command == "na" || command == "N" || command == "."){
				handle_control_nextalbum();
			} else if(command == "prevalbum" || command == "pa" || command == "P" || command == ","){
				handle_control_prevalbum();
			} else if(command == "pause" || command == " "){
				handle_control_playpause();
			} else if(w1n == "fwd" || w1n == "f"){
				int seconds = stoi(fn.next(""), 30);
				mpv_command_string(mpv, cs("seek +"+itos(seconds)));
				current_cursor.time_pos += seconds;
				printf_("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
			} else if(w1n == "bwd" || w1n == "b"){
				int seconds = stoi(fn.next(""), 30);
				mpv_command_string(mpv, cs("seek -"+itos(seconds)));
				current_cursor.time_pos -= seconds;
				printf_("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
			} else if(command == "playmode" || command == "m"){
				handle_control_playmode();
			} else if(command == "playmodeget" || command == "mg"){
				printf_("Track progress mode: %s\n",
						tpm_to_string(current_cursor.track_progress_mode));
			} else if(command == "pos"){
				printf_("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
			} else if(command == "save"){
				save_stuff();
			} else if(command.size() >= 2 && (command.substr(0, 1) == "/" ||
					command.substr(0, 1) == "1")){
				ss_ searchstring = command.substr(1);
				handle_control_search(searchstring);
				last_searchstring = searchstring;
			} else if(command == "/" || command == "1"){
				if(last_searchstring != "")
					handle_control_search(last_searchstring);
			} else if(w1n == "album"){
				int album_n = stoi(fn.next(""), -1);
				handle_control_album_number(album_n);
			} else if(w1n == "track"){
				int track_n = stoi(fn.next(""), -1);
				handle_control_track_number(track_n);
			} else if(w1n == "randomalbum" || w1n == "ra" || w1n == "r"){
				int approx_num_tracks = stoi(fn.next(""), -1);
				if(approx_num_tracks == -1)
					handle_control_random_album();
				else
					handle_control_random_album_approx_num_tracks(approx_num_tracks);
			} else if(w1n == "rg" || w1n == "g"){
				static int last_min_num_tracks = -1;
				int min_num_tracks = stoi(fn.next(""), -1);
				if(min_num_tracks != -1){
					handle_control_random_album_min_num_tracks(min_num_tracks);
					last_min_num_tracks = min_num_tracks;
				} else if(last_min_num_tracks != -1){
					printf_("Using previous parameter: %i\n", last_min_num_tracks);
					handle_control_random_album_min_num_tracks(last_min_num_tracks);
				}
			} else if(w1n == "rl" || w1n == "l"){
				static int last_max_num_tracks = -1;
				int max_num_tracks = stoi(fn.next(""), -1);
				if(max_num_tracks != -1){
					handle_control_random_album_max_num_tracks(max_num_tracks);
					last_max_num_tracks = max_num_tracks;
				} else if(last_max_num_tracks != -1){
					printf_("Using previous parameter: %i\n", last_max_num_tracks);
					handle_control_random_album_max_num_tracks(last_max_num_tracks);
				}
			} else if(w1 == "randomtrack" || w1 == "rt"){
				handle_control_random_track();
			} else if(command == "albumlist" || command == "al" || command == "la"){
				auto &cursor = current_cursor;
				for(size_t i=0; i<current_media_content.albums.size(); i++){
					if(cursor.album_name == current_media_content.albums[i].name)
						printf_("-> #%zu: %s\n", i+1, cs(current_media_content.albums[i].name));
					else
						printf_("#%zu: %s\n", i+1, cs(current_media_content.albums[i].name));
				}
			} else if(command == "tracklist" || command == "tl" || command == "lt"){
				auto &cursor = current_cursor;
				auto &mc = current_media_content;
				if(cursor.album_seq_i >= 0 && cursor.album_seq_i < (int)mc.albums.size()){
					auto &album = mc.albums[cursor.album_i(mc)];
					for(size_t i=0; i<album.tracks.size(); i++){
						if(cursor.track_name == album.tracks[i].display_name)
							printf_("-> #%zu: %s\n", i+1, cs(album.tracks[i].display_name));
						else
							printf_("#%zu: %s\n", i+1, cs(album.tracks[i].display_name));
					}
				}
			} else if(command == "intro"){
				void do_intro();
				do_intro();
			} else if(command == "i" || command == "info"){
				if(current_collection_part != "")
					printf_("Collection part: \"%s\"\n",
							cs(current_collection_part));
				printf_("Track progress mode: %s\n",
						tpm_to_string(current_cursor.track_progress_mode));
				printf_("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
			} else if(command == "path"){
				Track track = get_track(current_media_content, current_cursor);
				printf_("%s\n", cs(track.path));
			} else if(command == "np"){
				next_collection_part(1);
			} else if(command == "pp"){
				next_collection_part(-1);
			} else if(command == "rp"){
				set_collection_part("");
			} else if(command == "lp"){
				sv_<ss_> parts = get_collection_parts();
				for(size_t i=0; i<parts.size(); i++){
					if(parts[i] == current_collection_part)
						printf_("-> #%zu: %s\n", i+1, cs(parts[i]));
					else
						printf_("#%zu: %s\n", i+1, cs(parts[i]));
				}
			} else if(w1n == "sp"){
				sv_<ss_> parts = get_collection_parts();
				int part_i = stoi(fn.next(""), 0) - 1;
				if(part_i == -1)
					set_collection_part("");
				else if((size_t)part_i < parts.size())
					set_collection_part(parts[part_i]);
				else
					printf_("Part #%i doesn't exist\n", part_i+1);
			} else if(w1n == "keypress"){
				int key = stoi(fn.next(""), -1);
				if(key != -1){
					void handle_key_press(int key);
					handle_key_press(key);
				}
			} else if(w1n == "keyrelease"){
				int key = stoi(fn.next(""), -1);
				if(key != -1){
					void handle_key_release(int key);
					handle_key_release(key);
				}
			} else if(w1n == "reshuffle"){
				printf_("Reshuffling all media\n");
				reshuffle_all_media(current_media_content);
			} else {
				printf_("Invalid command: \"%s\"\n", cs(command));
			}
		}
	}
}

void handle_key_press(int key)
{
	current_keys.insert(key);

	if(key == 24){
		if(stateful_input_mode != SIM_NONE)
			handle_control_stateful_input_cancel();
		handle_control_playpause();
		return;
	}
	if(key == 12){
		if(stateful_input_mode != SIM_NONE)
			handle_control_stateful_input_enter();
		else
			handle_control_next();
		return;
	}
	if(key == 27){
		if(stateful_input_mode != SIM_NONE)
			handle_control_stateful_input_cancel();
		else
			handle_control_prev();
		return;
	}
	if(key == 23){
		if(stateful_input_mode != SIM_NONE)
			handle_control_stateful_input_enter();
		else
			handle_control_nextalbum();
		return;
	}
	if(key == 29){
		if(stateful_input_mode != SIM_NONE)
			handle_control_stateful_input_cancel();
		else
			handle_control_prevalbum();
		return;
	}
	if(key == 17){ // Upmost center
		if(stateful_input_mode != SIM_NONE)
			handle_control_stateful_input_cancel();
		handle_control_playmode();
		return;
	}
	if(key == 18){ // Right upper
		handle_control_random_album();
		return;
	}
	if(key == 13){ // Right lower
		handle_control_stateful_input_mode();
		return;
	}
	if(key == 21){ // 1
		handle_control_input_digit(1);
		return;
	}
	if(key == 16){ // 2
		handle_control_input_digit(2);
		return;
	}
	if(key == 10){ // 3
		handle_control_input_digit(3);
		return;
	}
	if(key == 15){ // 4
		handle_control_input_digit(4);
		return;
	}
	if(key == 20){ // 5
		handle_control_input_digit(5);
		return;
	}
	if(key == 25){ // 6
		handle_control_input_digit(6);
		return;
	}
}

void handle_key_release(int key)
{
	current_keys.erase(key);

	if(stateful_input_mode == SIM_NONE){
		if(key == 21 || key == 16 || key == 10 || key == 15 || key == 20 || key == 25){
			if(!current_keys.count(21) && !current_keys.count(16) &&
					!current_keys.count(10) && !current_keys.count(15) &&
					!current_keys.count(20) && !current_keys.count(25)){
				temp_display_album();
			}
		}
	}
}

void try_open_arduino_serial()
{
#ifdef __WIN32__
#else
	for(const ss_ &arduino_serial_path : arduino_serial_paths){
		arduino_serial_fd = open(arduino_serial_path.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
		if(arduino_serial_fd < 0){
			printf_("Failed to open %s\n", cs(arduino_serial_path));
			arduino_serial_fd = -1;
			continue;
		}
		if(!set_interface_attribs(arduino_serial_fd, 9600, 0)){
			printf_("Failed to set attributes for serial fd\n");
			continue;
		}
		printf_("Opened arduino serial port %s\n", cs(arduino_serial_path));
		arduino_serial_fd_path = arduino_serial_path;
		return;
	}
#endif
}

void handle_hwcontrols()
{
	update_stateful_input();

	if(arduino_serial_fd == -1){
		static time_t last_retry_time = 0;
		if(last_retry_time < time(0) - 5 && !arduino_serial_paths.empty()){
			last_retry_time = time(0);
			printf_("Retrying arduino serial\n");
			try_open_arduino_serial();
		}
		if(arduino_serial_fd == -1){
			return;
		}
	}
	bool error = false;
	ss_ serial_stuff = read_any(arduino_serial_fd, &error);
	if(error){
		arduino_serial_fd = -1;
		return;
	}
	for(char c : serial_stuff){
		if(arduino_message_accu.put_char(c)){
			arduino_last_incoming_message_timestamp = time(0);
			ss_ message = arduino_message_accu.command();
			Strfnd f(message);
			ss_ first = f.next(":");
			if(first == "<KEY_PRESS"){
				int key = stoi(f.next(":"));
				printf_("<KEY_PRESS  : %i\n", key);
				handle_key_press(key);
			} else if(first == "<KEY_RELEASE"){
				int key = stoi(f.next(":"));
				printf_("<KEY_RELEASE: %i\n", key);
				handle_key_release(key);
			} else if(first == "<BOOT"){
				printf_("<BOOT\n");
				arduino_set_extra_segments();
				temp_display_album();
				refresh_track();

				arduino_request_version();
			} else if(first == "<MODE"){
				ss_ mode = f.next(":");
				if(mode == "RASPBERRY"){
					if(current_cursor.current_pause_mode == PM_UNFOCUS_PAUSE){
						printf_("Leaving unfocus pause\n");
						check_mpv_error(mpv_command_string(mpv, "pause"));
						current_cursor.current_pause_mode = PM_PLAY;
					}
				} else {
					if(current_cursor.current_pause_mode == PM_PLAY){
						printf_("Entering unfocus pause\n");
						check_mpv_error(mpv_command_string(mpv, "pause"));
						current_cursor.current_pause_mode = PM_UNFOCUS_PAUSE;
					}
				}
			} else if(first == "<POWERDOWN_WARNING"){
				printf_("<POWERDOWN_WARNING\n");
				save_stuff();
			} else if(first == "<VERSION"){
				printf_("%s\n", cs(message));
				ss_ version = f.next("");
				if(!tried_to_update_arduino_firmware){
					tried_to_update_arduino_firmware = true;
					arduino_firmware_update_if_needed(version);
				}
			} else {
				printf_("%s (ignored)\n", cs(message));
			}
		}
	}

	// Trigger arduino firmware upgrade if arduino doesn't send anything
	if(arduino_serial_fd_path != "" &&
			!tried_to_update_arduino_firmware &&
			arduino_last_incoming_message_timestamp < time(0) - 10 &&
			startup_timestamp < time(0) - 10){
		tried_to_update_arduino_firmware = true;
		arduino_firmware_update_if_needed("(no version response)");
	}
}

void display_stateful_input()
{
	ss_ current_input;
	for(size_t i=0; i<stateful_input_accu.next_i; i++){
		current_input += stateful_input_accu.buffer[i];
	}
	switch(stateful_input_mode){
	case SIM_TRACK_NUMBER:
		arduino_set_text(current_input + (time(0)%2?"_":" ") + " TRACK");
		break;
	case SIM_ALBUM_NUMBER:
		arduino_set_text(current_input + (time(0)%2?"_":" ") + " ALBUM");
		break;
	case SIM_NONE:
	case SIM_NUM_MODES:
		arduino_set_text("ERROR");
		break;
	}
}

void update_display()
{
	display_update_timestamp = time(0);

	if(stateful_input_mode != SIM_NONE){
		display_stateful_input();
		return;
	}

	if(current_keys.count(21) || current_keys.count(16) ||
			current_keys.count(10) || current_keys.count(15) ||
			current_keys.count(20) || current_keys.count(25)){
		// Numeric key without any special mode.
		// Temporarily display album and track number until key isn't pressed.
		auto &mc = current_media_content;
		ss_ s = itos(current_cursor.album_i(mc)+1)+"-"+itos(current_cursor.track_i(mc)+1);
		arduino_set_temp_text(s);
	}

	if(current_media_content.albums.empty()){
		arduino_set_text("NO MEDIA");
		return;
	}

	ss_ track_name = get_track_name(current_media_content, current_cursor);
	if(minimize_display_updates && track_name == display_last_shown_track_name)
		return;
	if(track_name != display_last_shown_track_name){
		display_last_shown_track_name = track_name;
		display_next_startpos = 0;
	}
	ss_ squeezed = squeeze(track_name, arduino_display_width * 2, display_next_startpos);
	if(squeezed == ""){
		display_next_startpos = 0;
		squeezed = squeeze(track_name, arduino_display_width * 2, display_next_startpos);
	}
	if((int)squeezed.size() >= arduino_display_width)
		squeezed = squeeze(squeezed, arduino_display_width * 2);
	arduino_set_text(squeezed);
}

void handle_display()
{
	if(display_update_timestamp > time(0) - 1)
		return;
	update_display();
	if(!minimize_display_updates)
		display_next_startpos += arduino_display_width;
}

void eat_all_mpv_events()
{
	for(;;){
		mpv_event *event = mpv_wait_event(mpv, 0);
		if(event->event_id == MPV_EVENT_NONE)
			break;
		if(LOG_MPV)
			printf_("MPV: %s (eaten)\n", mpv_event_name(event->event_id));
	}
}

void wait_mpv_event(int event_id, int max_ms)
{
	for(int i=0; i<max_ms/5; i++){
		for(;;){
			mpv_event *event = mpv_wait_event(mpv, 0);
			if(event->event_id == MPV_EVENT_NONE)
				break;
			if(LOG_MPV)
				printf_("MPV: %s (waited over)\n", mpv_event_name(event->event_id));
			if(event->event_id == event_id)
				return;
		}
		usleep(5000);
	}
}

void automated_start_play_next_track()
{
	printf_("Automated start of next track\n");

	switch(current_cursor.track_progress_mode){
	case TPM_NORMAL:
	case TPM_ALBUM_REPEAT:
	case TPM_SHUFFLE_ALL:
	case TPM_SHUFFLE_TRACKS:
	case TPM_SMART_TRACK_SHUFFLE:
	case TPM_SMART_ALBUM_SHUFFLE:
	case TPM_MR_SHUFFLE:
		current_cursor.track_seq_i++;
		current_cursor.time_pos = 0;
		current_cursor.stream_pos = 0;
		// NOTE: Album repeat and shuffle is done here
		cursor_bound_wrap(current_media_content, current_cursor);
		current_cursor.track_name = get_track_name(current_media_content, current_cursor);
		current_cursor.album_name = get_album_name(current_media_content, current_cursor);
		printf_("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
		load_and_play_current_track_from_start();
		break;
	case TPM_ALBUM_REPEAT_TRACK:
		current_cursor.time_pos = 0;
		current_cursor.stream_pos = 0;
		printf_("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
		refresh_track();
		break;
	case TPM_NUM_MODES:
		break;
	}
}

void do_something_instead_of_idle()
{
	if(current_media_content.albums.empty()){
		// There are no tracks; do nothing
		return;
	}

	if(LOG_DEBUG)
		printf_("Trying to do something instead of idle\n");

	// If the currently playing file does not exist, and the current device does
	// not exist, stop playback and wait until the media is available again

	Track track = get_track(current_media_content, current_cursor);
	if(track.path == ""){
		// Weren't trying to play anything
		automated_start_play_next_track();
		return;
	}

	if(access(track.path.c_str(), F_OK) == 0){
		// File exists; go to next file
		automated_start_play_next_track();
		return;
	}

	void handle_changed_partitions();
	handle_changed_partitions();

	if(!current_media_content.albums.empty()){
		// The file disappeared but media is still available; go to next file
		automated_start_play_next_track();
		return;
	}

	// Media got unmounted; do nothing and wait until media is available again
	printf_("Media got unmounted and there are no tracks available\n");
}

void handle_mpv()
{
	for(;;){
		mpv_event *event = mpv_wait_event(mpv, 0);
		if(event->event_id == MPV_EVENT_NONE)
			break;
		if(LOG_MPV)
			printf_("MPV: %s\n", mpv_event_name(event->event_id));
		if(event->event_id == MPV_EVENT_SHUTDOWN){
			do_main_loop = false;
		}
		if(event->event_id == MPV_EVENT_IDLE){
			do_something_instead_of_idle();
		}
		if(event->event_id == MPV_EVENT_FILE_LOADED){
			track_was_loaded = true;
			if(current_cursor.stream_end == 0){
				int64_t stream_end = 0;
				mpv_get_property(mpv, "stream-end", MPV_FORMAT_INT64, &stream_end);
				current_cursor.stream_end = stream_end;
				current_cursor.stream_end = stream_end;
				if(LOG_DEBUG){
					printf_("Got current track stream_end: %" PRId64 "\n",
							current_cursor.stream_end);
				}
			}
			if(queued_pause){
				queued_pause = false;
				if(LOG_DEBUG)
					printf_("Executing queued pause\n");
				check_mpv_error(mpv_command_string(mpv, "pause"));
				arduino_set_temp_text("PAUSE");
				current_cursor.current_pause_mode = PM_PAUSE;
				arduino_set_extra_segments();
				printf_("Paused.\n");
			}
		}
	}

	static time_t last_time_pos_get_timestamp = 0;
	if(last_time_pos_get_timestamp != time(0)){
		last_time_pos_get_timestamp = time(0);

		int64_t stream_pos = 0;
		mpv_get_property(mpv, "stream-pos", MPV_FORMAT_INT64, &stream_pos);
		if(stream_pos > 0){
			double time_pos = 0;
			mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &time_pos);

			current_cursor.time_pos = time_pos;
			current_cursor.stream_pos = stream_pos;
			last_succesfully_playing_cursor = current_cursor;

			if(current_cursor.stream_end == 0){
				int64_t stream_end = 0;
				mpv_get_property(mpv, "stream-end", MPV_FORMAT_INT64, &stream_end);
				current_cursor.stream_end = stream_end;
				current_cursor.stream_end = stream_end;
				printf_("Got current track stream_end: %" PRId64 "\n",
						current_cursor.stream_end);
			}

			if(!minimize_display_updates || time(0) % 10 == 0){
				arduino_serial_write(">PROGRESS:"+
						itos(stream_pos * 255 / current_cursor.stream_end)+"\r\n");
			}

			// Reset starting position so that if this track is being looped, it
			// will start at the beginning
			mpv_set_option_string(mpv, "start", "#1");
		}
	}

	// Handle idle state that wasn't taken care of immediately for whatever
	// reason
	bool idle = mpv_is_idle();
	if(idle){
		printf_("MPV is idle\n");
		if(mpv_last_not_idle_timestamp == 0){
			mpv_last_not_idle_timestamp = time(0);
		} else if(mpv_last_not_idle_timestamp > time(0) - 5){
			// Fine enough until 5 seconds of idle
		} else {
			if(LOG_DEBUG)
				printf_("MPV Idled for too long; doing something\n");
			mpv_last_not_idle_timestamp = time(0);
			do_something_instead_of_idle();
		}
	} else {
		mpv_last_not_idle_timestamp = time(0);
	}
}

bool filename_supported(const ss_ &name)
{
	// Not all of these are even actually supported but at least nothing
	// ridiculous is included so that browsing random USB storage things is
	// possible
	static set_<ss_> supported_file_extensions = {
		"3ga", "aac", "aif", "aifc", "aiff", "amr", "au", "aup", "caf", "flac",
		"gsm", "iff", "kar", "m4a", "m4p", "m4r", "mid", "midi", "mmf", "mp2",
		"mp3", "mpga", "ogg", "oma", "opus", "qcp", "ra", "ram", "wav", "wma",
		"xspf", "3g2", "3gp", "3gpp", "asf", "avi", "divx", "f4v", "flv",
		"h264", "ifo", "m2ts", "m4v", "mkv", "mov", "mp4", "mpeg", "mpg",
		"mswmm", "mts", "mxf", "ogv", "rm", "swf", "ts", "vep", "vob", "webm",
		"wlmp", "wmv", "aac", "cue", "d64", "flac", "m4a", "mp4", "s3m", "sfv",
		"swf", "wav", "xd",
		// These don't really work properly (playlists or unsupported formats)
		//"m3u", "pls", "srt", "spc", "t64", "xm", "rar", "sid", "mid", "mod",
		//"it"
	};

	// Check file extension
	ss_ ext;
	for(int i=name.size()-1; i>=0; i--){
		if(name[i] == '.'){
			ext = name.substr(i+1);
			for(size_t i=0; i<ext.size(); i++)
				ext[i] = tolower(ext[i]);
			break;
		}
	}
	return supported_file_extensions.count(ext);
}

static bool is_default_root_name(const ss_ &name)
{
	if(name == "root")
		return true;
	if(name.size() >= 5 && name.substr(0, 5) == "root_")
		return true;
	return false;
}

void scan_directory(const ss_ &root_name, const ss_ &path, sv_<Album> &result_albums,
		Album *parent_dir_album=NULL)
{
	DirLister dl(path.c_str());

	Album root_album;
	if(root_name.size() <= 7 && parent_dir_album &&
			!is_default_root_name(parent_dir_album->name)){
		root_album.name = root_name+" | "+parent_dir_album->name;
	} else {
		root_album.name = root_name;
	}

	sv_<ss_> subdirs;

	for(;;){
		int ftype;
		char fname[PATH_MAX];
		if(!dl.get_next(&ftype, fname, PATH_MAX))
			break;
		if(fname[0] == '.')
			continue;
		if(ftype == FS_FILE){
			if(!filename_supported(fname))
				continue;
			//printf_("File: %s\n", cs(path+"/"+fname));
			char stripped[100];
			snprintf(stripped, sizeof stripped, fname);
			strip_file_extension(stripped);
			root_album.tracks.push_back(Track(path+"/"+fname, stripped));
		} else if(ftype == FS_DIR){
			//printf_("Dir: %s\n", cs(path+"/"+fname));
			subdirs.push_back(fname);
		}
	}

	// Sort subdirs
	std::sort(subdirs.begin(), subdirs.end());

	// Scan subdirs
	for(const ss_ &fname : subdirs){
		scan_directory(fname, path+"/"+fname, result_albums, &root_album);
	}

	// Sort by path
	std::sort(root_album.tracks.begin(), root_album.tracks.end());

	if(!root_album.tracks.empty()){
		if(parent_dir_album){
			// If there is only one track, don't create a new album and instead
			// just push the track to the parent directory album
			if(root_album.tracks.size() == 1)
				parent_dir_album->tracks.push_back(root_album.tracks[0]);
			else
				result_albums.push_back(root_album);
		} else {
			result_albums.push_back(root_album);
		}
	}
}

sv_<ss_> get_collection_parts()
{
	sv_<ss_> media_paths;

	if(!static_media_paths.empty()){
		for(const ss_ &path : static_media_paths)
			media_paths.push_back(path);
	} else {
		media_paths.push_back(current_mount_path);
	}

	sv_<ss_> subdirs;

	for(const ss_ &path : media_paths){
		DirLister dl(path.c_str());
		for(;;){
			int ftype;
			char fname[PATH_MAX];
			if(!dl.get_next(&ftype, fname, PATH_MAX))
				break;
			if(fname[0] == '.')
				continue;
			if(ftype == FS_FILE){
				continue;
			} else if(ftype == FS_DIR){
				//printf_("Dir: %s\n", cs(path+"/"+fname));
				subdirs.push_back(fname);
				// TODO: Don't add duplicates
			}
		}
	}

	// Sort subdirs
	std::sort(subdirs.begin(), subdirs.end());

	return subdirs;
}

void scan_current_mount()
{
	if(current_collection_part != "")
		printf_("Scanning (collection: \"%s\")\n", cs(current_collection_part));
	else
		printf_("Scanning...\n");

	//disappeared_tracks.clear();
	current_media_content.albums.clear();

	ss_ scan_midfix;
	if(current_collection_part != "")
		scan_midfix = "/"+current_collection_part;

	if(!static_media_paths.empty()){
		int n = 1;
		for(const ss_ &path : static_media_paths){
			ss_ root_name = static_media_paths.size() == 1 ? "root" : "root_"+itos(n++);
			scan_directory(root_name, path+scan_midfix, current_media_content.albums);
		}
	} else {
		scan_directory("root", current_mount_path+scan_midfix, current_media_content.albums);
	}

	// Create shuffled orders
	reshuffle_all_media(current_media_content);

	printf_("Scanned %zu albums.\n", current_media_content.albums.size());

	current_cursor = last_succesfully_playing_cursor;

	if(current_cursor.album_seq_i == 0 && current_cursor.track_seq_i == 0 &&
			current_cursor.track_name == ""){
		if(LOG_DEBUG)
			printf_("Starting without saved state; picking random album\n");
		handle_control_random_album();
		return;
	}

	if(!static_media_paths.empty() && current_media_content.albums.empty()){
		// There are static media paths and there are no tracks; do nothing
		printf_("No media.\n");
		return;
	}

	if(!force_resolve_track(current_media_content, current_cursor)){
		printf_("Force-resolve track failed; picking random album\n");
		handle_control_random_album();
		return;
	}

	temp_display_album();
	force_start_at_cursor();
}

bool check_partition_exists(const ss_ &devname0)
{
	std::ifstream f("/proc/partitions");
	if(!f.good()){
		printf_("Can't read /proc/partitions\n");
		return false;
	}
	ss_ proc_partitions_data = ss_((std::istreambuf_iterator<char>(f)),
			std::istreambuf_iterator<char>());

	Strfnd f_lines(proc_partitions_data);
	for(;;){
		if(f_lines.atend()) break;
		ss_ line = f_lines.next("\n");
		Strfnd f_columns(line);
		f_columns.while_any(" ");
		f_columns.next(" ");
		f_columns.while_any(" ");
		f_columns.next(" ");
		f_columns.while_any(" ");
		f_columns.next(" ");
		f_columns.while_any(" ");
		ss_ devname = f_columns.next(" ");
		if(devname == "")
			continue;
		if(devname == devname0)
			return true;
	}
	return false;
}

ss_ get_device_mountpoint(const ss_ &devname0)
{
	std::ifstream f("/proc/mounts");
	if(!f.good()){
		printf_("Can't read /proc/mounts\n");
		return "";
	}
	ss_ proc_mounts_data = ss_((std::istreambuf_iterator<char>(f)),
			std::istreambuf_iterator<char>());

	Strfnd f_lines(proc_mounts_data);
	for(;;){
		if(f_lines.atend()) break;
		ss_ line = f_lines.next("\n");
		Strfnd f_columns(line);
		f_columns.while_any(" ");
		ss_ devpath = f_columns.next(" ");
		f_columns.while_any(" ");
		ss_ mountpoint = f_columns.next(" ");
		Strfnd f_devpath(devpath);
		ss_ devname;
		for(;;){
			ss_ s = f_devpath.next("/");
			if(s != "")
				devname = s;
			if(f_devpath.atend())
				break;
		}
		if(devname == devname0){
			/*printf_("is_device_mounted(): %s is mounted at %s\n",
					cs(devname0), cs(mountpoint));*/
			return mountpoint;
		}
	}
	//printf_("is_device_mounted(): %s is not mounted\n", cs(devname0));
	return "";
}

void handle_changed_partitions()
{
	if(!static_media_paths.empty()){
		if(current_mount_path != static_media_paths[0]){
			printf_("Using static media paths:\n");
			for(size_t i=0; i<static_media_paths.size(); i++){
				printf_("- %s\n", cs(static_media_paths[i]));
			}
			current_mount_device = "dummy";
			current_mount_path = static_media_paths[0];
			scan_current_mount();
		}
		return;
	}

#ifndef __WIN32__
	if(current_mount_device != ""){
		if(!check_partition_exists(current_mount_device)){
			static time_t umount_last_failed_timestamp = 0;
			if(umount_last_failed_timestamp > time(0) - 15){
				// Stop flooding these dumb commands
			} else {
				// Unmount it if the partition doesn't exist anymore
				printf_("Device %s does not exist anymore; umounting\n",
						cs(current_mount_path));
				int r = umount(current_mount_path.c_str());
				if(r == 0){
					printf_("umount %s succesful\n", current_mount_path.c_str());
					current_mount_device = "";
					current_mount_path = "";
					current_media_content.albums.clear();
				} else {
					printf_("umount %s failed: %s\n", current_mount_path.c_str(), strerror(errno));
					umount_last_failed_timestamp = time(0);
				}
			}
		} else if(get_device_mountpoint(current_mount_device) == ""){
			printf_("Device %s got unmounted from %s\n", cs(current_mount_device),
					cs(current_mount_path));
			current_mount_device = "";
			current_mount_path = "";
			current_media_content.albums.clear();
		}
	}

	if(current_mount_device != ""){
		// This can get extremely spammy; thus it is commented out
		/*printf_("Ignoring partition change because we have mounted %s at %s\n",
				cs(current_mount_device), cs(current_mount_path));*/
		return;
	}

	std::ifstream f("/proc/partitions");
	if(!f.good()){
		printf_("Can't read /proc/partitions\n");
		return;
	}
	ss_ proc_partitions_data = ss_((std::istreambuf_iterator<char>(f)),
			std::istreambuf_iterator<char>());

	Strfnd f_lines(proc_partitions_data);
	for(;;){
		if(f_lines.atend()) break;
		ss_ line = f_lines.next("\n");
		Strfnd f_columns(line);
		f_columns.while_any(" ");
		f_columns.next(" ");
		f_columns.while_any(" ");
		f_columns.next(" ");
		f_columns.while_any(" ");
		f_columns.next(" ");
		f_columns.while_any(" ");
		ss_ devname = f_columns.next(" ");
		if(devname == "")
			continue;
		bool found = false;
		for(const ss_ &s : track_devices){
			if(devname.size() < s.size())
				continue;
			// Match beginning of device name
			if(devname.substr(0, s.size()) == s){
				found = true;
				break;
			}
		}
		if(!found)
			continue;
		printf_("Tracked partition: %s\n", cs(devname));

		ss_ existing_mountpoint = get_device_mountpoint(devname);
		if(existing_mountpoint != ""){
			printf_("%s is already mounted at %s; using it\n",
					cs(devname), cs(existing_mountpoint));
			current_mount_device = devname;
			current_mount_path = existing_mountpoint;

			scan_current_mount();
			return;
		}

		ss_ dev_path = "/dev/"+devname;
		ss_ new_mount_path = "/tmp/__autosoitin_mnt";
		printf_("Mounting %s at %s\n", cs(dev_path), cs(new_mount_path));
		mkdir(cs(new_mount_path), 0777);
		mkdir(cs(new_mount_path), 0777);
		int r = mount(dev_path.c_str(), new_mount_path.c_str(), "vfat",
				MS_MGC_VAL | MS_RDONLY | MS_NOEXEC | MS_NOSUID | MS_DIRSYNC |
						MS_NODEV | MS_SYNCHRONOUS,
				NULL);
		if(r == 0){
			printf_("Succesfully mounted.\n");
			current_mount_device = devname;
			current_mount_path = new_mount_path;

			scan_current_mount();
			return;
		} else {
			printf_("Failed to mount (%s); trying next\n", strerror(errno));
		}
	}
#endif
}

#ifndef __WIN32__
bool partitions_changed = false;

void handle_mount()
{
	if(!static_media_paths.empty())
		return;

	// Calls callbacks; eg. handle_changed_partitions()
	for(auto fd : partitions_watch->get_fds()){
		partitions_watch->report_fd(fd);
	}

	if(partitions_changed){
		partitions_changed = false;
		printf_("Partitions changed\n");
		handle_changed_partitions();
	}

	// Add watched paths after a delay because these paths don't necessarily
	// exist at the time this program starts up
	static int64_t startup_delay = -1;
	if(startup_delay == -1){
		startup_delay = time(0);
	} else if(startup_delay == -2){
		// Inotify watchers have been initialized

		// Still check once in a while because these systems are unreliable as
		// fuck for whatever reason; they should probably be fixed though
		static time_t last_timestamp = 0;
		if(last_timestamp <= time(0) - 10){
			last_timestamp = time(0);

			handle_changed_partitions();
		}
	} else if(startup_delay < time(0) - 15){
		startup_delay = -2;

		// Have a few of these because some of them seem to work on some systems
		// while others work on other systems
		try {
			partitions_watch->add("/dev/disk", [](const ss_ &path){
				partitions_changed = true;
			});
		} catch(Exception &e){}
		try {
			partitions_watch->add("/dev/disk/by-path", [](const ss_ &path){
				partitions_changed = true;
			});
		} catch(Exception &e){}
		try {
			partitions_watch->add("/dev/disk/by-uuid", [](const ss_ &path){
				partitions_changed = true;
			});
		} catch(Exception &e){}

		// Manually check for changed partitions for the last time
		handle_changed_partitions();
	} else {
		// Manually check for changed partitions during boot-up (every
		// second)
		static time_t last_timestamp = 0;
		if(last_timestamp != time(0)){
			last_timestamp = time(0);

			handle_changed_partitions();
		}
	}
}
#else
void handle_mount()
{
}
#endif

void handle_periodic_save()
{
	// If there is a non-clean shutdown, this should save us
	if(last_save_timestamp == 0){
		last_save_timestamp = time(0);
		return;
	}
	if(last_save_timestamp > time(0) - 60){
		return;
	}
	save_stuff();
}

#ifdef __WIN32__
BOOL WINAPI windowsConsoleCtrlHandler(DWORD signal)
{
	if(signal == CTRL_C_EVENT){
		printf_("CTRL+C\n");
		save_stuff();
		do_main_loop = false;
		return TRUE;
	}
	return FALSE;
}
#else
void sigint_handler(int _)
{
	printf_("SIGINT\n");
	save_stuff();
	do_main_loop = false;
}
#endif

void do_intro()
{
	printf_("⌁ OVER POWERED TRACK SWITCH ⌁\n");
}

// First call with command line arguments, then with config arguments
int handle_args(int argc, char *argv[], const char *error_prefix, bool from_config)
{
	c55_argi = 0; // Reset c55_getopt
	c55_cp = NULL; // Reset c55_getopt

	const char opts[100] = "hC:s:d:S:m:D:UW:l:";
	const char usagefmt[1000] =
			"Usage: %s [OPTION]...\n"
			"  -h                   Show this help\n"
			"  -C [path]            Configuration file path (default: $HOME/.config/opts/opts)\n"
			"  -s [path]            Serial port device of Arduino (pass multiple -s to specify many)\n"
			"  -d [dev1,dev2,...]   Block devices to track and mount (eg. sdc)\n"
			"  -S [path]            Saved state path\n"
			"  -m [path]            Static media path; automounting is disabled if set and root privileges are not needed; multiple allowed\n"
			"  -D [mode]            Set arduino serial debug mode (off/raw/fancy)\n"
			"  -U                   Minimize display updates\n"
			"  -W [integer]         Set text display width\n"
			"  -l [string]          Enable log source (mpv/debug)\n"
			;

	int c;
	while((c = c55_getopt(argc, argv, opts)) != -1)
	{
		switch(c)
		{
		case 'h':
			printf_(usagefmt, argv[0]);
			return 1;
		case 'C':
			config_path = c55_optarg;
			config_must_be_readable = true;
			break;
		case 's':
			arduino_serial_paths.push_back(c55_optarg);
			break;
		case 'd':
			{
				Strfnd f(c55_optarg);
				printf_("Tracking:");
				for(;;){
					ss_ dev = f.next(",");
					if(dev == "") break;
					printf_(" %s", cs(dev));
					track_devices.push_back(dev);
				}
				printf_("\n");
			}
			break;
		case 'S':
			saved_state_path = c55_optarg;
			break;
		case 'm':
			if(from_config && there_are_command_line_static_media_paths){
				// Don't add from config if some were specified on command line
			} else {
				static_media_paths.push_back(c55_optarg);
				there_are_command_line_static_media_paths = !from_config;
			}
			break;
		case 'D':
			arduino_serial_debug_mode = c55_optarg;
			break;
		case 'U':
			minimize_display_updates = true;
			break;
		case 'W':
			arduino_display_width = atoi(c55_optarg);
			break;
		case 'l':
			enabled_log_sources.insert(c55_optarg);
			break;
		default:
			if(error_prefix)
				fprintf_(stderr, "%s\n", error_prefix);
			fprintf_(stderr, "Invalid argument\n");
			fprintf_(stderr, usagefmt, argv[0]);
			return 1;
		}
	}
	return 0;
}

void generate_default_paths()
{
	ss_ config_dir;
#ifdef __WIN32__
	const char *appdata = getenv("APPDATA");
	if(!appdata){
		printf_("$APPDATA not set - cannot read config\n");
		return;
	}
	config_dir = ss_() + appdata + "/opts";
#else
	const char *home = getenv("HOME");
	if(!home){
		printf_("$HOME not set - cannot read config\n");
		return;
	}
	config_dir = ss_() + home + "/.config/opts";
#endif

	if(config_path == "__default__"){
		config_path = config_dir + "/opts";

		if(LOG_DEBUG)
			printf_("Default config_path: \"%s\"\n", cs(config_path));
	}

	if(saved_state_path == "__default__"){
		saved_state_path = config_dir + "/state";

		if(LOG_DEBUG)
			printf_("Default saved_state_path: \"%s\"\n", cs(saved_state_path));

		if(mkdir_p(config_dir.c_str())){
			printf_("Warning: Failed to create directory: \"%s\"\n", cs(config_dir));
		}
	}
}

// Modifies content to contain null bytes
bool read_config(char *content, size_t content_max_len, sv_<char*> &argv)
{
	if(LOG_DEBUG)
		printf_("Loading config file from %s\n", cs(config_path));
	ss_ config_content;
	if(!read_file_content(config_path, config_content)){
		if(LOG_DEBUG || config_must_be_readable)
			printf_("Couldn't read config file \"%s\"\n", cs(config_path));
		return config_must_be_readable ? false : true;
	}
	// Translate ~ in config to $HOME
	const char *home = getenv("HOME");
	if(home){
		replace_string(config_content, "~", home);
	}
	// Prepend with argv[0]="opts"
	size_t content_len = snprintf(content, content_max_len, "opts %s", cs(config_content));
	bool argv_added = false;
	char current_quote = 0;
	for(size_t i=0; i<content_len; i++){
		char &c = content[i];
		if(current_quote){
			if(c == current_quote){
				current_quote = 0;
				continue;
			}
			continue;
		}
		if(c == '"' || c == '\''){
			current_quote = c;
			continue;
		}
		if(c == ' ' || c == '\n' || c == '\r' || c == '\t'){
			c = 0;
			argv_added = false;
			continue;
		}
		if(!argv_added){
			argv.push_back(&content[i]);
			argv_added = true;
		}
	}
	if(current_quote){
		printf_("Warning: Config file has non-ending quote\n");
	}
	return true;
}

int main(int argc, char *argv[])
{
#ifdef __WIN32__
	SetConsoleCtrlHandler(windowsConsoleCtrlHandler, TRUE);
#else
	signal(SIGINT, sigint_handler);
#endif
	startup_timestamp = time(0);
	srand(time(0));

	if(int r = handle_args(argc, argv, NULL, false) != 0){
		return r;
	}

	generate_default_paths();

	char config_content[5000];
	sv_<char*> config_argv;
	if(!read_config(config_content, sizeof config_content, config_argv)){
		return 1;
	}

	if(LOG_DEBUG){
		printf_("Config args:\n");
		for(char *a : config_argv){
			printf_("  %s\n", a);
		}
	}

	if(int r = handle_args(config_argv.size(), &config_argv[0],
			"Error in configration file:", true) != 0){
		return r;
	}

	if(track_devices.empty() && static_media_paths.empty()){
		printf_("Use -d or -m\n");
		return 1;
	}

	if(arduino_serial_debug_mode != "off" && arduino_serial_debug_mode != "raw" &&
			arduino_serial_debug_mode != "fancy"){
		printf_("Invalid arduino serial debug mode (-D) (%s)\n",
				cs(arduino_serial_debug_mode));
		return 1;
	}

	do_intro();

	load_stuff();

	try_open_arduino_serial();

#ifndef __WIN32__
	partitions_watch.reset(createFileWatch(
			IN_MOVED_TO | IN_CREATE | IN_MOVED_FROM | IN_DELETE | IN_ATTRIB));
#endif

    mpv = mpv_create();
    if (!mpv) {
        printf_("mpv_create() failed");
        return 1;
    }
    
    mpv_set_option_string(mpv, "vo", "null");

    check_mpv_error(mpv_initialize(mpv));

	// Wait idle event so that we don't do things twice at startup
	arduino_set_text("WAIT IDLE");
	wait_mpv_event(MPV_EVENT_IDLE, 5000);
	arduino_set_text("OK");

	if(LOG_DEBUG)
		printf_("Doing initial partition scan\n");
	handle_changed_partitions();

	change_track_progress_mode(current_cursor.track_progress_mode);

	while(do_main_loop){
		handle_stdin();

		handle_hwcontrols();

		handle_display();

		handle_mpv();

		handle_mount();

		handle_periodic_save();

		usleep(1000000/60);
	}

    mpv_terminate_destroy(mpv);
    close(arduino_serial_fd);
    return 0;
}

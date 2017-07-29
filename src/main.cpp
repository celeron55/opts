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
#include "ui.hpp"
#include "print.hpp"
#include "library.hpp"
#include "play_cursor.hpp"
#include "arduino_global.hpp"
#include "media_scan.hpp"
#include "mpv_control.hpp"
#include "ui_output_queue.hpp"
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

time_t startup_timestamp = 0;

bool do_main_loop = true;
mpv_handle *mpv = NULL;
CommandAccumulator<100> stdin_command_accu;

ss_ current_collection_part;

bool queued_pause = false;
sv_<size_t> queued_album_shuffled_track_order;

MediaContent current_media_content;

TrackProgressMode track_progress_mode = TPM_NORMAL;
PlayCursor current_cursor;
PlayCursor last_succesfully_playing_cursor;

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

void command_playpause()
{
	ui_flush_display();

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
			ui_output_queue::push_message("PAUSE");
		} else {
			ui_output_queue::push_message("RESUME");
		}
		void arduino_set_extra_segments();
		arduino_set_extra_segments();
	} else {
		// No track is loaded; load from cursor
		force_start_at_cursor();
	}
}

void command_next()
{
	ui_flush_display();
	start_at_relative_track(0, 1);
}

void command_prev()
{
	ui_flush_display();
	start_at_relative_track(0, -1);
}

void command_nextalbum()
{
	ui_flush_display();
	start_at_relative_track(1, 0);
}

void command_prevalbum()
{
	ui_flush_display();
	start_at_relative_track(-1, 0);
}

// Call this before ui_output_queue::push_message or track change to get
// immediate display response
void ui_flush_display()
{
	ui_output_queue::unprioritize_queue();
	display_update_timestamp = 0;
}

void change_track_progress_mode(TrackProgressMode track_progress_mode)
{
	printf_("Track progress mode: %s\n", tpm_to_string(track_progress_mode));

	ui_flush_display();
	ui_output_queue::push_message(tpm_to_string(track_progress_mode));

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

void command_playmode()
{
	TrackProgressMode track_progress_mode = current_cursor.track_progress_mode;

	if(track_progress_mode < TPM_NUM_MODES - 1)
		track_progress_mode = (TrackProgressMode)(track_progress_mode + 1);
	else
		track_progress_mode = (TrackProgressMode)0;

	change_track_progress_mode(track_progress_mode);
}

void command_track_number(int track_n)
{
	if(track_n < 1){
		printf_("command_track_number(): track_n = %i < 1\n", track_n);
		ui_output_queue::push_message("PASS");
		return;
	}
	int track_media_index = track_n - 1;

	auto &cursor = current_cursor;
	auto &mc = current_media_content;
	if(cursor.album_seq_i >= (int)mc.albums.size()){
		printf_("command_track_number(): album_seq_i %i doesn't exist\n", cursor.album_seq_i);
		ui_output_queue::push_message("PASS A");
		return;
	}
	const Album &album = mc.albums[cursor.album_i(mc)];
	if(track_media_index >= (int)album.tracks.size()){
		printf_("command_track_number(): track_media_index %i doesn't exist\n", track_media_index);
		ui_output_queue::push_message("PASS T");
		return;
	}

	current_cursor.select_track_using_media_index(mc, track_media_index);
	start_at_relative_track(0, 0);
}

void command_album_number(int album_n)
{
	if(album_n < 1){
		printf_("command_album_number(): album_n = %i < 1\n", album_n);
		ui_output_queue::push_message("PASS");
		return;
	}
	int album_media_index = album_n - 1;

	auto &mc = current_media_content;
	if(album_media_index >= (int)mc.albums.size()){
		printf_("command_album_number(): album_media_index %i doesn't exist\n", album_media_index);
		ui_output_queue::push_message("PASS");
		return;
	}

	current_cursor.select_album_using_media_index(mc, album_media_index);
	current_cursor.track_seq_i = 0;
	start_at_relative_track(0, 0, true);
}

void command_next_collection_part(int dir)
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
		// Show part
		ui_flush_display();
		ui_output_queue::push_message("- All -");
	} else {
		if((size_t)current_collection_part_i < collection_parts.size())
			current_collection_part = collection_parts[current_collection_part_i];
		set_collection_part(current_collection_part);
		// Show part
		ui_flush_display();
		ui_output_queue::push_message(squeeze(current_collection_part, arduino_display_width));
	}
}

void temp_display_album()
{
	if(current_media_content.albums.empty())
		return;

	ui_output_queue::push_message(get_album_name(current_media_content, current_cursor),
			squeeze(get_album_name(current_media_content, current_cursor),
			            arduino_display_width * 3, 0, arduino_display_width));
}

void ui_show_changed_album()
{
	temp_display_album();
}

void command_search(const ss_ &searchstring)
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

void command_random_album()
{
	ui_flush_display();
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

void command_random_album_approx_num_tracks(size_t approx_num_tracks)
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

void command_random_album_min_num_tracks(size_t min_num_tracks)
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

void command_random_album_max_num_tracks(size_t max_num_tracks)
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

void command_random_track()
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
				command_next();
			} else if(command == "prev" || command == "p" || command == "-"){
				command_prev();
			} else if(command == "nextalbum" || command == "na" || command == "N" || command == "."){
				command_nextalbum();
			} else if(command == "prevalbum" || command == "pa" || command == "P" || command == ","){
				command_prevalbum();
			} else if(command == "pause" || command == " "){
				command_playpause();
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
				command_playmode();
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
				command_search(searchstring);
				last_searchstring = searchstring;
			} else if(command == "/" || command == "1"){
				if(last_searchstring != "")
					command_search(last_searchstring);
			} else if(w1n == "album"){
				int album_n = stoi(fn.next(""), -1);
				command_album_number(album_n);
			} else if(w1n == "track"){
				int track_n = stoi(fn.next(""), -1);
				command_track_number(track_n);
			} else if(w1n == "randomalbum" || w1n == "ra" || w1n == "r"){
				int approx_num_tracks = stoi(fn.next(""), -1);
				if(approx_num_tracks == -1)
					command_random_album();
				else
					command_random_album_approx_num_tracks(approx_num_tracks);
			} else if(w1n == "rg" || w1n == "g"){
				static int last_min_num_tracks = -1;
				int min_num_tracks = stoi(fn.next(""), -1);
				if(min_num_tracks != -1){
					command_random_album_min_num_tracks(min_num_tracks);
					last_min_num_tracks = min_num_tracks;
				} else if(last_min_num_tracks != -1){
					printf_("Using previous parameter: %i\n", last_min_num_tracks);
					command_random_album_min_num_tracks(last_min_num_tracks);
				}
			} else if(w1n == "rl" || w1n == "l"){
				static int last_max_num_tracks = -1;
				int max_num_tracks = stoi(fn.next(""), -1);
				if(max_num_tracks != -1){
					command_random_album_max_num_tracks(max_num_tracks);
					last_max_num_tracks = max_num_tracks;
				} else if(last_max_num_tracks != -1){
					printf_("Using previous parameter: %i\n", last_max_num_tracks);
					command_random_album_max_num_tracks(last_max_num_tracks);
				}
			} else if(w1 == "randomtrack" || w1 == "rt"){
				command_random_track();
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
				command_next_collection_part(1);
			} else if(command == "pp"){
				command_next_collection_part(-1);
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

	create_file_watch();

    mpv = mpv_create();
    if (!mpv) {
        printf_("mpv_create() failed");
        return 1;
    }
    
    mpv_set_option_string(mpv, "vo", "null");

    check_mpv_error(mpv_initialize(mpv));

	// Wait idle event so that we don't do things twice at startup
	arduino_set_text("WAIT IDLE");
	wait_until_mpv_idle();
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

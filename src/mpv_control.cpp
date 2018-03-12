#include "mpv_control.hpp"
#include "arduino_controls.hpp"
#include "stuff.hpp"
#include "file_watch.hpp"
#include "types.hpp"
#include "filesys.hpp"
#include "scope_end_trigger.hpp"
#include "stuff2.hpp"
#include "ui.hpp"
#include "print.hpp"
#include "library.hpp"
#include "play_cursor.hpp"
#include "arduino_global.hpp"
#include "ui_output_queue.hpp"
#include "../common/common.hpp"
#include <mpv/client.h>
#ifdef __WIN32__
#  include "windows_includes.hpp"
#else
#  include <unistd.h>
#endif

void after_mpv_loadfile(double start_pos, const ss_ &track_name, const ss_ &album_name);
void load_and_play_current_track_from_start();
void eat_all_mpv_events();
void wait_mpv_event(int event_id, int max_ms);
void automated_start_play_next_track();
void do_something_instead_of_idle();

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

void check_mpv_error(int status)
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

	eat_all_mpv_events();

	const char *cmd[] = {"loadfile", track.path.c_str(), NULL};
	check_mpv_error(mpv_command(mpv, cmd));

	after_mpv_loadfile(current_cursor.time_pos, track.display_name,
			get_album_name(current_media_content, current_cursor));

	// Wait for the start-file event
	wait_mpv_event(MPV_EVENT_START_FILE, 1000);

	refresh_track();
}

bool mpv_is_idle()
{
	char *idle_cs = NULL;
	// For some reason the idle property always says "yes" on Windows, so don't
	// even read it on Windows
#ifndef __WIN32__
	mpv_get_property(mpv, "idle-active", MPV_FORMAT_STRING, &idle_cs);
	if(idle_cs == NULL)
		mpv_get_property(mpv, "idle", MPV_FORMAT_STRING, &idle_cs);
#endif
	if(LOG_DEBUG)
		printf_("MPV idle(-active) = %s\n", idle_cs);
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

	//update_and_show_default_display();
	handle_display();
}

void refresh_track()
{
	//update_and_show_default_display();
	handle_display();

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

void start_at_relative_track(int album_add, int track_add, bool force_show_album)
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

void wait_until_mpv_idle()
{
	wait_mpv_event(MPV_EVENT_IDLE, 5000);
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
				ui_output_queue::push_message("PAUSE");
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
		if(LOG_DEBUG)
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


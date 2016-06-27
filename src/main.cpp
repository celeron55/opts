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
#include "../common/common.hpp"
#include <mpv/client.h>
#include <fstream>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

ss_ saved_state_path = "saved_state";

sv_<ss_> arduino_serial_paths;
ss_ test_file_path;
sv_<ss_> track_devices;
ss_ static_mount_path;
ss_ arduino_serial_debug_mode = "off"; // off / raw / fancy
int arduino_display_width = 8;
bool minimize_display_updates = false;
set_<ss_> enabled_log_sources;

time_t startup_timestamp = 0;

bool do_main_loop = true;
mpv_handle *mpv = NULL;
CommandAccumulator<100> stdin_command_accu;

int arduino_serial_fd = -1;
ss_ arduino_serial_fd_path;
bool tried_to_update_arduino_firmware = false;
time_t arduino_last_incoming_message_timestamp = 0;
CommandAccumulator<100> arduino_message_accu;

time_t display_update_timestamp = 0;
size_t display_next_startpos = 0;
ss_ display_last_shown_track_name;

up_<FileWatch> partitions_watch;

ss_ current_mount_device;
ss_ current_mount_path;

//set_<ss_> disappeared_tracks;
bool queued_pause = false;

enum TrackFindStrategyStep {
	TFSS_LOADFILE,
	TFSS_SCAN_CURRENT_MOUNT,
	TFSS_CHECK_CHANGED_PARTITIONS,
	TFSS_WAIT_2S,
	TFSS_WAIT_4S,

	TFSS_FORGET_IT,

	TFSS_COUNT,
};
// Track is switched only after this strategy is executed
const TrackFindStrategyStep TRACK_FIND_STRATEGY[] = {
	TFSS_WAIT_2S,
	TFSS_LOADFILE,
	TFSS_WAIT_2S,
	TFSS_SCAN_CURRENT_MOUNT,
	TFSS_LOADFILE,
	TFSS_CHECK_CHANGED_PARTITIONS,
	TFSS_WAIT_2S,
	TFSS_SCAN_CURRENT_MOUNT,
	TFSS_LOADFILE,
	TFSS_CHECK_CHANGED_PARTITIONS,
	TFSS_WAIT_4S,
	TFSS_SCAN_CURRENT_MOUNT,
	TFSS_LOADFILE,
	TFSS_CHECK_CHANGED_PARTITIONS,
	TFSS_WAIT_4S,
	TFSS_SCAN_CURRENT_MOUNT,
	TFSS_LOADFILE,
	TFSS_CHECK_CHANGED_PARTITIONS,
	TFSS_WAIT_4S,
	TFSS_SCAN_CURRENT_MOUNT,
	TFSS_LOADFILE,
	TFSS_CHECK_CHANGED_PARTITIONS,
	TFSS_FORGET_IT,
};
size_t track_find_strategy_next_i = 0;
time_t track_find_strategy_wait_time = 0;
time_t track_find_strategy_wait_timestamp = 0;
ss_ track_find_strategy_current_track_name;

struct Track
{
	ss_ path;
	ss_ display_name;

	Track(const ss_ &path="", const ss_ &display_name=""):
		path(path), display_name(display_name)
	{}
};

struct Album
{
	ss_ name;
	sv_<Track> tracks;
	//sv_<size_t> random_order;
};

struct MediaContent
{
	sv_<Album> albums;
};

MediaContent current_media_content;

struct PlayCursor
{
	int album_i = 0;
	int track_i = 0;
	//int random_order_i = 0;
	double time_pos = 0;
	int64_t stream_pos = 0;
	ss_ track_name;
};

PlayCursor current_cursor;
PlayCursor last_succesfully_playing_cursor;

// Yes, there are no modes for album progression. Albums progress sequentially,
// tracks inside albums can progress differencly.
enum TrackProgressMode {
	TPM_SEQUENTIAL,
	TPM_REPEAT,
	TPM_REPEAT_TRACK,
	TPM_SHUFFLE,

	TPM_NUM_MODES,
};

TrackProgressMode track_progress_mode = TPM_SEQUENTIAL;

bool track_was_loaded = false;
int num_time_pos_checked_seconds_during_unpaused_playtime_of_current_track = 0;
int64_t current_track_stream_end = 0;

time_t mpv_last_not_idle_timestamp = 0;

void on_loadfile(double start_pos, const ss_ &track_name)
{
	num_time_pos_checked_seconds_during_unpaused_playtime_of_current_track = start_pos;
	current_track_stream_end = 0; // Will be filled in at time-pos getter code or something

	if(current_cursor.track_name != track_name){
		printf("WARNING: Changing track name at loadfile to \"%s\"\n",
				cs(track_name));
		current_cursor.track_name = track_name;
	}

	// If track changed, reset track find strategy
	if(track_name != track_find_strategy_current_track_name){
		track_find_strategy_current_track_name = track_name;
		track_find_strategy_next_i = 0;
		track_find_strategy_wait_time = 0;
	}

	arduino_serial_write(">PROGRESS:0\r\n");
}

enum PauseMode {
	PM_PLAY,
	PM_PAUSE,
	// Not a real pause but one that is used while in power off mode (until power is
	// actually cut, or power off mode is switched off)
	PM_UNFOCUS_PAUSE,
};
PauseMode current_pause_mode = PM_PLAY;

enum StatefulInputMode {
	SIM_NONE,
	SIM_TRACK_NUMBER,
	SIM_ALBUM_NUMBER,

	SIM_NUM_MODES,
};

StatefulInputMode stateful_input_mode = SIM_NONE;
time_t stateful_input_mode_active_timestamp = 0;
CommandAccumulator<10> stateful_input_accu;

bool is_track_playing_fine()
{
	if(!track_was_loaded){
		return false;
	}

	int64_t stream_end = 0;
	mpv_get_property(mpv, "stream-end", MPV_FORMAT_INT64, &stream_end);
	// Don't care for <50kB files
	if(stream_end < 50000){
		return true;
	}

	int64_t stream_pos = 0;
	mpv_get_property(mpv, "stream-pos", MPV_FORMAT_INT64, &stream_pos);

	// Don't care if the track is about to end
	if(stream_pos > stream_end - 50000){
		return true;
	}

	if(num_time_pos_checked_seconds_during_unpaused_playtime_of_current_track < 3){
		return true;
	}
	if(current_cursor.time_pos < num_time_pos_checked_seconds_during_unpaused_playtime_of_current_track / 1.2 - 3){
		return false;
	}
	return true;
}

bool is_track_at_natural_end()
{
	if(track_progress_mode == TPM_REPEAT_TRACK){
		return false;
	}

	if(!track_was_loaded){
		printf("is_track_at_natural_end(): !track_was_loaded -> false\n");
		return false;
	}

	if(current_track_stream_end == 0){
		printf("is_track_at_natural_end(): current_track_stream_end == 0 -> false\n");
		return false;
	}

	int64_t stream_pos = current_cursor.stream_pos;

	bool is = (stream_pos >= current_track_stream_end * 0.9 - 5);

	printf("is_track_at_natural_end(): stream_pos=%" PRId64
			", current_track_stream_end=%" PRId64 " -> %s\n",
			stream_pos, current_track_stream_end, is?"true":"false");
	return is;
}

time_t last_save_timestamp = 0;

void save_stuff()
{
	last_save_timestamp = time(0);

	printf("Saving stuff to %s...\n", cs(saved_state_path));

	ss_ save_blob;
	save_blob += itos(last_succesfully_playing_cursor.album_i) + ";";
	save_blob += itos(last_succesfully_playing_cursor.track_i) + ";";
	save_blob += ftos(last_succesfully_playing_cursor.time_pos) + ";";
	save_blob += itos(last_succesfully_playing_cursor.stream_pos) + ";";
	save_blob += itos(current_pause_mode == PM_PAUSE) + ";";
	save_blob += itos(track_progress_mode) + ";";
	save_blob += "\n";
	save_blob += last_succesfully_playing_cursor.track_name + "\n";
	std::ofstream f(saved_state_path.c_str(), std::ios::binary);
	f<<save_blob;
	f.close();

	printf("Saved.\n");
}

void load_stuff()
{
	ss_ data;
	{
		std::ifstream f(saved_state_path.c_str());
		if(!f.good()){
			printf("No saved state at %s\n", cs(saved_state_path));
			return;
		}
		printf("Loading saved state from %s\n", cs(saved_state_path));
		data = ss_((std::istreambuf_iterator<char>(f)),
				std::istreambuf_iterator<char>());
	}
	Strfnd f(data);
	Strfnd f1(f.next("\n"));
	last_succesfully_playing_cursor.album_i = stoi(f1.next(";"), 0);
	last_succesfully_playing_cursor.track_i = stoi(f1.next(";"), 0);
	last_succesfully_playing_cursor.time_pos = stof(f1.next(";"), 0.0);
	last_succesfully_playing_cursor.stream_pos = stoi(f1.next(";"), 0);
	queued_pause = stoi(f1.next(";"), 0);
	track_progress_mode = (TrackProgressMode)stoi(f1.next(";"), 0);
	last_succesfully_playing_cursor.track_name = f.next("\n");

	current_cursor = last_succesfully_playing_cursor;

	if(queued_pause){
		printf("Queuing pause\n");
	}
}

Track get_track(const MediaContent &mc, const PlayCursor &cursor)
{
	if(cursor.album_i >= (int)mc.albums.size()){
		printf("Album cursor overflow\n");
		return Track();
	}
	const Album &album = mc.albums[cursor.album_i];
	if(cursor.track_i >= (int)album.tracks.size()){
		printf("Track cursor overflow\n");
		return Track();
	}
	return album.tracks[cursor.track_i];
}

void cursor_bound_wrap(const MediaContent &mc, PlayCursor &cursor)
{
	if(mc.albums.empty())
		return;
	if(cursor.album_i < 0)
		cursor.album_i = mc.albums.size() - 1;
	if(cursor.album_i >= (int)mc.albums.size())
		cursor.album_i = 0;
	const Album &album = mc.albums[cursor.album_i];
	if(cursor.track_i < 0){
		cursor.album_i--;
		if(cursor.album_i < 0)
			cursor.album_i = mc.albums.size() - 1;
		const Album &album2 = mc.albums[cursor.album_i];
		cursor.track_i = album2.tracks.size() - 1;
	} else if(cursor.track_i >= (int)album.tracks.size()){
		cursor.track_i = 0;
		cursor.album_i++;
		if(cursor.album_i >= (int)mc.albums.size())
			cursor.album_i = 0;
	}
}

void cursor_bound_wrap_repeat_album(const MediaContent &mc, PlayCursor &cursor)
{
	if(mc.albums.empty())
		return;
	if(cursor.album_i < 0)
		cursor.album_i = mc.albums.size() - 1;
	if(cursor.album_i >= (int)mc.albums.size())
		cursor.album_i = 0;
	const Album &album = mc.albums[cursor.album_i];
	if(cursor.track_i < 0){
		cursor.track_i = album.tracks.size() - 1;
	} else if(cursor.track_i >= (int)album.tracks.size()){
		cursor.track_i = 0;
	}
}

ss_ get_album_name(const MediaContent &mc, const PlayCursor &cursor)
{
	if(cursor.album_i >= (int)mc.albums.size()){
		printf("Album cursor overflow\n");
		return "ERR:AOVF";
	}
	const Album &album = mc.albums[cursor.album_i];
	return album.name;
}

ss_ get_track_name(const MediaContent &mc, const PlayCursor &cursor)
{
	if(cursor.album_i >= (int)mc.albums.size()){
		printf("Album cursor overflow\n");
		return "ERR:AOVF";
	}
	const Album &album = mc.albums[cursor.album_i];
	if(cursor.track_i >= (int)album.tracks.size()){
		printf("Track cursor overflow\n");
		return "ERR:TOVF";
	}
	return album.tracks[cursor.track_i].display_name;
}

ss_ get_cursor_info(const MediaContent &mc, const PlayCursor &cursor)
{
	if(current_media_content.albums.empty())
		return "No media";

	ss_ s = "Album "+itos(cursor.album_i)+" ("+get_album_name(mc, cursor)+
			"), track "+itos(cursor.track_i)+" ("+get_track_name(mc, cursor)+")"+
			(cursor.time_pos != 0.0 ? (", pos "+ftos(cursor.time_pos)+"s") : ss_());
	if(get_track_name(mc, cursor) != cursor.track_name)
		s += ", should be track ("+cursor.track_name+")";
	return s;
}

// If failed, return false and leave cursor as-is.
bool resolve_track_from_current_album(const MediaContent &mc, PlayCursor &cursor)
{
	if(cursor.album_i >= (int)mc.albums.size())
		return false;
	const Album &album = mc.albums[cursor.album_i];
	PlayCursor cursor1 = cursor;
	for(cursor1.track_i=0; cursor1.track_i<(int)album.tracks.size(); cursor1.track_i++){
		const Track &track = album.tracks[cursor1.track_i];
		if(track.display_name == cursor.track_name){
			cursor = cursor1;
			return true;
		}
	}
	return false;
}

// If failed, return false and leave cursor as-is.
bool resolve_track_from_any_album(const MediaContent &mc, PlayCursor &cursor)
{
	PlayCursor cursor1 = cursor;
	for(cursor1.album_i=0; cursor1.album_i<(int)mc.albums.size(); cursor1.album_i++){
		bool found = resolve_track_from_current_album(mc, cursor1);
		if(found){
			cursor = cursor1;
			return true;
		}
	}
	return false;
}

// Find track by the same name as near the cursor as possible. If failed, return
// false and leave cursor as-is.
bool force_resolve_track(const MediaContent &mc, PlayCursor &cursor)
{
	if(cursor.album_i >= (int)mc.albums.size()){
		return resolve_track_from_any_album(mc, cursor);
	}
	const Album &album = mc.albums[cursor.album_i];
	if(cursor.track_i >= (int)album.tracks.size()){
		bool found = resolve_track_from_current_album(mc, cursor);
		if(found)
			return true;
		return resolve_track_from_any_album(mc, cursor);
	}
	const Track &track = album.tracks[cursor.track_i];
	if(track.display_name != cursor.track_name){
		bool found = resolve_track_from_current_album(mc, cursor);
		if(found)
			return true;
		return resolve_track_from_any_album(mc, cursor);
	}
	return true;
}

void execute_track_find_strategy()
{
	// Let's hope the TFS system gets rid of a possible idle state
	mpv_last_not_idle_timestamp = time(0);

	if(track_find_strategy_wait_time > 0){
		if(track_find_strategy_wait_timestamp >= time(0) - track_find_strategy_wait_time)
			return;
		track_find_strategy_wait_time = 0;
	}
	TrackFindStrategyStep next_step = TRACK_FIND_STRATEGY[track_find_strategy_next_i++];
	switch(next_step){
	case TFSS_LOADFILE: {
		printf("TFS: Trying force resolve track\n");
		bool found = force_resolve_track(current_media_content, current_cursor);
		if(found){
			printf("TFS: -> Found; Trying force start at cursor\n");
			void force_start_at_cursor();
			force_start_at_cursor();
		} else {
			printf("TFS: -> Not found\n");
		}
		break; }
	case TFSS_SCAN_CURRENT_MOUNT: {
		printf("TFS: Scanning current mount\n");
		void scan_current_mount();
		scan_current_mount();
		break; }
	case TFSS_CHECK_CHANGED_PARTITIONS: {
		printf("TFS: Checking changed partitions\n");
		void handle_changed_partitions();
		handle_changed_partitions();
		break; }
	case TFSS_WAIT_2S: {
		printf("TFS: Waiting 2s\n");
		track_find_strategy_wait_time = 2;
		track_find_strategy_wait_timestamp = time(0);
		break; }
	case TFSS_WAIT_4S: {
		printf("TFS: Waiting 4s\n");
		track_find_strategy_wait_time = 4;
		track_find_strategy_wait_timestamp = time(0);
		break; }
	case TFSS_FORGET_IT: {
		printf("TFS: Giving up.\n");
		track_find_strategy_next_i = 0;
		void automated_start_play_next_track();
		automated_start_play_next_track();
		break; }
	case TFSS_COUNT: {
		abort();
		break; }
	}
}

size_t get_total_tracks(const MediaContent &mc)
{
	size_t total = 0;
	for(auto &a : mc.albums)
		total += a.tracks.size();
	return total;
}

static inline void check_mpv_error(int status)
{
    if (status < 0) {
        printf("mpv API error: %s\n", mpv_error_string(status));
        exit(1);
    }
}

ss_ read_any(int fd, bool *dst_error=NULL)
{
	struct pollfd fds;
	int ret;
	fds.fd = fd;
	fds.events = POLLIN;
	ret = poll(&fds, 1, 0);
	if(ret == 1){
		char buf[1000];
		ssize_t n = read(fd, buf, 1000);
		if(n == 0)
			return "";
		return ss_(buf, n);
	} else if(ret == 0){
		return "";
	} else {
		// Error
		if(dst_error)
			*dst_error = true;
		return "";
	}
}

void handle_control_play_test_file()
{
	printf("Playing test file \"%s\"\n", cs(test_file_path));
	const char *cmd[] = {"loadfile", test_file_path.c_str(), NULL};
	check_mpv_error(mpv_command(mpv, cmd));

	on_loadfile(0, "test file");
}

void force_start_at_cursor()
{
	printf("Force-start at cursor\n");
	printf("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));

	Track track = get_track(current_media_content, current_cursor);
	if(track.display_name != "" && current_cursor.track_name == ""){
		printf("Warning: Cursor has empty track name\n");
	} else if(track.display_name != current_cursor.track_name){
		printf("Track name does not match cursor name\n");
		track_was_loaded = false;
		return;
	}

	if(current_cursor.time_pos >= 0.001){
		printf("Force-starting at %fs\n", current_cursor.time_pos);
		mpv_set_option_string(mpv, "start", cs(ftos(current_cursor.time_pos)));
	} else {
		printf("Force-starting at 0s\n");
		mpv_set_option_string(mpv, "start", "#1");
	}

	void eat_all_mpv_events();
	eat_all_mpv_events();

	const char *cmd[] = {"loadfile", track.path.c_str(), NULL};
	check_mpv_error(mpv_command(mpv, cmd));

	on_loadfile(current_cursor.time_pos, track.display_name);

	// Wait for the start-file event
	void wait_mpv_event(int event_id, int max_ms);
	wait_mpv_event(MPV_EVENT_START_FILE, 1000);

	void refresh_track();
	refresh_track();
}

bool mpv_is_idle()
{
	char *idle_cs = NULL;
	mpv_get_property(mpv, "idle", MPV_FORMAT_STRING, &idle_cs);
	if(idle_cs == NULL){
		static bool warned = false;
		if(!warned){
			warned = true;
			printf("WARNING: MPV property \"idle\" returns NULL; "
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

		// Some kind of track is loaded; toggle playback
		check_mpv_error(mpv_command_string(mpv, "pause"));

		current_pause_mode = was_pause ? PM_PLAY : PM_PAUSE; // Invert

		if(!was_pause){
			arduino_set_temp_text("PAUSE");
		} else {
			arduino_set_temp_text("RESUME");
		}
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

	on_loadfile(0, track.display_name);

	/*// Check if the file actually even exists; if not, increment a
	// counter of broken tracks and re-scan media at some point
	if(access(track.path.c_str(), F_OK) == -1){
		printf("This track has disappeared\n");
		disappeared_tracks.insert(track.path);
		track_was_loaded = false;
		if(disappeared_tracks.size() > get_total_tracks(current_media_content) / 10 ||
				disappeared_tracks.size() >= 10){
			printf("Too many disappeared tracks; re-scanning media\n");
			void scan_current_mount();
			scan_current_mount();
		}
	}*/
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
	//printf("Currently playing: %s\n", playing_path);

	Track track = get_track(current_media_content, current_cursor);
	if(track.path != ""){
		current_cursor.track_name = track.display_name;

		if(playing_path == NULL || ss_(playing_path) != track.path){
			printf("Playing path does not match current track; Switching track.\n");

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
	switch(track_progress_mode){
	case TPM_SEQUENTIAL:
		break;
	case TPM_REPEAT:
		extra_segment_flags |= (1<<DISPLAY_FLAG_REPEAT);
		break;
	case TPM_REPEAT_TRACK:
		extra_segment_flags |= (1<<DISPLAY_FLAG_REPEAT) | (1<<DISPLAY_FLAG_REPEAT_ONE);
		break;
	case TPM_SHUFFLE:
		extra_segment_flags |= (1<<DISPLAY_FLAG_SHUFFLE);
		break;
	case TPM_NUM_MODES:
		break;
	}
	arduino_serial_write(">EXTRA_SEGMENTS:"+itos(extra_segment_flags)+"\r\n");
}

void handle_control_next()
{
	current_cursor.track_i++;
	current_cursor.time_pos = 0;
	current_cursor.stream_pos = 0;
	cursor_bound_wrap(current_media_content, current_cursor);
	printf("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
	load_and_play_current_track_from_start();
}

void handle_control_prev()
{
	current_cursor.track_i--;
	current_cursor.time_pos = 0;
	current_cursor.stream_pos = 0;
	cursor_bound_wrap(current_media_content, current_cursor);
	printf("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
	load_and_play_current_track_from_start();
}

void handle_control_nextalbum()
{
	current_cursor.album_i++;
	current_cursor.track_i = 0;
	current_cursor.time_pos = 0;
	current_cursor.stream_pos = 0;
	cursor_bound_wrap(current_media_content, current_cursor);

	temp_display_album();

	printf("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
	load_and_play_current_track_from_start();
}

void handle_control_prevalbum()
{
	current_cursor.album_i--;
	current_cursor.track_i = 0;
	current_cursor.time_pos = 0;
	current_cursor.stream_pos = 0;
	cursor_bound_wrap(current_media_content, current_cursor);

	temp_display_album();

	printf("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
	load_and_play_current_track_from_start();
}

void handle_changed_track_progress_mode()
{
	const char *mode_s = 
			track_progress_mode == TPM_SEQUENTIAL ? "SEQUENTIAL" :
			track_progress_mode == TPM_REPEAT ? "REPEAT" :
			track_progress_mode == TPM_REPEAT_TRACK ? "TRACK REPEAT" :
			track_progress_mode == TPM_SHUFFLE ? "SHUFFLE" :
			"UNKNOWN";

	printf("Track progress mode: %s\n", mode_s);

	if(track_progress_mode == TPM_SHUFFLE){
		arduino_set_temp_text("NOT IMPL");
	} else {
		arduino_set_temp_text(mode_s);
	}

	// Seamless looping!
	if(track_progress_mode == TPM_REPEAT_TRACK){
		mpv_set_property_string(mpv, "loop", "inf");
	} else {
		mpv_set_property_string(mpv, "loop", "no");
	}

	void arduino_set_extra_segments();
	arduino_set_extra_segments();
}

void handle_control_shufflerepeat()
{
	if(track_progress_mode < TPM_NUM_MODES - 1)
		track_progress_mode = (TrackProgressMode)(track_progress_mode + 1);
	else
		track_progress_mode = (TrackProgressMode)0;

	handle_changed_track_progress_mode();
}

void handle_control_track_number(int track_n)
{
	if(track_n < 1){
		printf("handle_control_track_number(): track_n = %i < 1\n", track_n);
		arduino_set_temp_text("PASS");
		return;
	}
	int track_i = track_n - 1;

	auto &cursor = current_cursor;
	auto &mc = current_media_content;
	if(cursor.album_i >= (int)mc.albums.size()){
		printf("handle_control_track_number(): album_i %i doesn't exist\n", cursor.album_i);
		arduino_set_temp_text("PASS A");
		return;
	}
	const Album &album = mc.albums[cursor.album_i];
	if(cursor.track_i >= (int)album.tracks.size()){
		printf("handle_control_track_number(): track_i %i doesn't exist\n", track_i);
		arduino_set_temp_text("PASS T");
		return;
	}

	current_cursor.track_i = track_i;
	current_cursor.time_pos = 0;
	current_cursor.stream_pos = 0;
	cursor_bound_wrap(current_media_content, current_cursor);
	printf("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
	load_and_play_current_track_from_start();
}

void handle_control_album_number(int album_n)
{
	if(album_n < 1){
		printf("handle_control_album_number(): album_n = %i < 1\n", album_n);
		arduino_set_temp_text("PASS");
		return;
	}
	int album_i = album_n - 1;

	auto &cursor = current_cursor;
	auto &mc = current_media_content;
	if(cursor.album_i >= (int)mc.albums.size()){
		printf("handle_control_album_number(): album_i %i doesn't exist\n", album_i);
		arduino_set_temp_text("PASS");
		return;
	}

	current_cursor.album_i = album_i;
	current_cursor.track_i = 0;
	current_cursor.time_pos = 0;
	current_cursor.stream_pos = 0;
	cursor_bound_wrap(current_media_content, current_cursor);

	temp_display_album();

	printf("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
	load_and_play_current_track_from_start();
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
		printf("Stateful input command: %s\n", cs(command));
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

void handle_control_input_digit(int input_digit)
{
	if(stateful_input_mode != SIM_NONE){
		return handle_control_stateful_input_mode_input('0'+input_digit);
	}

	arduino_set_temp_text(
			itos(current_cursor.album_i+1)+"-"+itos(current_cursor.track_i+1));
}

void handle_stdin()
{
	ss_ stdin_stuff = read_any(0); // 0=stdin
	for(char c : stdin_stuff){
		if(stdin_command_accu.put_char(c)){
			ss_ command = stdin_command_accu.command();
			if(command == "help" || command == "h" || command == "?"){
				printf("Commands:\n");
				printf("  next, n, +\n");
				printf("  prev, p, -\n");
				printf("  nextalbum, N, .\n");
				printf("  prevalbum, P, ,\n");
				printf("  pause, [space][enter]\n");
				printf("  fwd, f\n");
				printf("  bwd, b\n");
				printf("  shufflerepeat, sf\n");
				printf("  pos\n");
				printf("  save\n");
			} else if(command == "next" || command == "n" || command == "+"){
				handle_control_next();
			} else if(command == "prev" || command == "p" || command == "-"){
				handle_control_prev();
			} else if(command == "nextalbum" || command == "N" || command == "."){
				handle_control_nextalbum();
			} else if(command == "prevalbum" || command == "P" || command == ","){
				handle_control_prevalbum();
			} else if(command == "pause" || command == " "){
				handle_control_playpause();
			} else if(command == "fwd" || command == "f"){
				mpv_command_string(mpv, "seek +30");
				current_cursor.time_pos += 30;
				printf("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
			} else if(command == "bwd" || command == "b"){
				mpv_command_string(mpv, "seek -30");
				current_cursor.time_pos -= 30;
				printf("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
			} else if(command == "shufflerepeat" || command == "sf"){
				handle_control_shufflerepeat();
			} else if(command == "pos"){
				printf("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
			} else if(command == "save"){
				save_stuff();
			} else if(command == "test"){
				handle_control_play_test_file();
			} else {
				printf("Invalid command: \"%s\"\n", cs(command));
			}
		}
	}
}

void handle_key_press(int key)
{
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
		handle_control_shufflerepeat();
		return;
	}
	if(key == 18){ // Right upper
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
}

void try_open_arduino_serial()
{
	for(const ss_ &arduino_serial_path : arduino_serial_paths){
		arduino_serial_fd = open(arduino_serial_path.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
		if(arduino_serial_fd < 0){
			printf("Failed to open %s\n", cs(arduino_serial_path));
			arduino_serial_fd = -1;
			continue;
		}
		if(!set_interface_attribs(arduino_serial_fd, 9600, 0)){
			printf("Failed to set attributes for serial fd\n");
			continue;
		}
		printf("Opened arduino serial port %s\n", cs(arduino_serial_path));
		arduino_serial_fd_path = arduino_serial_path;
		return;
	}
}

void handle_hwcontrols()
{
	update_stateful_input();

	if(arduino_serial_fd == -1){
		static time_t last_retry_time = 0;
		if(last_retry_time < time(0) - 5 && !arduino_serial_paths.empty()){
			last_retry_time = time(0);
			printf("Retrying arduino serial\n");
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
				printf("<KEY_PRESS  : %i\n", key);
				handle_key_press(key);
			} else if(first == "<KEY_RELEASE"){
				int key = stoi(f.next(":"));
				printf("<KEY_RELEASE: %i\n", key);
				handle_key_release(key);
			} else if(first == "<BOOT"){
				printf("<BOOT\n");
				arduino_set_extra_segments();
				temp_display_album();
				refresh_track();

				arduino_request_version();
			} else if(first == "<MODE"){
				ss_ mode = f.next(":");
				if(mode == "RASPBERRY"){
					if(current_pause_mode == PM_UNFOCUS_PAUSE){
						printf("Leaving unfocus pause\n");
						check_mpv_error(mpv_command_string(mpv, "pause"));
						current_pause_mode = PM_PLAY;
					}
				} else {
					if(current_pause_mode == PM_PLAY){
						printf("Entering unfocus pause\n");
						check_mpv_error(mpv_command_string(mpv, "pause"));
						current_pause_mode = PM_UNFOCUS_PAUSE;
					}
				}
			} else if(first == "<POWERDOWN_WARNING"){
				printf("<POWERDOWN_WARNING\n");
				save_stuff();
			} else if(first == "<VERSION"){
				printf("%s\n", cs(message));
				ss_ version = f.next("");
				if(!tried_to_update_arduino_firmware){
					tried_to_update_arduino_firmware = true;
					arduino_firmware_update_if_needed(version);
				}
			} else {
				printf("%s (ignored)\n", cs(message));
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
		if(enabled_log_sources.count("mpv"))
			printf("MPV: %s (eaten)\n", mpv_event_name(event->event_id));
	}
}

void wait_mpv_event(int event_id, int max_ms)
{
	for(int i=0; i<max_ms/5; i++){
		for(;;){
			mpv_event *event = mpv_wait_event(mpv, 0);
			if(event->event_id == MPV_EVENT_NONE)
				break;
			if(enabled_log_sources.count("mpv"))
				printf("MPV: %s (waited over)\n", mpv_event_name(event->event_id));
			if(event->event_id == event_id)
				return;
		}
		usleep(5000);
	}
}

void automated_start_play_next_track()
{
	printf("Automated start of next track\n");

	track_find_strategy_next_i = 0;

	switch(track_progress_mode){
	case TPM_SEQUENTIAL:
		current_cursor.track_i++;
		current_cursor.time_pos = 0;
		current_cursor.stream_pos = 0;
		cursor_bound_wrap(current_media_content, current_cursor);
		break;
	case TPM_REPEAT:
		current_cursor.track_i++;
		current_cursor.time_pos = 0;
		current_cursor.stream_pos = 0;
		cursor_bound_wrap_repeat_album(current_media_content, current_cursor);
		break;
	case TPM_REPEAT_TRACK:
		current_cursor.time_pos = 0;
		current_cursor.stream_pos = 0;
		break;
	case TPM_SHUFFLE:
		// NOTE: Not implemented; currently behaves like sequential
		// TODO
		current_cursor.track_i++;
		current_cursor.time_pos = 0;
		current_cursor.stream_pos = 0;
		cursor_bound_wrap(current_media_content, current_cursor);
		break;
	case TPM_NUM_MODES:
		break;
	}

	printf("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
	refresh_track();
}

void do_something_instead_of_idle()
{
	if(is_track_at_natural_end()){
		automated_start_play_next_track();
	} else {
		execute_track_find_strategy();
	}
}

void handle_mpv()
{
	for(;;){
		mpv_event *event = mpv_wait_event(mpv, 0);
		if(event->event_id == MPV_EVENT_NONE)
			break;
		if(enabled_log_sources.count("mpv"))
			printf("MPV: %s\n", mpv_event_name(event->event_id));
		if(event->event_id == MPV_EVENT_SHUTDOWN){
			do_main_loop = false;
		}
		if(event->event_id == MPV_EVENT_IDLE){
			do_something_instead_of_idle();
		}
		if(event->event_id == MPV_EVENT_FILE_LOADED){
			track_was_loaded = true;
			if(current_track_stream_end == 0){
				int64_t stream_end = 0;
				mpv_get_property(mpv, "stream-end", MPV_FORMAT_INT64, &stream_end);
				current_track_stream_end = stream_end;
				printf("Got current track stream_end: %" PRId64 "\n",
						current_track_stream_end);
			}
			if(queued_pause){
				queued_pause = false;
				printf("Executing queued pause\n");
				check_mpv_error(mpv_command_string(mpv, "pause"));
				arduino_set_temp_text("PAUSE");
				current_pause_mode = PM_PAUSE;
			}
		}
		if(event->event_id == MPV_EVENT_END_FILE){
			if(track_progress_mode == TPM_REPEAT_TRACK){
				num_time_pos_checked_seconds_during_unpaused_playtime_of_current_track = 0;
			}
		}
	}

	static time_t last_time_pos_get_timestamp = 0;
	if(last_time_pos_get_timestamp != time(0)){
		last_time_pos_get_timestamp = time(0);

		if(current_pause_mode == PM_PLAY)
			num_time_pos_checked_seconds_during_unpaused_playtime_of_current_track++;

		double time_pos = 0;
		mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &time_pos);
		int64_t stream_pos = 0;
		mpv_get_property(mpv, "stream-pos", MPV_FORMAT_INT64, &stream_pos);
		if(time_pos >= 2 && stream_pos > 0){
			current_cursor.time_pos = time_pos;
			current_cursor.stream_pos = stream_pos;
			last_succesfully_playing_cursor = current_cursor;

			if(current_track_stream_end == 0){
				int64_t stream_end = 0;
				mpv_get_property(mpv, "stream-end", MPV_FORMAT_INT64, &stream_end);
				current_track_stream_end = stream_end;
				printf("Got current track stream_end: %" PRId64 "\n",
						current_track_stream_end);
			}

			if(!minimize_display_updates || time(0) % 10 == 0){
				arduino_serial_write(">PROGRESS:"+
						itos(stream_pos * 255 / current_track_stream_end)+"\r\n");
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
		if(mpv_last_not_idle_timestamp == 0){
			mpv_last_not_idle_timestamp = time(0);
		} else if(mpv_last_not_idle_timestamp > time(0) - 5){
			// Fine enough until 5 seconds of idle
		} else {
			printf("MPV Idled for too long; doing something\n");
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
		"h264", "ifo", "m2ts", "m4v", "mkv", "mod", "mov", "mp4", "mpeg",
		"mpg", "mswmm", "mts", "mxf", "ogv", "rm", "srt", "swf", "ts", "vep",
		"vob", "webm", "wlmp", "wmv", "aac", "cue", "d64", "flac", "it",
		"m3u", "m4a", "mid", "mod", "mp3", "mp4", "ogg", "pls", "rar", "s3m",
		"sfv", "sid", "spc", "swf", "t64", "wav", "xd", "xm",
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

void scan_directory(const ss_ &root_name, const ss_ &path, sv_<Album> &result_albums)
{
	DirLister dl(path.c_str());

	Album root_album;
	root_album.name = root_name;

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
			//printf("File: %s\n", cs(path+"/"+fname));
			char stripped[100];
			snprintf(stripped, sizeof stripped, fname);
			strip_file_extension(stripped);
			root_album.tracks.push_back(Track(path+"/"+fname, stripped));
		} else if(ftype == FS_DIR){
			//printf("Dir: %s\n", cs(path+"/"+fname));
			scan_directory(fname, path+"/"+fname, result_albums);
		}
	}

	if(!root_album.tracks.empty())
		result_albums.push_back(root_album);
}

void scan_current_mount()
{
	printf("Scanning...\n");

	//disappeared_tracks.clear();
	current_media_content.albums.clear();

	scan_directory("root", current_mount_path, current_media_content.albums);

	printf("Scanned %zu albums.\n", current_media_content.albums.size());

	current_cursor = last_succesfully_playing_cursor;

	if(!force_resolve_track(current_media_content, current_cursor)){
		printf("Force-resolve track failed\n");
		execute_track_find_strategy();
	}

	temp_display_album();

	force_start_at_cursor();
}

bool check_partition_exists(const ss_ &devname0)
{
	std::ifstream f("/proc/partitions");
	if(!f.good()){
		printf("Can't read /proc/partitions\n");
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
		printf("Can't read /proc/mounts\n");
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
			/*printf("is_device_mounted(): %s is mounted at %s\n",
					cs(devname0), cs(mountpoint));*/
			return mountpoint;
		}
	}
	//printf("is_device_mounted(): %s is not mounted\n", cs(devname0));
	return "";
}

void handle_changed_partitions()
{
	if(static_mount_path != ""){
		if(current_mount_path != static_mount_path){
			printf("Using static mount path %s\n", cs(static_mount_path));
			current_mount_device = "dummy";
			current_mount_path = static_mount_path;
			scan_current_mount();
		}
		return;
	}

	if(current_mount_device != ""){
		if(!check_partition_exists(current_mount_device)){
			static time_t umount_last_failed_timestamp = 0;
			if(umount_last_failed_timestamp > time(0) - 15){
				// Stop flooding these dumb commands
			} else {
				// Unmount it if the partition doesn't exist anymore
				printf("Device %s does not exist anymore; umounting\n",
						cs(current_mount_path));
				int r = umount(current_mount_path.c_str());
				if(r == 0){
					printf("umount %s succesful\n", current_mount_path.c_str());
					current_mount_device = "";
					current_mount_path = "";
					current_media_content.albums.clear();
				} else {
					printf("umount %s failed: %s\n", current_mount_path.c_str(), strerror(errno));
					umount_last_failed_timestamp = time(0);
				}
			}
		} else if(get_device_mountpoint(current_mount_device) == ""){
			printf("Device %s got unmounted from %s\n", cs(current_mount_device),
					cs(current_mount_path));
			current_mount_device = "";
			current_mount_path = "";
			current_media_content.albums.clear();
		}
	}

	if(current_mount_device != ""){
		// This can get extremely spammy; thus it is commented out
		/*printf("Ignoring partition change because we have mounted %s at %s\n",
				cs(current_mount_device), cs(current_mount_path));*/
		return;
	}

	std::ifstream f("/proc/partitions");
	if(!f.good()){
		printf("Can't read /proc/partitions\n");
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
		printf("Tracked partition: %s\n", cs(devname));

		ss_ existing_mountpoint = get_device_mountpoint(devname);
		if(existing_mountpoint != ""){
			printf("%s is already mounted at %s; using it\n",
					cs(devname), cs(existing_mountpoint));
			current_mount_device = devname;
			current_mount_path = existing_mountpoint;

			scan_current_mount();
			return;
		}

		ss_ dev_path = "/dev/"+devname;
		ss_ new_mount_path = "/tmp/__autosoitin_mnt";
		printf("Mounting %s at %s\n", cs(dev_path), cs(new_mount_path));
		mkdir(cs(new_mount_path), 0777);
		int r = mount(dev_path.c_str(), new_mount_path.c_str(), "vfat",
				MS_MGC_VAL | MS_RDONLY | MS_NOEXEC | MS_NOSUID | MS_DIRSYNC |
						MS_NODEV | MS_SYNCHRONOUS,
				NULL);
		if(r == 0){
			printf("Succesfully mounted.\n");
			current_mount_device = devname;
			current_mount_path = new_mount_path;

			scan_current_mount();
			return;
		} else {
			printf("Failed to mount (%s); trying next\n", strerror(errno));
		}
	}
}

bool partitions_changed = false;

void handle_mount()
{
	if(static_mount_path != "")
		return;

	// Calls callbacks; eg. handle_changed_partitions()
	for(auto fd : partitions_watch->get_fds()){
		partitions_watch->report_fd(fd);
	}

	if(partitions_changed){
		partitions_changed = false;
		printf("Partitions changed\n");
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

void handle_track_find_strategy()
{
	if(!is_track_playing_fine())
		execute_track_find_strategy();
}

void sigint_handler(int _)
{
	printf("SIGINT\n");
	save_stuff();
	do_main_loop = false;
}

int main(int argc, char *argv[])
{
	signal(SIGINT, sigint_handler);
	startup_timestamp = time(0);

	const char opts[100] = "hs:t:d:S:m:D:UW:l:";
	const char usagefmt[1000] =
			"Usage: %s [OPTION]...\n"
			"  -h                   Show this help\n"
			"  -s [path]            Serial port device of Arduino (pass multiple -s to specify many)\n"
			"  -t [path]            Test file path\n"
			"  -d [dev1,dev2,...]   Block devices to track and mount (eg. sdc)\n"
			"  -S [path]            Saved state path\n"
			"  -m [path]            Static mount path; automounting is disabled if set and root privileges are not needed\n"
			"  -D [mode]            Set arduino serial debug mode (off/raw/fancy)\n"
			"  -U                   Minimize display updates\n"
			"  -W [integer]         Set text display width\n"
			"  -l [string]          Enable log source (eg. mpv)\n"
			;

	int c;
	while((c = c55_getopt(argc, argv, opts)) != -1)
	{
		switch(c)
		{
		case 'h':
			printf(usagefmt, argv[0]);
			return 1;
		case 's':
			arduino_serial_paths.push_back(c55_optarg);
			break;
		case 't':
			test_file_path = c55_optarg;
			break;
		case 'd':
			{
				Strfnd f(c55_optarg);
				printf("Tracking:");
				for(;;){
					ss_ dev = f.next(",");
					if(dev == "") break;
					printf(" %s", cs(dev));
					track_devices.push_back(dev);
				}
				printf("\n");
			}
			break;
		case 'S':
			saved_state_path = c55_optarg;
			break;
		case 'm':
			static_mount_path = c55_optarg;
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
			fprintf(stderr, "Invalid argument\n");
			fprintf(stderr, usagefmt, argv[0]);
			return 1;
		}
	}

	if(track_devices.empty() && static_mount_path.empty()){
		printf("Use -d or -m\n");
		return 1;
	}

	if(arduino_serial_debug_mode != "off" && arduino_serial_debug_mode != "raw" &&
			arduino_serial_debug_mode != "fancy"){
		printf("Invalid arduino serial debug mode (-D) (%s)\n",
				cs(arduino_serial_debug_mode));
		return 1;
	}

	load_stuff();

	try_open_arduino_serial();

	partitions_watch.reset(createFileWatch());

    mpv = mpv_create();
    if (!mpv) {
        printf("mpv_create() failed");
        return 1;
    }
    
    mpv_set_option_string(mpv, "vo", "null");

    check_mpv_error(mpv_initialize(mpv));

	handle_changed_track_progress_mode();

	printf("Doing initial partition scan\n");
	handle_changed_partitions();

	while(do_main_loop){
		handle_stdin();

		handle_hwcontrols();

		handle_display();

		handle_mpv();

		handle_mount();

		handle_periodic_save();

		handle_track_find_strategy();

		usleep(1000000/60);
	}

    mpv_terminate_destroy(mpv);
    close(arduino_serial_fd);
    return 0;
}

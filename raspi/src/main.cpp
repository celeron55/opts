#include "stuff.hpp"
#include "c55_getopt.h"
#include "command_accumulator.hpp"
#include "string_util.hpp"
#include "file_watch.hpp"
#include "types.hpp"
#include "filesys.hpp"
#include "scope_end_trigger.hpp"
#include <mpv/client.h>
#include <fstream>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

ss_ arduino_serial_path;
ss_ test_file_path;
sv_<ss_> track_devices;

bool do_main_loop = true;
mpv_handle *mpv = NULL;
CommandAccumulator<100> stdin_command_accu;
int arduino_serial_fd = -1;
CommandAccumulator<100> arduino_message_accu;

up_<FileWatch> partitions_watch;

ss_ current_mount_device;
ss_ current_mount_path;

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
};

PlayCursor current_cursor;

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
	return ss_()+"Album "+itos(cursor.album_i)+" ("+get_album_name(mc, cursor)+
			"), track "+itos(cursor.track_i)+" ("+get_track_name(mc, cursor)+")";
}

ss_ truncate(const ss_ &s, size_t len)
{
	if(s.size() < len) return s;
	return s.substr(0, len);
}

ss_ squeeze(const ss_ &s0, size_t len)
{
	ss_ s = s0;
	for(size_t i=0; i<s.size(); i++)
		s[i] = toupper(s[i]);

	if(s.size() < len) return s;

	size_t good_beginning = 0;
	for(size_t i=0; i<s.size(); i++){
		if((s[i] < 'a' || s[i] > 'z') && (s[i] < 'A' || s[i] > 'Z')){
			good_beginning = i;
		} else {
			good_beginning = i;
			break;
		}
	}
	if(good_beginning < s.size() - 2){
		s = s.substr(good_beginning);
	}

	return s.substr(0, len);
}

static inline void check_mpv_error(int status)
{
    if (status < 0) {
        printf("mpv API error: %s\n", mpv_error_string(status));
        exit(1);
    }
}

ss_ read_any(int fd)
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
		return "";
	}
}

void handle_control_play_test_file()
{
	printf("Playing test file \"%s\"\n", cs(test_file_path));
	const char *cmd[] = {"loadfile", test_file_path.c_str(), NULL};
	check_mpv_error(mpv_command(mpv, cmd));
}

void force_start_current_track()
{
	printf("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
	Track track = get_track(current_media_content, current_cursor);
	const char *cmd[] = {"loadfile", track.path.c_str(), NULL};
	check_mpv_error(mpv_command(mpv, cmd));
}

void handle_control_playpause()
{
	int idle = 0;
	mpv_get_property(mpv, "idle", MPV_FORMAT_FLAG, &idle);

	if(!idle){
		int was_pause = 0;
		mpv_get_property(mpv, "pause", MPV_FORMAT_FLAG, &was_pause);

		// Some kind of track is loaded; toggle playback
		check_mpv_error(mpv_command_string(mpv, "pause"));

		if(!was_pause){
			char buf[30];
			int l = snprintf(buf, 30, ">SET_TEMP_TEXT:PAUSE\r\n");
			write(arduino_serial_fd, buf, l);
		} else {
			char buf[30];
			int l = snprintf(buf, 30, ">SET_TEMP_TEXT:RESUME\r\n");
			write(arduino_serial_fd, buf, l);
		}
	} else {
		// No track is loaded; load from cursor
		force_start_current_track();
	}
}

void refresh_track()
{
	// Update display
	char buf[30];
	int l = snprintf(buf, 30, ">SET_TEXT:%s\r\n",
			cs(squeeze(get_track_name(current_media_content, current_cursor), 8)));
	write(arduino_serial_fd, buf, l);

	char *playing_path = NULL;
	ScopeEndTrigger set([&](){ mpv_free(playing_path); });
	mpv_get_property(mpv, "path", MPV_FORMAT_STRING, &playing_path);
	//printf("Currently playing: %s\n", playing_path);

	Track track = get_track(current_media_content, current_cursor);
	if(track.path != ""){
		if(playing_path == NULL || ss_(playing_path) != track.path){
			printf("Playing path does not match current track; Switching track.\n");
			const char *cmd[] = {"loadfile", track.path.c_str(), NULL};
			check_mpv_error(mpv_command(mpv, cmd));
		}
	}
}

void temp_display_album()
{
	char buf[30];
	int l = snprintf(buf, 30, ">SET_TEMP_TEXT:%s\r\n",
			cs(squeeze(get_album_name(current_media_content, current_cursor), 8)));
	write(arduino_serial_fd, buf, l);
}

void handle_control_next()
{
	current_cursor.track_i++;
	cursor_bound_wrap(current_media_content, current_cursor);
	printf("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
	refresh_track();
}

void handle_control_prev()
{
	current_cursor.track_i--;
	cursor_bound_wrap(current_media_content, current_cursor);
	printf("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
	refresh_track();
}

void handle_control_nextalbum()
{
	current_cursor.album_i++;
	current_cursor.track_i = 0;
	cursor_bound_wrap(current_media_content, current_cursor);

	temp_display_album();

	printf("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
	refresh_track();
}

void handle_control_prevalbum()
{
	current_cursor.album_i--;
	current_cursor.track_i = 0;
	cursor_bound_wrap(current_media_content, current_cursor);

	temp_display_album();

	printf("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
	refresh_track();
}

void handle_stdin()
{
	ss_ stdin_stuff = read_any(0); // 0=stdin
	for(char c : stdin_stuff){
		if(stdin_command_accu.put_char(c)){
			ss_ command = stdin_command_accu.command();
			if(command == "next"){
				handle_control_next();
			} else if(command == "prev"){
				handle_control_prev();
			} else if(command == "nextalbum"){
				handle_control_nextalbum();
			} else if(command == "prevalbum"){
				handle_control_prevalbum();
			} else if(command == "pause"){
				handle_control_playpause();
			} else if(command == "fwd"){
				mpv_command_string(mpv, "seek +30");
			} else if(command == "bwd"){
				mpv_command_string(mpv, "seek -30");
			} else if(command == "pos"){
				double pos = 0;
				mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos);
				printf("pos: %f\n", pos);
			} else if(command == "test"){
				handle_control_play_test_file();
			} else {
				printf("Invalid command: \"%s\"", cs(command));
			}
		}
	}
}

void handle_key_press(int key)
{
	if(key == 21){
		handle_control_play_test_file();
		return;
	}
	if(key == 24){
		handle_control_playpause();
		return;
	}
	if(key == 12){
		handle_control_next();
		return;
	}
	if(key == 27){
		handle_control_prev();
		return;
	}
	if(key == 23){
		handle_control_nextalbum();
		return;
	}
	if(key == 29){
		handle_control_prevalbum();
		return;
	}
}

void handle_key_release(int key)
{
}

void handle_hwcontrols()
{
	if(arduino_serial_fd == -1)
		return;
	ss_ serial_stuff = read_any(arduino_serial_fd);
	for(char c : serial_stuff){
		if(arduino_message_accu.put_char(c)){
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
				temp_display_album();
				refresh_track();
			} else {
				printf("%s (ignored)\n", cs(message));
			}
		}
	}
}

void handle_mpv()
{
	for(;;){
		mpv_event *event = mpv_wait_event(mpv, 0);
		if(event->event_id == MPV_EVENT_NONE)
			break;
		printf("MPV: %s\n", mpv_event_name(event->event_id));
		if(event->event_id == MPV_EVENT_SHUTDOWN){
			do_main_loop = false;
		}
		if(event->event_id == MPV_EVENT_IDLE){
			current_cursor.track_i++;
			cursor_bound_wrap(current_media_content, current_cursor);
			printf("%s\n", cs(get_cursor_info(current_media_content, current_cursor)));
			refresh_track();
		}
	}
}

bool filename_supported(const ss_ &name)
{
	// Not all of these are even actually supported but at least nothing
	// ridiculous is included
	static set_<ss_> supported_file_extensions = {
		"aac", "cue", "d64", "flac", "it", "m3u", "m4a", "mid", "mod", "mp3",
		"mp4", "ogg", "pls", "rar", "s3m", "sfv", "sid", "spc", "swf", "t64",
		"wav", "xd", "xm",
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

	current_media_content.albums.clear();

	scan_directory("root", current_mount_path, current_media_content.albums);

	printf("Scanned %zu albums.\n", current_media_content.albums.size());

	temp_display_album();
	force_start_current_track();
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
	if(current_mount_device != ""){
		if(!check_partition_exists(current_mount_device)){
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
		printf("Ignoring partition change because we have mounted %s at %s\n",
				cs(current_mount_device), cs(current_mount_path));
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

void handle_mount()
{
	// Calls callbacks; eg. handle_changed_partitions()
	for(auto fd : partitions_watch->get_fds()){
		partitions_watch->report_fd(fd);
	}
}

int main(int argc, char *argv[])
{
	const char opts[100] = "hs:t:d:";
	const char usagefmt[1000] =
			"Usage: %s [OPTION]...\n"
			"  -h                   Show this help\n"
			"  -s [path]            Serial port device of Arduino\n"
			"  -t [path]            Test file path\n"
			"  -d [dev1,dev2,...]   Block devices to track and mount (eg. sdc)\n"
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
			arduino_serial_path = c55_optarg;
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
		default:
			fprintf(stderr, "Invalid argument\n");
			fprintf(stderr, usagefmt, argv[0]);
			return 1;
		}
	}

	if(arduino_serial_path != ""){
		arduino_serial_fd = open(arduino_serial_path.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
		if(arduino_serial_fd < 0){
			printf("Failed to open %s\n", cs(arduino_serial_path));
			return 1;
		}
		if(!set_interface_attribs(arduino_serial_fd, 9600, 0)){
			return 1;
		}
	}

	partitions_watch.reset(createFileWatch());
	partitions_watch->add("/dev/disk", [](const ss_ &path){
		printf("Partitions changed (%s)\n", cs(path));
		handle_changed_partitions();
	});

    mpv = mpv_create();
    if (!mpv) {
        printf("mpv_create() failed");
        return 1;
    }
    
    mpv_set_option_string(mpv, "vo", "null");

    check_mpv_error(mpv_initialize(mpv));

	printf("Doing initial partition scan\n");
	handle_changed_partitions();

	while(do_main_loop){
		handle_stdin();

		handle_hwcontrols();

		handle_mpv();

		handle_mount();

		usleep(1000000/60);
	}

    mpv_terminate_destroy(mpv);
    close(arduino_serial_fd);
    return 0;
}

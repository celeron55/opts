#include "media_scan.hpp"
#include "stuff.hpp"
#include "string_util.hpp"
#include "file_watch.hpp"
#include "filesys.hpp"
#include "stuff2.hpp"
#include "terminal.hpp"
#include "ui.hpp"
#include "print.hpp"
#include "library.hpp"
#include "play_cursor.hpp"
#include "mpv_control.hpp"
#include "../common/common.hpp"
#include "types.hpp"
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

up_<FileWatch> partitions_watch;

ss_ current_mount_device;
ss_ current_mount_path;

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
		Album *parent_dir_album)
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
			snprintf(stripped, sizeof stripped, "%s", fname);
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
			if(ftype != FS_DIR)
				continue;
			if(fname[0] == '.')
				continue;
			if(ss_(fname) == "FW")
				continue;
			//printf_("Dir: %s\n", cs(path+"/"+fname));
			subdirs.push_back(fname);
			// TODO: Don't add duplicates
		}
	}

	// Sort subdirs
	std::sort(subdirs.begin(), subdirs.end());

	return subdirs;
}

void set_collection_part(const ss_ &part)
{
	current_collection_part = part;

	printf_("Switched to part \"%s\"\n", cs(current_collection_part));

	if(part != ""){
		// Reset cursor (unless switching away from parts into full mode)
		last_succesfully_playing_cursor = PlayCursor();
	}

	// Re-scan
	scan_current_mount();
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
		command_random_album();
		return;
	}

	if(!static_media_paths.empty() && current_media_content.albums.empty()){
		// There are static media paths and there are no tracks; do nothing
		printf_("No media.\n");
		return;
	}

	if(!force_resolve_track(current_media_content, current_cursor)){
		printf_("Force-resolve track failed; picking random album\n");
		command_random_album();
		return;
	}

	force_start_at_cursor();
	ui_show_changed_album();
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

void create_file_watch()
{
#ifndef __WIN32__
	partitions_watch.reset(createFileWatch(
			IN_MOVED_TO | IN_CREATE | IN_MOVED_FROM | IN_DELETE | IN_ATTRIB));
#endif
}


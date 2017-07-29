#pragma once
#include "types.hpp"
#include "library.hpp"
#include "play_cursor.hpp"

bool filename_supported(const ss_ &name);
void scan_directory(const ss_ &root_name, const ss_ &path, sv_<Album> &result_albums,
		Album *parent_dir_album=NULL);
sv_<ss_> get_collection_parts();
void set_collection_part(const ss_ &part);
void scan_current_mount();
bool check_partition_exists(const ss_ &devname0);
ss_ get_device_mountpoint(const ss_ &devname0);
void handle_changed_partitions();
void handle_mount();
void create_file_watch();

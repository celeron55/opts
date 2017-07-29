#pragma once
#include "types.hpp"

#ifdef __WIN32__
#  include "windows_includes.hpp"
#else
#  include <dirent.h>
#endif
#define MAX_PATH_LEN 4096

#define FS_FILE 0
#define FS_DIR 1
#define FS_OTHER 2

/* "image.png", "png" -> TRUE */
bool check_file_extension(const char *path, const char *ext);
void strip_file_extension(char *path);
void strip_filename(char *path);

struct DirLister
{
#ifdef __WIN32__
	HANDLE hFind;
	WIN32_FIND_DATA FindFileData;
#else
	DIR *dir = nullptr;
#endif

	DirLister(const char *path);
	~DirLister();

	bool valid();
	bool get_next(int *type, char *name, unsigned int maxlen);
};


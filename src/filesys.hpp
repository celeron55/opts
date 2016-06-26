#pragma once

#include <dirent.h>
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
	DIR *dir = nullptr;

	DirLister(const char *path);
	~DirLister();

	bool valid(){ return dir != nullptr; }
	bool get_next(int *type, char *name, unsigned int maxlen);
};


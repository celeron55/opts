#include "filesys.hpp"
#include <stdio.h>
#include <string.h>

/* "image.png", "png" -> true */
bool check_file_extension(const char *path, const char *ext)
{
	int extlen = strlen(ext);
	int pathlen = strlen(path);
	if(extlen > pathlen - 1)
		return false;
	int i;
	for(i=0; i<extlen; i++){
		if(path[pathlen-1-i] != ext[extlen-1-i])
			return false;
	}
	if(path[pathlen-1-i] != '.')
		return false;
	return true;
}

void strip_file_extension(char *path)
{
	int pathlen = strlen(path);
	int i;
	for(i=pathlen-1; i>=0; i--){
		if(path[i] == '.')
			break;
	}
	path[i] = 0;
}

void strip_filename(char *path)
{
	int pathlen = strlen(path);
	int i;
	for(i=pathlen-1; i>=0; i--){
		if(path[i] == '/' || path[i] == '\\')
			break;
	}
	path[i] = 0;
}

#ifdef __WIN32__

DirLister::DirLister(const char *path)
{
	ss_ pattern = ss_() + path + "/*";
	if((hFind = FindFirstFile(pattern.c_str(), &FindFileData)) == INVALID_HANDLE_VALUE)
		printf("ERROR: Failed to open path %s\n", path);
}

DirLister::~DirLister()
{
	if(hFind != INVALID_HANDLE_VALUE)
		FindClose(hFind);
}

bool DirLister::get_next(int *type, char *name, unsigned int maxlen)
{
	if(hFind == INVALID_HANDLE_VALUE)
		return false;

	if(FindFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		*type = FS_DIR;
	else
		*type = FS_FILE;
	snprintf(name, maxlen, FindFileData.cFileName);

	if(!FindNextFile(hFind, &FindFileData)){
		FindClose(hFind);
		hFind = INVALID_HANDLE_VALUE;
	}
	return true;
}

bool DirLister::valid()
{
	return hFind != INVALID_HANDLE_VALUE;
}

#else // __WIN32__

DirLister::DirLister(const char *path)
{
	dir = opendir(path);
}

DirLister::~DirLister()
{
	if(dir != NULL) closedir(dir);
}

bool DirLister::get_next(int *type, char *name, unsigned int maxlen)
{
	if(dir == NULL) return false;
	struct dirent *dp = readdir(dir);
	if(dp == NULL) return false;
	snprintf(name, maxlen, dp->d_name);
	if(dp->d_type == DT_REG) *type = FS_FILE;
	else if(dp->d_type == DT_DIR) *type = FS_DIR;
	else *type = FS_OTHER;
	return true;
}

bool DirLister::valid()
{
	return dir != nullptr;
}

#endif // __WIN32__


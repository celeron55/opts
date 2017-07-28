// http://www.apache.org/licenses/LICENSE-2.0
// Copyright 2014 Perttu Ahola <celeron55@gmail.com>
#include "../file_watch.hpp"
#include <cstring>
#include <cstdio>
#define MODULE "__filewatch"

namespace interface {

struct CFileWatch: FileWatch
{
	CFileWatch()
	{
		printf("ERROR: ""CFileWatch not implemented\n");
	}

	~CFileWatch()
	{
	}

	void add(const ss_ &path, std::function<void(const ss_ &path)> cb)
	{
		printf("ERROR: ""CFileWatch::add() not implemented\n");
	}

	// Used on Linux; no-op on Windows
	sv_<int> get_fds()
	{
		return {};
	}
	void report_fd(int fd)
	{
	}

	// Used on Windows; no-op on Linux
	void update()
	{
		printf("ERROR: ""CFileWatch::update() not implemented\n");
	}
};

FileWatch* createFileWatch()
{
	return new CFileWatch();
}

}

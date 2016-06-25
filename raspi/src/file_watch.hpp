// Copyright 2014 Perttu Ahola <celeron55@gmail.com>
#pragma once
#include "types.hpp"
#include <functional>

struct FileWatch
{
	virtual ~FileWatch(){}

	virtual void add(const ss_ &path,
			std::function<void(const ss_&path)> cb) = 0;

	// Used on Linux; no-op on Windows
	virtual sv_<int> get_fds() = 0;
	virtual void report_fd(int fd) = 0;

	// Used on Windows; no-op on Linux
	virtual void update() = 0;
};

// cb is called at either report_fd() or update().
FileWatch* createFileWatch();

// vim: set noet ts=4 sw=4:

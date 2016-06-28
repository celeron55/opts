#pragma once
#include "types.hpp"
#include <fstream>

static bool read_file_content(const ss_ &path, ss_ &result_data)
{
	std::ifstream f(path.c_str());
	if(!f.good())
		return false;
	result_data = ss_((std::istreambuf_iterator<char>(f)),
			std::istreambuf_iterator<char>());
	return true;
}


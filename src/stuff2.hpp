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

#include <algorithm>

static void create_shuffled_order(sv_<size_t> &shuffled_order, size_t n)
{
	shuffled_order.resize(n);
	for(size_t i=0; i<n; i++)
		shuffled_order[i] = i;
	std::random_shuffle(shuffled_order.begin(), shuffled_order.end());
}


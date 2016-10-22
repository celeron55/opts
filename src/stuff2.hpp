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

static void create_mr_shuffled_order(sv_<size_t> &shuffled_order, size_t n)
{
	size_t n0 = (n + 4) / 5;
	sv_<size_t> shuffled_order0;
	create_shuffled_order(shuffled_order0, n0);
	shuffled_order.clear();
	shuffled_order.reserve(n);
	for(size_t i0=0; i0<n0; i0++){
		for(size_t i1=0; i1<5; i1++){
			size_t track_i = shuffled_order0[i0] * 5 + i1;
			if(track_i < n)
				shuffled_order.push_back(track_i);
		}
	}
}


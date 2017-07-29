#pragma once
#include "stuff2.hpp"

struct Track
{
	ss_ path;
	ss_ display_name;

	Track(const ss_ &path="", const ss_ &display_name=""):
		path(path), display_name(display_name)
	{}
	bool operator < (const Track &other){
		return (path < other.path);
	}
};

struct Album
{
	ss_ name;
	sv_<Track> tracks;
	mutable sv_<size_t> shuffled_track_order;
	bool mr_shuffle_tracks = false;

	void ensure_shuffled_track_order_exists() const {
		if(shuffled_track_order.size() != tracks.size())
			create_shuffled_order(shuffled_track_order, tracks.size());
	}
};

struct MediaContent
{
	sv_<Album> albums;
	mutable sv_<size_t> shuffled_album_order;
	mutable sv_<size_t> mr_shuffled_album_order;
};

size_t get_total_tracks(const MediaContent &mc)
{
	size_t total = 0;
	for(auto &a : mc.albums)
		total += a.tracks.size();
	return total;
}

extern MediaContent current_media_content;


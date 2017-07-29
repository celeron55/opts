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
	bool shuffle_tracks_in_smart_mode = false;

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

static size_t get_total_tracks(const MediaContent &mc)
{
	size_t total = 0;
	for(auto &a : mc.albums)
		total += a.tracks.size();
	return total;
}

static ss_ get_filename_from_path(const ss_ &path)
{
	size_t i = path.size();
	for(;;){
		if(i == 0)
			break;
		if(path[i-1] == '/')
			break;
		i--;
	}
	return path.substr(i);
}

static int detect_track_number(const Track &track)
{
	ss_ filename = get_filename_from_path(track.path);
	const char *p = filename.c_str();
	for(;;){
		if(*p == 0)
			return -1;
		if(*p >= '0' && *p <= '9'){
			return atoi(p);
		}
		p++;
	}
}

static void smart_shuffle_scan_albums(MediaContent &mc)
{
	// Determine bool shuffle_tracks_in_smart_mode based on whether the album has
	// numbered tracks from 1 to something or not
	for(auto &album : mc.albums){
		sv_<int> track_numbers; // -1 = not detected
		for(auto &track : album.tracks){
			track_numbers.push_back(detect_track_number(track));
		}
		bool numbers_found = true;
		for(int i=0; i<(int)track_numbers.size(); i++){
			bool found = false;
			for(int j=0; j<(int)track_numbers.size(); j++){
				if(track_numbers[j] == i+1){
					found = true;
					break;
				}
			}
			if(!found){
				numbers_found = false;
				break;
			}
		}
		album.shuffle_tracks_in_smart_mode = !numbers_found;
	}
}

static void reshuffle_all_media(MediaContent &mc)
{
	// Clear track orders
	for(auto &album : mc.albums)
		album.shuffled_track_order.clear();

	// Create shuffled album order
	mc.shuffled_album_order.clear();
	create_shuffled_order(mc.shuffled_album_order, mc.albums.size());

	// Create mr. shuffled album order
	mc.mr_shuffled_album_order.clear();
	create_mr_shuffled_order(mc.mr_shuffled_album_order, mc.albums.size());

	// Detect which albums are to be shuffled in smart shuffle mode
	smart_shuffle_scan_albums(mc);
}

extern MediaContent current_media_content;


#pragma once

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
};

struct MediaContent
{
	sv_<Album> albums;
	mutable sv_<size_t> shuffled_album_order;
	mutable sv_<size_t> mr_shuffled_album_order;
};

extern MediaContent current_media_content;


#pragma once
#include "stuff2.hpp"

enum TrackProgressMode {
	TPM_NORMAL,
	TPM_ALBUM_REPEAT,
	TPM_ALBUM_REPEAT_TRACK,
	TPM_SHUFFLE_ALL,
	TPM_SHUFFLE_TRACKS,
	TPM_MR_SHUFFLE,

	TPM_NUM_MODES,
};

struct PlayCursor
{
public:
	TrackProgressMode track_progress_mode = TPM_NORMAL;
	int album_seq_i = 0;
	int track_seq_i = 0;
	double time_pos = 0;
	int64_t stream_pos = 0;
	ss_ track_name;
	ss_ album_name;

	int album_i(const MediaContent &mc) const {
		if(mc.albums.empty())
			return 0;
		if(album_seq_i < 0 || album_seq_i >= (int)mc.albums.size()){
			printf_("album_seq_i overflow\n");
			return 0;
		}
		if(track_progress_mode == TPM_SHUFFLE_ALL){
			return mc.shuffled_album_order[album_seq_i];
		} else if(track_progress_mode == TPM_MR_SHUFFLE){
			return mc.mr_shuffled_album_order[album_seq_i];
		} else {
			return album_seq_i;
		}
	}
	int track_i(const MediaContent &mc) const {
		if(mc.albums.empty())
			return 0;
		const Album &album = mc.albums[album_i(mc)];
		if(album.tracks.empty())
			return 0;
		if(track_seq_i < 0 || track_seq_i >= (int)album.tracks.size()){
			printf_("track_seq_i overflow\n");
			return 0;
		}
		if(track_progress_mode == TPM_SHUFFLE_ALL ||
				track_progress_mode == TPM_SHUFFLE_TRACKS){
			if(album.shuffled_track_order.size() != album.tracks.size())
				create_shuffled_order(album.shuffled_track_order, album.tracks.size());
			return album.shuffled_track_order[track_seq_i];
		} else if(track_progress_mode == TPM_MR_SHUFFLE){
			if(!album.mr_shuffle_tracks)
				return track_seq_i;
			if(album.shuffled_track_order.size() != album.tracks.size())
				create_shuffled_order(album.shuffled_track_order, album.tracks.size());
			return album.shuffled_track_order[track_seq_i];
		} else {
			return track_seq_i;
		}
	}

	void set_track_progress_mode(const MediaContent &mc, TrackProgressMode new_tpm){
		if(new_tpm == track_progress_mode)
			return;
		if((new_tpm == TPM_SHUFFLE_ALL || new_tpm == TPM_SHUFFLE_TRACKS)
				&& !mc.albums.empty()){
			// Resolve into shuffled indexing
			const Album &album = mc.albums[album_seq_i];
			if(album.shuffled_track_order.size() != album.tracks.size())
				create_shuffled_order(album.shuffled_track_order, album.tracks.size());
			for(int ti1=0; ti1<(int)album.tracks.size(); ti1++){
				if((int)album.shuffled_track_order[ti1] == track_seq_i){
					track_seq_i = ti1;
					break;
				}
			}
			if(new_tpm == TPM_SHUFFLE_ALL){
				for(int ai1=0; ai1<(int)mc.albums.size(); ai1++){
					if((int)mc.shuffled_album_order[ai1] == album_seq_i){
						album_seq_i = ai1;
						break;
					}
				}
			}
		}
		if((new_tpm == TPM_MR_SHUFFLE) && !mc.albums.empty()){
			// Resolve into shuffled indexing
			const Album &album = mc.albums[album_seq_i];
			if(album.shuffled_track_order.size() != album.tracks.size())
				create_shuffled_order(album.shuffled_track_order, album.tracks.size());
			for(int ti1=0; ti1<(int)album.tracks.size(); ti1++){
				if((int)album.shuffled_track_order[ti1] == track_seq_i){
					track_seq_i = ti1;
					break;
				}
			}
			for(int ai1=0; ai1<(int)mc.albums.size(); ai1++){
				if((int)mc.mr_shuffled_album_order[ai1] == album_seq_i){
					album_seq_i = ai1;
					break;
				}
			}
		}
		if(track_progress_mode == TPM_SHUFFLE_ALL ||
				track_progress_mode == TPM_SHUFFLE_TRACKS ||
				track_progress_mode == TPM_MR_SHUFFLE){
			// Resolve back from shuffled indexing
			int new_album_seq_i = album_i(mc);
			int new_track_seq_i = track_i(mc);
			if(track_progress_mode == TPM_SHUFFLE_ALL ||
					track_progress_mode == TPM_MR_SHUFFLE){
				album_seq_i = new_album_seq_i;
			}
			track_seq_i = new_track_seq_i;
		}
		track_progress_mode = new_tpm;
	}
};

extern PlayCursor current_cursor;
extern PlayCursor last_succesfully_playing_cursor;

static Track get_track(const MediaContent &mc, const PlayCursor &cursor)
{
	if(cursor.album_seq_i >= (int)mc.albums.size()){
		printf_("Album cursor overflow\n");
		return Track();
	}
	const Album &album = mc.albums[cursor.album_i(mc)];
	if(cursor.track_seq_i >= (int)album.tracks.size()){
		printf_("Track cursor overflow\n");
		return Track();
	}
	return album.tracks[cursor.track_i(mc)];
}

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

enum PauseMode {
	PM_PLAY,
	PM_PAUSE,
	// Not a real pause but one that is used while in power off mode (until power is
	// actually cut, or power off mode is switched off)
	PM_UNFOCUS_PAUSE,
};

struct PlayCursor
{
public:
	TrackProgressMode track_progress_mode = TPM_NORMAL;
	PauseMode current_pause_mode = PM_PLAY;
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

void cursor_bound_wrap(const MediaContent &mc, PlayCursor &cursor)
{
	if(mc.albums.empty())
		return;
	if(cursor.album_seq_i < 0)
		cursor.album_seq_i = mc.albums.size() - 1;
	if(cursor.album_seq_i >= (int)mc.albums.size())
		cursor.album_seq_i = 0;

	if(cursor.track_progress_mode == TPM_ALBUM_REPEAT){
		const Album &album = mc.albums[cursor.album_i(mc)];
		if(cursor.track_seq_i < 0){
			cursor.track_seq_i = album.tracks.size() - 1;
		} else if(cursor.track_seq_i >= (int)album.tracks.size()){
			cursor.track_seq_i = 0;
		}
	} else {
		const Album &album = mc.albums[cursor.album_i(mc)];
		if(cursor.track_seq_i < 0){
			cursor.album_seq_i--;
			if(cursor.album_seq_i < 0)
				cursor.album_seq_i = mc.albums.size() - 1;
			const Album &album2 = mc.albums[cursor.album_i(mc)];
			cursor.track_seq_i = album2.tracks.size() - 1;
		} else if(cursor.track_seq_i >= (int)album.tracks.size()){
			cursor.track_seq_i = 0;
			cursor.album_seq_i++;
			if(cursor.album_seq_i >= (int)mc.albums.size())
				cursor.album_seq_i = 0;
		}
	}
}

ss_ get_album_name(const MediaContent &mc, const PlayCursor &cursor)
{
	if(cursor.album_seq_i >= (int)mc.albums.size()){
		printf_("Album cursor overflow\n");
		return "ERR:AOVF";
	}
	const Album &album = mc.albums[cursor.album_i(mc)];
	return album.name;
}

ss_ get_track_name(const MediaContent &mc, const PlayCursor &cursor)
{
	if(cursor.album_seq_i >= (int)mc.albums.size()){
		printf_("Album cursor overflow\n");
		return "ERR:AOVF";
	}
	const Album &album = mc.albums[cursor.album_i(mc)];
	if(cursor.track_seq_i >= (int)album.tracks.size()){
		printf_("Track cursor overflow\n");
		return "ERR:TOVF";
	}
	return album.tracks[cursor.track_i(mc)].display_name;
}

ss_ get_cursor_info(const MediaContent &mc, const PlayCursor &cursor)
{
	if(mc.albums.empty())
		return "No media";

	ss_ s;
	if(cursor.track_progress_mode == TPM_SHUFFLE_ALL || cursor.track_progress_mode == TPM_MR_SHUFFLE){
		s += "Album #"+itos(cursor.album_seq_i+1)+"="+itos(cursor.album_i(mc)+1)+
				" ("+get_album_name(mc, cursor)+")"+
				", track #"+itos(cursor.track_seq_i+1)+"="+itos(cursor.track_i(mc)+1)+
				" ("+get_track_name(mc, cursor)+")"+
				(cursor.time_pos != 0.0 ? (", pos "+ftos(cursor.time_pos)+"s") : ss_());
	} else if(cursor.track_progress_mode == TPM_SHUFFLE_TRACKS){
		s += "Album #"+itos(cursor.album_i(mc)+1)+" ("+get_album_name(mc, cursor)+")"+
				", track #"+itos(cursor.track_seq_i+1)+"="+itos(cursor.track_i(mc)+1)+
				" ("+get_track_name(mc, cursor)+")"+
				(cursor.time_pos != 0.0 ? (", pos "+ftos(cursor.time_pos)+"s") : ss_());
	} else {
		s += "Album #"+itos(cursor.album_i(mc)+1)+" ("+get_album_name(mc, cursor)+
				"), track #"+itos(cursor.track_i(mc)+1)+" ("+get_track_name(mc, cursor)+")"+
				(cursor.time_pos != 0.0 ? (", pos "+ftos(cursor.time_pos)+"s") : ss_());
	}
	if(get_track_name(mc, cursor) != cursor.track_name)
		s += ", should be track ("+cursor.track_name+")";
	return s;
}

// If failed, return false and leave cursor as-is.
bool resolve_track_from_current_album(const MediaContent &mc, PlayCursor &cursor)
{
	if(cursor.album_seq_i >= (int)mc.albums.size())
		return false;
	const Album &album = mc.albums[cursor.album_i(mc)];
	PlayCursor cursor1 = cursor;
	for(cursor1.track_seq_i=0; cursor1.track_seq_i<(int)album.tracks.size(); cursor1.track_seq_i++){
		const Track &track = album.tracks[cursor1.track_i(mc)];
		if(track.display_name == cursor.track_name){
			cursor = cursor1;
			return true;
		}
	}
	return false;
}

// If failed, return false and leave cursor as-is.
bool resolve_track_from_any_album(const MediaContent &mc, PlayCursor &cursor)
{
	PlayCursor cursor1 = cursor;
	for(cursor1.album_seq_i=0; cursor1.album_seq_i<(int)mc.albums.size(); cursor1.album_seq_i++){
		bool found = resolve_track_from_current_album(mc, cursor1);
		if(found){
			cursor = cursor1;
			return true;
		}
	}
	return false;
}

// Find track by the same name as near the cursor as possible. If failed, return
// false and leave cursor as-is.
bool force_resolve_track(const MediaContent &mc, PlayCursor &cursor)
{
	printf_("Force-resolving track\n");

	// First find album
	PlayCursor cursor1 = cursor;
	bool album_found = false;
	for(cursor1.album_seq_i=0; cursor1.album_seq_i<(int)mc.albums.size();
			cursor1.album_seq_i++){
		const Album &album = mc.albums[cursor1.album_i(mc)];
		if(album.name == cursor.album_name){
			album_found = true;
			cursor = cursor1;
			break;
		}
	}
	if(!album_found){
		printf_("-> Didn't find album \"%s\"\n", cs(cursor.album_name));
		return resolve_track_from_any_album(mc, cursor);
	}

	// Get queued shuffled track order if such exists
	if(!queued_album_shuffled_track_order.empty()){
		printf_("Applying queued album shuffled track order\n");
		if(cursor.album_i(mc) < (int)mc.albums.size()){
			const Album &album = mc.albums[cursor.album_i(mc)];
			if(queued_album_shuffled_track_order.size() == album.tracks.size()){
				album.shuffled_track_order = queued_album_shuffled_track_order;
				queued_album_shuffled_track_order.clear();
			} else {
				printf_("Applying queued album shuffled track order: track number mismatch\n");
			}
		} else {
			printf_("Applying queued album shuffled track order: overflow\n");
		}
	}

	// Then find track on the album
	const Album &album = mc.albums[cursor.album_i(mc)];
	const Track &track = album.tracks[cursor.track_i(mc)];
	if(track.display_name == cursor.track_name){
		printf_("Found as track #%i on album #%i\n",
				cursor.track_i(mc)+1, cursor.album_i(mc)+1);
		return true;
	}
	bool found = resolve_track_from_current_album(mc, cursor);
	if(found){
		printf_("Found as track #%i on album #%i\n",
				cursor.track_i(mc)+1, cursor.album_i(mc)+1);
		return true;
	}
	printf_("Didn't find track on current album; searching everywhere\n");
	return resolve_track_from_any_album(mc, cursor);
}

extern PlayCursor current_cursor;
extern PlayCursor last_succesfully_playing_cursor;


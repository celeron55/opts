#pragma once
#include "library.hpp"
#include "play_cursor.hpp"
#include "print.hpp"
#include "arduino_global.hpp"
#include "../common/common.hpp"

static void temp_display_album()
{
	if(current_media_content.albums.empty())
		return;

	arduino_set_temp_text(squeeze(get_album_name(current_media_content, current_cursor),
			arduino_display_width));

	// Delay track scroll for one second
	display_update_timestamp = time(0) + 1;
}

static void arduino_set_extra_segments()
{
	uint8_t extra_segment_flags = 0;
	switch(current_cursor.track_progress_mode){
	case TPM_NORMAL:
		break;
	case TPM_ALBUM_REPEAT:
		extra_segment_flags |= (1<<DISPLAY_FLAG_REPEAT);
		break;
	case TPM_ALBUM_REPEAT_TRACK:
		extra_segment_flags |= (1<<DISPLAY_FLAG_REPEAT) | (1<<DISPLAY_FLAG_REPEAT_ONE);
		break;
	case TPM_SHUFFLE_ALL:
		extra_segment_flags |= (1<<DISPLAY_FLAG_SHUFFLE);
		break;
	case TPM_SHUFFLE_TRACKS:
		extra_segment_flags |= (1<<DISPLAY_FLAG_SHUFFLE) | (1<<DISPLAY_FLAG_REPEAT_ONE);
		break;
	case TPM_SMART_TRACK_SHUFFLE:
	case TPM_SMART_ALBUM_SHUFFLE:
	case TPM_MR_SHUFFLE:
		extra_segment_flags |= (1<<DISPLAY_FLAG_SHUFFLE) | (1<<DISPLAY_FLAG_REPEAT) |
				(1<<DISPLAY_FLAG_REPEAT_ONE);
		break;
	case TPM_NUM_MODES:
		break;
	}
	if(current_cursor.current_pause_mode == PM_PAUSE){
		extra_segment_flags |= (1<<DISPLAY_FLAG_PAUSE);
	}
	arduino_serial_write(">EXTRA_SEGMENTS:"+itos(extra_segment_flags)+"\r\n");
}


#include "arduino_controls.hpp"
#include "library.hpp"
#include "play_cursor.hpp"
#include "arduino_global.hpp"
#include "arduino_firmware.hpp"
#include "command_accumulator.hpp"
#include "types.hpp"
#include "ui.hpp"
#include "print.hpp"
#include "string_util.hpp"
#include "sleep.hpp"
#include "terminal.hpp"
#include "mpv_control.hpp"
#include "ui_output_queue.hpp"
#include "../common/common.hpp"
#include <mpv/client.h>

int arduino_serial_fd = -1;
ss_ arduino_serial_fd_path;
bool tried_to_update_arduino_firmware = false;
time_t arduino_last_incoming_message_timestamp = 0;
CommandAccumulator<100> arduino_message_accu;
set_<int> current_keys;

StatefulInputMode stateful_input_mode = SIM_NONE;
time_t stateful_input_mode_active_timestamp = 0;
CommandAccumulator<10> stateful_input_accu;

time_t display_update_timestamp = 0;

void arduino_set_extra_segments()
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

void handle_key_press(int key)
{
	current_keys.insert(key);

	if(key == 24){
		if(stateful_input_mode != SIM_NONE)
			stateful_input_cancel();
		command_playpause();
		return;
	}
	if(key == 12){
		if(stateful_input_mode != SIM_NONE)
			stateful_input_enter();
		else
			command_next();
		return;
	}
	if(key == 27){
		if(stateful_input_mode != SIM_NONE)
			stateful_input_cancel();
		else
			command_prev();
		return;
	}
	if(key == 23){
		if(stateful_input_mode != SIM_NONE)
			stateful_input_enter();
		else
			command_nextalbum();
		return;
	}
	if(key == 29){
		if(stateful_input_mode != SIM_NONE)
			stateful_input_cancel();
		else
			command_prevalbum();
		return;
	}
	if(key == 17){ // Upmost center
		if(stateful_input_mode != SIM_NONE)
			stateful_input_cancel();
		command_playmode();
		return;
	}
	if(key == 18){ // Right upper
		command_random_album();
		return;
	}
	if(key == 13){ // Right lower
		stateful_input_mode_select();
		return;
	}
	if(key == 21){ // 1
		hwcontrol_input_digit(1);
		return;
	}
	if(key == 16){ // 2
		hwcontrol_input_digit(2);
		return;
	}
	if(key == 10){ // 3
		hwcontrol_input_digit(3);
		return;
	}
	if(key == 15){ // 4
		hwcontrol_input_digit(4);
		return;
	}
	if(key == 20){ // 5
		hwcontrol_input_digit(5);
		return;
	}
	if(key == 25){ // 6
		hwcontrol_input_digit(6);
		return;
	}
}

void handle_key_release(int key)
{
	current_keys.erase(key);

	if(stateful_input_mode == SIM_NONE){
		if(key == 21 || key == 16 || key == 10 || key == 15 || key == 20 || key == 25){
			if(!current_keys.count(21) && !current_keys.count(16) &&
					!current_keys.count(10) && !current_keys.count(15) &&
					!current_keys.count(20) && !current_keys.count(25)){
				temp_display_album();
			}
		}
	}
}

void try_open_arduino_serial()
{
#ifdef __WIN32__
#else
	for(const ss_ &arduino_serial_path : arduino_serial_paths){
		arduino_serial_fd = open(arduino_serial_path.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
		if(arduino_serial_fd < 0){
			printf_("Failed to open %s\n", cs(arduino_serial_path));
			arduino_serial_fd = -1;
			continue;
		}
		if(!set_interface_attribs(arduino_serial_fd, 9600, 0)){
			printf_("Failed to set attributes for serial fd\n");
			continue;
		}
		printf_("Opened arduino serial port %s\n", cs(arduino_serial_path));
		arduino_serial_fd_path = arduino_serial_path;
		return;
	}
#endif
}

void handle_hwcontrols()
{
	update_stateful_input();

	if(arduino_serial_fd == -1){
		static time_t last_retry_time = 0;
		if(last_retry_time < time(0) - 5 && !arduino_serial_paths.empty()){
			last_retry_time = time(0);
			printf_("Retrying arduino serial\n");
			try_open_arduino_serial();
		}
		if(arduino_serial_fd == -1){
			return;
		}
	}
	bool error = false;
	ss_ serial_stuff = read_any(arduino_serial_fd, &error);
	if(error){
		arduino_serial_fd = -1;
		return;
	}
	for(char c : serial_stuff){
		if(arduino_message_accu.put_char(c)){
			arduino_last_incoming_message_timestamp = time(0);
			ss_ message = arduino_message_accu.command();
			Strfnd f(message);
			ss_ first = f.next(":");
			if(first == "<KEY_PRESS"){
				int key = stoi(f.next(":"));
				printf_("<KEY_PRESS  : %i\n", key);
				handle_key_press(key);
			} else if(first == "<KEY_RELEASE"){
				int key = stoi(f.next(":"));
				printf_("<KEY_RELEASE: %i\n", key);
				handle_key_release(key);
			} else if(first == "<BOOT"){
				printf_("<BOOT\n");
				arduino_set_extra_segments();
				temp_display_album();
				refresh_track();

				arduino_request_version();
			} else if(first == "<MODE"){
				ss_ mode = f.next(":");
				if(mode == "RASPBERRY"){
					if(current_cursor.current_pause_mode == PM_UNFOCUS_PAUSE){
						printf_("Leaving unfocus pause\n");
						check_mpv_error(mpv_command_string(mpv, "pause"));
						current_cursor.current_pause_mode = PM_PLAY;
					}
				} else {
					if(current_cursor.current_pause_mode == PM_PLAY){
						printf_("Entering unfocus pause\n");
						check_mpv_error(mpv_command_string(mpv, "pause"));
						current_cursor.current_pause_mode = PM_UNFOCUS_PAUSE;
					}
				}
			} else if(first == "<POWERDOWN_WARNING"){
				printf_("<POWERDOWN_WARNING\n");
				save_stuff();
			} else if(first == "<VERSION"){
				printf_("%s\n", cs(message));
				ss_ version = f.next("");
				if(!tried_to_update_arduino_firmware){
					tried_to_update_arduino_firmware = true;
					arduino_firmware_update_if_needed(version);
				}
			} else {
				printf_("%s (ignored)\n", cs(message));
			}
		}
	}

	// Trigger arduino firmware upgrade if arduino doesn't send anything
	if(arduino_serial_fd_path != "" &&
			!tried_to_update_arduino_firmware &&
			arduino_last_incoming_message_timestamp < time(0) - 10 &&
			startup_timestamp < time(0) - 10){
		tried_to_update_arduino_firmware = true;
		arduino_firmware_update_if_needed("(no version response)");
	}
}

void display_stateful_input()
{
	ss_ current_input;
	for(size_t i=0; i<stateful_input_accu.next_i; i++){
		current_input += stateful_input_accu.buffer[i];
	}
	switch(stateful_input_mode){
	case SIM_TRACK_NUMBER:
		arduino_set_text(current_input + (time(0)%2?"_":" ") + " TRACK");
		break;
	case SIM_ALBUM_NUMBER:
		arduino_set_text(current_input + (time(0)%2?"_":" ") + " ALBUM");
		break;
	case SIM_NONE:
	case SIM_NUM_MODES:
		arduino_set_text("ERROR");
		break;
	}
}

ss_ current_displayed_track_name;
sv_<ss_> current_displayed_track_name_pieces;
size_t current_displayed_track_name_next_shown_piece = 0;

void update_and_show_default_display()
{
	display_update_timestamp = time(0);

	if(stateful_input_mode != SIM_NONE){
		display_stateful_input();
		return;
	}

	if(current_keys.count(21) || current_keys.count(16) ||
			current_keys.count(10) || current_keys.count(15) ||
			current_keys.count(20) || current_keys.count(25)){
		// Numeric key without any special mode.
		// Temporarily display album and track number until key isn't pressed.
		auto &mc = current_media_content;
		ss_ s = itos(current_cursor.album_i(mc)+1)+"-"+itos(current_cursor.track_i(mc)+1);
		ui_output_queue::push_message(s);
	}

	if(current_media_content.albums.empty()){
		arduino_set_text("NO MEDIA");
		return;
	}

	ss_ track_name = get_track_name(current_media_content, current_cursor);
	if(minimize_display_updates && track_name == current_displayed_track_name)
		return;
	if(track_name != current_displayed_track_name){
		current_displayed_track_name = track_name;
		current_displayed_track_name_pieces = toupper(split_string_to_clean_ui_pieces(
				track_name, arduino_display_width));
		current_displayed_track_name_next_shown_piece = 0;
	}
	if(current_displayed_track_name_next_shown_piece >=
			current_displayed_track_name_pieces.size()){
		// Full track name shown. Get next one.
		current_displayed_track_name_next_shown_piece = 0;
		// Show separator
		arduino_set_text(" - - -  ");
		display_update_timestamp = time(0);
		return; // Showing empty screen
	}
	ss_ text_to_show = current_displayed_track_name_pieces[
			current_displayed_track_name_next_shown_piece];
	current_displayed_track_name_next_shown_piece++;
	arduino_set_text(text_to_show);
	display_update_timestamp = time(0);
	return; // Showing message
}

ss_ current_output_message;
sv_<ss_> current_output_message_pieces;
size_t current_output_message_next_shown_piece = 0;

void handle_display()
{
	if(display_update_timestamp > time(0) - 1)
		return;

	// Loop to find something to show
	for(;;){
		ui_output_queue::Message m = ui_output_queue::get_message();
		if(m.short_text == "")
			break; // No messages
		if(m.short_text != current_output_message){
			current_output_message = m.short_text;
			current_output_message_pieces = toupper(split_string_to_clean_ui_pieces(
					m.short_text, arduino_display_width));
			current_output_message_next_shown_piece = 0;
		}
		if(current_output_message_next_shown_piece >=
				current_output_message_pieces.size()){
			// Full message shown. Get next one.
			current_output_message = "";
			current_output_message_next_shown_piece = 0;
			ui_output_queue::pop_message();
			// But meanwhile, show an empty screen to separate messages
			arduino_set_text(" - - -  ");
			display_update_timestamp = time(0);
			current_displayed_track_name = ""; // Restart default display
			current_displayed_track_name_next_shown_piece = 0; // Restart default display
			return; // Showing empty screen
		}
		ss_ text_to_show = current_output_message_pieces[
				current_output_message_next_shown_piece];
		current_output_message_next_shown_piece++;
		arduino_set_text(text_to_show);
		display_update_timestamp = time(0);
		current_displayed_track_name = ""; // Restart default display
		current_displayed_track_name_next_shown_piece = 0; // Restart default display
		return; // Showing message
	}

	update_and_show_default_display();
}

void stateful_input_mode_select()
{
	if(stateful_input_mode < SIM_NUM_MODES - 1)
		stateful_input_mode = (StatefulInputMode)(stateful_input_mode + 1);
	else
		stateful_input_mode = (StatefulInputMode)0;

	update_and_show_default_display();
}

void stateful_input_mode_input(char input_char)
{
	if(stateful_input_accu.put_char(input_char)){
		ss_ command = stateful_input_accu.command();
		printf_("Stateful input command: %s\n", cs(command));
		if(command.size() == 0)
			return;
		int input_number = stoi(command, -1);
		if(input_number == -1)
			return;
		switch(stateful_input_mode){
		case SIM_TRACK_NUMBER:
			stateful_input_accu.reset();
			command_track_number(input_number);
			break;
		case SIM_ALBUM_NUMBER:
			stateful_input_accu.reset();
			command_album_number(input_number);
			break;
		case SIM_NONE:
		case SIM_NUM_MODES:
			break;
		}
	}
	update_and_show_default_display();
}

void stateful_input_enter()
{
	stateful_input_mode_input('\r');
}

void stateful_input_cancel()
{
	stateful_input_accu.reset();
	stateful_input_mode = SIM_NONE;
	update_and_show_default_display();
}

void update_stateful_input()
{
}

void hwcontrol_input_digit(int input_digit)
{
	if(stateful_input_mode != SIM_NONE){
		stateful_input_mode_input('0'+input_digit);
		return;
	}

	if(input_digit == 1){
		const int seconds = 30;
		mpv_command_string(mpv, cs("seek -"+itos(seconds)));
		return;
	}

	if(input_digit == 2){
		const int seconds = 30;
		mpv_command_string(mpv, cs("seek +"+itos(seconds)));
		return;
	}

	if(input_digit == 3){
		command_next_collection_part(-1);
		// TODO: Add a temp text display prioritization system
		sleep(1); // Wait while directory is shown on screen
		return;
	}

	if(input_digit == 4){
		command_next_collection_part(1);
		// TODO: Add a temp text display prioritization system
		sleep(1); // Wait while directory is shown on screen
		return;
	}

	// Album and track number will be displayed
	update_and_show_default_display();
}

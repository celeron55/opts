#include "arduino_controls.hpp"
#include "arduino_semiglobal.hpp"
#include "arduino_global.hpp"
#include "arduino_firmware.hpp"
#include "command_accumulator.hpp"
#include "types.hpp"
#include "ui.hpp"
#include "print.hpp"
#include "string_util.hpp"
#include "sleep.hpp"
#include "terminal.hpp"
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
size_t display_next_startpos = 0;
ss_ display_last_shown_track_name;

void handle_key_press(int key)
{
	current_keys.insert(key);

	if(key == 24){
		if(stateful_input_mode != SIM_NONE)
			on_ui_stateful_input_cancel();
		on_ui_playpause();
		return;
	}
	if(key == 12){
		if(stateful_input_mode != SIM_NONE)
			on_ui_stateful_input_enter();
		else
			on_ui_next();
		return;
	}
	if(key == 27){
		if(stateful_input_mode != SIM_NONE)
			on_ui_stateful_input_cancel();
		else
			on_ui_prev();
		return;
	}
	if(key == 23){
		if(stateful_input_mode != SIM_NONE)
			on_ui_stateful_input_enter();
		else
			on_ui_nextalbum();
		return;
	}
	if(key == 29){
		if(stateful_input_mode != SIM_NONE)
			on_ui_stateful_input_cancel();
		else
			on_ui_prevalbum();
		return;
	}
	if(key == 17){ // Upmost center
		if(stateful_input_mode != SIM_NONE)
			on_ui_stateful_input_cancel();
		on_ui_playmode();
		return;
	}
	if(key == 18){ // Right upper
		on_ui_random_album();
		return;
	}
	if(key == 13){ // Right lower
		on_ui_stateful_input_mode();
		return;
	}
	if(key == 21){ // 1
		on_ui_input_digit(1);
		return;
	}
	if(key == 16){ // 2
		on_ui_input_digit(2);
		return;
	}
	if(key == 10){ // 3
		on_ui_input_digit(3);
		return;
	}
	if(key == 15){ // 4
		on_ui_input_digit(4);
		return;
	}
	if(key == 20){ // 5
		on_ui_input_digit(5);
		return;
	}
	if(key == 25){ // 6
		on_ui_input_digit(6);
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

void update_display()
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
		arduino_set_temp_text(s);
	}

	if(current_media_content.albums.empty()){
		arduino_set_text("NO MEDIA");
		return;
	}

	ss_ track_name = get_track_name(current_media_content, current_cursor);
	if(minimize_display_updates && track_name == display_last_shown_track_name)
		return;
	if(track_name != display_last_shown_track_name){
		display_last_shown_track_name = track_name;
		display_next_startpos = 0;
	}
	ss_ squeezed = squeeze(track_name, arduino_display_width * 2, display_next_startpos);
	if(squeezed == ""){
		display_next_startpos = 0;
		squeezed = squeeze(track_name, arduino_display_width * 2, display_next_startpos);
	}
	if((int)squeezed.size() >= arduino_display_width)
		squeezed = squeeze(squeezed, arduino_display_width * 2);
	arduino_set_text(squeezed);
}

void handle_display()
{
	if(display_update_timestamp > time(0) - 1)
		return;
	update_display();
	if(!minimize_display_updates)
		display_next_startpos += arduino_display_width;
}

void on_ui_stateful_input_mode()
{
	if(stateful_input_mode < SIM_NUM_MODES - 1)
		stateful_input_mode = (StatefulInputMode)(stateful_input_mode + 1);
	else
		stateful_input_mode = (StatefulInputMode)0;

	update_display();
}

void on_ui_stateful_input_mode_input(char input_char)
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
			on_ui_track_number(input_number);
			break;
		case SIM_ALBUM_NUMBER:
			stateful_input_accu.reset();
			on_ui_album_number(input_number);
			break;
		case SIM_NONE:
		case SIM_NUM_MODES:
			break;
		}
	}
	update_display();
}

void on_ui_stateful_input_enter()
{
	on_ui_stateful_input_mode_input('\r');
}

void on_ui_stateful_input_cancel()
{
	stateful_input_accu.reset();
	stateful_input_mode = SIM_NONE;
	update_display();
}

void update_stateful_input()
{
}

void on_ui_input_digit(int input_digit)
{
	if(stateful_input_mode != SIM_NONE){
		on_ui_stateful_input_mode_input('0'+input_digit);
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
		next_collection_part(-1);
		// TODO: Add a temp text display prioritization system
		sleep(1); // Wait while directory is shown on screen
		return;
	}

	if(input_digit == 4){
		next_collection_part(1);
		// TODO: Add a temp text display prioritization system
		sleep(1); // Wait while directory is shown on screen
		return;
	}

	// Album and track number will be displayed
	update_display();
}

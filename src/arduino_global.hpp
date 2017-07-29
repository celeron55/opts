#pragma once

enum StatefulInputMode {
	SIM_NONE,
	SIM_TRACK_NUMBER,
	SIM_ALBUM_NUMBER,

	SIM_NUM_MODES,
};

extern int arduino_serial_fd;

extern time_t display_update_timestamp;

void handle_key_press(int key);
void handle_key_release(int key);
void try_open_arduino_serial();
void handle_hwcontrols();
void display_stateful_input();
void update_display();
void handle_display();
void on_ui_stateful_input_mode();
void on_ui_stateful_input_mode_input(char input_char);
void on_ui_stateful_input_enter();
void on_ui_stateful_input_cancel();
void update_stateful_input();
void on_ui_input_digit(int input_digit);

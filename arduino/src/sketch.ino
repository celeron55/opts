#include "lcd.h"
#include "command_accumulator.h"
#include "../version.h"
#include <avr/eeprom.h>

#define RASPBERRY_REAL_POWER_OFF_DELAY 600
#define RASPBERRY_POWER_OFF_WARNING_DELAY 5

const int PIN_LED = 13;
const int PIN_MAIN_POWER_CONTROL = 4;
const int PIN_LCD_CE = 7;
const int PIN_LCD_CL = 8;
const int PIN_LCD_DI = 9;
const int PIN_LCD_DO = 10;
const int PIN_ENCODER1 = A0;
const int PIN_ENCODER2 = A1;
const int PIN_VOL_CE = 11;
const int PIN_VOL_DI = 12;
const int PIN_VOL_CL = 13;
const int PIN_STANDBY_DISABLE = 5;
const int PIN_RASPBERRY_POWER_OFF = 2;
const int PIN_IGNITION_INPUT = A2;

bool g_saveables_dirty = false;

uint8_t g_encoder_last_state = 0xff;

#define CONFIG_MENU_TIMER_RESET_VALUE 3000
uint16_t g_config_menu_show_timer = 0; // milliseconds; counts down
enum ConfigOption {
	CO_BASS,
	CO_TREBLE,
	CO_BENIS,
	CO_PI_CYCLE,

	CO_NUM_OPTIONS,
};
ConfigOption g_config_option = CO_BASS;

bool g_benis_mode_enabled = false;

uint8_t g_previous_keys[4] = {0, 0, 0, 0};
uint8_t g_current_keys[4] = {0, 0, 0, 0};
// Timestamp is needed because no-keys-pressed is not received
uint32_t g_last_keys_timestamp = 0;

uint32_t g_second_counter_timestamp = 0;
uint32_t g_millisecond_counter_timestamp = 0;

uint8_t g_boot_message_delay = 100; // milliseconds; counts down

bool g_manual_power_state = false; // True if powered on/off regardless of ignition input

bool g_amplifier_power_on = false;
uint8_t g_amplifier_real_power_off_delay = 0; // seconds; counts down

bool g_raspberry_power_on = false;
uint8_t g_raspberry_power_off_warning_delay = 0; // seconds; counts down
uint8_t g_raspberry_real_power_off_delay = 0; // seconds; counts down

bool g_lcd_do_sleep = false;
uint8_t g_display_data[] = {
	0x11, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
};
uint8_t g_temp_display_data[] = {
	0x11, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
};
uint16_t g_temp_display_data_timer = 0; // milliseconds; counts down

struct VolumeControls {
	uint8_t fader = 15; // 0...15 (-infdB...0dB)
	uint8_t super_bass = 0; // 0...10
	int8_t bass = 0; // -7...7
	int8_t treble = 0; // -7...7
	uint8_t volume = 45;
	// NOTE: in2=5=CD, in5=0=AUX
	uint8_t input_switch = 5; // valid values: in1=4, in2=5, in3=6, in4=7, in5=0
	uint8_t mute_switch = 0; // 0, 1
	uint8_t channel_sel = 3; // 0=initial, 1=L, 2=R, 3=both
	uint8_t output_gain = 3; // 0=0dB, 1=0dB, 2=+6.5dB, 3=+8.5dB
};
VolumeControls g_volume_controls;

//uint8_t g_debug_test_segment_i = 0;

CommandAccumulator<50> command_accumulator;

enum ControlMode {
	CM_POWER_OFF,
	CM_AUX,
	CM_RASPBERRY,
} g_control_mode = CM_RASPBERRY;

struct Mode {
	const char *name;
	void (*update)();
	void (*handle_keys)();
};

void power_off_update();
void power_off_handle_keys();

void aux_update();
void aux_handle_keys();

void raspberry_update();
void raspberry_handle_keys();

Mode CONTROL_MODES[] = {
	{
		"POWER_OFF",
		power_off_update,
		power_off_handle_keys,
	},
	{
		"AUX",
		aux_update,
		aux_handle_keys,
	},
	{
		"RASPBERRY",
		raspberry_update,
		raspberry_handle_keys,
	},
};

char g_raspberry_display_text[9] = "RASPBERR";

void power_off_update()
{
	set_segments(g_display_data, 0, "POWER OFF");
}

void power_off_handle_keys()
{
	// Power button
	if(lcd_is_key_pressed(g_current_keys, 22) && !lcd_is_key_pressed(g_previous_keys, 22)){
		power_on();
		g_control_mode = CM_AUX;
		g_saveables_dirty = true;

		if(!digitalRead(PIN_IGNITION_INPUT)){
			g_manual_power_state = true;
		} else {
			// Powering on while ignition is on resets the manual power state,
			// and the next time ignition is turned on the player will turn on
			g_manual_power_state = false;
		}

		Serial.print(F("<MODE:"));
		Serial.println(CONTROL_MODES[g_control_mode].name);
	}
}

void aux_update()
{
	char buf[10] = {0};
	snprintf(buf, 10, "AUX %i    ", g_volume_controls.volume);
	set_segments(g_display_data, 0, buf);

	if(g_volume_controls.input_switch != 0){
		g_volume_controls.input_switch = 0;
		send_volume_update();
	}
}

void aux_handle_keys()
{
	// Power button
	if(lcd_is_key_pressed(g_current_keys, 22) && !lcd_is_key_pressed(g_previous_keys, 22)){
		power_on();
		g_control_mode = CM_RASPBERRY;
		g_saveables_dirty = true;

		Serial.print(F("<MODE:"));
		Serial.println(CONTROL_MODES[g_control_mode].name);
	}
}

void raspberry_update()
{
	while(command_accumulator.read(Serial)){
		const char *command = command_accumulator.command();
		if(strcmp(command, ">VERSION") == 0){
			Serial.print(F("<VERSION:"));
			Serial.println(VERSION_STRING);
			continue;
		}
		if(strncmp(command, ">SET_TEXT:", 10) == 0){
			const char *text = &command[10];
			snprintf(g_raspberry_display_text, sizeof g_raspberry_display_text, text);
			continue;
		}
		if(strncmp(command, ">SET_TEMP_TEXT:", 15) == 0){
			const char *text = &command[15];
			memset(g_temp_display_data + 1, 0, sizeof g_temp_display_data - 1);
			char buf[10] = {0};
			snprintf(buf, 10, "%s", text);
			set_segments(g_display_data, 0, buf);
			set_all_segments(g_temp_display_data, buf);
			g_temp_display_data_timer = 1000;
			continue;
		}
	}

	set_all_segments(g_display_data, g_raspberry_display_text);

	if(g_volume_controls.input_switch != 5){
		g_volume_controls.input_switch = 5;
		send_volume_update();
	}
}

void raspberry_handle_keys()
{
	// Power button
	if(lcd_is_key_pressed(g_current_keys, 22) && !lcd_is_key_pressed(g_previous_keys, 22)){
		g_control_mode = CM_POWER_OFF;
		g_saveables_dirty = true;
		power_off();

		if(digitalRead(PIN_IGNITION_INPUT)){
			g_manual_power_state = true;
		}

		Serial.print(F("<MODE:"));
		Serial.println(CONTROL_MODES[g_control_mode].name);
	}
	for(uint8_t i=0; i<30; i++){
		if(i == 22 || i == 28)
			continue;
		if(lcd_is_key_pressed(g_current_keys, i) && !lcd_is_key_pressed(g_previous_keys, i)){
			Serial.print(F("<KEY_PRESS:"));
			Serial.println(i);
		}
		if(!lcd_is_key_pressed(g_current_keys, i) && lcd_is_key_pressed(g_previous_keys, i)){
			Serial.print(F("<KEY_RELEASE:"));
			Serial.println(i);
		}
	}
}

void init_io()
{
	Serial.begin(9600);

	pinMode(PIN_LED, OUTPUT);

	pinMode(PIN_MAIN_POWER_CONTROL, OUTPUT);
	pinMode(PIN_LCD_CE, OUTPUT);
	pinMode(PIN_LCD_CL, OUTPUT);
	pinMode(PIN_LCD_DI, OUTPUT);
	pinMode(PIN_LCD_DO, INPUT);
	digitalWrite(PIN_LCD_CL, HIGH); // Stop LCD_CL at high level

	pinMode(PIN_ENCODER1, INPUT);
	pinMode(PIN_ENCODER2, INPUT);

	pinMode(PIN_VOL_CE, OUTPUT);
	pinMode(PIN_VOL_DI, OUTPUT);
	pinMode(PIN_VOL_CL, OUTPUT);

	pinMode(PIN_STANDBY_DISABLE, OUTPUT);
	pinMode(PIN_RASPBERRY_POWER_OFF, OUTPUT);

	pinMode(PIN_IGNITION_INPUT, INPUT);
}

// Bits are sent LSB first
void lcd_send_byte(uint8_t b)
{
	for(uint8_t i=0; i<8; i++){
		digitalWrite(PIN_LCD_DI, b & (1<<i) ? HIGH : LOW);
		digitalWrite(PIN_LCD_CL, LOW);
		//_delay_us(3);
		digitalWrite(PIN_LCD_CL, HIGH);
		//_delay_us(3);
	}
}

// data: 21 bytes:
//    0... 5: 44 bits (D1...D44); 4 bits unused
//    6...10: 40 bits (D45...D84)
//   11...15: 40 bits (D85...D124)
//   16...21: 40 bits (D125...D164)
void lcd_send_display(uint8_t control, uint8_t *data)
{
	lcd_send_byte(0x42);
	digitalWrite(PIN_LCD_CE, HIGH);
	//_delay_us(3);
	lcd_send_byte(data[0]);
	lcd_send_byte(data[1]);
	lcd_send_byte(data[2]);
	lcd_send_byte(data[3]);
	lcd_send_byte(data[4]);
	lcd_send_byte((data[5] & 0x0f) | ((control & 0x03) << 6));
	lcd_send_byte((0x00          ) | ((control & 0xfc) >> 2));
	digitalWrite(PIN_LCD_CE, LOW);
	//_delay_us(10);

	lcd_send_byte(0x42);
	digitalWrite(PIN_LCD_CE, HIGH);
	//_delay_us(3);
	lcd_send_byte(data[6]);
	lcd_send_byte(data[7]);
	lcd_send_byte(data[8]);
	lcd_send_byte(data[9]);
	lcd_send_byte(data[10]);
	lcd_send_byte(0x00);
	lcd_send_byte(0x80);
	digitalWrite(PIN_LCD_CE, LOW);
	//_delay_us(10);

	lcd_send_byte(0x42);
	digitalWrite(PIN_LCD_CE, HIGH);
	//_delay_us(3);
	lcd_send_byte(data[11]);
	lcd_send_byte(data[12]);
	lcd_send_byte(data[13]);
	lcd_send_byte(data[14]);
	lcd_send_byte(data[15]);
	lcd_send_byte(0x00);
	lcd_send_byte(0x40);
	digitalWrite(PIN_LCD_CE, LOW);
	//_delay_us(10);

	lcd_send_byte(0x42);
	digitalWrite(PIN_LCD_CE, HIGH);
	//_delay_us(3);
	lcd_send_byte(data[16]);
	lcd_send_byte(data[17]);
	lcd_send_byte(data[18]);
	lcd_send_byte(data[19]);
	lcd_send_byte(data[20]);
	lcd_send_byte(0x00);
	lcd_send_byte(0xc0);
	digitalWrite(PIN_LCD_CE, LOW);
	//_delay_us(10);
}

// Bits are received LSB first
uint8_t lcd_receive_byte()
{
	uint8_t b = 0;
	for(uint8_t i=0; i<8; i++){
		digitalWrite(PIN_LCD_CL, LOW);
		//_delay_us(3);
		digitalWrite(PIN_LCD_CL, HIGH);
		if(digitalRead(PIN_LCD_DO))
			b |= (1<<i);
		//_delay_us(3);
	}
	return b;
}

// data: 4 bytes (NOTE: 30 keys + SA="sleep acknowledge data")
// returns: true: read, false: not read
bool lcd_receive_frame(uint8_t *data)
{
	// If DO is not low, reading is forbidden
	if(digitalRead(PIN_LCD_DO) == HIGH)
		return false;

	lcd_send_byte(0x43);

	digitalWrite(PIN_LCD_CE, HIGH);

	//_delay_us(3);

	for(uint8_t i=0; i<4; i++){
		data[i] = lcd_receive_byte();
	}

	digitalWrite(PIN_LCD_CE, LOW);

	return true;
}

bool lcd_can_receive_frame()
{
	return (digitalRead(PIN_LCD_DO) == LOW);
}

bool lcd_is_key_pressed(const uint8_t *data, uint8_t key)
{
	return (data[key/8] & (1<<(key&7)));
}

// Bits are sent LSB first
void vol_send_byte(uint8_t b)
{
	for(uint8_t i=0; i<8; i++){
		digitalWrite(PIN_VOL_DI, b & (1<<i) ? HIGH : LOW);
		_delay_us(1);
		digitalWrite(PIN_VOL_CL, HIGH);
		_delay_us(1);
		digitalWrite(PIN_VOL_CL, LOW);
	}
}
void vol_send_halfbyte(uint8_t b)
{
	for(uint8_t i=0; i<4; i++){
		digitalWrite(PIN_VOL_DI, b & (1<<i) ? HIGH : LOW);
		_delay_us(1);
		digitalWrite(PIN_VOL_CL, HIGH);
		_delay_us(1);
		digitalWrite(PIN_VOL_CL, LOW);
	}
}

// data: 6 bytes (last 4 bits unused)
void vol_send_data(uint8_t *data)
{
	vol_send_byte(0x81);
	_delay_us(1);
	digitalWrite(PIN_VOL_CE, HIGH);
	_delay_us(1);
	vol_send_byte(data[0]);
	vol_send_byte(data[1]);
	vol_send_byte(data[2]);
	vol_send_byte(data[3]);
	vol_send_byte(data[4]);
	vol_send_halfbyte(data[5]);
	digitalWrite(PIN_VOL_CE, LOW);
	_delay_us(1);
}

// -7...7 -> 0=neutral, 1...7=boost, 9...15=cut
uint8_t map_basstreble(int8_t v)
{
	if(v < -7) return 15;
	if(v < 0) return 8 - v;
	if(v > 7) return 7;
	return v;
}

uint8_t map_volume(uint8_t volume)
{
	if(volume == 0)
		return 0;
	uint8_t result = 9;
	for(uint8_t i=1; i<volume; i++){
		do {
			result++;
		} while(((result + 3) & 0x07) < 4);
		if(result > 164)
			return 164;
	}
	return result;
}

void send_volume_update()
{
	const VolumeControls &vc = g_volume_controls;
	uint8_t data[] = {
		0x00 | ((vc.fader & 0x0f) << 0) | ((vc.super_bass & 0x0f) << 4),
		// 0=neutral, 1...7=boost, 9...15=cut
		0x00 | ((map_basstreble(vc.bass) & 0x0f) << 0) |
				((map_basstreble(vc.treble) & 0x0f) << 4),
		map_volume(vc.volume), // Only weirdly selected values are allowed
		0x00 | ((vc.input_switch & 0x03) << 4) | ((vc.output_gain & 0x01) << 6),
		0x00 | ((vc.input_switch & 0x04) >> 2) | ((vc.channel_sel & 0x03) << 1) | ((vc.mute_switch & 0x01) << 3) | ((vc.output_gain & 0x02) << 6),
		0x00, // 4 test mode bits and 4 dummy bits
	};
	vol_send_data(data);
}

void handle_encoder_value(int8_t rot)
{
	if(g_config_menu_show_timer == 0){
		g_volume_controls.volume += rot;
		if(g_volume_controls.volume > 250)
			g_volume_controls.volume = 0;
		else if(g_volume_controls.volume > 80)
			g_volume_controls.volume = 80;
		send_volume_update();
		g_saveables_dirty = true;

		memset(g_temp_display_data + 1, 0, sizeof g_temp_display_data - 1);
		char buf[10] = {0};
		snprintf(buf, 10, "VOL %i    ", g_volume_controls.volume);
		set_segments(g_display_data, 0, buf);
		set_all_segments(g_temp_display_data, buf);
		g_temp_display_data_timer = 1000;
	} else if(g_config_option == CO_BASS){
		static int8_t a = 0;
		a += rot;
		if(a / 4 != 0){
			g_volume_controls.bass += a / 4;
			if(g_volume_controls.bass > 7)
				g_volume_controls.bass = 7;
			else if(g_volume_controls.bass < -7)
				g_volume_controls.bass = -7;
			send_volume_update();
			g_saveables_dirty = true;
			a = 0;
			g_config_menu_show_timer = CONFIG_MENU_TIMER_RESET_VALUE;
		}
		memset(g_temp_display_data + 1, 0, sizeof g_temp_display_data - 1);
		char buf[10] = {0};
		snprintf(buf, 10, "BASS %i    ", g_volume_controls.bass);
		set_segments(g_display_data, 0, buf);
		set_all_segments(g_temp_display_data, buf);
		g_temp_display_data_timer = 1;
	} else if(g_config_option == CO_TREBLE){
		static int8_t a = 0;
		a += rot;
		if(a / 4 != 0){
			g_volume_controls.treble += a / 4;
			if(g_volume_controls.treble > 7)
				g_volume_controls.treble = 7;
			else if(g_volume_controls.treble < -7)
				g_volume_controls.treble = -7;
			send_volume_update();
			g_saveables_dirty = true;
			a = 0;
			g_config_menu_show_timer = CONFIG_MENU_TIMER_RESET_VALUE;
		}
		memset(g_temp_display_data + 1, 0, sizeof g_temp_display_data - 1);
		char buf[10] = {0};
		snprintf(buf, 10, "TREB %i    ", g_volume_controls.treble);
		set_segments(g_display_data, 0, buf);
		set_all_segments(g_temp_display_data, buf);
		g_temp_display_data_timer = 1;
	} else if(g_config_option == CO_BENIS){
		static int8_t a = 0;
		a += rot;
		if(a <= -4 || a >= 4){
			g_benis_mode_enabled = !g_benis_mode_enabled;
			a = 0;
			g_config_menu_show_timer = CONFIG_MENU_TIMER_RESET_VALUE;
		}
		memset(g_temp_display_data + 1, 0, sizeof g_temp_display_data - 1);
		char buf[10] = {0};
		snprintf(buf, 10, "BENIS %i", g_benis_mode_enabled);
		set_segments(g_display_data, 0, buf);
		set_all_segments(g_temp_display_data, buf);
		g_temp_display_data_timer = 1;
	} else if(g_config_option == CO_PI_CYCLE){
		if(rot != 1)
			g_config_menu_show_timer = CONFIG_MENU_TIMER_RESET_VALUE;
		static int8_t a = 0;
		a += rot;
		if(a < 0){
			a = 0;
		} else if(a >= 100){
			a = 0;
			set_all_segments(g_temp_display_data, "PI OFF");
			lcd_send_display(0x24 | (false ? 0x07 : 0), g_temp_display_data);
			digitalWrite(PIN_RASPBERRY_POWER_OFF, HIGH);
			delay(2000);
			set_all_segments(g_temp_display_data, "PI ON");
			lcd_send_display(0x24 | (false ? 0x07 : 0), g_temp_display_data);
			digitalWrite(PIN_RASPBERRY_POWER_OFF, LOW);
			delay(2000);
			snprintf(g_raspberry_display_text, sizeof g_raspberry_display_text,
					"RASPBERR");
		}
		memset(g_temp_display_data + 1, 0, sizeof g_temp_display_data - 1);
		char buf[10] = {0};
		if(a < 10){
			snprintf(buf, 10, "PI CYCLE");
		} else {
			snprintf(buf, 10, "P C %i%", a);
		}
		set_segments(g_display_data, 0, buf);
		set_all_segments(g_temp_display_data, buf);
		g_temp_display_data_timer = 1;
	}
}

void handle_encoder()
{
	bool e1 = digitalRead(PIN_ENCODER1);
	bool e2 = digitalRead(PIN_ENCODER2);

	int8_t rot = 0;

	if(g_encoder_last_state != 0xff){
		bool le1 = (g_encoder_last_state & 1) ? true : false;
		bool le2 = (g_encoder_last_state & 2) ? true : false;

		if(e1 != le1 || e2 != le2){
			/*Serial.print("E1: ");
			Serial.print(le1);
			Serial.print(" -> ");
			Serial.print(e1);
			Serial.print("  E2: ");
			Serial.print(le2);
			Serial.print(" -> ");
			Serial.print(e2);
			Serial.println();*/

			if(!le1 && !le2){
				if(e1 && !e2){
					rot++;
				} else if(!e1 && e2){
					rot--;
				} else {
					Serial.println(F("<ENCODER DESYNC 1"));
				}
			} else if(le1 && !le2){
				if(e1 && e2){
					rot++;
				} else if(!e1 && !e2){
					rot--;
				} else {
					Serial.println(F("<ENCODER DESYNC 2"));
				}
			} else if(le1 && le2){
				if(!e1 && e2){
					rot++;
				} else if(e1 && !e2){
					rot--;
				} else {
					Serial.println(F("<ENCODER DESYNC 3"));
				}
			} else if(!le1 && le2){
				if(!e1 && !e2){
					rot++;
				} else if(e1 && e2){
					rot--;
				} else {
					Serial.println(F("<ENCODER DESYNC 4"));
				}
			}
		}
	}

	if(rot != 0){
		/*g_debug_test_segment_i += rot;
		Serial.println(g_debug_test_segment_i);
		uint8_t a = g_debug_test_segment_i;
		memset(g_display_data, 0, sizeof g_display_data);
		g_display_data[a/8] = 1 << (a % 8);*/

		handle_encoder_value(rot);
	}


	g_encoder_last_state = (e1 ? 1 : 0) | (e2 ? 2 : 0);
}

void mode_update()
{
	if(CONTROL_MODES[g_control_mode].update)
		(*CONTROL_MODES[g_control_mode].update)();
}

void handle_config_item_switch()
{
	if(g_config_option < CO_NUM_OPTIONS - 1)
		g_config_option = (ConfigOption)(g_config_option + 1);
	else
		g_config_option = (ConfigOption)0;
	g_config_menu_show_timer = CONFIG_MENU_TIMER_RESET_VALUE;
}

void handle_config_button()
{
	static uint8_t press_timer = 0;
	if(g_config_menu_show_timer == 0){
		if(lcd_is_key_pressed(g_current_keys, 28)){
			press_timer++;
			if(press_timer >= 40){
				g_config_menu_show_timer = CONFIG_MENU_TIMER_RESET_VALUE;
				press_timer = 0;
			}
		} else {
			press_timer = 0;
		}
	} else {
		if(lcd_is_key_pressed(g_current_keys, 28) && !lcd_is_key_pressed(g_previous_keys, 28)){
			handle_config_item_switch();
		}
	}
}

void handle_keys()
{
	handle_config_button();

	if(CONTROL_MODES[g_control_mode].handle_keys)
		(*CONTROL_MODES[g_control_mode].handle_keys)();
}

// Returns true if special stuff is shown
// Sets special stuff into g_temp_display_data and sets
// g_temp_display_data_timer to 1 in order for it to be shown during the next
// update
void display_special_stuff()
{
	if(!g_amplifier_power_on && g_amplifier_real_power_off_delay > 0){
		memset(g_temp_display_data + 1, 0, sizeof g_temp_display_data - 1);
		set_all_segments(g_temp_display_data, "--------");
		g_temp_display_data_timer = 1;
		return;
	}
	if(g_config_menu_show_timer > 0){
		if(g_config_option == CO_BASS){
			handle_encoder_value(0);
			return;
		}
		if(g_config_option == CO_TREBLE){
			handle_encoder_value(0);
			return;
		}
		if(g_config_option == CO_BENIS){
			handle_encoder_value(0);
			return;
		}
		if(g_config_option == CO_PI_CYCLE){
			handle_encoder_value(0);
			return;
		}
	}
	if(g_benis_mode_enabled && g_temp_display_data_timer == 0){
		memset(g_temp_display_data + 1, 0, sizeof g_temp_display_data - 1);
		set_all_segments(g_temp_display_data, " BENIS ");
		g_temp_display_data_timer = 1;
		return;
	}
}

void save_everything()
{
	Serial.println(F("<SAVING\r\n"));
	g_saveables_dirty = false;

	uint8_t *wa = 0;
	// Identification byte
	eeprom_write_byte(wa++, 251);
	// Version byte
	eeprom_write_byte(wa++, 1);
	// Parameters
	eeprom_write_byte(wa++, g_control_mode);
	eeprom_write_byte(wa++, g_volume_controls.volume);
	eeprom_write_byte(wa++, g_volume_controls.bass);
	eeprom_write_byte(wa++, g_volume_controls.treble);
}

void load_everything()
{
	Serial.println(F("<LOADING\r\n"));

	uint8_t *ra = 0;
	// Identification byte
	uint8_t identification = eeprom_read_byte(ra++);
	if(identification != 251){
		Serial.println("<DEBUG:Wrong data identification in EEPROM");
		return;
	}
	g_saveables_dirty = false;
	// Version byte
	uint8_t version = eeprom_read_byte(ra++);
	// Parameters
	g_control_mode = (ControlMode)eeprom_read_byte(ra++);
	g_volume_controls.volume = eeprom_read_byte(ra++);
	g_volume_controls.bass = eeprom_read_byte(ra++);
	g_volume_controls.treble = eeprom_read_byte(ra++);
}

void save_everything_with_rate_limit(uint32_t rate_limit_ms)
{
	static uint32_t last_ms = 0;
	if(last_ms < millis() - rate_limit_ms || last_ms > millis()){
		last_ms = millis();
		save_everything();
	}
}

void power_off()
{
	// Standby amplifier
	digitalWrite(PIN_STANDBY_DISABLE, LOW);

	g_lcd_do_sleep = true;

	g_raspberry_power_on = false;
	g_raspberry_power_off_warning_delay = RASPBERRY_POWER_OFF_WARNING_DELAY;
	g_raspberry_real_power_off_delay = RASPBERRY_REAL_POWER_OFF_DELAY;

	g_amplifier_power_on = false;
	g_amplifier_real_power_off_delay = 2;

	save_everything_with_rate_limit(5000);
}

void power_on()
{
	// Power up raspberry pi
	digitalWrite(PIN_RASPBERRY_POWER_OFF, LOW);
	g_raspberry_power_on = true;

	g_lcd_do_sleep = false;

	// Power up main amplifier board supply
	digitalWrite(PIN_MAIN_POWER_CONTROL, HIGH);

	mode_update();

	// Wait for a bit so that the volume controller is ready to receive data and
	// then update it before doing anything else
	_delay_ms(10);
	send_volume_update();

	// Disable amplifier standby after writing all that prior stuff
	digitalWrite(PIN_STANDBY_DISABLE, HIGH);
	g_amplifier_power_on = true;
}

void setup()
{
	init_io();

	load_everything();

	if(g_control_mode != CM_POWER_OFF){
		power_on();
	} else {
		power_off();
	}
}

void loop()
{
	if(!g_manual_power_state){
		if(digitalRead(PIN_IGNITION_INPUT) && !g_amplifier_power_on){
			power_on();
		}
		else if(!digitalRead(PIN_IGNITION_INPUT) && g_amplifier_power_on){
			power_off();
		}
	}

	if(g_second_counter_timestamp < millis() - 1000 || g_second_counter_timestamp > millis()){
		g_second_counter_timestamp = millis();

		if(!g_raspberry_power_on && g_raspberry_power_off_warning_delay > 0){
			g_raspberry_power_off_warning_delay--;
			if(g_raspberry_power_off_warning_delay == 0){
				// This allows settings to be saved
				Serial.println("<POWERDOWN_WARNING");
			}
		}

		if(!g_raspberry_power_on && g_raspberry_real_power_off_delay > 0){
			g_raspberry_real_power_off_delay--;
			if(g_raspberry_real_power_off_delay == 0){
				// Power down raspberry pi
				digitalWrite(PIN_RASPBERRY_POWER_OFF, HIGH);
			}
		}

		if(!g_amplifier_power_on && g_amplifier_real_power_off_delay > 0){
			g_amplifier_real_power_off_delay--;
			if(g_amplifier_real_power_off_delay == 0){
				// Power down main amplifier board supply
				digitalWrite(PIN_MAIN_POWER_CONTROL, LOW);
			}
		}
	}
	if(g_millisecond_counter_timestamp > millis()){
		// Overflow; just reset
		g_millisecond_counter_timestamp = millis();
	} else {
		uint32_t dt_ms = millis() - g_millisecond_counter_timestamp;
		g_millisecond_counter_timestamp = millis();

		if(g_boot_message_delay != 0){
			if(g_boot_message_delay > dt_ms){
				g_boot_message_delay -= dt_ms;
			} else {
				g_boot_message_delay = 0;
				Serial.println(F("<BOOT"));
			}
		}

		if(g_temp_display_data_timer > dt_ms)
			g_temp_display_data_timer -= dt_ms;
		else
			g_temp_display_data_timer = 0;

		if(g_config_menu_show_timer > dt_ms)
			g_config_menu_show_timer -= dt_ms;
		else
			g_config_menu_show_timer = 0;
	}

	handle_encoder();

	if(lcd_can_receive_frame()){
		digitalWrite(PIN_LED, HIGH);
		memcpy(g_previous_keys, g_current_keys, sizeof g_previous_keys);
		lcd_receive_frame(g_current_keys);
		handle_keys();
		digitalWrite(PIN_LED, LOW);
		g_last_keys_timestamp = millis();
	} else if(g_last_keys_timestamp < millis() - 50){
		memcpy(g_previous_keys, g_current_keys, sizeof g_previous_keys);
		memset(g_current_keys, 0, sizeof g_current_keys);
		handle_keys();
		g_last_keys_timestamp = millis();
	}

	mode_update();

	display_special_stuff();

	if(g_temp_display_data_timer > 0)
		lcd_send_display(0x24 | (false ? 0x07 : 0), g_temp_display_data);
	else
		lcd_send_display(0x24 | (g_lcd_do_sleep ? 0x07 : 0), g_display_data);

	if(g_saveables_dirty){
		save_everything_with_rate_limit(60000);
	}
}


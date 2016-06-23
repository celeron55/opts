#include "lcd.h"
#include "command_accumulator.h"

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

uint8_t g_encoder_last_state = 0xff;

uint8_t g_previous_keys[4] = {0, 0, 0, 0};
uint8_t g_current_keys[4] = {0, 0, 0, 0};
// Timestamp is needed because no-keys-pressed is not received
uint32_t g_last_keys_timestamp = 0;

enum ControlMode {
	CM_POWER_OFF,
	CM_AUX,
	CM_INTERNAL,
} g_control_mode = CM_AUX;

struct VolumeControls {
	uint8_t fader = 15; // 0...15 (-infdB...0dB)
	uint8_t super_bass = 0; // 0...10
	uint8_t bass = 0; // 0=neutral, 1...7=boost, 9...15=cut
	uint8_t treble = 0; // 0=neutral, 1...7=boost, 9...15=cut
	uint8_t volume = 70;
	// NOTE: in2=5=CD, in5=0=AUX
	uint8_t input_switch = 5; // valid values: in1=4, in2=5, in3=6, in4=7, in5=0
	uint8_t mute_switch = 0; // 0, 1
	uint8_t channel_sel = 3; // 0=initial, 1=L, 2=R, 3=both
	uint8_t output_gain = 2; // 0=0dB, 1=0dB, 2=+6.5dB, 3=+8.5dB
};
VolumeControls g_volume_controls;

//uint8_t g_debug_test_segment_i = 0;

bool g_lcd_do_sleep = false;

CommandAccumulator<50> command_accumulator;

struct Mode {
	void (*update)();
	void (*handle_keys)();
	void (*handle_encoder)(int8_t rot);
};

void power_off_update();
void power_off_handle_keys();
Mode g_mode_power_off = {
	power_off_update,
	power_off_handle_keys,
	NULL,
};

void aux_update();
void aux_handle_keys();
void aux_handle_encoder(int8_t rot);
Mode g_mode_aux = {
	aux_update,
	aux_handle_keys,
	aux_handle_encoder,
};

void internal_update();
void internal_handle_keys();
void internal_handle_encoder(int8_t rot);
Mode g_mode_internal = {
	internal_update,
	internal_handle_keys,
	internal_handle_encoder,
};
char g_internal_display_text[9] = "INTERNAL";

Mode *g_current_mode = &g_mode_internal;

void power_off_update()
{
	set_segments(0, "POWER OFF");
}

void power_off_handle_keys()
{
	// Power button
	if(lcd_is_key_pressed(g_current_keys, 22) && !lcd_is_key_pressed(g_previous_keys, 22)){
		power_on();
		g_current_mode = &g_mode_aux;
	}
}

void aux_update()
{
	char buf[10] = {0};
	snprintf(buf, 10, "AUX %i    ", g_volume_controls.volume);
	set_segments(0, buf);

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
		g_current_mode = &g_mode_internal;
	}
}

void aux_handle_encoder(int8_t rot)
{
	g_volume_controls.volume += rot;
	if(g_volume_controls.volume > 80)
		g_volume_controls.volume = 80;
	send_volume_update();
}

void internal_update()
{
	while(command_accumulator.read(Serial)){
		const char *command = command_accumulator.command();
		if(strncmp(command, ">SET_TEXT:", 10) == 0){
			const char *text = &command[10];
			snprintf(g_internal_display_text, sizeof g_internal_display_text, text);
			continue;
		}
	}

	set_all_segments(g_internal_display_text);

	if(g_volume_controls.input_switch != 5){
		g_volume_controls.input_switch = 5;
		send_volume_update();
	}
}

void internal_handle_keys()
{
	// Power button
	if(lcd_is_key_pressed(g_current_keys, 22) && !lcd_is_key_pressed(g_previous_keys, 22)){
		power_off();
		g_current_mode = &g_mode_power_off;
	}
	for(uint8_t i=0; i<30; i++){
		if(i == 22)
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

void internal_handle_encoder(int8_t rot)
{
	Serial.print(F("<ENC:"));
	Serial.println(rot);
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
		0x00 | ((vc.bass & 0x0f) << 0) | ((vc.treble & 0x0f) << 4),
		map_volume(vc.volume), // Only weirdly selected values are allowed
		0x00 | ((vc.input_switch & 0x03) << 4) | ((vc.output_gain & 0x01) << 6),
		0x00 | ((vc.input_switch & 0x04) >> 2) | ((vc.channel_sel & 0x03) << 1) | ((vc.mute_switch & 0x01) << 3) | ((vc.output_gain & 0x02) << 6),
		0x00, // 4 test mode bits and 4 dummy bits
	};
	vol_send_data(data);
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
	
		if(g_current_mode && g_current_mode->handle_encoder)
			(*g_current_mode->handle_encoder)(rot);
	}


	g_encoder_last_state = (e1 ? 1 : 0) | (e2 ? 2 : 0);
}

void mode_update()
{
	if(g_current_mode && g_current_mode->update)
		(*g_current_mode->update)();
}

void mode_handle_keys(uint8_t *keys)
{
	if(g_current_mode && g_current_mode->handle_keys)
		(*g_current_mode->handle_keys)();
}

void power_off()
{
	digitalWrite(PIN_MAIN_POWER_CONTROL, LOW);
	digitalWrite(PIN_STANDBY_DISABLE, LOW);
	g_lcd_do_sleep = true;
}

void power_on()
{
	g_lcd_do_sleep = false;

	digitalWrite(PIN_MAIN_POWER_CONTROL, HIGH);

	mode_update();

	// Wait for a bit so that the volume controller is ready to receive data and
	// then update it before doing anything else
	_delay_ms(10);
	send_volume_update();

	// Disable amplifier standby after writing all that prior stuff
	digitalWrite(PIN_STANDBY_DISABLE, HIGH);
}

void setup()
{
	init_io();

	//digitalWrite(PIN_LED, HIGH);

	power_on();
}

void loop()
{
	handle_encoder();

	if(lcd_can_receive_frame()){
		digitalWrite(PIN_LED, HIGH);
		memcpy(g_previous_keys, g_current_keys, sizeof g_previous_keys);
		lcd_receive_frame(g_current_keys);
		mode_handle_keys(g_current_keys);
		digitalWrite(PIN_LED, LOW);
		g_last_keys_timestamp = millis();
	} else if(g_last_keys_timestamp < millis() - 100){
		memcpy(g_previous_keys, g_current_keys, sizeof g_previous_keys);
		memset(g_current_keys, 0, sizeof g_current_keys);
		mode_handle_keys(g_current_keys);
		g_last_keys_timestamp = millis();
	}

	mode_update();

	lcd_send_display(0x24 | (g_lcd_do_sleep ? 0x07 : 0), g_display_data);
}


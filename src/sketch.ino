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

//uint8_t g_debug_test_segment_i = 0;

uint8_t g_display_data[] = {
	0x11, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,
};

// 14 segment digits, total of 8
const uint8_t SEG_I_14[] PROGMEM = {
//    1   2   3   4   5   6   7   8   9  10  11  12  13  14 (sequential)
//   12  13  14   3   2   1   5  10   4   8   9   6   7  11 (as found on LCD)
	 36, 37, 38, 26, 25, 24, 29, 34, 28, 32, 33, 30, 31, 35,
	 56, 57, 58, 42, 41, 40, 49, 54, 48, 52, 53, 50, 51, 55,
	 72, 73, 74, 62, 61, 60, 65, 70, 64, 68, 69, 66, 67, 71,
	 88, 89, 90, 78, 77, 76, 81, 86, 80, 84, 85, 82, 83, 87,
	108,109,110, 98, 97, 96,101,106,100,104,105,102,103,107,
	124,125,126,114,113,112,117,122,116,120,121,118,119,123,
	144,145,146,134,133,132,137,142,136,140,141,138,139,143,
	160,161,162,150,149,148,153,158,152,156,157,154,155,159,
};

//   11111111111111
// 32           1  2
// 32  2   5   0   2
// 32   5  1  2    2
// 32    6 2 4     2
//     64      128
// 16    2 4 8     4
// 16   0  0  1    4
// 16  4   9   9   4
// 16 8    6    2  4
//   88888888888888   16384

const uint16_t CHAR_MAP_14SEG[] PROGMEM = {
	0x1,    //
	0x2,    //
	0x4,    //
	0x8,    //
	0x10,   //
	0x20,   //
	0x40,   //
	0x80,   //
	0x100,  //
	0x200,  //
	0x400,  //
	0x800,  //
	0x1000, //
	0x2000, //
	0x4000, //
	0x8000, //
	0x0,    //
	0x0,    //
	0x0,    //
	0x0,    //
	0x0,    //
	0x0,    //
	0x0,    //
	0x0,    //
	0x12c9, //
	0x15c0, //
	0x12f9, //
	0xe3,   //
	0x530,  //
	0x12c8, //
	0x3a00, //
	0x1700, //
	0x0,    //
	0x6,    // !
	0x220,  // "
	0x12ce, // //
	0x12ed, // $
	0xc24,  // %
	0x235d, // &
	0x400,  // '
	0x2400, // (
	0x900,  // )
	0x3fc0, // *
	0x12c0, // +
	0x800,  // ,
	0xc0,   // -
	0x0,    // .
	0xc00,  // /
	0xc3f,  // 0
	0x6,    // 1
	0xdb,   // 2
	0x8f,   // 3
	0xe6,   // 4
	0x2069, // 5
	0xfd,   // 6
	0x7,    // 7
	0xff,   // 8
	0xef,   // 9
	0x1200, // :
	0xa00,  // ;
	0x2400, // <
	0xc8,   // =
	0x900,  // >
	0x1083, // ?
	0x2bb,  // @
	0xf7,   // A
	0x128f, // B
	0x39,   // C
	0x120f, // D
	0xf9,   // E
	0x71,   // F
	0xbd,   // G
	0xf6,   // H
	0x1200, // I
	0x1e,   // J
	0x2470, // K
	0x38,   // L
	0x536,  // M
	0x2136, // N
	0x3f,   // O
	0xf3,   // P
	0x203f, // Q
	0x20f3, // R
	0xed,   // S
	0x1201, // T
	0x3e,   // U
	0xc30,  // V
	0x2836, // W
	0x2d00, // X
	0x1500, // Y
	0xc09,  // Z
	0x39,   // [
	0x2100, //
	0xf,    // ]
	0xc03,  // ^
	0x8,    // _
	0x100,  // `
	0x1058, // a
	0x2078, // b
	0xd8,   // c
	0x88e,  // d
	0x858,  // e
	0x71,   // f
	0x48e,  // g
	0x1070, // h
	0x1000, // i
	0xe,    // j
	0x3600, // k
	0x30,   // l
	0x10d4, // m
	0x1050, // n
	0xdc,   // o
	0x170,  // p
	0x486,  // q
	0x50,   // r
	0x2088, // s
	0x78,   // t
	0x1c,   // u
	0x2004, // v
	0x2814, // w
	0x28c0, // x
	0x200c, // y
	0x848,  // z
	0x949,  // {
	0x1200, // |
	0x2489, // }
	0x520,  // ~
	0x3fff  //
};

void set_segment_char(uint8_t seg_i, char c)
{
	const uint16_t segment_bits = pgm_read_word(&CHAR_MAP_14SEG[c]);
	const uint8_t *seg_indexes = &SEG_I_14[seg_i * 14];
	for(uint8_t i=0; i<14; i++){
		uint8_t seg_i = pgm_read_byte(&seg_indexes[i]);
		if(segment_bits & (1<<i))
			g_display_data[seg_i/8] |= (1 << (seg_i % 8));
		else
			g_display_data[seg_i/8] &= ~(1 << (seg_i % 8));
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
		Serial.print(F("<ENC:"));
		Serial.println(rot);

		/*g_debug_test_segment_i += rot;
		Serial.println(g_debug_test_segment_i);
		uint8_t a = g_debug_test_segment_i;
		memset(g_display_data, 0, sizeof g_display_data);
		g_display_data[a/8] = 1 << (a % 8);*/
	}


	g_encoder_last_state = (e1 ? 1 : 0) | (e2 ? 2 : 0);
}

void setup()
{
	init_io();

	//digitalWrite(PIN_LED, HIGH);

	digitalWrite(PIN_MAIN_POWER_CONTROL, HIGH);

	set_segment_char(0, 'A');
	set_segment_char(1, 'B');
	set_segment_char(2, 'C');
	set_segment_char(3, 'D');
	set_segment_char(4, 'E');
	set_segment_char(5, 'F');
	set_segment_char(6, 'G');
	set_segment_char(7, 'H');
}

void loop()
{
	handle_encoder();

	if(lcd_can_receive_frame()){
		digitalWrite(PIN_LED, HIGH);

		uint8_t data[4];
		lcd_receive_frame(data);

		for(uint8_t i=0; i<30; i++){
			if(data[i/8] & (1<<(i&7))){
				Serial.print(F("<KEY:"));
				Serial.println(i);
			}
		}

		digitalWrite(PIN_LED, LOW);
	}

	{
		lcd_send_display(0x24, g_display_data);
	}

	static uint8_t aa = 255;
	aa++;
	if(aa == 0){
		uint8_t fader = 15; // 0...15 (-infdB...0dB)
		uint8_t super_bass = 0; // 0...10
		uint8_t bass = 0; // 0=neutral, 1...7=boost, 9...15=cut
		uint8_t treble = 0; // 0=neutral, 1...7=boost, 9...15=cut
		uint8_t volume = 130; // max. 164
		uint8_t input_switch = 0; // valid values: in1=4, in2=5, in3=6, in4=7, in5=0
		uint8_t mute_switch = 0; // 0, 1
		uint8_t channel_sel = 3; // 0=initial, 1=L, 2=R, 3=both
		uint8_t output_gain = 0; // 0=0dB, 1=0dB, 2=+6.5dB, 3=+8.5dB
		uint8_t data[] = {
			0x00 | ((fader & 0x0f) << 0) | ((super_bass & 0x0f) << 4),
			0x00 | ((bass & 0x0f) << 0) | ((treble & 0x0f) << 4),
			volume,
			0x00 | ((input_switch & 0x03) << 4) | ((output_gain & 0x01) << 6),
			0x00 | ((input_switch & 0x04) >> 2) | ((channel_sel & 0x03) << 1) | ((mute_switch & 0x01) << 3) | ((output_gain & 0x02) << 6),
			0x00, // 4 test mode bits and 4 dummy bits
		};
		vol_send_data(data);
	}

	// Disable standby after writing all that prior stuff
	digitalWrite(PIN_STANDBY_DISABLE, HIGH);

	/*{
		static uint8_t a = 0;
		uint8_t data[] = {
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00,
		};
		data[a/8] = 1 << (a % 8);
		lcd_send_display(0x24, data);
		a++;
		if(a >= 164)
			a = 0;
		_delay_ms(100);
	}*/
}


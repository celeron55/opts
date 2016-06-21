const int PIN_LED = 13;
const int PIN_MAIN_POWER_CONTROL = 4;
const int PIN_LCD_CE = 7;
const int PIN_LCD_CL = 8;
const int PIN_LCD_DI = 9;
const int PIN_LCD_DO = 10;

void init_io()
{
	Serial.begin(9600);

	pinMode(PIN_LED, OUTPUT);

	pinMode(PIN_MAIN_POWER_CONTROL, OUTPUT);
	pinMode(PIN_LCD_CE, OUTPUT);
	pinMode(PIN_LCD_CL, OUTPUT);
	pinMode(PIN_LCD_DI, OUTPUT);
	pinMode(PIN_LCD_DO, INPUT);

	// Stop LCD_CL at high level
	digitalWrite(PIN_LCD_CL, HIGH);
}

// Bits are sent LSB first
void lcd_send_byte(uint8_t b)
{
	for(uint8_t i=0; i<8; i++){
		digitalWrite(PIN_LCD_DI, b & (1<<i) ? HIGH : LOW);
		digitalWrite(PIN_LCD_CL, LOW);
		_delay_us(3);
		digitalWrite(PIN_LCD_CL, HIGH);
		_delay_us(3);
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
	_delay_us(3);
	lcd_send_byte(data[0]);
	lcd_send_byte(data[1]);
	lcd_send_byte(data[2]);
	lcd_send_byte(data[3]);
	lcd_send_byte(data[4]);
	lcd_send_byte((data[5] & 0x0f) | ((control & 0x03) << 6));
	lcd_send_byte((0x00          ) | ((control & 0xfc) >> 2));
	digitalWrite(PIN_LCD_CE, LOW);
	_delay_us(10);

	lcd_send_byte(0x42);
	digitalWrite(PIN_LCD_CE, HIGH);
	_delay_us(3);
	lcd_send_byte(data[6]);
	lcd_send_byte(data[7]);
	lcd_send_byte(data[8]);
	lcd_send_byte(data[9]);
	lcd_send_byte(data[10]);
	lcd_send_byte(0x00);
	lcd_send_byte(0x80);
	digitalWrite(PIN_LCD_CE, LOW);
	_delay_us(10);

	lcd_send_byte(0x42);
	digitalWrite(PIN_LCD_CE, HIGH);
	_delay_us(3);
	lcd_send_byte(data[11]);
	lcd_send_byte(data[12]);
	lcd_send_byte(data[13]);
	lcd_send_byte(data[14]);
	lcd_send_byte(data[15]);
	lcd_send_byte(0x00);
	lcd_send_byte(0x40);
	digitalWrite(PIN_LCD_CE, LOW);
	_delay_us(10);

	lcd_send_byte(0x42);
	digitalWrite(PIN_LCD_CE, HIGH);
	_delay_us(3);
	lcd_send_byte(data[16]);
	lcd_send_byte(data[17]);
	lcd_send_byte(data[18]);
	lcd_send_byte(data[19]);
	lcd_send_byte(data[20]);
	lcd_send_byte(0x00);
	lcd_send_byte(0xc0);
	digitalWrite(PIN_LCD_CE, LOW);
	_delay_us(10);
}

// Bits are received LSB first
uint8_t lcd_receive_byte()
{
	uint8_t b = 0;
	for(uint8_t i=0; i<8; i++){
		digitalWrite(PIN_LCD_CL, LOW);
		_delay_us(3);
		digitalWrite(PIN_LCD_CL, HIGH);
		if(digitalRead(PIN_LCD_DO))
			b |= (1<<i);
		_delay_us(3);
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

	_delay_us(3);

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

void setup()
{
	init_io();

	//digitalWrite(PIN_LED, HIGH);

	digitalWrite(PIN_MAIN_POWER_CONTROL, HIGH);
}

void loop()
{
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
	}

	/*{
		static uint8_t a = 0;
		// First 44 bits are display data
		uint8_t buf[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
		buf[a/8] = 1 << (a % 8);
		lcd_send_frame(buf);
		a++;
		if(a >= 44)
			a = 12;
	}*/

	_delay_ms(100);
}


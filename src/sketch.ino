const int PIN_MAIN_POWER_CONTROL = 4;
const int PIN_LCD_CE = 7;
const int PIN_LCD_CL = 8;
const int PIN_LCD_DI = 9;
const int PIN_LCD_DO = 10;

void init_io()
{
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

// data: 7 bytes
void lcd_send_frame(uint8_t *data)
{
	lcd_send_byte(0x42);

	digitalWrite(PIN_LCD_CE, HIGH);

	_delay_us(3);

	for(uint8_t i=0; i<7; i++){
		lcd_send_byte(data[i]);
	}

	digitalWrite(PIN_LCD_CE, LOW);
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

void setup()
{
	init_io();

	digitalWrite(PIN_MAIN_POWER_CONTROL, HIGH);
}

void loop()
{
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


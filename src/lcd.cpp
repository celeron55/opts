#include "lcd.h"

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

void set_segment_char(uint8_t *data, uint8_t seg_i, char c)
{
	const uint16_t segment_bits = pgm_read_word(&CHAR_MAP_14SEG[c]);
	const uint8_t *seg_indexes = &SEG_I_14[seg_i * 14];
	for(uint8_t i=0; i<14; i++){
		uint8_t seg_i = pgm_read_byte(&seg_indexes[i]);
		if(segment_bits & (1<<i))
			data[seg_i/8] |= (1 << (seg_i % 8));
		else
			data[seg_i/8] &= ~(1 << (seg_i % 8));
	}
}

void set_segments(uint8_t *data, uint8_t i0, const char *text)
{
	const char *p = text;
	for(uint8_t i = i0; i<8; i++){
		if(*p == 0)
			break;
		set_segment_char(data, i, *p);
		p++;
	}
}

void set_all_segments(uint8_t *data, const char *text)
{
	const char *p = text;
	for(uint8_t i = 0; i<8; i++){
		if(*p != 0){
			set_segment_char(data, i, *p);
			p++;
		} else {
			set_segment_char(data, i, ' ');
		}
	}
}


#pragma once
#include "stuff.hpp"
#include "types.hpp"
#include <unistd.h>

extern int arduino_serial_fd;
extern ss_ arduino_serial_debug_mode;

static ss_ truncate(const ss_ &s, size_t len)
{
	if(s.size() < len) return s;
	return s.substr(0, len);
}

static ss_ squeeze(const ss_ &s0, size_t len, size_t startpos=0, size_t try_len=8)
{
	ss_ s = s0;
	for(size_t i=0; i<s.size(); i++)
		s[i] = toupper(s[i]);

	if(s.size() > try_len){
		size_t good_beginning = 0;
		for(size_t i=0; i<s.size(); i++){
			if((s[i] < 'a' || s[i] > 'z') && (s[i] < 'A' || s[i] > 'Z')){
				good_beginning = i;
			} else {
				good_beginning = i;
				break;
			}
		}
		if(good_beginning < s.size() - 2){
			s = s.substr(good_beginning);
		}
	}

	if(startpos >= s.size())
		return "";
	if(startpos + len >= s.size())
		return s.substr(startpos);
	return s.substr(startpos, len);
}

static void arduino_serial_write(const char *data, size_t len)
{
	if(arduino_serial_debug_mode == "raw" && data != NULL && len != 0){
		printf("%s", cs(ss_(data, len)));
	}
	if(arduino_serial_fd != -1){
		size_t r = write(arduino_serial_fd, data, len);
		if(r != len){
			printf("WARNING: Arduino serial didn't take the entire message\n");
			// TODO: Maybe handle this properly
		}
	}
}

static void arduino_set_text(const ss_ &text)
{
	char buf[30];
	int l = snprintf(buf, 30, ">SET_TEXT:%s\r\n", cs(truncate(text, 8)));
	arduino_serial_write(buf, l);

	if(arduino_serial_debug_mode == "fancy"){
		printf("[%s]\n", cs(truncate(text, 8)));
	}
}

static void arduino_set_temp_text(const ss_ &text)
{
	char buf[30];
	int l = snprintf(buf, 30, ">SET_TEMP_TEXT:%s\r\n", cs(truncate(text, 8)));
	arduino_serial_write(buf, l);

	if(arduino_serial_debug_mode == "fancy"){
		printf("[[%s]]\n", cs(truncate(text, 8)));
	}
}


#pragma once
#include "stuff.hpp"
#include "types.hpp"
#include <unistd.h>

extern int arduino_serial_fd;

static ss_ truncate(const ss_ &s, size_t len)
{
	if(s.size() < len) return s;
	return s.substr(0, len);
}

static ss_ squeeze(const ss_ &s0, size_t len, size_t startpos=0)
{
	ss_ s = s0;
	for(size_t i=0; i<s.size(); i++)
		s[i] = toupper(s[i]);

	if(s.size() > len){
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

static void arduino_set_text(const ss_ &text)
{
	char buf[30];
	int l = snprintf(buf, 30, ">SET_TEXT:%s\r\n", cs(truncate(text, 8)));
	write(arduino_serial_fd, buf, l);
}

static void arduino_set_temp_text(const ss_ &text)
{
	char buf[30];
	int l = snprintf(buf, 30, ">SET_TEMP_TEXT:%s\r\n", cs(truncate(text, 8)));
	write(arduino_serial_fd, buf, l);
}


#pragma once
#include "types.hpp"
#include <unistd.h>

extern int arduino_serial_fd;
extern ss_ arduino_serial_debug_mode;
extern int arduino_display_width;

static ss_ truncate(const ss_ &s, size_t len)
{
	if(s.size() < len) return s;
	return s.substr(0, len);
}

static sv_<ss_> split_string_to_clean_ui_pieces(const ss_ &s, size_t piece_len)
{
	sv_<ss_> result;
	size_t start_from = 0;
	for(;;){
		if(start_from >= s.size())
			break;
		size_t start_from_instead = start_from;
		for(size_t i=start_from; i<s.size(); i++){
			if((s[i] < 'a' || s[i] > 'z') && (s[i] < 'A' || s[i] > 'Z') &&
					(s[i] < '0' || s[i] > '9')){
				start_from_instead = i + 1;
				continue;
			}
			break;
		}
		if(start_from_instead >= s.size()){
			result.push_back(s.substr(start_from));
			break;
		}
		if(start_from_instead >= s.size() - piece_len){
			result.push_back(s.substr(start_from_instead));
			break;
		}
		size_t end_at = start_from_instead + piece_len;
		if(s.size() > end_at + 2 && end_at >= 3){
			if(s[end_at-2] == ' ' && s[end_at-1] != ' ' && s[end_at] != ' ' && (s[end_at+1] == ' ' || s[end_at+2] == ' ')){
				end_at = end_at - 1;
			} else if(s[end_at-3] == ' ' && s[end_at-2] != ' ' && s[end_at-1] != ' ' && s[end_at] != ' ' && (s[end_at+1] == ' ' || s[end_at+2] == ' ')){
				end_at = end_at - 2;
			}
		}
		result.push_back(s.substr(start_from_instead, end_at - start_from_instead));
		start_from = end_at;
	}
	return result;
}

static sv_<ss_> toupper(const sv_<ss_> &ss)
{
	sv_<ss_> result = ss;
	for(auto &s : result){
		for(size_t i=0; i<s.size(); i++)
			s[i] = toupper(s[i]);
	}
	return result;
}

static ss_ squeeze(const ss_ &s0, size_t len, size_t startpos=0, size_t try_len=SIZE_MAX)
{
	if(try_len == SIZE_MAX)
		try_len = arduino_display_width;
	ss_ s = s0;
	for(size_t i=0; i<s.size(); i++)
		s[i] = toupper(s[i]);

	if(s.size() > try_len){
		size_t good_beginning = 0;
		for(size_t i=0; i<s.size(); i++){
			if(s[i] == '|'){
				// There was no good beginning in the first part of the name;
				// fall back to just using the entire name
				good_beginning = 0;
				break;
			} else if((s[i] < 'a' || s[i] > 'z') && (s[i] < 'A' || s[i] > 'Z')){
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
		int r = write(arduino_serial_fd, data, len);
		if(r == -1){
			printf("Arduino write error\n");
			arduino_serial_fd = -1;
		} else if((size_t)r != len){
			printf("WARNING: Arduino serial didn't take the entire message\n");
			// TODO: Maybe handle this properly
		}
	}
}

static void arduino_serial_write(const ss_ &data)
{
	arduino_serial_write(data.c_str(), data.size());
}

static void arduino_set_text(const ss_ &text)
{
	char buf[30];
	int l = snprintf(buf, 30, ">SET_TEXT:%s\r\n",
			cs(truncate(text, arduino_display_width)));
	arduino_serial_write(buf, l);

	if(arduino_serial_debug_mode == "fancy"){
		printf("[%s]\n", cs(truncate(text, arduino_display_width)));
	}
}

// NOTE: Don't use if possible; interferes with other displayed things
static void arduino_set_temp_text(const ss_ &text)
{
	char buf[30];
	int l = snprintf(buf, 30, ">SET_TEMP_TEXT:%s\r\n",
			cs(truncate(text, arduino_display_width)));
	arduino_serial_write(buf, l);

	if(arduino_serial_debug_mode == "fancy"){
		printf("[[%s]]\n", cs(truncate(text, arduino_display_width)));
	}
}

static void arduino_request_version()
{
	char buf[30];
	int l = snprintf(buf, 30, ">VERSION\r\n");
	arduino_serial_write(buf, l);
}


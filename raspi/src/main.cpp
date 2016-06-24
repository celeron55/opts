#include "c55_getopt.h"
#include "command_accumulator.hpp"
#include "types.hpp"
#include <mpv/client.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/poll.h>
#include <unistd.h>

ss_ arduino_serial_path;
ss_ test_file_path;

bool do_main_loop = true;
mpv_handle *mpv = NULL;
CommandAccumulator<100> command_accu;

static inline void check_mpv_error(int status)
{
    if (status < 0) {
        printf("mpv API error: %s\n", mpv_error_string(status));
        exit(1);
    }
}

ss_ get_stdin_stuff()
{
	struct pollfd fds;
	int ret;
	fds.fd = 0; // 0=stdin
	fds.events = POLLIN;
	ret = poll(&fds, 1, 0);
	if(ret == 1){
		char buf[1000];
		ssize_t n = read(0, buf, 1000);
		if(n == 0)
			return "";
		return ss_(buf, n);
	} else if(ret == 0){
		return "";
	} else {
		// Error
		return "";
	}
}

void handle_stdin()
{
	ss_ stdin_stuff = get_stdin_stuff();
	for(char c : stdin_stuff){
		if(command_accu.put_char(c)){
			ss_ command = command_accu.command();
			if(command == "next"){
			} else if(command == "prev"){
			} else if(command == "nextalbum"){
			} else if(command == "prevalbum"){
			} else if(command == "pause"){
				check_mpv_error(mpv_command_string(mpv, "pause"));
			} else if(command == "fwd"){
				mpv_command_string(mpv, "seek +30");
			} else if(command == "bwd"){
				mpv_command_string(mpv, "seek -30");
			} else if(command == "pos"){
				double pos = 0;
				mpv_get_property(mpv, "time-pos", MPV_FORMAT_DOUBLE, &pos);
				printf("pos: %f\n", pos);
			} else if(command == "test"){
				const char *cmd[] = {"loadfile", test_file_path.c_str(), NULL};
				check_mpv_error(mpv_command(mpv, cmd));
			} else {
				printf("Invalid command: \"%s\"", cs(command));
			}
		}
	}
}

void handle_hwcontrols()
{
	// TODO
}

void handle_mpv()
{
	for(;;){
		mpv_event *event = mpv_wait_event(mpv, 0);
		if(event->event_id == MPV_EVENT_NONE)
			break;
		printf("event: %s\n", mpv_event_name(event->event_id));
		if(event->event_id == MPV_EVENT_SHUTDOWN){
			do_main_loop = false;
		}
	}
}

void handle_mount()
{
	// TODO: Return if not much time has passed since last check

	// TODO: Check if any new USB devices have been plugged in

	// TODO: If no USB device is mounted, mount the one that was plugged in

	// TODO: Scan the device and start playing it
}

int main(int argc, char *argv[])
{
	const char opts[100] = "hs:t:";
	const char usagefmt[1000] =
			"Usage: %s [OPTION]...\n"
			"  -h                   Show this help\n"
			"  -s [path]            Specify serial port device of Arduino\n"
			"  -t [path]            Speficy test file path\n"
			;

	int c;
	while((c = c55_getopt(argc, argv, opts)) != -1)
	{
		switch(c)
		{
		case 'h':
			printf(usagefmt, argv[0]);
			return 1;
		case 's':
			arduino_serial_path = c55_optarg;
			break;
		case 't':
			test_file_path = c55_optarg;
			break;
		default:
			fprintf(stderr, "Invalid argument\n");
			fprintf(stderr, usagefmt, argv[0]);
			return 1;
		}
	}

    mpv = mpv_create();
    if (!mpv) {
        printf("failed creating context\n");
        return 1;
    }
    
    mpv_set_option_string(mpv, "vo", "null");

    check_mpv_error(mpv_initialize(mpv));

	while(do_main_loop){
		handle_stdin();

		handle_hwcontrols();

		handle_mpv();

		handle_mount();

		usleep(1000000/60);
	}

    mpv_terminate_destroy(mpv);
    return 0;
}

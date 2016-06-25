#include "c55_getopt.h"
#include "command_accumulator.hpp"
#include "string_util.hpp"
#include "file_watch.hpp"
#include "types.hpp"
#include <mpv/client.h>
#include <fstream>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

ss_ arduino_serial_path;
ss_ test_file_path;
sv_<ss_> track_devices;

bool do_main_loop = true;
mpv_handle *mpv = NULL;
CommandAccumulator<100> stdin_command_accu;
int arduino_serial_fd = -1;
CommandAccumulator<100> arduino_message_accu;

up_<FileWatch> partitions_watch;

ss_ current_mount_device;
ss_ current_mount_path;

static inline void check_mpv_error(int status)
{
    if (status < 0) {
        printf("mpv API error: %s\n", mpv_error_string(status));
        exit(1);
    }
}

ss_ read_any(int fd)
{
	struct pollfd fds;
	int ret;
	fds.fd = fd;
	fds.events = POLLIN;
	ret = poll(&fds, 1, 0);
	if(ret == 1){
		char buf[1000];
		ssize_t n = read(fd, buf, 1000);
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

bool set_interface_attribs(int fd, int speed, int parity)
{
	struct termios tty;
	memset(&tty, 0, sizeof tty);
	if(tcgetattr(fd, &tty) != 0){
		printf("Error %d from tcgetattr\n", errno);
		return false;
	}

	cfsetospeed(&tty, speed);
	cfsetispeed(&tty, speed);

	tty.c_cflag =(tty.c_cflag & ~CSIZE) | CS8;     // 8-bit chars
	// disable IGNBRK for mismatched speed tests; otherwise receive break
	// as \000 chars
	tty.c_iflag &= ~IGNBRK;	 // disable break processing
	tty.c_lflag = 0;		// no signaling chars, no echo,
					// no canonical processing
	tty.c_oflag = 0;		// no remapping, no delays
	tty.c_cc[VMIN]  = 0;	    // read doesn't block
	tty.c_cc[VTIME] = 5;	    // 0.5 seconds read timeout

	tty.c_iflag &= ~(IXON | IXOFF | IXANY); // shut off xon/xoff ctrl

	tty.c_cflag |=(CLOCAL | CREAD);// ignore modem controls,
					// enable reading
	tty.c_cflag &= ~(PARENB | PARODD);      // shut off parity
	tty.c_cflag |= parity;
	tty.c_cflag &= ~CSTOPB;
	tty.c_cflag &= ~CRTSCTS;

	if(tcsetattr(fd, TCSANOW, &tty) != 0){
		printf("Error %d from tcsetattr\n", errno);
		return false;
	}
	return true;
}

void handle_control_play_test_file()
{
	printf("Playing test file \"%s\"\n", cs(test_file_path));
	const char *cmd[] = {"loadfile", test_file_path.c_str(), NULL};
	check_mpv_error(mpv_command(mpv, cmd));
}

void handle_control_next()
{
}

void handle_control_prev()
{
}

void handle_control_nextalbum()
{
}

void handle_control_prevalbum()
{
}

void handle_stdin()
{
	ss_ stdin_stuff = read_any(0); // 0=stdin
	for(char c : stdin_stuff){
		if(stdin_command_accu.put_char(c)){
			ss_ command = stdin_command_accu.command();
			if(command == "next"){
				handle_control_next();
			} else if(command == "prev"){
				handle_control_prev();
			} else if(command == "nextalbum"){
				handle_control_nextalbum();
			} else if(command == "prevalbum"){
				handle_control_prevalbum();
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
				handle_control_play_test_file();
			} else {
				printf("Invalid command: \"%s\"", cs(command));
			}
		}
	}
}

void handle_key_press(int key)
{
	if(key == 21){
		handle_control_play_test_file();
		return;
	}
}

void handle_key_release(int key)
{
}

void handle_hwcontrols()
{
	if(arduino_serial_fd == -1)
		return;
	ss_ serial_stuff = read_any(arduino_serial_fd);
	for(char c : serial_stuff){
		if(arduino_message_accu.put_char(c)){
			ss_ message = arduino_message_accu.command();
			Strfnd f(message);
			ss_ first = f.next(":");
			if(first == "<KEY_PRESS"){
				int key = stoi(f.next(":"));
				printf("<KEY_PRESS  : %i\n", key);
				handle_key_press(key);
			} else if(first == "<KEY_RELEASE"){
				int key = stoi(f.next(":"));
				printf("<KEY_RELEASE: %i\n", key);
				handle_key_release(key);
			} else {
				printf("%s (ignored)\n", cs(message));
			}
		}
	}
}

void handle_mpv()
{
	for(;;){
		mpv_event *event = mpv_wait_event(mpv, 0);
		if(event->event_id == MPV_EVENT_NONE)
			break;
		printf("MPV: %s\n", mpv_event_name(event->event_id));
		if(event->event_id == MPV_EVENT_SHUTDOWN){
			do_main_loop = false;
		}
	}
}

bool check_partition_exists(const ss_ &devname0)
{
	std::ifstream f("/proc/partitions");
	if(!f.good()){
		printf("Can't read /proc/partitions\n");
		return false;
	}
	ss_ proc_partitions_data = ss_((std::istreambuf_iterator<char>(f)),
			std::istreambuf_iterator<char>());

	Strfnd f_lines(proc_partitions_data);
	for(;;){
		if(f_lines.atend()) break;
		ss_ line = f_lines.next("\n");
		Strfnd f_columns(line);
		f_columns.while_any(" ");
		f_columns.next(" ");
		f_columns.while_any(" ");
		f_columns.next(" ");
		f_columns.while_any(" ");
		f_columns.next(" ");
		f_columns.while_any(" ");
		ss_ devname = f_columns.next(" ");
		if(devname == "")
			continue;
		if(devname == devname0)
			return true;
	}
	return false;
}

ss_ get_device_mountpoint(const ss_ &devname0)
{
	std::ifstream f("/proc/mounts");
	if(!f.good()){
		printf("Can't read /proc/mounts\n");
		return "";
	}
	ss_ proc_mounts_data = ss_((std::istreambuf_iterator<char>(f)),
			std::istreambuf_iterator<char>());

	Strfnd f_lines(proc_mounts_data);
	for(;;){
		if(f_lines.atend()) break;
		ss_ line = f_lines.next("\n");
		Strfnd f_columns(line);
		f_columns.while_any(" ");
		ss_ devpath = f_columns.next(" ");
		f_columns.while_any(" ");
		ss_ mountpoint = f_columns.next(" ");
		Strfnd f_devpath(devpath);
		ss_ devname;
		for(;;){
			ss_ s = f_devpath.next("/");
			if(s != "")
				devname = s;
			if(f_devpath.atend())
				break;
		}
		if(devname == devname0){
			/*printf("is_device_mounted(): %s is mounted at %s\n",
					cs(devname0), cs(mountpoint));*/
			return mountpoint;
		}
	}
	//printf("is_device_mounted(): %s is not mounted\n", cs(devname0));
	return "";
}

void handle_changed_partitions()
{
	if(current_mount_device != ""){
		if(!check_partition_exists(current_mount_device)){
			// Unmount it if the partition doesn't exist anymore
			printf("Device %s does not exist anymore; umounting\n",
					cs(current_mount_path));
			int r = umount(current_mount_path.c_str());
			if(r == 0){
				printf("umount %s succesful\n", current_mount_path.c_str());
				current_mount_device = "";
				current_mount_path = "";
			} else {
				printf("umount %s failed: %s\n", current_mount_path.c_str(), strerror(errno));
			}
		} else if(get_device_mountpoint(current_mount_device) == ""){
			printf("Device %s got unmounted from %s\n", cs(current_mount_device),
					cs(current_mount_path));
			current_mount_device = "";
			current_mount_path = "";
		}
	}

	if(current_mount_device != ""){
		printf("Ignoring partition change because we have mounted %s at %s\n",
				cs(current_mount_device), cs(current_mount_path));
		return;
	}

	std::ifstream f("/proc/partitions");
	if(!f.good()){
		printf("Can't read /proc/partitions\n");
		return;
	}
	ss_ proc_partitions_data = ss_((std::istreambuf_iterator<char>(f)),
			std::istreambuf_iterator<char>());

	Strfnd f_lines(proc_partitions_data);
	for(;;){
		if(f_lines.atend()) break;
		ss_ line = f_lines.next("\n");
		Strfnd f_columns(line);
		f_columns.while_any(" ");
		f_columns.next(" ");
		f_columns.while_any(" ");
		f_columns.next(" ");
		f_columns.while_any(" ");
		f_columns.next(" ");
		f_columns.while_any(" ");
		ss_ devname = f_columns.next(" ");
		if(devname == "")
			continue;
		bool found = false;
		for(const ss_ &s : track_devices){
			if(devname.size() < s.size())
				continue;
			// Match beginning of device name
			if(devname.substr(0, s.size()) == s){
				found = true;
				break;
			}
		}
		if(!found)
			continue;
		printf("Tracked partition: %s\n", cs(devname));

		ss_ existing_mountpoint = get_device_mountpoint(devname);
		if(existing_mountpoint != ""){
			printf("%s is already mounted at %s; using it\n",
					cs(devname), cs(existing_mountpoint));
			current_mount_device = devname;
			current_mount_path = existing_mountpoint;
			return;
		}

		ss_ dev_path = "/dev/"+devname;
		ss_ new_mount_path = "/tmp/__autosoitin_mnt";
		printf("Mounting %s at %s\n", cs(dev_path), cs(new_mount_path));
		mkdir(cs(new_mount_path), 0777);
		int r = mount(dev_path.c_str(), new_mount_path.c_str(), "vfat",
				MS_MGC_VAL | MS_RDONLY | MS_NOEXEC | MS_NOSUID | MS_DIRSYNC |
						MS_NODEV | MS_SYNCHRONOUS,
				NULL);
		if(r == 0){
			printf("Succesfully mounted.\n");
			current_mount_device = devname;
			current_mount_path = new_mount_path;
			return;
		} else {
			printf("Failed to mount (%s); trying next\n", strerror(errno));
		}
	}
}

void handle_mount()
{
	/*// Return if not much time has passed since last check
	static time_t last_checked_time = 0;
	if(last_checked_time >= time(0) - 2)
		return;
	last_checked_time = time(0);*/

	// Calls callbacks; eg. handle_changed_partitions()
	for(auto fd : partitions_watch->get_fds()){
		partitions_watch->report_fd(fd);
	}

	// TODO: Check if any new USB devices have been plugged in

	// TODO: If no USB device is mounted, mount the one that was plugged in

	// TODO: Scan the device and start playing it
}

int main(int argc, char *argv[])
{
	const char opts[100] = "hs:t:d:";
	const char usagefmt[1000] =
			"Usage: %s [OPTION]...\n"
			"  -h                   Show this help\n"
			"  -s [path]            Serial port device of Arduino\n"
			"  -t [path]            Test file path\n"
			"  -d [dev1,dev2,...]   Block devices to track and mount (eg. sdc)\n"
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
		case 'd':
			{
				Strfnd f(c55_optarg);
				for(;;){
					ss_ dev = f.next(",");
					if(dev == "") break;
					printf("Tracking \"%s\"\n", cs(dev));
					track_devices.push_back(dev);
				}
			}
			break;
		default:
			fprintf(stderr, "Invalid argument\n");
			fprintf(stderr, usagefmt, argv[0]);
			return 1;
		}
	}

	if(arduino_serial_path != ""){
		arduino_serial_fd = open(arduino_serial_path.c_str(), O_RDWR | O_NOCTTY | O_SYNC);
		if(arduino_serial_fd < 0){
			printf("Failed to open %s\n", cs(arduino_serial_path));
			return 1;
		}
		if(!set_interface_attribs(arduino_serial_fd, 9600, 0)){
			return 1;
		}
	}

	partitions_watch.reset(createFileWatch());
	partitions_watch->add("/dev/disk", [](const ss_ &path){
		printf("Partitions changed (%s)\n", cs(path));
		handle_changed_partitions();
	});

    mpv = mpv_create();
    if (!mpv) {
        printf("mpv_create() failed");
        return 1;
    }
    
    mpv_set_option_string(mpv, "vo", "null");

    check_mpv_error(mpv_initialize(mpv));

	printf("Doing initial partition scan\n");
	handle_changed_partitions();

	while(do_main_loop){
		handle_stdin();

		handle_hwcontrols();

		handle_mpv();

		handle_mount();

		usleep(1000000/60);
	}

    mpv_terminate_destroy(mpv);
    close(arduino_serial_fd);
    return 0;
}

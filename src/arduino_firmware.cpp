#include "arduino_firmware.hpp"
#include "string_util.hpp"
#include "arduino_controls.hpp"
#include <fstream>
#include <stdio.h>

bool read_file_content(const ss_ &path, ss_ &result_data)
{
	std::ifstream f(path.c_str());
	if(!f.good())
		return false;
	result_data = ss_((std::istreambuf_iterator<char>(f)),
			std::istreambuf_iterator<char>());
	return true;
}

void arduino_firmware_update_if_needed(const ss_ &current_version)
{
	// NOTE: current_version is the md5 hash of arduino/src/sketch.ino.
	// Assuming the latest arduino code has been built, we can find it in
	// arduino/version.h in the format
	// 'static const char *VERSION_STRING = "4a7097141e9a52341e53cbb39646ad79";'
	//
	// The built firmware is located at "arduino/.build_ano/nano/firmware.hex",
	// and can be flashed without ino or ano by using avrdude:
	// 'avrdude -c arduino -p atmega328p -P<arduino_serial_fd_path> -b57600
	//     -V -U flash:w:arduino/.build_ano/nano/firmware.hex'

	ss_ version_h_path = "arduino/version.h";
	ss_ version_h_data;
	bool ok = read_file_content(version_h_path, version_h_data);
	if(!ok){
		printf("Can't read %s\n", cs(version_h_path));
		printf("Not updating firmware from version %s\n", cs(current_version));
		arduino_set_text("FWUFAIL1");
		usleep(3000000);
		return;
	}

	Strfnd version_h_data_f(version_h_data);
	version_h_data_f.next("\"");
	ss_ version_h_version = version_h_data_f.next("\"");
	if(version_h_version.size() != 32){
		printf("version.h version length is not 32 (\"%s\")\n", cs(version_h_version));
		printf("Not updating firmware from version %s\n", cs(current_version));
		arduino_set_text("FWUFAIL2");
		usleep(3000000);
		return;
	}

	if(version_h_version == current_version){
		printf("Arduino version is up to date at %s.\n", cs(current_version));
		return;
	}

	printf("Updating arduino firmware from %s to %s\n",
			cs(current_version), cs(version_h_version));

	// Set this text so that the LCD will be showing it while the firmware is
	// being uploaded and arduino reboots
	arduino_set_temp_text("FW UP");
	usleep(100000);

	ss_ command = "avrdude -c arduino -p atmega328p -P"+arduino_serial_fd_path+
			" -b57600 -V -U flash:w:arduino/.build_ano/nano/firmware.hex"+
			" -q -l avrdude.log";
	int r = system(command.c_str());
	if(r != 0){
		printf("Failed to execute avrdude.\n");
		printf("Not updating firmware from version %s\n", cs(current_version));
		arduino_set_text("FWUFAIL3");
		usleep(3000000);
	} else {
		printf("Arduino firmware updated from %s to %s\n",
				cs(current_version), cs(version_h_version));
		// NOTE: Can't show anything on the display right away, so we sleep
		// first for bit and hope the arduino has booted up during that time.
		usleep(2000000);
		arduino_set_text("FWU OK");
		usleep(2000000);
		arduino_set_text("FWU OK");
		usleep(2000000);
		arduino_set_text("FWU OK");
		arduino_set_temp_text("FWU OK");
	}
}


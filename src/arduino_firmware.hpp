#pragma once
#include "types.hpp"

extern ss_ arduino_serial_fd_path;

void arduino_firmware_update_if_needed(const ss_ &current_version);


#pragma once
#ifdef __WIN32__
#  include "windows_includes.hpp"
#else
#  define printf_(...) printf(__VA_ARGS__)
#  define fprintf_(f, ...) fprintf(f, __VA_ARGS__)
#endif

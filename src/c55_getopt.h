#ifndef __C55_GETOPT_H__
#define __C55_GETOPT_H__

extern const char *c55_optarg;
//extern int c55_optind;

extern int c55_argi;
extern const char *c55_cp;

int c55_getopt(int argc, char *argv[], const char *argspec);

#endif

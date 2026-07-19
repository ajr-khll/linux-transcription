#ifndef WHISPRD_LOG_H
#define WHISPRD_LOG_H

#include <stdio.h>

extern int log_verbose;

#define log_err(...)  fprintf(stderr, "whisprd: error: " __VA_ARGS__)
#define log_warn(...) fprintf(stderr, "whisprd: warn: " __VA_ARGS__)
#define log_info(...) fprintf(stderr, "whisprd: " __VA_ARGS__)
#define log_dbg(...)  do { if (log_verbose) fprintf(stderr, "whisprd: dbg: " __VA_ARGS__); } while (0)

#endif

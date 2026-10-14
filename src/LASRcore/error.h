#ifndef ERROR_H
#define ERROR_H

#include <string>
extern thread_local int last_error_code;
extern thread_local std::string last_error;

#ifdef USING_R
#define R_NO_REMAP 1
#include <Rinternals.h>
SEXP make_R_error(const char* message);
#endif

#endif

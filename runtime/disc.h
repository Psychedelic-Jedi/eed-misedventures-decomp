#pragma once
#include "cpu/cpu.h"
extern int disc_trace;
int disc_open(const char *path);          /* ISO/GCM, CISO, or extracted directory */
void disc_read(u32 off, void *dst, u32 len);

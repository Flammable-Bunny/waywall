#ifndef WAYWALL_UTIL_SYSINFO_H
#define WAYWALL_UTIL_SYSINFO_H

#include <stddef.h>
#include <stdint.h>

void sysinfo_dump_log();

// Query available VRAM in bytes
// Returns 0 if unable to query
size_t sysinfo_query_vram_available();

// Query total VRAM in bytes
// Returns 0 if unable to query
size_t sysinfo_query_vram_total();

#endif

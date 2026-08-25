#pragma once

#include <stdbool.h>

/*
 * OpenWrt stage2 can terminate the sysupgrade wrapper after accepting the
 * request but before opening the image. edge_process_run_timeout reports that
 * signal termination as -1, so every result except exec failure must preserve
 * the image for the handoff grace period.
 */
static inline bool edge_firmware_sysupgrade_may_have_handed_off(int status) {
    return status != 127;
}

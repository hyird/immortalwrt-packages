#pragma once

#include <stdbool.h>
#include <stddef.h>

#define EDGE_INTERFACE_NAME_CAPACITY 33U

bool edge_interface_has_subinterface(
    const char *candidate,
    const char names[][EDGE_INTERFACE_NAME_CAPACITY],
    size_t count);

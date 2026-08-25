#include "edge_interface.h"

#include <string.h>

bool edge_interface_has_subinterface(
    const char *candidate,
    const char names[][EDGE_INTERFACE_NAME_CAPACITY],
    size_t count) {
    if (candidate == NULL || candidate[0] == '\0' || names == NULL)
        return false;
    const size_t length = strlen(candidate);
    if (length >= EDGE_INTERFACE_NAME_CAPACITY - 1U)
        return false;
    for (size_t index = 0; index < count; ++index)
        if (strncmp(names[index], candidate, length) == 0 &&
            names[index][length] == '.' && names[index][length + 1U] != '\0')
            return true;
    return false;
}

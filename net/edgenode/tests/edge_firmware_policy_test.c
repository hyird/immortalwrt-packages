#include <assert.h>

#include "edge_firmware_policy.h"

int main(void) {
    assert(edge_firmware_sysupgrade_may_have_handed_off(0));
    assert(edge_firmware_sysupgrade_may_have_handed_off(1));
    assert(edge_firmware_sysupgrade_may_have_handed_off(-1));
    assert(!edge_firmware_sysupgrade_may_have_handed_off(127));
    return 0;
}

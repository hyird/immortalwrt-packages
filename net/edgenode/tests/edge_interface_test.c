#include <stdio.h>
#include <string.h>

#include "edge_interface.h"

static int failures;

static void expect_true(bool value, const char *message) {
    if (!value) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static void expect_false(bool value, const char *message) {
    if (value) {
        fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

int main(void) {
    const char names[][EDGE_INTERFACE_NAME_CAPACITY] = {
        "br-lan", "eth0", "eth0.1", "eth1", "wwan0", "wwan0:1"};
    const size_t count = sizeof(names) / sizeof(names[0]);

    expect_true(edge_interface_has_subinterface("eth0", names, count),
                "VLAN parent must not be reported separately");
    expect_false(edge_interface_has_subinterface("eth0.1", names, count),
                 "VLAN interface must remain visible");
    expect_false(edge_interface_has_subinterface("br-lan", names, count),
                 "bridge must remain visible");
    expect_false(edge_interface_has_subinterface("eth1", names, count),
                 "standalone interface must remain visible");
    expect_false(edge_interface_has_subinterface("wwan0", names, count),
                 "alias syntax is not a VLAN child");
    expect_false(edge_interface_has_subinterface(NULL, names, count),
                 "null candidate must be safe");

    if (failures != 0)
        return 1;
    puts("edge interface tests passed");
    return 0;
}

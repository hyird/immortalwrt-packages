#pragma once

#include <stdbool.h>

#include "edge.pb.h"

bool edge_capability_has_terminal(void);

/* Reports runtime interfaces and UCI logical networks, excluding the protected uplink. */
bool edge_capability_collect_network(iot_edge_v1_CapabilityReport *report,
                                     const char *protected_device);

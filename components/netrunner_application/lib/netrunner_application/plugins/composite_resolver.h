#pragma once

// Compat shim: upstream apps include <lib/netrunner_application/plugins/composite_resolver.h>;
// in this port the real header lives one directory level up under netrunner_application/.
// Mirrors the existing lib/netrunner_application/netrunner_application.h shim.
#include "../../../netrunner_application/plugins/composite_resolver.h"

#pragma once

#include "madt.h"

int acpi_init(void);
const struct acpi_topology *acpi_topology(void);

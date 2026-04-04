#pragma once

#include <memory>

#include "gpio_output_driver.h"
#include "kasa_output_driver.h"
#include "shelly_output_driver.h"

std::unique_ptr<OutputDriver> createOutputDriver(const OutputConfig &config, const String &role);

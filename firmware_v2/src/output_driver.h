#pragma once

#include "app_types.h"

class OutputDriver {
 public:
  virtual ~OutputDriver() = default;
  virtual bool begin() = 0;
  virtual bool setState(bool on) = 0;
  virtual bool refresh() = 0;
  virtual DriverStatus status() const = 0;
};

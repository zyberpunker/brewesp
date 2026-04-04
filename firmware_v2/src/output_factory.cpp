#include "output_factory.h"

std::unique_ptr<OutputDriver> createOutputDriver(const OutputConfig &config, const String &role) {
  if (config.driver == "gpio") {
    return std::unique_ptr<OutputDriver>(new GpioOutputDriver(config, role));
  }
  if (config.driver == "shelly_http_rpc") {
    return std::unique_ptr<OutputDriver>(new ShellyOutputDriver(config, role));
  }
  if (config.driver == "kasa_local") {
    return std::unique_ptr<OutputDriver>(new KasaOutputDriver(config, role));
  }
  return nullptr;
}

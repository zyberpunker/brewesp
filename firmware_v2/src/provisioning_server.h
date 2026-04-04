#pragma once

#include <WebServer.h>

#include "app_types.h"

class ProvisioningServer {
 public:
  void begin(const SystemConfig &defaults);
  void loop();
  bool consumePendingConfig(SystemConfig &config);

 private:
  SystemConfig buildFormConfig();
  String renderPage() const;
  String formValue(const char *key, const String &fallback);
  int formIntValue(const char *key, int fallback);
  bool formChecked(const char *key, bool fallback);
  void handleRoot();
  void handleSave();
  void handleApiSave();
  void handleNotFound();

  WebServer server_{80};
  SystemConfig defaults_;
  SystemConfig pending_;
  bool has_pending_ = false;
  String last_message_;
};

#pragma once

#include "system/hardware/base.h"
#include "common/util.h"

#ifdef QCOM
#include "system/hardware/eon/hardware.h"
#define Hardware HardwareEon
#elif QCOM2
#include "system/hardware/tici/hardware.h"
#define Hardware HardwareTici
#else
class HardwarePC : public HardwareNone {
public:
  static std::string get_os_version() { return "openpilot for PC"; }
  static bool PC() { return true; }
  static bool TICI() { return util::getenv("TICI", 0) == 1; }
};
#define Hardware HardwarePC
#endif

namespace Path {
inline std::string log_root() {
  if (const char *env = getenv("LOG_ROOT")) {
    return env;
  }
  return Hardware::PC() ? util::getenv("HOME") + "/.comma/media/0/realdata" : "/data/media/0/realdata";
}
inline std::string params() {
  return Hardware::PC() ? util::getenv("HOME") + "/.comma/params" : "/data/params";
}
inline std::string rsa_file() {
  return Hardware::PC() ? util::getenv("HOME") + "/.comma/persist/comma/id_rsa" : "/persist/comma/id_rsa";
}
}  // namespace Path

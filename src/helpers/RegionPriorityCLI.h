#pragma once

#include "RegionMap.h"
#include <Utils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace RegionPriorityCLI {

// Keep parsing independent of board hardware so the exact CLI contract can be
// exercised by native tests, including extra arguments and malformed numbers.
inline bool handle(RegionMap& regions, char* command, char* reply, size_t size,
                   bool supported) {
  if (strncmp(command, "region prio", 11) != 0 ||
      (command[11] != ' ' && command[11] != '\0')) return false;
  if (!supported) {
    snprintf(reply, size, "Err - region priority is only available on repeaters");
    return true;
  }
  const char* parts[5];
  int count = mesh::Utils::parseTextParts(command, parts, 5, ' ');
  if (count == 2) {
    auto region = regions.getPriorityRegion();
    if (region) {
      snprintf(reply, size, " priority is %s, reserve %u%%%s", region->name,
               (unsigned int)regions.getConfiguredPriorityPercent(),
               region->flags & REGION_DENY_FLOOD ? " (flood denied)" : "");
    } else {
      snprintf(reply, size, " priority is off");
    }
  } else if (count == 4) {
    auto region = regions.findByName(parts[2]);
    char* end;
    long percent = strtol(parts[3], &end, 10);
    if (end == parts[3] || *end || percent < 0 || percent > 50) {
      snprintf(reply, size, "Err - percentage must be 0-50");
    } else if (!region || region->isWildcard()) {
      snprintf(reply, size, "Err - unknown named region");
    } else if (!regions.setPriorityRegion(region, percent)) {
      snprintf(reply, size,
               "Err - region must allow floods and have a transport key");
    } else {
      snprintf(reply, size,
               "OK - %s priority reserve %ld%%; use region save to persist",
               region->name, percent);
    }
  } else {
    snprintf(reply, size, "Err - use region prio <name> <0-50>");
  }
  return true;
}

}  // namespace RegionPriorityCLI

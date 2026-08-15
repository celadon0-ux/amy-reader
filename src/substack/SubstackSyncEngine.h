#pragma once

#include <functional>
#include <string>

#include "SubstackStore.h"

class SubstackSyncEngine {
 public:
  using ProgressCallback = std::function<void(size_t feedIndex, size_t feedCount, const std::string& publication)>;

  static SubstackSyncSummary sync(const ProgressCallback& progress = nullptr);
};

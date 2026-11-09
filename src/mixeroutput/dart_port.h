#pragma once

#include "mixer_output.h"
#include <cstdint>
#include <cstring>
#include <limits>

namespace mixer_output_port {
using PostInteger = bool (*)(int64_t port, int64_t message);

// NativeApi.initializeApiDLData exposes this versioned table. Resolve only
// Dart_PostInteger, using the version-2 layout in the SDK's
// internal/dart_api_dl_impl.h. The entry points belong to the VM, not to an
// isolate, so they remain callable after a receiving isolate exits. Keeping
// this small resolver here avoids adding SDK sources to every native runner.
inline PostInteger resolve(const void *data) {
  struct Entry { const char *name; void (*function)(); };
  struct Api { int major; int minor; const Entry *functions; };
  if (data == nullptr) return nullptr;
  const auto &api = *static_cast<const Api *>(data);
  if (api.major != 2 || api.functions == nullptr) return nullptr;
  for (const Entry *entry = api.functions; entry->name != nullptr; ++entry)
    if (std::strcmp(entry->name, "Dart_PostInteger") == 0)
      return reinterpret_cast<PostInteger>(entry->function);
  return nullptr;
}

inline bool send(PostInteger post, int64_t port, uint8_t *token) {
  const uintptr_t value = reinterpret_cast<uintptr_t>(token);
  // A failed post means the port was closed, including by Isolate.exit or
  // Isolate.kill. Retire the unclaimed packet immediately; no isolate-owned
  // trampoline is ever invoked. Tokens must also fit the VM's signed message.
  if (post == nullptr || port <= 0 ||
      value > static_cast<uintptr_t>(std::numeric_limits<int64_t>::max()) ||
      !post(port, static_cast<int64_t>(value)))
  {
    MixerOutput::discardPacket(token);
    return false;
  }
  return true;
}
}

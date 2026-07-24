#pragma once

#include <mutex>

// Shared gate for every retained Dart callback pointer. The gate must be held
// while loading and invoking a callback, and while clearing registrations.
extern std::mutex dart_callback_invocation_mutex;

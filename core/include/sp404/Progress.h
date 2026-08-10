#pragma once

#include <functional>
#include <string>

namespace sp404 {

// Reports incremental progress during a long-running bulk operation (bank/pattern save/load,
// card sync). `current` is 1-based (the step just started or completed -- see each function's own
// doc comment for which), `total` is the known step count for this run, `label` names the current
// step (typically a filename). Called synchronously from whatever thread actually runs the
// operation -- see plugin/BackgroundOperation.h, which runs these on a background thread and
// republishes current/total/label for the message thread to poll, so nothing here talks to the
// UI directly. Default-constructed (empty) is a valid "no progress reporting wanted" callback --
// every function taking one checks it before calling.
using ProgressCallback = std::function<void(int current, int total, const std::string& label)>;

} // namespace sp404

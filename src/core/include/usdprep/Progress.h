// How far a long operation is, for whoever shows it - and a way to stop it.
//
// The operation writes, another thread reads: every member is atomic.
// `step` always points at a string literal, so it can be read at any time.
#pragma once

#include <atomic>

namespace usdprep {

struct Progress {
    std::atomic<float> fraction{0.0f};  // 0..1, never goes back
    std::atomic<const char*> step{""};  // what is being done, in words
    std::atomic<bool> cancel{false};    // set by the caller: stop at the next safe point
};

}  // namespace usdprep

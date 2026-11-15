// Local UI animation only. Never reads or drives Rack/ZOIA signals or saved state.
#pragma once
#include <algorithm>
#include <cmath>
namespace zoia {
struct SequencerPreview {
  bool running = false;
  double phase = 0;
  void reset() { phase = 0; }
  void advance(double seconds, int steps, int pages, double bpm = 120) {
    if (!running || !std::isfinite(seconds) || seconds <= 0) return;
    int total = std::max(1, std::min(32, steps)) * std::max(1, std::min(8, pages));
    phase = std::fmod(phase + seconds * std::max(20., std::min(300., bpm)) / 60., total);
  }
  int step(int steps) const { return int(phase) % std::max(1, steps); }
  int page(int steps) const { return int(phase) / std::max(1, steps); }
};
}

// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace zoia {
struct Macro {
  int cc = 20;
  int minimum = 0;
  int maximum = 127;
  float initial = 0.f;
};
struct Message {
  uint8_t status, data1, data2;
};
inline bool allowedCC(int cc) {
  return cc >= 0 && cc < 120 && (cc < 60 || cc > 63);
}
inline int ccValue(const Macro &m, float knob, float voltage) {
  if (!std::isfinite(knob))
    knob = 0.f;
  if (!std::isfinite(voltage))
    voltage = 0.f;
  float n = std::max(0.f, std::min(1.f, knob + voltage / 10.f));
  return int(std::lround(m.minimum + n * (m.maximum - m.minimum)));
}
struct Sender {
  std::array<int, 8> last;
  int cursor = 0;
  double elapsed = 0.;
  Sender() { reset(); }
  void reset() {
    last.fill(-1);
    cursor = 0;
    elapsed = 0.;
  }
  // At most 120 CC messages/second across the entire eight-control bank.
  template <typename Send>
  void process(double dt, const std::array<Macro, 8> &map,
               const std::array<float, 8> &knobs,
               const std::array<float, 8> &volts, int count, int channel,
               bool active, Send send) {
    if (!active) {
      reset();
      return;
    }
    elapsed = std::min(1. / 120., elapsed + dt);
    if (elapsed < 1. / 120.)
      return;
    for (int step = 0; step < count; ++step) {
      int i = (cursor + step) % count,
          value = ccValue(map[i], knobs[i], volts[i]);
      if (value == last[i])
        continue;
      send(
          Message{uint8_t(0xb0 + channel), uint8_t(map[i].cc), uint8_t(value)});
      last[i] = value;
      cursor = (i + 1) % count;
      elapsed = 0.;
      return;
    }
  }
};
struct Voice {
  int note = -1, channel = -1;
  bool high = false;
  template <typename Send> void stop(Send send) {
    if (note >= 0)
      send(Message{uint8_t(0x80 + channel), uint8_t(note), 0});
    note = -1;
    channel = -1;
    high = false;
  }
  template <typename Send>
  void process(float pitch, float gate, float velocity, int ch, bool active,
               Send send) {
    if (!active || !std::isfinite(pitch) || !std::isfinite(gate)) {
      stop(send);
      return;
    }
    bool next = high ? gate > 0.1f : gate >= 1.f;
    int n = std::max(
        0, std::min(127, int(std::lround(
                             std::max(-5.f, std::min(5.583333f, pitch)) * 12.f +
                             60.f))));
    if (note >= 0 && (!next || n != note || ch != channel)) {
      send(Message{uint8_t(0x80 + channel), uint8_t(note), 0});
      note = -1;
    }
    if (next && note < 0) {
      int v =
          std::isfinite(velocity)
              ? std::max(1, std::min(127, int(std::lround(velocity * 12.7f))))
              : 100;
      send(Message{uint8_t(0x90 + ch), uint8_t(n), uint8_t(v)});
      note = n;
      channel = ch;
    }
    high = next;
  }
};
} // namespace zoia

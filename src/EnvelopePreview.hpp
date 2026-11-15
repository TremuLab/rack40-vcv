// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include <utility>
namespace zoia {
struct EnvelopeTimes { double attack, decay, release, delay, peakHold, releaseHold; };
struct EnvelopePreview {
  bool valid = false;
  double windowMs = 1000, gateOffMs = -1;
  std::vector<std::pair<double,double>> points;
};
// Same linear time-axis contract as the browser; the 500 ms sustain is an
// explicitly illustrative gate hold, not a setting or hardware observation.
inline EnvelopePreview envelopePreview(EnvelopeTimes t, double sustain, bool hasSustain) {
  EnvelopePreview result;
  for (double value : {t.attack,t.decay,t.release,t.delay,t.peakHold,t.releaseHold})
    if (!std::isfinite(value) || value < 0) return result;
  sustain = hasSustain ? std::max(0.,std::min(1.,sustain)) : 0;
  auto &p=result.points;
  p.push_back({0,0}); double at=t.delay; p.push_back({at,0});
  at+=t.attack;p.push_back({at,1});at+=t.peakHold;p.push_back({at,1});
  at+=t.decay;p.push_back({at,sustain});
  if(hasSustain) {result.gateOffMs=at+500;at=result.gateOffMs+t.releaseHold;p.push_back({at,sustain});at+=t.release;p.push_back({at,0});}
  double minimum=std::max(1000.,at),magnitude=std::pow(10.,std::floor(std::log10(minimum)));
  for(double factor:{1.,2.,5.,10.}) if(factor*magnitude>=minimum){result.windowMs=factor*magnitude;break;}
  p.push_back({result.windowMs,0});result.valid=true;return result;
}
}

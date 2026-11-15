// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include "ControlEngine.hpp"
namespace zoia {
// Runtime-only, explicitly confirmed current page. Never pedal state readback.
struct RemoteGrid {
  int page = -1, channel = -1, cell = -1;
  int64_t owner = -1;
  uint64_t serial = 0, held = 0;
  double expires = 0;
  template<class Send> void release(uint64_t token, Send send) {
    if (!token || token != held) return;
    const int upCell=cell, upChannel=channel;
    held=0; owner=-1; cell=-1; expires=0;
    send(Message{uint8_t(0xb0+upChannel-1),62,uint8_t(upCell)});
  }
  template<class Send> void stop(Send send) {
    release(held,send); page=-1; channel=-1;
  }
  bool ready(int p,int ch) const { return p>=0 && p<=63 && ch>=1 && ch<=16 && page==p && channel==ch; }
  template<class Send> bool confirm(int p,int ch,Send send) {
    stop(send);
    if(p<0 || p>63 || ch<1 || ch>16) return false;
    page=p; channel=ch; return true;
  }
  template<class Send> uint64_t press(int64_t who,int p,int c,int ch,bool latch,double now,Send send) {
    if(who<0 || c<0 || c>39 || !ready(p,ch) || held) return 0;
    if(++serial==0) ++serial;
    held=serial; owner=who; cell=c; expires=now+.5;
    try { send(Message{uint8_t(0xb0+ch-1),61,uint8_t(c)}); }
    catch(...) { try { release(held,send); } catch(...) {} page=-1; channel=-1; throw; }
    auto token=held;
    if(latch) release(token,send);
    return token;
  }
  void renew(uint64_t token,double now) { if(token && token==held) expires=now+.5; }
  template<class Send> void tick(double now,Send send) {
    if(held && now>=expires) stop(send);
  }
};
}

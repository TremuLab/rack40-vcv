// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>
namespace zoia {
inline bool gainControl(const std::string& type, const std::string& key) {
  return (type=="VCA" && key=="level_control") ||
    (type=="Audio Output" && key=="gain") ||
    (type=="Euro Headphone Amp" && key=="level") ||
    (type=="Audio Mixer" && key.size()==6 && key.substr(0,5)=="gain_" && key[5]>='1' && key[5]<='8');
}
// Same approximate metadata curve as the browser's parameter readout. NaN
// represents a null range endpoint, not an acquired signal or calibrated gain.
inline double configuredGainDb(double raw, std::vector<double> values, double defaultRaw) {
  const double missing=std::numeric_limits<double>::quiet_NaN();
  if (!std::isfinite(raw) || (values.size()!=2 && values.size()!=5)) return missing;
  if (raw<=0 && std::isnan(values.front())) return -std::numeric_limits<double>::infinity();
  if (std::isnan(values.back())) return missing;
  for (size_t i=0;i<values.size();i++) if(std::isnan(values[i])) values[i]=i==0?-120:120;
  double n=std::max(0.,std::min(65535.,raw))/65535.;
  if(values.size()==2)return values[0]+(values[1]-values[0])*n;
  std::vector<double> anchors={0,.25,.5,.75,1};
  if(std::find(values.begin(),values.end(),0.)==values.end() && defaultRaw>0 && defaultRaw<65535) {
    auto at=std::upper_bound(anchors.begin(),anchors.end(),defaultRaw/65535.);auto index=at-anchors.begin();
    anchors.insert(at,defaultRaw/65535.);values.insert(values.begin()+index,0.);
  }
  if(n<=anchors.front())return values.front();
  for(size_t i=1;i<anchors.size();i++)if(n<=anchors[i]) {
    double t=std::pow((n-anchors[i-1])/(anchors[i]-anchors[i-1]),1.6);
    return values[i-1]+(values[i]-values[i-1])*t;
  }
  return values.back();
}
inline int gainSegments(double db) {
  if(std::isnan(db) || db==std::numeric_limits<double>::infinity())return -1;
  double gain=db==-std::numeric_limits<double>::infinity()?0:std::pow(10.,db/20.);
  return int(std::round(std::min(1.,gain)*18))+(db>0?1:0)+(db>=6?1:0);
}
}

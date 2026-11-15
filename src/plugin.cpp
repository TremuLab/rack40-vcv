// SPDX-License-Identifier: GPL-3.0-only
#include "ControlEngine.hpp"
#include "RemoteGrid.hpp"
#include "GainIndicator.hpp"
#include "AtomicJSON.hpp"
#include "EnvelopePreview.hpp"
#include "SequencerPreview.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <memory>
#include <mutex>
#include <set>
#include <regex>
#include <osdialog.h>
#include <rack.hpp>
using namespace rack;
Plugin *pluginInstance;
static std::string str(json_t *o, const char *k, std::string d = "") {
  auto v = json_object_get(o, k);
  return json_is_string(v) ? json_string_value(v) : d;
}
static json_t *readJSON(std::string path) {
  json_error_t e;
  return json_load_file(path.c_str(), JSON_REJECT_DUPLICATES, &e);
}
static void warn(std::string s) {
  osdialog_message(OSDIALOG_WARNING, OSDIALOG_OK, s.c_str());
}
struct Profile {
  std::string name = "Unassigned patch", id = "unassigned", original;
  int channel = 0, count = 8;
  std::array<zoia::Macro, 8> macros;
  std::array<std::string, 8> labels;
  Profile() {
    for (int i = 0; i < 8; i++) {
      macros[i].cc = 20 + i;
      labels[i] = "Macro " + std::to_string(i + 1);
    }
  }
};
static bool integer(json_t *o, const char *k, int lo, int hi, int &n) {
  auto v = json_object_get(o, k);
  if (!json_is_integer(v))
    return false;
  auto x = json_integer_value(v);
  if (x < lo || x > hi)
    return false;
  n = int(x);
  return true;
}
static std::shared_ptr<Profile> parseProfile(json_t *root) {
  int version, ch;
  if (!json_is_object(root) || str(root, "format") != "zoia-control-map" ||
      !integer(root, "schemaVersion", 1, 1, version) ||
      !integer(root, "channel", 1, 16, ch))
    return nullptr;
  auto patch = json_object_get(root, "patch"),
       cs = json_object_get(root, "controls");
  if (!json_is_object(patch) || !json_is_array(cs) || json_array_size(cs) < 1 ||
      json_array_size(cs) > 8)
    return nullptr;
  auto p = std::make_shared<Profile>();
  p->name = str(patch, "name");
  p->id = str(patch, "documentId");
  p->channel = ch - 1;
  p->count = json_array_size(cs);
  if (p->name.empty() || p->name.size() > 128 || p->id.empty() ||
      p->id.size() > 128)
    return nullptr;
  std::array<bool, 128> used{};
  std::set<std::string> ids;
  for (int i = 0; i < p->count; i++) {
    auto c = json_array_get(cs, i);
    auto &m = p->macros[i];
    p->labels[i] = str(c, "label");
    if (p->labels[i].empty() || p->labels[i].size() > 64 ||
        str(c, "id").empty() || !ids.insert(str(c, "id")).second ||
        !integer(c, "cc", 0, 119, m.cc) || !zoia::allowedCC(m.cc) ||
        used[m.cc] || !integer(c, "minimum", 0, 127, m.minimum) ||
        !integer(c, "maximum", 0, 127, m.maximum) || m.minimum > m.maximum)
      return nullptr;
    auto v = json_object_get(c, "defaultNormalized");
    if (!json_is_number(v))
      return nullptr;
    double n = json_number_value(v);
    if (!std::isfinite(n) || n < 0 || n > 1)
      return nullptr;
    m.initial = n;
    used[m.cc] = true;
  }
  char *dump = json_dumps(root, JSON_COMPACT);
  p->original = dump ? dump : "";
  free(dump);
  return p;
}
static json_t *profileJSON(const Profile &p) {
  auto root = p.original.empty() ? json_object()
                                 : json_loads(p.original.c_str(), 0, nullptr);
  if (!root)
    root = json_object();
  json_object_set_new(root, "format", json_string("zoia-control-map"));
  json_object_set_new(root, "schemaVersion", json_integer(1));
  json_object_set_new(root, "channel", json_integer(p.channel + 1));
  auto patch = json_object();
  json_object_set_new(patch, "name", json_string(p.name.c_str()));
  json_object_set_new(patch, "documentId", json_string(p.id.c_str()));
  json_object_set_new(root, "patch", patch);
  if (!json_object_get(root, "controls")) {
    auto cs = json_array();
    for (int i = 0; i < p.count; i++) {
      auto c = json_pack("{s:s,s:s,s:i,s:i,s:i,s:f}", "id",
                         ("macro-" + std::to_string(i + 1)).c_str(), "label",
                         p.labels[i].c_str(), "cc", p.macros[i].cc, "minimum",
                         p.macros[i].minimum, "maximum", p.macros[i].maximum,
                         "defaultNormalized", double(p.macros[i].initial));
      json_array_append_new(cs, c);
    }
    json_object_set_new(root, "controls", cs);
  }
  return root;
}
struct Live : Module {
  enum { ARM = 8, TEST = 9, PARAMS };
  enum { PITCH = 8, GATE = 9, INPUTS };
  midi::Output output;
  std::shared_ptr<Profile> profile = std::make_shared<Profile>();
  zoia::Sender sender;
  zoia::Voice voice;
  zoia::RemoteGrid grid;
  std::mutex transport;
  std::atomic<int> sent{0};
  // Enumeration may call an OS MIDI driver: keep it off the audio thread and
  // independent of window visibility. Shutdown joins before output is destroyed.
  std::mutex monitorMutex;
  std::condition_variable monitorWake;
  std::atomic<bool> monitorStopped{false};
  std::thread portMonitor;
  double noteElapsed = 0.;
  Live() {
    config(PARAMS, INPUTS, 0, 1);
    for (int i = 0; i < 8; i++) {
      configParam(i, 0, 1, 0, "Macro " + std::to_string(i + 1), "%", 0, 100);
      configInput(i, "Macro " + std::to_string(i + 1) +
                         " CV (0–10V, added to knob)");
    }
    configSwitch(ARM, 0, 1, 0, "Live MIDI", {"Muted", "Enabled"})->randomizeEnabled = false;
    configButton(TEST, "Test note C4")->randomizeEnabled = false;
    configInput(PITCH, "Pitch (1V/oct, 0V=C4)");
    configInput(GATE, "Gate");
    configLight(0, "Live MIDI enabled");
    output.setChannel(-1);
    auto ownerContext = contextGet();
    portMonitor = std::thread([this, ownerContext]() {
      contextSet(ownerContext);
      std::unique_lock<std::mutex> waitLock(monitorMutex);
      while (!monitorWake.wait_for(waitLock, std::chrono::milliseconds(250),
                                   [this]() { return monitorStopped.load(); })) {
        waitLock.unlock();
        refreshPortAvailability();
        waitLock.lock();
      }
    });
  }
  void send(zoia::Message x) {
    midi::Message m;
    m.bytes = {x.status, x.data1, x.data2};
    output.sendMessage(m);
    sent.fetch_add(1, std::memory_order_relaxed);
  }
  void mute() {
    params[ARM].setValue(0);
    params[TEST].setValue(0);
    noteElapsed = 0.;
    auto releasingVoice=voice;voice=zoia::Voice();
    try { releasingVoice.stop([&](zoia::Message x) { send(x); }); } catch(...) {}
    try { grid.stop([&](zoia::Message x) { send(x); }); } catch(...) {grid.page=-1;grid.channel=-1;}
    sender.reset();
  }
  void refreshPortAvailability() {
    std::lock_guard<std::mutex> lock(transport);
    try {grid.tick(remoteNow(), [&](zoia::Message x) { send(x); });}
    catch(...) {mute();}
    const auto device = output.getDeviceId();
    if (device < 0) {
      if (params[ARM].getValue() > .5f)
        mute();
      return;
    }
    const auto available = output.getDeviceIds();
    if (!output.getDevice() ||
        std::find(available.begin(), available.end(), device) == available.end()) {
      // Release only through the original subscription, then forget that port.
      // A later device with the same ID cannot silently resume this session.
      mute();
      output.setDeviceId(-1);
    }
  }
  static double remoteNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  bool remoteActive() {
    return params[ARM].getValue()>.5f && output.getDeviceId()>=0 && output.getDevice();
  }
  bool confirmGrid(int page,int channel) {
    std::lock_guard<std::mutex> lock(transport);
    try {grid.stop([&](zoia::Message x){send(x);});} catch(...) {mute();return false;}
    return remoteActive() && grid.confirm(page,channel,[&](zoia::Message x){send(x);});
  }
  void stopGrid() {
    std::lock_guard<std::mutex> lock(transport);
    try {grid.stop([&](zoia::Message x){send(x);});} catch(...) {mute();}
  }
  uint64_t pressGrid(int64_t owner,int page,int cell,int channel,bool latch) {
    std::lock_guard<std::mutex> lock(transport);
    if(!remoteActive()) return 0;
    const auto available=output.getDeviceIds();
    if(std::find(available.begin(),available.end(),output.getDeviceId())==available.end()) {
      mute(); output.setDeviceId(-1); return 0;
    }
    try {return grid.press(owner,page,cell,channel,latch,remoteNow(),[&](zoia::Message x){send(x);});}
    catch(...) {mute();return 0;}
  }
  void releaseGrid(uint64_t token) {
    std::lock_guard<std::mutex> lock(transport);
    try {grid.release(token,[&](zoia::Message x){send(x);});}
    catch(...) {mute();}
  }
  void renewGrid(uint64_t token) {
    std::lock_guard<std::mutex> lock(transport);
    grid.renew(token,remoteNow());
  }
  bool gridReady(int page,int channel) {
    std::lock_guard<std::mutex> lock(transport);
    return remoteActive() && grid.ready(page,channel);
  }
  void selectPort(int driver, int device) {
    std::lock_guard<std::mutex> lock(transport);
    mute();
    output.setDriverId(driver);
    output.setDeviceId(device);
    output.setChannel(-1);
  }
  void apply(std::shared_ptr<Profile> p, bool defaults) {
    std::lock_guard<std::mutex> lock(transport);
    mute();
    std::atomic_store(&profile, p);
    if (defaults)
      for (int i = 0; i < p->count; i++)
        params[i].setValue(p->macros[i].initial);
  }
  ~Live() {
    monitorStopped.store(true);
    monitorWake.notify_all();
    if (portMonitor.joinable())
      portMonitor.join();
    std::lock_guard<std::mutex> lock(transport);
    mute();
  }
  void onReset(const ResetEvent &e) override {
    Module::onReset(e);
    std::lock_guard<std::mutex> lock(transport);
    mute();
  }
  void onRandomize(const RandomizeEvent &e) override {
    std::lock_guard<std::mutex> lock(transport);
    mute();
    Module::onRandomize(e);
  }
  void onBypass(const BypassEvent &) override {
    std::lock_guard<std::mutex> lock(transport);
    mute();
  }
  void process(const ProcessArgs &a) override {
    std::unique_lock<std::mutex> lock(transport, std::try_to_lock);
    if (!lock.owns_lock())
      return;
    auto p = std::atomic_load(&profile);
    bool active = params[ARM].getValue() > .5f && output.getDeviceId() >= 0 &&
                  output.getDevice() &&
                  p->id != "unassigned";
    std::array<float, 8> values{}, volts{};
    for (int i = 0; i < 8; i++) {
      values[i] = params[i].getValue();
      volts[i] = inputs[i].getVoltage();
    }
    auto sendMsg = [&](zoia::Message x) { send(x); };
    try {sender.process(a.sampleTime, p->macros, values, volts, p->count, p->channel,
                   active, sendMsg);} catch(...) {mute();return;}
    if (!active) {
      if(!remoteActive()) try {grid.stop(sendMsg);} catch(...) {mute();}
      try {voice.stop(sendMsg);} catch(...) {mute();}
      noteElapsed = 0.;
    } else {
      noteElapsed += a.sampleTime;
      if (noteElapsed >= .005) {
        noteElapsed = 0.;
        try {voice.process(
            params[TEST].getValue() > .5f ? 0.f : inputs[PITCH].getVoltage(),
            params[TEST].getValue() > .5f ? 10.f : inputs[GATE].getVoltage(),
            100.f / 12.7f, p->channel, true, sendMsg);} catch(...) {mute();}
      }
    }
    lights[0].setBrightness(remoteActive() && (active || grid.page>=0) ? 1 : 0);
  }
  json_t *dataToJson() override {
    std::lock_guard<std::mutex> lock(transport);
    auto o = json_object();
    json_object_set_new(o, "profile", profileJSON(*std::atomic_load(&profile)));
    json_object_set_new(o, "midiOutput", output.toJson());
    return o;
  }
  void dataFromJson(json_t *o) override {
    auto p = parseProfile(json_object_get(o, "profile"));
    if (p)
      apply(p, false);
    std::lock_guard<std::mutex> lock(transport);
    mute();
    auto port = json_object_get(o, "midiOutput");
    if (json_is_object(port))
      output.fromJson(port);
    output.setChannel(-1);
  }
};
struct Definition {
  std::string id, name;
  json_t *catalog;
  json_t *panel;
  std::vector<json_t *> params, inputs, outputs;
  Definition(json_t *source, json_t *layout)
      : id(str(source, "id")), name(str(source, "name")),
        catalog(json_deep_copy(source)), panel(json_deep_copy(layout)) {
    auto c = catalog;
    size_t i;
    json_t *v;
    auto ps = json_object_get(c, "parameters");
    json_array_foreach(ps, i, v) params.push_back(v);
    auto es = json_object_get(c, "endpoints");
    json_array_foreach(es, i, v) {
      auto k = str(v, "kind");
      if (k == "audioInput" || k == "cvInput")
        inputs.push_back(v);
      if (k == "audioOutput" || k == "cvOutput")
        outputs.push_back(v);
    }
  }
  json_t *item(const char *list, const std::string &key) const {
    size_t i;
    json_t *v;
    json_array_foreach(json_object_get(panel, list), i,
                       v) if (str(v, "key") == key) return v;
    throw Exception("Missing shared panel item: %s", key.c_str());
  }
  Vec point(const char *list, json_t *v) const {
    auto o = item(list, str(v, "key"));
    return Vec(json_number_value(json_object_get(o, "x")),
               json_number_value(json_object_get(o, "y")));
  }
  float width() const {
    return json_number_value(json_object_get(panel, "width"));
  }
  ~Definition() {
    json_decref(catalog);
    json_decref(panel);
  }
};
// UI-thread state keyed by Rack's stable cable identity. Every touching module
// saves its active cables, so moving either plug does not change ZOIA strength.
static std::map<int64_t, int> cableStrengths;
// The shared editor bank retains dormant pages/tracks without guessing the
// pedal firmware's binary sequencer layout. It is document state, not DSP.
static std::string validatedSequencerBank(json_t *bank, const std::string &type) {
  if (!bank) return "";
  int version;
  auto values = json_object_get(bank, "values");
  if (type != "Sequencer" || !json_is_object(bank) ||
      json_object_size(bank) != 2 || !integer(bank, "version", 1, 1, version) ||
      !json_is_object(values) || json_object_size(values) > 2048)
    throw Exception("Invalid sequencer page/track state. No preset was applied.");
  static const std::regex cell("^[0-7]:[0-7]:([0-9]|[12][0-9]|3[01])$");
  const char *key; json_t *value;
  json_object_foreach(values, key, value) {
    if (!std::regex_match(key, cell) || !json_is_integer(value) ||
        json_integer_value(value) < 0 || json_integer_value(value) > 65535)
      throw Exception("Invalid sequencer cell. No preset was applied.");
  }
  char *dump = json_dumps(bank, JSON_COMPACT);
  std::string result = dump ? dump : "";
  free(dump);
  return result;
}
struct Authoring : Module {
  std::shared_ptr<Definition> d;
  std::string original;
  std::string sequencerBank;
  // Deliberately not saved/exported: output association and physical page must
  // be checked again after a reload, clone or configuration replacement.
  int64_t remoteController = -1;
  int remotePage = -1, remoteCell = -1, remoteChannel = 1;
  // Rack cable IDs are stable in a saved Rack patch. Strength is ZOIA state,
  // separate from Rack cable colour, which remains a presentation preference.
  std::map<int64_t, int> connectionStrengths;
  int64_t clonedFrom = -1;
  bool cloneWindow = true;
  int connectionStrength(int64_t cableId) const {
    auto shared = cableStrengths.find(cableId);
    if (shared != cableStrengths.end())
      return shared->second;
    // Rack duplicates its cable graph after cloning module JSON, before the
    // first widget step. Recover weights for those newly assigned cable IDs.
    if (cloneWindow && clonedFrom >= 0 && clonedFrom != id) {
      auto cable = APP->engine->getCable(cableId);
      if (cable &&
          (cable->inputModule == this || cable->outputModule == this)) {
        auto originalId = [](Module *m) {
          auto author = dynamic_cast<Authoring *>(m);
          return author && author->cloneWindow && author->clonedFrom >= 0 && author->clonedFrom != author->id ? author->clonedFrom : m->id;
        };
        auto source = originalId(cable->outputModule);
        auto target = originalId(cable->inputModule);
        for (auto otherId : APP->engine->getCableIds()) {
          auto other = APP->engine->getCable(otherId);
          if (otherId != cableId && other->outputModule->id == source &&
              other->inputModule->id == target &&
              other->outputId == cable->outputId &&
              other->inputId == cable->inputId) {
            auto weight = cableStrengths.find(otherId);
            if (weight != cableStrengths.end())
              return weight->second;
          }
        }
      }
    }
    auto cable = APP->engine->getCable(cableId);
    auto source =
        cable ? dynamic_cast<Authoring *>(cable->outputModule) : nullptr;
    if (source && source != this && source->cloneWindow &&
        source->clonedFrom >= 0 && source->clonedFrom != source->id)
      return source->connectionStrength(cableId);
    auto it = connectionStrengths.find(cableId);
    return it == connectionStrengths.end() ? 10000 : it->second;
  }
  Authoring(std::shared_ptr<Definition> def) : d(def) {
    config(d->params.size(), d->inputs.size(), d->outputs.size(), 0);
    for (size_t i = 0; i < d->params.size(); i++) {
      auto p = d->params[i];
      auto quantity = configParam(i, 0, 65535,
                  json_integer_value(json_object_get(p, "defaultRawValue")),
                  str(p, "name"), " raw");
      quantity->snapEnabled = true;
      if(zoia::gainControl(str(d->catalog,"type"),str(p,"key")))
        quantity->description = "Estimated configured gain contribution. Modulation can change actual gain; this is not an audio level measurement.";
    }
    for (size_t i = 0; i < d->inputs.size(); i++)
      configInput(i, str(d->inputs[i], "name") + " — patch connection only");
    for (size_t i = 0; i < d->outputs.size(); i++)
      configOutput(i, str(d->outputs[i], "name") + " — patch connection only");
  }
  json_t *dataToJson() override {
    auto o = json_pack("{s:s}", "configurationId", d->id.c_str());
    json_object_set_new(o, "authoringModuleId", json_integer(id));
    if (!sequencerBank.empty())
      json_object_set_new(o, "sequencerBank", json_loads(sequencerBank.c_str(), 0, nullptr));
    auto weights = json_object();
    for (auto entry : connectionStrengths)
      json_object_set_new(weights, std::to_string(entry.first).c_str(),
                          json_integer(connectionStrength(entry.first)));
    for (auto cableId : APP->engine->getCableIds()) {
      auto cable = APP->engine->getCable(cableId);
      if (cable->inputModule == this || cable->outputModule == this)
        json_object_set_new(weights, std::to_string(cableId).c_str(),
                            json_integer(connectionStrength(cableId)));
    }
    json_object_set_new(o, "connectionStrengths", weights);
    if (!original.empty())
      json_object_set_new(o, "originalDocument",
                          json_loads(original.c_str(), 0, nullptr));
    return o;
  }
  void resetRemoteBinding() {
    if(remoteController>=0) if(auto live=dynamic_cast<Live*>(APP->engine->getModule(remoteController))) live->stopGrid();
    remoteController=-1;remotePage=-1;remoteCell=-1;remoteChannel=1;
  }
  void onReset(const ResetEvent &e) override {resetRemoteBinding();Module::onReset(e);}
  void onRandomize(const RandomizeEvent &e) override {resetRemoteBinding();Module::onRandomize(e);}
  void onBypass(const BypassEvent &) override {resetRemoteBinding();}
  void dataFromJson(json_t *o) override {
    resetRemoteBinding();
    auto nextBank = validatedSequencerBank(json_object_get(o, "sequencerBank"), str(d->catalog, "type"));
    sequencerBank = nextBank;
    auto origin = json_object_get(o, "authoringModuleId");
    clonedFrom = json_is_integer(origin) ? json_integer_value(origin) : -1;
    cloneWindow = true;
    connectionStrengths.clear();
    auto weights = json_object_get(o, "connectionStrengths");
    if (weights && !json_is_object(weights))
      throw Exception("Invalid ZOIA connection strengths");
    const char *key;
    json_t *weight;
    json_object_foreach(weights, key, weight) {
      size_t consumed = 0;
      int64_t cableId = std::stoll(key, &consumed);
      auto raw = json_integer_value(weight);
      if (consumed != std::strlen(key) || cableId < 0 ||
          !json_is_integer(weight) || raw < 0 || raw > 65535)
        throw Exception("Invalid ZOIA connection strength");
      connectionStrengths[cableId] = int(raw);
      cableStrengths[cableId] = int(raw);
    }
    auto v = json_object_get(o, "originalDocument");
    if (json_is_object(v)) {
      char *dump = json_dumps(v, JSON_COMPACT);
      original = dump ? dump : "";
      free(dump);
    }
  }
  // Authoring jacks carry graph structure; this module intentionally implements
  // no DSP.
};
struct ConfigurationCable {
  int64_t id, source, target;
  int output, input, strength;
};
struct ConfigurationPlan {
  std::vector<int> raw;
  std::vector<ConfigurationCable> kept;
  int removed = 0;
};
static int matchingPort(const std::vector<json_t *> &ports, json_t *old) {
  for (size_t i = 0; i < ports.size(); i++)
    if (str(ports[i], "key") == str(old, "key") &&
        str(ports[i], "kind") == str(old, "kind")) return int(i);
  return -1;
}
// A browser-authored free graph is editable even when carrying its Notes and
// interchange identity. Binary-backed or malformed source state stays locked.
static bool protectedOriginal(const Authoring *module) {
  if (module->original.empty()) return false;
  auto document = json_loads(module->original.c_str(), 0, nullptr);
  bool editable = json_is_object(document) && str(document, "authoringMode") == "free" && json_is_null(json_object_get(document, "source"));
  if (document) json_decref(document);
  return !editable;
}
// Plan before touching the engine. Numeric port positions are configuration
// dependent; the canonical key + signal kind is the stable identity.
static ConfigurationPlan planConfiguration(Authoring *old, const Definition &next) {
  if (!old->sequencerBank.empty()) {
    auto bank = json_loads(old->sequencerBank.c_str(), 0, nullptr);
    bool hasValues = json_object_size(json_object_get(bank, "values")) > 0;
    json_decref(bank);
    if (hasValues) throw Exception("This sequencer has extra page/track state. Change its configuration in the browser editor; no data was discarded.");
  }
  if (protectedOriginal(old)) throw Exception("Imported source state is protected. Configuration switching is available for editable Rack40 modules.");
  if (!old->original.empty()) {
    auto document = json_loads(old->original.c_str(), 0, nullptr);
    auto meta = json_object_get(json_object_get(document, "extensions"), "rack-lab.native.v1");
    auto originalId = str(json_object_get(meta, "rackIds"), ("rack-" + std::to_string(old->id)).c_str());
    size_t index; json_t *module;
    bool extraState = false;
    json_array_foreach(json_object_get(document, "modules"), index, module) {
      if (str(module, "id") != originalId) continue;
      const char *key; json_t *value;
      json_object_foreach(json_object_get(module, "opaque"), key, value)
        if (std::string(key) != "experimentalCodec") extraState = true;
    }
    json_decref(document);
    if (extraState) throw Exception("This configuration has extra saved state. Change it in the browser editor; no controls or cables were changed.");
  }
  if (str(old->d->catalog, "type") != str(next.catalog, "type"))
    throw Exception("Choose a configuration of the same ZOIA module type.");
  ConfigurationPlan plan;
  for (auto parameter : next.params) {
    int raw = json_integer_value(json_object_get(parameter, "defaultRawValue"));
    for (size_t i = 0; i < old->d->params.size(); i++)
      if (str(parameter, "key") == str(old->d->params[i], "key"))
        raw = int(std::lround(old->params[i].getValue()));
    plan.raw.push_back(raw);
  }
  for (auto id : APP->engine->getCableIds()) {
    auto c = APP->engine->getCable(id);
    if (c->inputModule != old && c->outputModule != old) continue;
    ConfigurationCable cable{ id, c->outputModule->id, c->inputModule->id,
                               c->outputId, c->inputId, old->connectionStrength(id) };
    if (c->inputModule == old) cable.input = matchingPort(next.inputs, old->d->inputs.at(c->inputId));
    if (c->outputModule == old) cable.output = matchingPort(next.outputs, old->d->outputs.at(c->outputId));
    if (cable.input < 0 || cable.output < 0) plan.removed++;
    else plan.kept.push_back(cable);
  }
  return plan;
}
static void switchConfiguration(int64_t moduleId, Model *model, std::shared_ptr<Definition> def) {
  auto rack = APP->scene->rack;
  auto widget = rack->getModule(moduleId);
  auto old = widget ? dynamic_cast<Authoring *>(widget->module) : nullptr;
  if (!old || old->d->id == def->id) return;
  auto plan = planConfiguration(old, *def);
  auto pos = widget->box.pos;
  bool selected = rack->isSelected(widget);
  // Build replacement module JSON without registering a duplicate engine ID.
  std::unique_ptr<Module> next(model->createModule());
  next->id = moduleId;
  auto author = dynamic_cast<Authoring *>(next.get());
  author->original = old->original;
  author->sequencerBank = old->sequencerBank;
  author->connectionStrengths = old->connectionStrengths;
  author->cloneWindow = false;
  for (const auto &c : plan.kept) author->connectionStrengths[c.id] = c.strength;
  for (size_t i = 0; i < plan.raw.size(); i++) author->params[i].setValue(plan.raw[i]);
  auto add = new history::ModuleAdd;
  add->moduleId = moduleId; add->model = model; add->pos = pos;
  add->moduleJ = next->toJson();
  // A bypassed Rack authoring module remains bypassed, never a pedal command.
  json_object_set_new(add->moduleJ, "bypass", json_boolean(old->isBypassed()));
  auto remove = new history::ModuleRemove;
  remove->setModule(widget);
  auto batch = new history::ComplexAction;
  batch->name = "change ZOIA module configuration";
  for (auto cable : rack->getCompleteCables()) {
    if (cable->cable->inputModule != old && cable->cable->outputModule != old) continue;
    auto action = new history::CableRemove;
    action->setCable(cable); action->redo(); batch->push(action);
  }
  remove->redo(); batch->push(remove);
  add->redo(); batch->push(add);
  auto replacement = rack->getModule(moduleId);
  rack->setModulePosNearest(replacement, pos);
  add->pos = replacement->box.pos;
  if (selected) rack->select(replacement);
  for (const auto &c : plan.kept) {
    auto action = new history::CableAdd;
    action->cableId = c.id; action->outputModuleId = c.source;
    action->inputModuleId = c.target; action->outputId = c.output; action->inputId = c.input;
    for (auto entry : batch->actions) {
      auto original = dynamic_cast<history::CableRemove *>(entry);
      if (original && original->cableId == c.id) action->color = original->color;
    }
    action->redo(); batch->push(action);
  }
  if (APP->history) APP->history->push(batch); else delete batch;
}
// UI-thread snapshot: hardware codec validation remains a separate layer.
static json_t *authorDocument(const std::string &name) {
  auto root = json_pack(
      "{s:s,s:i,s:s,s:s,s:s,s:[],s:[],s:[],s:[],s:[],s:n,s:{},s:{},s:{s:i,s:i}"
      "}",
      "format", "zoia-patch", "schemaVersion", 1, "documentId", "rack-authored",
      "name", name.c_str(), "authoringMode", "free", "modules", "connections",
      "pages", "starred", "colors", "source", "opaque", "extensions",
      "sequences", "nextModule", 0, "nextConnection", 0);
  std::map<int64_t, Authoring *> authors;
  auto modules = json_object_get(root, "modules");
  auto maps = json_array();
  for (auto id : APP->engine->getModuleIds()) {
    auto node = APP->engine->getModule(id);
    if (auto live = dynamic_cast<Live *>(node)) {
      auto p = std::atomic_load(&live->profile);
      if (p->id != "unassigned")
        json_array_append_new(maps, profileJSON(*p));
      continue;
    }
    auto m = dynamic_cast<Authoring *>(node);
    if (!m)
      continue;
    if (m->isBypassed()) {
      json_decref(root);
      json_decref(maps);
      throw Exception("Rack module bypass is not a ZOIA module option. "
                      "Unbypass the Rack40 module before exporting.");
    }
    if (!m->original.empty()) {
      json_decref(root);
      json_decref(maps);
      throw Exception(
          "This Rack graph contains a preserved imported document. Save the "
          "Rack file and use the file bridge to retain its original state.");
    }
    authors[id] = m;
    auto c = m->d->catalog;
    auto mod = json_object();
    auto moduleId = "rack-" + std::to_string(id);
    json_object_set_new(mod, "id", json_string(moduleId.c_str()));
    json_object_set_new(mod, "configurationId", json_string(m->d->id.c_str()));
    json_object_set_new(mod, "name",
                        json_string(str(c, "type").substr(0, 16).c_str()));
    json_object_set(mod, "type", json_object_get(c, "type"));
    json_object_set(mod, "category", json_object_get(c, "category"));
    auto ps = json_array();
    for (size_t i = 0; i < m->d->params.size(); i++) {
      auto p = m->d->params[i];
      auto v = json_object();
      json_object_set_new(
          v, "id", json_string(("parameter-" + std::to_string(i)).c_str()));
      json_object_set(v, "key", json_object_get(p, "key"));
      json_object_set_new(v, "kind", json_string("parameter"));
      json_object_set(v, "name", json_object_get(p, "name"));
      json_object_set_new(
          v, "rawValue",
          json_integer(int(std::lround(m->params[i].getValue()))));
      json_object_set_new(v, "displayValue", json_string("Raw value"));
      json_object_set_new(v, "decoded", json_true());
      for (auto field : {"unit", "range", "defaultRawValue"}) {
        auto value = json_object_get(p, field);
        if (value)
          json_object_set(v, field, value);
      }
      json_array_append_new(ps, v);
    }
    auto options = json_object_get(c, "options");
    size_t i;
    json_t *o;
    json_array_foreach(options, i, o) {
      auto v = json_object();
      json_object_set_new(v, "id",
                          json_string(("option-" + std::to_string(i)).c_str()));
      json_object_set(v, "key", json_object_get(o, "key"));
      json_object_set(v, "name", json_object_get(o, "name"));
      json_object_set_new(v, "kind", json_string("option"));
      json_object_set(v, "rawValue", json_object_get(o, "selectedValue"));
      json_object_set_new(v, "displayValue", json_string("Selected variant"));
      json_object_set_new(v, "decoded", json_true());
      json_array_append_new(ps, v);
    }
    json_object_set_new(mod, "parameters", ps);
    json_object_set(mod, "endpoints", json_object_get(c, "endpoints"));
    json_object_set_new(mod, "hardware", json_null());
    if (!m->sequencerBank.empty()) {
      auto opaque = json_object();
      json_object_set_new(opaque, "rack40SequencerBankV1", json_loads(m->sequencerBank.c_str(), 0, nullptr));
      json_object_set_new(mod, "opaque", opaque);
    }
    json_array_append_new(modules, mod);
  }
  auto cleanup = [&]() {
    json_decref(root);
    json_decref(maps);
  };
  if (authors.empty()) {
    cleanup();
    throw Exception("Add ZOIA modules before exporting.");
  }
  auto connections = json_object_get(root, "connections");
  for (auto id : APP->engine->getCableIds()) {
    auto cable = APP->engine->getCable(id);
    auto s = authors.find(cable->outputModule->id),
         t = authors.find(cable->inputModule->id);
    if (s == authors.end() && t == authors.end())
      continue;
    if (s == authors.end() || t == authors.end()) {
      cleanup();
      throw Exception("Rack40 patch sockets cannot connect to ordinary Rack "
                      "DSP modules in an exported patch.");
    }
    auto se = s->second->d->outputs.at(cable->outputId),
         te = t->second->d->inputs.at(cable->inputId);
    auto kind = str(se, "kind") == "audioOutput" ? "audio" : "cv";
    if (str(te, "kind") !=
        (std::string(kind) == "audio" ? "audioInput" : "cvInput")) {
      cleanup();
      throw Exception("Incompatible audio/CV cable in ZOIA graph.");
    }
    auto e = json_pack(
        "{s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:s,s:i}", "id",
        ("rack-cable-" + std::to_string(id)).c_str(), "sourceModuleId",
        ("rack-" + std::to_string(s->first)).c_str(), "targetModuleId",
        ("rack-" + std::to_string(t->first)).c_str(), "sourceEndpointId",
        str(se, "id").c_str(), "targetEndpointId", str(te, "id").c_str(),
        "sourceEndpoint", str(se, "name").c_str(), "targetEndpoint",
        str(te, "name").c_str(), "kind", kind, "strengthRaw",
        t->second->connectionStrength(id));
    if (APP->scene && APP->scene->rack) {
      auto drawing = APP->scene->rack->getCable(id);
      if (drawing) json_object_set_new(e, "cableColor", json_string(color::toHexString(drawing->color).c_str()));
    }
    json_array_append_new(connections, e);
  }
  std::string identity =
      json_array_size(maps) == 1
          ? str(json_object_get(json_array_get(maps, 0), "patch"), "documentId")
          : "rack-" + std::to_string(authors.begin()->first);
  json_object_set_new(root, "documentId", json_string(identity.c_str()));
  json_object_set_new(json_object_get(root, "extensions"),
                      "rack-lab.controlMaps.v1", maps);
  auto seq = json_object_get(root, "sequences");
  json_object_set_new(seq, "nextModule", json_integer(authors.size()));
  json_object_set_new(seq, "nextConnection",
                      json_integer(json_array_size(connections)));
  return root;
}
static void exportDocumentDialog() {
  char *path =
      osdialog_file(OSDIALOG_SAVE, nullptr, "My Patch.zoia.json", nullptr);
  if (!path)
    return;
  std::string filename = path;
  free(path);
  auto slash = filename.find_last_of("/\\");
  auto name = filename.substr(slash == std::string::npos ? 0 : slash + 1);
  auto suffix = name.find(".zoia.json");
  if (suffix != std::string::npos)
    name = name.substr(0, suffix);
  try {
    auto root = authorDocument(name.empty() ? "Rack Patch" : name);
    int result = zoia::saveJSON(root, filename);
    json_decref(root);
    if (result)
      warn("Could not write ZOIA document.");
  } catch (const std::exception &e) {
    warn(e.what());
  }
}

// Rack40 presentation is independent of ZOIA module colour metadata.
static NVGcolor brandColor(const char *key, const char *family = nullptr) {
  static json_t *tokens = readJSON(asset::plugin(pluginInstance, "res/brand/tokens.json"));
  auto palette = json_object_get(json_object_get(tokens, "color"),
                                settings::preferDarkPanels ? "dark" : "light");
  auto value = family ? str(json_object_get(palette, "module"), family) : str(palette, key);
  return color::fromHexString(value);
}
static const char *brandFamily(const Definition *d) {
  if (!d) return "interface";
  auto type = str(d->catalog, "type"), category = str(d->catalog, "category");
  std::transform(type.begin(), type.end(), type.begin(), ::tolower);
  std::transform(category.begin(), category.end(), category.begin(), ::tolower);
  if (type == "audio input" || type == "audio output") return "audio";
  if (category.find("effect") != std::string::npos) return "effect";
  for (auto name : {"control", "analysis", "logic", "sequence"})
    if (category.find(name) != std::string::npos) return "control";
  if (category == "cv") return "control";
  if (category.find("interface") != std::string::npos || category.find("midi") != std::string::npos) return "interface";
  return "audio";
}
static json_t *categoryAccent(const Definition *d) {
  static json_t *system = readJSON(asset::plugin(pluginInstance, "res/category-colors.v3.json"));
  return d ? json_object_get(json_object_get(system, "modules"), str(d->catalog,"type").c_str()) : nullptr;
}
static NVGcolor panelAccent(const Definition *d) {
  static json_t *system = readJSON(asset::plugin(pluginInstance, "res/category-colors.v3.json"));
  auto entry = categoryAccent(d);
  if (!entry) return brandColor("", brandFamily(d));
  auto palette = json_object_get(json_object_get(system,"palette"),str(entry,"accent").c_str());
  return color::fromHexString(str(palette,settings::preferDarkPanels ? "dark" : "light"));
}
static void label(NVGcontext *vg, float x, float y, std::string s,
                  float size = 10, bool display = false) {
  bool mono = s.find("raw ") == 0 || s.find("CC ") == 0 || s.find("CHANNEL ") == 0;
  auto f = APP->window->loadFont(asset::plugin(pluginInstance,
      mono ? "res/brand/jetbrains-mono.ttf" : "res/brand/archivo.ttf"));
  nvgFontFaceId(vg, f->handle);
  nvgFontSize(vg, size);
  nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
  nvgFillColor(vg, display ? nvgRGB(235, 237, 240) : brandColor("text"));
  nvgText(vg, x, y, s.c_str(), nullptr);
}
struct Face : Widget {
  zoia::SequencerPreview sequencerPreview;
  double previewTime = 0;
  int sequenceOption(const char *key, int fallback) const {
    if (!d) return fallback;
    size_t i; json_t *option;
    json_array_foreach(json_object_get(d->catalog,"options"), i, option) {
      if (str(option,"key") == key) {
        auto value = json_object_get(option,"selectedValue");
        return json_is_integer(value) ? int(json_integer_value(value)) : fallback;
      }
    }
    return fallback;
  }
  void step() override {
    Widget::step();
    double now = system::getTime();
    if (previewTime > 0 && d && str(d->catalog,"type") == "Sequencer")
      sequencerPreview.advance(now-previewTime,sequenceOption("number_of_steps",1),sequenceOption("number_of_pages",1));
    previewTime = now;
  }
  Live *live = nullptr;
  Authoring *author = nullptr;
  std::shared_ptr<Definition> d;
  float configuredRaw(const std::string &key) const {
    if (!d) return 0;
    for (size_t i=0; i<d->params.size(); ++i)
      if (str(d->params[i], "key") == key)
        return author ? author->params[i].getValue() : json_number_value(json_object_get(d->params[i], "defaultRawValue"));
    return 0;
  }
  void onButton(const event::Button &e) override {
    if (author && d && str(d->catalog,"type")=="Quantizer" && e.button==GLFW_MOUSE_BUTTON_LEFT && e.action==GLFW_PRESS && e.pos.x>=15 && e.pos.x<105 && e.pos.y>=92 && e.pos.y<156) {
      int semitones=std::round(configuredRaw("cv_input")/65535.f*120.f);
      int index=std::min(5, int((e.pos.x-15)/15)) + (e.pos.y>=124 ? 6:0);
      float next=std::min(120,(semitones/12)*12+index)/120.f*65535.f;
      auto old=author->toJson();
      for(size_t i=0;i<d->params.size();++i) if(str(d->params[i],"key")=="cv_input") author->params[i].setValue(std::round(next));
      if(APP->history){auto h=new history::ModuleChange;h->name="set ZOIA quantizer input bias";h->moduleId=author->id;h->oldModuleJ=old;h->newModuleJ=author->toJson();APP->history->push(h);}else json_decref(old);
      e.consume(this); return;
    }
    Widget::onButton(e);
  }
  void draw(const DrawArgs &a) override {
    auto vg = a.vg;
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, box.size.x, box.size.y);
    auto family = brandColor("", brandFamily(d.get()));
    bool dark = settings::preferDarkPanels;
    auto top = dark ? nvgRGB(68, 70, 74) : nvgRGB(246, 245, 242);
    auto bottom = dark ? nvgRGB(37, 39, 42) : nvgRGB(191, 193, 197);
    nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, box.size.x, 45, top, bottom));
    nvgFill(vg);
    nvgStrokeColor(vg, dark ? nvgRGB(23, 24, 26) : nvgRGB(146, 149, 154));
    nvgStrokeWidth(vg, 1);
    nvgStroke(vg);
    // Original metal brushing and bevels; no VCV artwork is copied.
    for (float y = 1; y < box.size.y; y += 3) {
      nvgBeginPath(vg); nvgMoveTo(vg, 1, y); nvgLineTo(vg, box.size.x - 1, y);
      nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 4)); nvgStrokeWidth(vg, .5f); nvgStroke(vg);
    }
    nvgBeginPath(vg); nvgMoveTo(vg, 1, box.size.y - 1); nvgLineTo(vg, 1, 1); nvgLineTo(vg, box.size.x - 1, 1);
    nvgStrokeColor(vg, nvgRGBA(255, 255, 255, dark ? 40 : 160)); nvgStroke(vg);
    nvgBeginPath(vg); nvgMoveTo(vg, 15, d ? 40 : 29); nvgLineTo(vg, box.size.x - 15, d ? 40 : 29);
    nvgStrokeColor(vg, panelAccent(d.get())); nvgStrokeWidth(vg, 2); nvgStroke(vg);
    for (auto x : {6.f, box.size.x - 6.f}) for (auto y : {6.f, box.size.y - 6.f}) {
      nvgBeginPath(vg); nvgCircle(vg, x, y, 3.5f);
      nvgFillPaint(vg, nvgLinearGradient(vg, x - 3, y - 3, x + 3, y + 3, nvgRGB(225, 228, 232), nvgRGB(85, 90, 98))); nvgFill(vg);
      nvgStrokeColor(vg, nvgRGB(30, 32, 36)); nvgStrokeWidth(vg, .7f); nvgStroke(vg);
      nvgBeginPath(vg); nvgMoveTo(vg, x - 2, y + 1); nvgLineTo(vg, x + 2, y - 1); nvgStroke(vg);
    }
    // The supplied Rack40 lockup identifies every module, including narrow panels.
    {
      auto logo = APP->window->loadSvg(asset::plugin(pluginInstance,
          settings::preferDarkPanels ? "res/brand/rack40-horizontal-on-dark.svg" : "res/brand/rack40-horizontal-on-light.svg"));
      float logoWidth = d ? std::min(box.size.x - 30.f, 140.f) : 105.f;
      float logoHeight = logoWidth * logo->getSize().y / logo->getSize().x;
      nvgSave(vg);
      nvgTranslate(vg, (box.size.x - logoWidth) / 2, d ? 6 : 9);
      nvgScale(vg, logoHeight / logo->getSize().y, logoHeight / logo->getSize().y);
      logo->draw(vg);
      nvgRestore(vg);
    }
    if (!d) {
      auto p =
          live ? std::atomic_load(&live->profile) : std::make_shared<Profile>();
      label(vg, box.size.x / 2, 40, "LIVE CONTROL", 11);
      label(vg, box.size.x / 2, 61, p->name.substr(0, 27));
      label(vg, box.size.x / 2, 79, "ZOIA HARDWARE REQUIRED", 7);
      label(vg, box.size.x / 2, 99,
            p->id == "unassigned"                       ? "IMPORT A CONTROL MAP"
            : (!live || live->output.getDeviceId() < 0) ? "SELECT MIDI OUTPUT"
            : (live && live->params[Live::ARM].getValue() > .5f)
                ? "LIVE MIDI ENABLED"
                : "MUTED");
      for (int i = 0; i < 8; i++) {
        // Give each macro a complete vertical band: name, knob, value and CV
        // input. The compact internal offsets reserve a clear gap between the
        // first-row jack artwork and the second-row name.
        float x = 42 + (i % 4) * 63, y = 132 + (i / 4) * 88;
        label(vg, x, y - 20,
              i < p->count ? p->labels[i].substr(0, 11) : "UNUSED", 8);
        label(vg, x, y + 28,
              i < p->count ? "CC " + std::to_string(p->macros[i].cc) : "—", 8);
      }
      label(vg, 42, 288, "LIVE", 9);
      label(vg, 112, 288, "TEST NOTE", 9);
      label(vg, 190, 288, "PITCH", 9);
      label(vg, 251, 288, "GATE", 9);
      label(vg, box.size.x / 2, 344, "RIGHT CLICK: MIDI OUTPUT / IMPORT MAP",
            8);
      label(vg, box.size.x / 2, 359, "AUDIO: ZOIA → AMP", 9);
    } else if (d) {
      label(vg, box.size.x / 2, 52, str(d->panel, "title"), 12);
      auto category = categoryAccent(d.get());
      auto categoryText = category ? str(category,"label") : std::string(brandFamily(d.get()));
      std::transform(categoryText.begin(), categoryText.end(), categoryText.begin(), ::toupper);
      label(vg, box.size.x / 2, 31, categoryText, 7);
      nvgFillColor(vg,panelAccent(d.get())); nvgText(vg,box.size.x / 2,31,categoryText.c_str(),nullptr);
      // A short, centered status line prevents authoring-only modules from
      // being mistaken for standalone VCV DSP processors. Keep it neutral so
      // it reads as supporting information rather than another category label.
      {
        constexpr float statusY = 65.f;
        label(vg, box.size.x / 2.f, statusY, "ZOIA HARDWARE REQUIRED", 5.5f);
        nvgFillColor(vg, nvgRGB(18, 18, 20));
        nvgText(vg, box.size.x / 2.f, statusY, "ZOIA HARDWARE REQUIRED", nullptr);
      }
      if (str(d->catalog,"type")=="Sequencer") {
        int count=std::max(1,std::min(32,sequenceOption("number_of_steps",1)));
        for(int i=0;i<32;i++) {
          bool lit=sequencerPreview.running && sequencerPreview.page(count)==0 && sequencerPreview.step(count)==i;
          nvgBeginPath(vg);nvgCircle(vg,42+(i%8)*80+24,144+(i/8)*44-20,2.5f);
          nvgFillColor(vg,lit?nvgRGB(255,212,90):nvgRGB(65,68,65));nvgFill(vg);
        }
      }
      size_t illustrationIndex;
      json_t *illustration;
      json_array_foreach(json_object_get(d->panel, "illustrations"), illustrationIndex, illustration) {
        auto kind=str(illustration,"kind");
        float x=json_number_value(json_object_get(illustration,"x")),y=json_number_value(json_object_get(illustration,"y"));
        float w=json_number_value(json_object_get(illustration,"width")),h=json_number_value(json_object_get(illustration,"height"));
        auto displayLabel = [&](float xx, float yy, const std::string &text, float size) { label(vg, xx, yy, text, size, true); };
        nvgBeginPath(vg); nvgRect(vg,x,y,w,h);nvgFillColor(vg,nvgRGB(18,21,24));nvgFill(vg);
        if(kind=="pitch-reference") {
          const char* names[]={"A","A#","B","C","C#","D","D#","E","F","F#","G","G#"};
          int index=int(std::round(configuredRaw("cv_input")/65535.f*120.f))%12;
          for(int i=0;i<12;i++){float kx=x+(i%6)*w/6,ky=y+(i/6)*32; nvgBeginPath(vg);nvgRect(vg,kx+1,ky+1,w/6-2,30);nvgFillColor(vg,i==index?family:nvgRGB(105,109,115));nvgFill(vg);displayLabel(kx+w/12,ky+16,names[i],7);}
          displayLabel(x+w/2,y+76,"INPUT BIAS",7);displayLabel(x+w/2,y+89,"NOT OUTPUT READBACK",6);
        } else {
          auto norm=[&](const char* key){return configuredRaw(key)/65535.f;};
          nvgBeginPath(vg);
          if(kind=="envelope-preview") {
            auto hasParameter=[&](const char *key){for(auto p:d->params)if(str(p,"key")==key)return true;return false;};
            auto optionOff=[&](const char *key){size_t oi;json_t *option;json_array_foreach(json_object_get(d->catalog,"options"),oi,option)if(str(option,"key")==key)return str(option,"selectedValue")=="off";return false;};
            auto milliseconds=[&](const char *key){
              for(auto p:d->params) {
                if(str(p,"key")!=key)continue;
                auto range=json_object_get(p,"range");size_t count=json_array_size(range);
                double raw=std::max(0.f,std::min(65535.f,configuredRaw(key)))/65535.,value=-1;
                if(count==2)value=json_number_value(json_array_get(range,0))+(json_number_value(json_array_get(range,1))-json_number_value(json_array_get(range,0)))*raw;
                else if(count==5){double at=raw*4;int j=std::min(3,int(at));double f=std::pow(at-j,1.6);value=json_number_value(json_array_get(range,j))+(json_number_value(json_array_get(range,j+1))-json_number_value(json_array_get(range,j)))*f;}
                return str(p,"unit")=="s"?value*1000:str(p,"unit")=="ms"?value:-1.;
              }
              return -1.;
            };
            bool sustain=!optionOff("str")&&hasParameter("sustain");
            auto shape=zoia::envelopePreview({milliseconds("attack"),milliseconds("decay"),sustain?milliseconds("release"):0,
              !optionOff("initial_delay")&&hasParameter("delay")?milliseconds("delay"):0,
              !optionOff("hold_attack_decay")&&hasParameter("hold_attack_decay")?milliseconds("hold_attack_decay"):0,
              sustain&&!optionOff("hold_sustain_release")&&hasParameter("hold_sustain_release")?milliseconds("hold_sustain_release"):0},norm("sustain"),sustain);
            if(shape.valid) {
              for(size_t i=0;i<shape.points.size();++i) {
                float px=x+3+(w-6)*shape.points[i].first/shape.windowMs,py=y+5+(h-28)*(1-shape.points[i].second);
                if(!i)nvgMoveTo(vg,px,py);else nvgLineTo(vg,px,py);
              }
              displayLabel(x+w/2,y+h-14,string::f("~ %.1f s VIEW / %s",shape.windowMs/1000,sustain?"0.5 s SUSTAIN HOLD":"AD ONLY"),5);
            } else displayLabel(x+w/2,y+h/2,"TIMING METADATA UNAVAILABLE",5);

          } else if(kind=="waveform-preview") {
            std::string shape;
            size_t oi;json_t *option;json_array_foreach(json_object_get(d->catalog,"options"),oi,option)if(str(option,"key")=="waveform")shape=str(option,"selectedValue");
            double hz=0;
            for(auto parameter:d->params) {
              auto key=str(parameter,"key"),unit=str(parameter,"unit");
              if(key!="frequency" && key!="cv_control" && key!="tap_control") continue;
              auto range=json_object_get(parameter,"range");size_t count=json_array_size(range);
              double raw=std::max(0.f,std::min(65535.f,configuredRaw(key)))/65535.;
              if(count==2) hz=json_number_value(json_array_get(range,0))+(json_number_value(json_array_get(range,1))-json_number_value(json_array_get(range,0)))*raw;
              else if(count==5){double at=raw*4;int j=std::min(3,int(at));double t=std::pow(at-j,1.6);hz=json_number_value(json_array_get(range,j))+(json_number_value(json_array_get(range,j+1))-json_number_value(json_array_get(range,j)))*t;}
              if(unit=="BPM") hz/=60;
            }
            double window=str(d->catalog,"type")=="Oscillator"?.01:1,cycles=hz*window;
            if(shape=="random" || cycles>32) { displayLabel(x+w/2,y+(h-16)/2,shape=="random"?"RANDOM: NOT PREDICTABLE":"RATE BEYOND WINDOW",6); }
            else {
              int samples=std::max(128,int(std::ceil(cycles*32)));
              for(int i=0;i<=samples;i++){double phase=i/double(samples)*cycles,p=phase-std::floor(phase);float v=0;
                if(shape=="sine")v=std::sin(phase*2*M_PI);else if(shape=="square")v=p<.5?1:-1;else if(shape=="triangle")v=1-4*std::abs(p-.5);else if(shape=="sawtooth")v=2*p-1;else if(shape=="ramp")v=1-2*p;
                float py=y+(h-16)/2-v*(h-24)/2;if(i==0)nvgMoveTo(vg,x,py);else nvgLineTo(vg,x+w*i/samples,py);
              }
            }
          } else if(kind=="level-preview") {
            int n=0;for(auto p:d->params){auto key=str(p,"key");if(key.find("gain_")==0||key.find("atten_")==0||key.find("step_")==0)n++;}
            int i=0;for(auto p:d->params){auto key=str(p,"key");if(key.find("gain_")!=0&&key.find("atten_")!=0&&key.find("step_")!=0)continue;float level=configuredRaw(key)/65535.f*(h-20);nvgRect(vg,x+i*w/n+3,y+h-16-level,w/n-6,level);i++;}nvgFillColor(vg,family);nvgFill(vg);
          }
          nvgStrokeColor(vg,family);nvgStrokeWidth(vg,1.2f);nvgStroke(vg);displayLabel(x+w/2,y+h-6,kind=="envelope-preview"?"ESTIMATED TIMING / NOT MEASURED":"CONFIGURED SCHEMATIC",6);
        }
      }
      size_t gi;
      json_t *group;
      json_array_foreach(json_object_get(d->panel, "groups"), gi, group) {
        float y = json_number_value(json_object_get(group, "y"));
        nvgBeginPath(vg);
        nvgMoveTo(vg, 15, y - 6);
        nvgLineTo(vg, box.size.x - 15, y - 6);
        nvgStrokeColor(vg, brandColor("line"));
        nvgStrokeWidth(vg, 1);
        nvgStroke(vg);
        label(vg, box.size.x / 2, y, str(group, "label"), 8);
      }
      if (str(d->catalog,"type")=="Sequencer") {
        int steps=0, pages=1, tracks=1;
        for(auto p:d->params) if(str(p,"key").find("step_")==0) steps++;
        size_t oi; json_t *option;
        json_array_foreach(json_object_get(d->catalog,"options"),oi,option) {
          auto key=str(option,"key");
          if(key=="number_of_pages") pages=int(json_integer_value(json_object_get(option,"selectedValue")));
          if(key=="num_of_tracks") tracks=int(json_integer_value(json_object_get(option,"selectedValue")));
        }
        label(vg,box.size.x/2,76,string::f("%d STEPS / %d TRACKS / %d PAGES",steps,tracks,pages),8);
        label(vg,box.size.x/2,106,"STEP VALUES - FIRST TRACK / FIRST PAGE",8);
        for(int step=steps;step<32;step++) {
          float x=42+(step%8)*80,y=144+(step/8)*44;
          nvgBeginPath(vg);nvgCircle(vg,x,y,13);
          nvgFillColor(vg,nvgRGBA(95,98,100,60));nvgFill(vg);
          nvgStrokeColor(vg,nvgRGBA(100,103,105,90));nvgStrokeWidth(vg,1);nvgStroke(vg);
          label(vg,x,y-18,string::f("STEP %d",step+1),6);
          label(vg,x,y+18,"INACTIVE",5);
        }
      }
      for (size_t i = 0; i < d->params.size(); i++) {
        auto pos = d->point("controls", d->params[i]);
        auto controlItem = d->item("controls", str(d->params[i], "key"));
        auto controlLabel = str(controlItem, "label");
        label(vg, pos.x, pos.y - (str(d->catalog,"type")=="Sequencer"?18:23), controlLabel.empty() ? str(d->params[i], "name") : controlLabel, str(d->catalog,"type")=="Sequencer"?6:8);
        auto raw = author ? int(author->params[i].getValue())
                          : int(json_integer_value(json_object_get(
                                d->params[i], "defaultRawValue")));
        label(vg, pos.x, pos.y + (str(d->catalog,"type")=="Sequencer"?18:25), "raw " + std::to_string(raw), str(d->catalog,"type")=="Sequencer"?6:7);
        if(zoia::gainControl(str(d->catalog,"type"),str(d->params[i],"key")) && str(d->params[i],"unit")=="dB") {
          std::vector<double> range;size_t ri;json_t *point;
          json_array_foreach(json_object_get(d->params[i],"range"),ri,point)range.push_back(json_is_number(point)?json_number_value(point):std::numeric_limits<double>::quiet_NaN());
          int lit=zoia::gainSegments(zoia::configuredGainDb(raw,range,json_number_value(json_object_get(d->params[i],"defaultRawValue"))));
          if(lit>=0) {
            const bool prominent=str(d->catalog,"type")=="VCA";
            if(prominent) {
              nvgBeginPath(vg);nvgRect(vg,box.size.x/2-21,194,42,94);
              nvgFillColor(vg,nvgRGB(12,17,14));nvgFill(vg);
              nvgStrokeColor(vg,nvgRGB(170,183,173));nvgStrokeWidth(vg,1);nvgStroke(vg);
            }
            for(int segment=0;segment<20;segment++) {
              nvgBeginPath(vg);
              if(prominent)nvgRect(vg,box.size.x/2-17,284-(segment+1)*2.4f-segment*2,34,2.4f);
              else nvgRect(vg,pos.x+20,pos.y+14-segment*1.5f,4,1);
              nvgFillColor(vg,segment<lit?(segment>=18?nvgRGB(255,166,57):nvgRGB(255,220,36)):(prominent?nvgRGB(102,117,107):nvgRGB(38,56,42)));nvgFill(vg);
            }
          }
        }
      }
      if(str(d->catalog,"type")=="Pushbutton") {
        label(vg,box.size.x/2,269,"RIGHT CLICK: LIVE MIDI SETUP",5.5);
        label(vg,box.size.x/2,282,"VERIFY PEDAL PAGE / NO MENU",5.5);
      }
      auto drawPort = [&](json_t *port) {
        if (str(d->catalog,"type")=="Sequencer" && str(port,"kind")=="cvInput") return;
        auto item = d->item("ports", str(port, "key"));
        auto pos = d->point("ports", port);
        bool paired = json_is_string(json_object_get(item, "parameterKey"));
        if (!paired)
          label(vg, pos.x, pos.y - 20, str(item, "label"), 7);
        if (paired)
          label(vg, pos.x - 25, pos.y, "CV", 7);
        else if (pos.y <= 338)
          // The socket name above remains visible. Omit a redundant kind
          // caption when it would collide with the fixed panel footer.
          label(vg, pos.x, pos.y + 24,
                str(port, "kind").find("audio") == 0
                    ? (str(port, "kind") == "audioInput" ? "AUDIO IN"
                                                         : "AUDIO OUT")
                    : (paired || str(port, "kind") == "cvInput" ? "CV IN"
                                                                : "CV OUT"),
                7);
      };
      for (auto port : d->inputs)
        drawPort(port);
      for (auto port : d->outputs)
        drawPort(port);
      label(vg, box.size.x / 2, 370, "Rack40", 8);
    } else {
      label(vg, box.size.x / 2, 40, "LIVE CONTROL", 12);
      label(vg, box.size.x / 2, 62, "ZOIA HARDWARE REQUIRED", 8);
      label(vg, box.size.x / 2, 82, "MIDI performance for your ZOIA patch", 9);
    }
  }
};
struct ZoiaKnob : app::Knob {
  ZoiaKnob() {
    box.size = Vec(30, 30);
    minAngle = -.75 * M_PI;
    maxAngle = .75 * M_PI;
  }
  void draw(const DrawArgs &a) override {
    float v = getParamQuantity() ? getParamQuantity()->getScaledValue() : 0;
    auto vg = a.vg;
    nvgBeginPath(vg);
    nvgCircle(vg, 15, 15, 14);
    nvgFillPaint(vg, nvgRadialGradient(vg, 10, 7, 2, 21, nvgRGB(101, 105, 112), nvgRGB(14, 16, 19)));
    nvgFill(vg);
    nvgStrokeColor(vg, nvgRGB(147, 152, 158)); nvgStrokeWidth(vg, 1.5f); nvgStroke(vg);
    float angle = minAngle + v * (maxAngle - minAngle);
    nvgBeginPath(vg);
    nvgMoveTo(vg, 15, 15);
    nvgLineTo(vg, 15 + 11 * std::sin(angle), 15 - 11 * std::cos(angle));
    nvgStrokeColor(vg, nvgRGB(238, 236, 231));
    nvgStrokeWidth(vg, 2);
    nvgStroke(vg);
  }
};
// Same original vector as the browser: smaller visible hardware with standard
// Rack cable handling and unchanged jack centres / interaction bounds.
struct Rack40Port : app::SvgPort {
  Rack40Port() {
    setSvg(APP->window->loadSvg(asset::plugin(pluginInstance, "res/components/rack40-jack.svg")));
    shadow->opacity = 0.f;
  }
};
// Rack's standard Info submenu captures its ModuleWidget. A patch replacement
// can leave that menu open after the widget is deleted. Close only this widget's
// menu, deferring destruction so configuration/delete callbacks can finish safely.
struct MenuLifetimeModuleWidget : ModuleWidget {
  WeakPtr<ui::MenuOverlay> contextMenuOverlay;
  void appendContextMenu(Menu *menu) override {
    contextMenuOverlay = menu->getAncestorOfType<ui::MenuOverlay>();
  }
  void onRemove(const RemoveEvent &e) override {
    if (auto overlay = contextMenuOverlay.get()) {
      overlay->hide();
      overlay->requestDelete();
    }
    ModuleWidget::onRemove(e);
  }
};
struct LiveWidget : MenuLifetimeModuleWidget {
  LiveWidget(Live *m) {
    setModule(m);
    box.size = Vec(285, 380);
    auto f = new Face;
    f->live = m;
    f->box.size = box.size;
    addChild(f);
    for (int i = 0; i < 8; i++) {
      float x = 42 + (i % 4) * 63, y = 132 + (i / 4) * 88;
      addParam(createParamCentered<ZoiaKnob>(Vec(x, y), m, i));
      addInput(createInputCentered<Rack40Port>(Vec(x, y + 44), m, i));
    }
    addParam(createParamCentered<CKSS>(Vec(42, 312), m, Live::ARM));
    addParam(createParamCentered<TL1105>(Vec(112, 312), m, Live::TEST));
    addInput(createInputCentered<Rack40Port>(Vec(190, 312), m, Live::PITCH));
    addInput(createInputCentered<Rack40Port>(Vec(251, 312), m, Live::GATE));
  }
  void appendContextMenu(Menu *menu) override {
    MenuLifetimeModuleWidget::appendContextMenu(menu);
    auto m = dynamic_cast<Live *>(module);
    if (!m)
      return;
    menu->addChild(new MenuSeparator);
    menu->addChild(createMenuItem("Export ZOIA patch…", "",
                                  []() { exportDocumentDialog(); }));
    menu->addChild(createSubmenuItem("MIDI output", "", [m](Menu *sub) {
      for (int driver : midi::getDriverIds()) {
        auto d = midi::getDriver(driver);
        if (!d)
          continue;
        sub->addChild(
            createSubmenuItem(d->getName(), "", [m, d, driver](Menu *dev) {
              dev->addChild(createMenuItem("No device", "", [m, driver]() {
                m->selectPort(driver, -1);
              }));
              for (int id : d->getOutputDeviceIds())
                dev->addChild(createCheckMenuItem(
                    d->getOutputDeviceName(id), "",
                    [m, driver, id]() {
                      return m->output.getDriverId() == driver &&
                             m->output.getDeviceId() == id;
                    },
                    [m, driver, id]() { m->selectPort(driver, id); }));
            }));
      }
    }));
    std::vector<std::string> channels;
    for (int i = 1; i <= 16; i++)
      channels.push_back(std::to_string(i));
    menu->addChild(createIndexSubmenuItem(
        "MIDI channel", channels,
        [m]() { return size_t(std::atomic_load(&m->profile)->channel); },
        [m](size_t ch) {
          auto p = std::make_shared<Profile>(*std::atomic_load(&m->profile));
          p->channel = ch;
          m->apply(p, false);
        }));
    menu->addChild(createMenuItem("Import control map…", "", [m]() {
      char *path = osdialog_file(OSDIALOG_OPEN, nullptr, nullptr, nullptr);
      if (!path)
        return;
      auto root = readJSON(path);
      free(path);
      auto p = parseProfile(root);
      if (root)
        json_decref(root);
      if (!p) {
        warn("Invalid ZOIA control map: use version 1, 1–8 controls, unique "
             "CCs and valid ranges. CC60–63 and CC120–127 are reserved.");
        return;
      }
      m->apply(p, true);
    }));
    menu->addChild(createMenuItem("Export control map…", "", [m]() {
      char *path = osdialog_file(OSDIALOG_SAVE, nullptr,
                                 "patch.zoia-controls.json", nullptr);
      if (!path)
        return;
      auto root = profileJSON(*std::atomic_load(&m->profile));
      int result = zoia::saveJSON(root, path);
      json_decref(root);
      free(path);
      if (result)
        warn("Could not save the control map.");
    }));
    menu->addChild(createMenuItem("Mute / release held note", "", [m]() {
      std::lock_guard<std::mutex> lock(m->transport);
      m->mute();
    }));
    menu->addChild(
        createMenuLabel("Load matching ZOIA patch before enabling LIVE."));
  }
};
static void changeStrength(Authoring *m, int64_t cableId, int raw) {
  if (raw < 0 || raw > 65535 || m->connectionStrength(cableId) == raw)
    return;
  auto old = m->toJson();
  m->connectionStrengths[cableId] = raw;
  cableStrengths[cableId] = raw;
  if (APP->history) {
    auto h = new history::ModuleChange;
    h->name = "change ZOIA connection strength";
    h->moduleId = m->id;
    h->oldModuleJ = old;
    h->newModuleJ = m->toJson();
    APP->history->push(h);
  } else
    json_decref(old);
}
struct StrengthField : ui::TextField {
  Authoring *author = nullptr;
  int64_t cableId = -1;
  void onAction(const ActionEvent &e) override {
    try {
      size_t consumed = 0;
      int raw = std::stoi(text, &consumed);
      if (consumed != text.size() || raw < 0 || raw > 65535)
        throw Exception("Raw strength must be 0..65535");
      changeStrength(author, cableId, raw);
      e.consume(this);
    } catch (const std::exception &) {
      text = std::to_string(author->connectionStrength(cableId));
    }
  }
};
struct ZoiaAuthoringPort : Rack40Port {
  void appendContextMenu(Menu *menu) override {
    menu->addChild(new MenuSeparator);
    menu->addChild(
        createSubmenuItem("ZOIA connection strengths", "", [this](Menu *sub) {
          bool any = false;
          for (auto id : APP->engine->getCableIds()) {
            auto c = APP->engine->getCable(id);
            bool matches =
                type == engine::Port::INPUT
                    ? c->inputModule == module && c->inputId == portId
                    : c->outputModule == module && c->outputId == portId;
            auto target = dynamic_cast<Authoring *>(c->inputModule);
            auto source = dynamic_cast<Authoring *>(c->outputModule);
            if (!matches || !target || !source)
              continue;
            any = true;
            auto name = source->d->name + " → " +
                        str(target->d->inputs.at(c->inputId), "name");
            sub->addChild(createSubmenuItem(
                name, std::to_string(target->connectionStrength(id)) + " raw",
                [target, id](Menu *edit) {
                  edit->addChild(
                      createMenuLabel("Exact ZOIA raw strength (0..65535)"));
                  auto field = new StrengthField;
                  field->author = target;
                  field->cableId = id;
                  field->text = std::to_string(target->connectionStrength(id));
                  field->box.size.x = 220;
                  edit->addChild(field);
                  for (int percent : {0, 25, 50, 75, 100}) {
                    int raw =
                        percent == 0
                            ? 0
                            : int(std::lround(
                                  10000 + 2000 * std::log10(percent / 100.0)));
                    edit->addChild(createMenuItem(
                        std::to_string(percent) + "%",
                        std::to_string(raw) + " raw", [target, id, raw]() {
                          changeStrength(target, id, raw);
                        }));
                  }
                }));
          }
          if (!any)
            sub->addChild(createMenuLabel("No ZOIA cables on this port."));
        }));
  }
};
// Portable control-state preset shared with the browser. Rack's .vcvm remains
// the host preset format; these files never include cable IDs or MIDI devices.
static std::string presetSequencerBank(Authoring *m) {
  if (!m->sequencerBank.empty() || m->original.empty() || str(m->d->catalog, "type") != "Sequencer")
    return m->sequencerBank;
  // Legacy Rack saves carried the bank only inside the original document.
  // Resolve the stable bridge identity instead of exporting an empty preset.
  auto doc = json_loads(m->original.c_str(), 0, nullptr);
  auto mapping = json_object_get(json_object_get(json_object_get(doc, "extensions"), "rack-lab.native.v1"), "rackIds");
  auto identity = str(mapping, ("rack-" + std::to_string(m->id)).c_str());
  if (identity.empty() && m->clonedFrom >= 0)
    identity = str(mapping, ("rack-" + std::to_string(m->clonedFrom)).c_str());
  std::string bank;
  bool unresolved = false;
  size_t i; json_t *module;
  json_array_foreach(json_object_get(doc, "modules"), i, module) {
    auto saved = json_object_get(json_object_get(module, "opaque"), "rack40SequencerBankV1");
    if (!saved) continue;
    if (identity.empty()) { unresolved = true; continue; }
    if (str(module, "id") != identity) continue;
    try { bank = validatedSequencerBank(saved, "Sequencer"); }
    catch (...) { json_decref(doc); throw; }
  }
  json_decref(doc);
  if (unresolved) throw Exception("Cannot resolve this legacy sequencer's extra page/track state. Reopen it through the current browser file bridge before saving a preset.");
  return bank;
}
static json_t *rack40ModulePreset(Authoring *m) {
  // Resolve/validate legacy state before allocating the output tree.
  auto bank = presetSequencerBank(m);
  auto root = json_pack("{s:s,s:i,s:s,s:s}", "format", "rack40-module-preset",
                        "version", 1, "configurationId", m->d->id.c_str(),
                        "type", str(m->d->catalog, "type").c_str());
  auto values = json_object();
  for (size_t i = 0; i < m->d->params.size(); i++)
    json_object_set_new(values, str(m->d->params[i], "key").c_str(),
                        json_integer(int(std::lround(m->params[i].getValue()))));
  json_object_set_new(root, "parameters", values);
  if (!bank.empty())
    json_object_set_new(root, "sequencer", json_loads(bank.c_str(), 0, nullptr));
  return root;
}
// UI boundary: invalid legacy state and filesystem errors must leave the old
// destination intact and return an actionable error, never escape a Rack menu.
static std::string saveRack40ModulePreset(Authoring *m, const std::string &path) {
  try {
    std::unique_ptr<json_t, decltype(&json_decref)> root(rack40ModulePreset(m), &json_decref);
    if (zoia::saveJSON(root.get(), path))
      return "Could not save the Rack40 module preset. The previous file was not replaced.";
    return "";
  } catch (const std::exception &e) {
    return e.what();
  }
}
static void loadRack40ModulePreset(Authoring *m, json_t *root) {
  int version;
  if (!root || str(root, "format") != "rack40-module-preset" ||
      !integer(root, "version", 1, 1, version) ||
      str(root, "configurationId") != m->d->id ||
      str(root, "type") != str(m->d->catalog, "type"))
    throw Exception("Choose a Rack40 preset for this exact module configuration. No controls or cables were changed.");
  if (protectedOriginal(m))
    throw Exception("Preserved imported modules cannot load control presets yet.");
  const auto bank = validatedSequencerBank(json_object_get(root, "sequencer"), str(m->d->catalog, "type"));
  auto values = json_object_get(root, "parameters");
  if (!json_is_object(values)) throw Exception("Preset parameters are missing.");
  std::vector<int> raw;
  for (auto p : m->d->params) {
    int value;
    if (!integer(values, str(p, "key").c_str(), 0, 65535, value))
      throw Exception("Preset contains missing or invalid control values.");
    raw.push_back(value);
  }
  if (json_object_size(values) != raw.size())
    throw Exception("Preset contains unknown controls.");
  auto old = m->toJson();
  for (size_t i = 0; i < raw.size(); i++) m->params[i].setValue(raw[i]);
  m->sequencerBank = bank.empty() && str(m->d->catalog, "type") == "Sequencer"
      ? "{\"version\":1,\"values\":{}}" : bank;
  if (APP->history) {
    auto h = new history::ModuleChange;
    h->name = "load Rack40 module preset"; h->moduleId = m->id;
    h->oldModuleJ = old; h->newModuleJ = m->toJson(); APP->history->push(h);
  } else json_decref(old);
}
static std::vector<std::pair<Model *, std::shared_ptr<Definition>>> authorVariants;
// Numeric readouts open Rack's own exact-value field; knob double-click still resets.
struct AuthorValueEntry : widget::OpaqueWidget {
  app::ParamWidget *knob = nullptr;
  void onDoubleClick(const DoubleClickEvent &e) override {
    if (knob && knob->module) {
      knob->createContextMenu();
      e.consume(this);
    }
  }
  void onButton(const ButtonEvent &e) override {
    widget::OpaqueWidget::onButton(e);
    if (knob && knob->module && e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_RIGHT) {
      knob->createContextMenu();
      e.consume(this);
    }
  }
};
struct NativePushbutton : widget::OpaqueWidget {
  Authoring *author = nullptr;
  std::shared_ptr<Definition> definition;
  bool pointerDown = false;
  int heldKey = 0;
  int64_t controllerId = -1;
  uint64_t token = 0;
  Live *controller(int64_t id) const {
    return id>=0 ? dynamic_cast<Live*>(APP->engine->getModule(id)) : nullptr;
  }
  void release() {
    if(auto live=controller(controllerId)) live->releaseGrid(token);
    token=0; controllerId=-1; pointerDown=false; heldKey=0;
  }
  bool ready() const {
    auto live=author ? controller(author->remoteController) : nullptr;
    return live && !author->isBypassed() && author->remoteCell>=0 && author->remoteCell<40 && live->gridReady(author->remotePage,author->remoteChannel);
  }
  void press() {
    if(token || !author || !ready()) return;
    bool latch=false; size_t i; json_t *option;
    json_array_foreach(json_object_get(author->d->catalog,"options"),i,option)
      if(str(option,"key")=="action") latch=str(option,"selectedValue")=="latching";
    controllerId=author->remoteController;
    auto live=controller(controllerId);
    token=live->pressGrid(author->id,author->remotePage,author->remoteCell,author->remoteChannel,latch);
  }
  ~NativePushbutton() { release(); }
  void onButton(const ButtonEvent &e) override {
    if(e.button!=GLFW_MOUSE_BUTTON_LEFT) { OpaqueWidget::onButton(e); return; }
    if(e.action==GLFW_PRESS && !heldKey && !pointerDown) {pointerDown=true;press();}
    else if(e.action==GLFW_RELEASE && pointerDown) release();
    e.consume(this);
  }
  void onDragStart(const DragStartEvent &e) override { e.consume(this); }
  void onDoubleClick(const DoubleClickEvent &e) override { e.consume(this); }
  void onDragEnd(const DragEndEvent &) override { release(); }
  void onLeave(const LeaveEvent &) override { release(); }
  void onDeselect(const DeselectEvent &) override { release(); }
  void onHide(const HideEvent &e) override { release(); OpaqueWidget::onHide(e); }
  void onRemove(const RemoveEvent &) override { release(); }
  void onSelectKey(const SelectKeyEvent &e) override {
    if(e.key!=GLFW_KEY_SPACE && e.key!=GLFW_KEY_ENTER) return;
    if(e.action==GLFW_PRESS && (e.mods&(GLFW_MOD_CONTROL|GLFW_MOD_SUPER|GLFW_MOD_ALT))) return;
    if(e.action==GLFW_PRESS && !heldKey && !pointerDown) {heldKey=e.key;press();}
    else if(e.action==GLFW_RELEASE && heldKey==e.key) release();
    e.consume(this);
  }
  void step() override {
    OpaqueWidget::step();
    if(!token) return;
    auto live=controller(controllerId);
    // UI disappearance stalls renewal; the independent monitor releases after
    // 500ms even if no further widget event/step is delivered.
    bool focused=APP->window && APP->window->win && glfwGetWindowAttrib(APP->window->win,GLFW_FOCUSED) && glfwGetWindowAttrib(APP->window->win,GLFW_VISIBLE);
    if(!focused || !ready()) { release(); if(live) live->stopGrid(); }
    else live->renewGrid(token);
  }
  void draw(const DrawArgs &a) override {
    bool enabled=ready();auto vg=a.vg;
    nvgBeginPath(vg);nvgRoundedRect(vg,0,0,box.size.x,box.size.y,4);
    nvgFillPaint(vg,nvgLinearGradient(vg,0,0,0,box.size.y,nvgRGB(232,234,229),nvgRGB(157,164,157)));nvgFill(vg);
    nvgStrokeColor(vg,enabled?nvgRGB(212,235,205):nvgRGB(110,114,111));nvgStrokeWidth(vg,1.5);nvgStroke(vg);
    auto font=APP->window->loadFont(asset::system("res/fonts/DejaVuSans.ttf"));
    if(font) {nvgFontFaceId(vg,font->handle);nvgFontSize(vg,9);nvgTextAlign(vg,NVG_ALIGN_CENTER|NVG_ALIGN_MIDDLE);nvgFillColor(vg,nvgRGB(25,30,26));
      bool latch=false;size_t i;json_t*o;json_array_foreach(json_object_get(definition->catalog,"options"),i,o)if(str(o,"key")=="action")latch=str(o,"selectedValue")=="latching";
      nvgText(vg,box.size.x/2,box.size.y/2,enabled?(latch?"Press / toggle":"Tap / hold"):"Live MIDI disabled",nullptr);
    }
  }
};
static void nativePushbuttonMenu(Menu *menu, Authoring *author) {
  menu->addChild(createSubmenuItem("Pushbutton Live MIDI setup", "", [author](Menu *sub) {
    sub->addChild(createMenuLabel("Runtime only. Verify the actual Pushbutton on ZOIA."));
    sub->addChild(createMenuLabel("Select MIDI output; enable Live Control."));
    auto reset=[author](){ if(auto live=dynamic_cast<Live*>(APP->engine->getModule(author->remoteController))) live->stopGrid(); };
    sub->addChild(createSubmenuItem("Live Control", "", [author,reset](Menu *items) {
      bool found=false;
      for(auto id:APP->engine->getModuleIds()) if(auto live=dynamic_cast<Live*>(APP->engine->getModule(id))) {
        found=true;auto name=std::atomic_load(&live->profile)->name;
        items->addChild(createCheckMenuItem(name+" #"+std::to_string(id),"",[author,id](){return author->remoteController==id;},[author,id,reset](){reset();author->remoteController=id;}));
      }
      if(!found) items->addChild(createMenuLabel("Add Rack40 Live Control first."));
    }));
    sub->addChild(createSubmenuItem("Pedal page (0–63)", "", [author,reset](Menu *items) {
      for(int page=0;page<64;page++) items->addChild(createCheckMenuItem(std::to_string(page),"",[author,page](){return author->remotePage==page;},[author,page,reset](){reset();author->remotePage=page;}));
    }));
    sub->addChild(createSubmenuItem("Pushbutton grid cell", "", [author,reset](Menu *rows) {
      for(int row=0;row<5;row++) rows->addChild(createSubmenuItem("Row "+std::to_string(row+1),"",[author,row,reset](Menu *cols) {
        for(int col=0;col<8;col++) {int cell=row*8+col;cols->addChild(createCheckMenuItem("Column "+std::to_string(col+1),"",[author,cell](){return author->remoteCell==cell;},[author,cell,reset](){reset();author->remoteCell=cell;}));}
      }));
    }));
    sub->addChild(createSubmenuItem("ZOIA Config MIDI channel", "", [author,reset](Menu *items) {
      for(int channel=1;channel<=16;channel++) items->addChild(createCheckMenuItem(std::to_string(channel),"",[author,channel](){return author->remoteChannel==channel;},[author,channel,reset](){reset();author->remoteChannel=channel;}));
    }));
    sub->addChild(createMenuItem("Confirm matching patch, current page, no pedal menu", "", [author]() {
      auto live=dynamic_cast<Live*>(APP->engine->getModule(author->remoteController));
      if(author->remoteCell<0 || !live || !live->confirmGrid(author->remotePage,author->remoteChannel))
        warn("Choose controller, page and cell; select MIDI output and enable Live MIDI first. Then verify the physical pedal page with no menu open.");
    }));
    sub->addChild(createMenuItem("Disable remote buttons", "", reset));
    sub->addChild(createMenuLabel("No CC map is needed for remote Pushbuttons."));
    sub->addChild(createMenuLabel("Reconfirm after changing pedal pages. No page/state readback."));
  }));
}
struct AuthorWidget : MenuLifetimeModuleWidget {
  Face *face = nullptr;
  void step() override {
    ModuleWidget::step();
    auto author = dynamic_cast<Authoring *>(module);
    if (!author || !author->cloneWindow)
      return;
    for (auto id : APP->engine->getCableIds()) {
      auto cable = APP->engine->getCable(id);
      if (cable->inputModule == author || cable->outputModule == author) {
        int raw = author->connectionStrength(id);
        cableStrengths[id] = raw;
        author->connectionStrengths[id] = raw;
      }
    }
    author->cloneWindow = false;
  }
  void appendContextMenu(Menu *menu) override {
    MenuLifetimeModuleWidget::appendContextMenu(menu);
    menu->addChild(new MenuSeparator);
    auto author = dynamic_cast<Authoring *>(module);
    if (author) {
      auto def = author->d;
      if(str(def->catalog,"type")=="Pushbutton") nativePushbuttonMenu(menu,author);
      if (str(def->catalog,"type")=="Sequencer" && face) {
        auto display = face;
        menu->addChild(createCheckMenuItem("Local motion preview (120 BPM)", "", [display](){return display->sequencerPreview.running;}, [display](){display->sequencerPreview.reset();display->sequencerPreview.running=!display->sequencerPreview.running;}));
        menu->addChild(createMenuLabel("UI animation only; not ZOIA playback or a patch parameter."));
      }
      menu->addChild(createSubmenuItem("Rack40 portable preset", "", [author](Menu *sub) {
        sub->addChild(createMenuLabel("Shared with the browser · control state only"));
        sub->addChild(createMenuItem("Load…", "", [author]() {
          char *path = osdialog_file(OSDIALOG_OPEN, nullptr, nullptr, nullptr);
          if (!path) return;
          auto root = readJSON(path); free(path);
          try { loadRack40ModulePreset(author, root); }
          catch (const std::exception &e) { warn(e.what()); }
          if (root) json_decref(root);
        }));
        sub->addChild(createMenuItem("Save as…", "", [author]() {
          char *path = osdialog_file(OSDIALOG_SAVE, nullptr, "module.rack40-module.json", nullptr);
          if (!path) return;
          std::string destination(path); free(path);
          auto error = saveRack40ModulePreset(author, destination);
          if (!error.empty()) warn(error);
        }));
      }));
      int64_t id = author->id;
      menu->addChild(createSubmenuItem("Module configuration", "", [id, def](Menu *sub) {
        sub->addChild(createMenuLabel("Compatible settings/cables stay. Removed sockets disconnect."));
        sub->addChild(createMenuLabel("Undo restores the previous configuration and wiring."));
        for (const auto &variant : authorVariants) {
          auto model = variant.first; auto next = variant.second;
          if (str(next->catalog, "type") != str(def->catalog, "type")) continue;
          std::string label = next->id;
          // The label uses canonical option names/values, as in the browser inspector.
          std::string options;
          size_t i; json_t *option;
          json_array_foreach(json_object_get(next->panel, "options"), i, option) {
            auto value = json_object_get(option, "value");
            if (!options.empty()) options += ", ";
            options += str(option, "label") + ": " + (json_is_string(value) ? std::string(json_string_value(value)) : std::to_string(json_integer_value(value)));
          }
          if (!options.empty()) label = options;
          auto codec = json_object_get(next->catalog, "codec");
          if (json_integer_value(json_object_get(codec, "moduleIndex")) == 49)
            label += json_integer_value(json_object_get(codec, "moduleVersion")) >= 1 ? " · Dividend / divisor" : " · Modifier";
          sub->addChild(createCheckMenuItem(label, "", [id, next]() {
            auto current = dynamic_cast<Authoring *>(APP->engine->getModule(id));
            return current && current->d->id == next->id;
          }, [id, model, next]() {
            try { switchConfiguration(id, model, next); }
            catch (const std::exception &e) { warn(e.what()); }
          }, protectedOriginal(dynamic_cast<Authoring *>(APP->engine->getModule(id)))));
        }
      }));
    }
    menu->addChild(createMenuItem("Export ZOIA patch…", "",
                                  []() { exportDocumentDialog(); }));
  }
  AuthorWidget(Authoring *m, std::shared_ptr<Definition> d) {
    setModule(m);
    box.size = Vec(d->width(), 380);
    auto f = new Face;
    face = f;
    f->d = d;
    f->author = m;
    f->box.size = box.size;
    addChild(f);
    if(str(d->catalog,"type")=="Pushbutton") {
      auto pad=new NativePushbutton;pad->author=m;pad->definition=d;pad->box.pos=Vec(10,216);pad->box.size=Vec(d->width()-20,36);addChild(pad);
    }
    for (size_t i = 0; i < d->params.size(); i++) {
      auto position = d->point("controls", d->params[i]);
      auto knob = createParamCentered<ZoiaKnob>(position, m, i);
      addParam(knob);
      auto entry = new AuthorValueEntry;
      entry->knob = knob;
      entry->box.pos = position.plus(Vec(-30, str(d->catalog, "type") == "Sequencer" ? 10 : 17));
      entry->box.size = Vec(60, 16);
      addChild(entry);
    }
    for (size_t i = 0; i < d->inputs.size(); i++)
      addInput(createInputCentered<ZoiaAuthoringPort>(d->point("ports",d->inputs[i]),m,i));
    for (size_t i = 0; i < d->outputs.size(); i++)
      addOutput(createOutputCentered<ZoiaAuthoringPort>(d->point("ports",d->outputs[i]),m,i));
  }
};
struct AuthorModel : Model {
  std::shared_ptr<Definition> d;
  AuthorModel(json_t *c, json_t *panel)
      : d(std::make_shared<Definition>(c, panel)) {
    slug = d->id;
  }
  Module *createModule() override {
    auto m = new Authoring(d);
    m->model = this;
    return m;
  }
  ModuleWidget *createModuleWidget(Module *m) override {
    auto w = new AuthorWidget(dynamic_cast<Authoring *>(m), d);
    w->setModel(this);
    return w;
  }
};
void init(Plugin *p) {
  pluginInstance = p;
  p->addModel(createModel<Live, LiveWidget>("Live"));
  auto root = readJSON(asset::plugin(p, "res/module-catalog.json"));
  if (!root)
    throw Exception("ZOIA module catalog unavailable");
  auto layouts = readJSON(asset::plugin(p, "res/module-panels.json"));
  if (!layouts)
    throw Exception("Shared ZOIA panel definitions unavailable");
  auto cs = json_object_get(root, "configurations");
  size_t i;
  json_t *c;
  std::set<std::string> visibleTypes;
  json_array_foreach(cs, i, c) {
    json_t *layout = nullptr, *candidate;
    size_t j;
    json_array_foreach(json_object_get(layouts, "panels"), j,
                       candidate) if (str(candidate, "configurationId") ==
                                      str(c, "id")) layout = candidate;
    if (!layout)
      throw Exception("Module has no shared panel layout");
    auto model = new AuthorModel(c, layout);
    model->hidden = str(c, "type") == "Sequencer" ? str(c, "id") != "sequencer-32-loop" : !visibleTypes.insert(str(c, "type")).second;
    p->addModel(model);
    authorVariants.push_back({model, model->d});
  }
  json_decref(layouts);
  json_decref(root);
}

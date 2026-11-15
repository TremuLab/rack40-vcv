"""Generate portable, key-addressed module panels for browser and Rack.

Coordinates are Rack logical pixels (380 high). Parameter order/IDs in the
codec remain untouched. Matching CV inputs sit below their parameter knob.
"""

import argparse
import json
import math
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Two-control utilities/effects fit a vertical strip while retaining a normal
# knob, its CV socket, and the fixed rail height. Wider I/O stays readable.
STACKED_TYPES = {
    'Fuzz', 'OD & Distortion', 'Ring Modulator', 'Tremolo', 'Vibrato',
    'CV Delay', 'Reverb Lite', 'Multi Filter', 'SV Filter', 'Audio Balance',
    'Comparator', 'CV Filter', 'In Switch', 'Logic Gate', 'Multiplier',
    'Out Switch', 'Sample and Hold', 'Slew Limiter',
    'Clock Divider', 'Stereo Spread', 'Audio Panner', 'Midi Note Out', 'Midi PC Out',
}


def groups_for(c):
    keys = [p["key"] for p in c["parameters"]]
    groups = []

    def take(label, candidates):
        chosen = [k for k in candidates if k in keys]
        if chosen:
            groups.append((label, chosen))
            for k in chosen:
                keys.remove(k)

    if "grain_size" in keys:
        take("Grains", ["grain_size", "grain_position", "density"])
        take("Shape / playback", ["texture", "speed_pitch", "freeze"])
    elif any(k.startswith("atten_") for k in keys):
        take("Channel inputs", [f"cv_in_{i}" for i in range(1, 9)])
        take("Attenuation", [f"atten_{i}" for i in range(1, 9)])
    elif all(k in keys for k in ("attack", "decay", "sustain", "release")):
        take("Envelope", ["attack", "decay", "sustain", "release"])
        take("Gate", ["gate_input"])
    elif "sample_playback" in keys:
        take("Playback", ["sample_playback", "speed_pitch"])
        take("Sample window", ["start", "length"])
    elif "record" in keys:
        take("Transport", ["record", "restart_playback", "play", "restart_loop"])
        take("Playback", ["speed_pitch", "playback_speed", "cv_input"])
    elif "delay_time" in keys:
        take("Delay", ["delay_time", "feedback", "mix", "pitch", "cv_input"])
        take("Modulation", ["mod_rate", "mod_depth"])
    elif "frequency" in keys:
        take(
            (
                "Tuning"
                if "audio_in" not in [e["key"] for e in c["endpoints"]]
                else "Filter"
            ),
            ["frequency", "resonance", "q", "mix"],
        )
    elif "decay_time" in keys:
        take("Reverb", ["decay_time", "mix"])
        take("Tone", ["low_eq", "high_eq", "lpf_freq"])
    elif "gain_1" in keys:
        take("Channel levels", ["gain_1", "gain_2"])
    elif "input_gain" in keys:
        take("Gain staging", ["input_gain", "output_gain"])
    elif "rate" in keys:
        take("Modulation", ["rate", "depth", "width", "resonance"])
        take("Tone / mix", ["regen", "tone_tilt_eq", "decay_feedback", "mix"])
    take("Controls", keys[:])
    return groups


def build_panel(c):
    groups = groups_for(c)
    stacked = c['type'] in STACKED_TYPES and len(c['parameters']) <= (3 if c['type'] == 'Clock Divider' else 2)
    rows = []
    for title, keys in groups:
        # An ADSR gets its four stages side by side; other groups max 3/row.
        limit = (
            1 if stacked else 8
            if title in ("Channel inputs", "Attenuation")
            else (4 if title == "Envelope" else 3)
        )
        for i in range(0, len(keys), limit):
            rows.append((title, keys[i : i + limit]))
    ports = [
        e
        for e in c["endpoints"]
        if e["kind"] in ("audioInput", "audioOutput", "cvInput", "cvOutput")
    ]
    paired = {
        e["key"]
        for e in ports
        if e["kind"] == "cvInput" and e["key"] in [p["key"] for p in c["parameters"]]
    }
    remaining = [e for e in ports if e["key"] not in paired]
    expanded = c.get('libraryHidden', False)
    if expanded and stacked and len(remaining) > 3 and len(c['parameters']) > 1:
        stacked = False
        rows = groups
    if expanded and len(rows) > 2 and not stacked:
        keys = [key for _, group in rows for key in group]
        per_row = math.ceil(len(keys) / 2)
        rows = [('Controls', keys[i:i+per_row]) for i in range(0, len(keys), per_row)]
    columns = max([len(keys) for _, keys in rows] + [1])
    width = max(120, columns * 75 + 30, (len(remaining)-1)*45+60 if expanded and not stacked else 180 if len(remaining)>2 else 120)
    if stacked:
        width = 150 if len(remaining) > 2 else 120
    io_columns = max(1, len(remaining)) if expanded and not stacked else min(3 if stacked else 4, max(1, len(remaining)))
    io_rows = math.ceil(len(remaining) / io_columns)
    height = 380  # Rack panels have a fixed physical rail height.
    result = {
        "configurationId": c["id"],
        "title": c["type"],
        "width": width,
        "height": height,
        "groups": [],
        "controls": [],
        "ports": [],
        "illustrations": [],
        "options": [
            {"key": o["key"], "label": o["name"], "value": o["selectedValue"]}
            for o in c.get("options", [])
        ],
    }
    params = {p["key"]: p for p in c["parameters"]}
    for row, (title, keys) in enumerate(rows):
        y = 112 + row * 110
        if not stacked or row == 0:
            result["groups"].append({"label": title, "y": y - (34 if row == 0 else 30)})
        for i, key in enumerate(keys):
            x = width / 2 + (i - (len(keys) - 1) / 2) * 75
            result["controls"].append(
                {"key": key, "label": params[key]["name"], "x": x, "y": y}
            )
            if key in paired:
                e = next(e for e in ports if e["key"] == key)
                result["ports"].append(
                    {
                        "key": key,
                        "label": "CV",
                        "kind": e["kind"],
                        "x": x,
                        "y": y + 52,
                        "parameterKey": key,
                    }
                )
    # Inputs on the left and outputs on the right, preserving stereo order.
    remaining.sort(key=lambda e: e["kind"].endswith("Output"))
    start_y = height - 48 - max(0, io_rows - 1) * 65
    # A narrow two-control tap-tempo panel can also have two rows of stereo
    # audio jacks. Keep the second knob's CV socket and both I/O rows distinct.
    if stacked and len(rows) == 2 and io_rows == 2:
        start_y = 285
    if remaining:
        result["groups"].append({"label": "Signal connections", "y": start_y - 30})
    for i, e in enumerate(remaining):
        row, col = divmod(i, io_columns)
        count = min(io_columns, len(remaining) - row * io_columns)
        x = width / 2 if count == 1 else 30 + col * (width - 60) / (count - 1)
        result["ports"].append(
            {
                "key": e["key"],
                "label": e["name"].replace("Audio In", "In").replace("Audio Out", "Out") if e["kind"].startswith("audio") else e["name"],
                "kind": e["kind"],
                "x": x,
                "y": start_y + row * 65,
            }
        )
    if c['type'] == 'Quantizer' and [p['key'] for p in c['parameters']] == ['cv_input']:
        result.update(width=120,
            groups=[{'label':'Pitch reference','y':78},{'label':'Input bias','y':205}],
            controls=[{'key':'cv_input','label':'CV Bias','x':60,'y':235}],
            ports=[{'key':'cv_input','label':'CV','kind':'cvInput','x':60,'y':287,'parameterKey':'cv_input'},
                   {'key':'cv_output','label':'Out','kind':'cvOutput','x':60,'y':332}],
            illustrations=[{'kind':'pitch-reference','x':15,'y':92,'width':90,'height':100}])
    if c['type'] == 'Quantizer' and 'cv_input' in params and not result['illustrations']:
        # Keep the familiar piano-first layout in configurations with extra jacks.
        result['groups'] = [{'label':'Pitch reference','y':78},{'label':'Controls','y':205}]
        result['illustrations'] = [{'kind':'pitch-reference','x':15,'y':92,'width':width-30,'height':100}]
        for control in result['controls']:
            control['y'] = 235
        for port in result['ports']:
            port['y'] = 287 if port.get('parameterKey') else 332
    if c['type'] == 'Clock Divider':
        result['groups'] = [{'label':'Clock division','y':78}]
        # Record1 adds a fourth control; retain room for labels, raw values,
        # paired CV sockets and the output instead of overflowing the rails.
        four_controls = len(result['controls']) > 3
        if four_controls:
            result['width'] = 195
        for i, control in enumerate(result['controls']):
            control.update(x=52.5+(i%2)*90 if four_controls else 40,
                           y=112+(i//2)*110 if four_controls else 112+i*86)
        for port in result['ports']:
            paired_control = next((p for p in result['controls'] if p['key'] == port.get('parameterKey')), None)
            if four_controls:
                port.update(x=paired_control['x'] if paired_control else 97.5,
                            y=paired_control['y']+52 if paired_control else 332)
            else:
                port.update(x=90 if paired_control else 60, y=paired_control['y'] if paired_control else 350)
    if c['type'] == 'ADSR' and len(rows) == 2:
        # Keep ordered stages; the gate sits left of a configured envelope sketch.
        for control in result['controls']:
            if control['key'] == 'gate_input': control.update(x=40, y=235)
        for port in result['ports']:
            if port['key'] == 'gate_input': port.update(x=40, y=287)
        result['illustrations'].append({'kind':'envelope-preview','x':85,'y':215,'width':width-105,'height':65})
    elif c['type'] in ('Oscillator','LFO') and len(rows) <= 1:
        result['illustrations'].append({'kind':'waveform-preview','x':20,'y':190,'width':width-40,'height':55,'waveformX':width*.28,'windowX':width*.72,'controlsY':273})
    elif c['type'] in ('Audio Mixer','CV Mixer','Sequencer') and len(rows) <= 1:
        result['illustrations'].append({'kind':'level-preview','x':20,'y':205,'width':width-40,'height':65})
    if c['type'] == 'Sequencer':
        g = json.loads((ROOT / 'shared/sequencer-panel.v1.json').read_text())
        result.update(width=g['width'], height=g['height'], groups=[], illustrations=[])
        def point(key):
            if key.startswith('step_'):
                n=int(key[5:])-1
                return dict(x=g['firstX']+(n%8)*g['columnSpacing'],y=g['firstY']+(n//8)*g['rowSpacing'])
            return dict(x=42 if key=='gate_in' else 142,y=g['ioY'])
        result['controls']=[dict(key=p['key'],label=p['name'],**point(p['key'])) for p in c['parameters']]
        result['ports']=[]
        for e in ports:
            if e['kind']=='cvInput':
                pos=point(e['key']);pos['x']+=g['stepPortOffset']
            else: pos=dict(x=250+int(e['key'].split('_')[-1])*45,y=g['ioY'])
            result['ports'].append(dict(key=e['key'],label='CV' if e['key'].startswith('step_') else e['name'],kind=e['kind'],**pos))
    return result


def generate():
    registry = json.loads((ROOT / "shared/module-configurations.v1.json").read_text())
    return {
        "format": "zoia-module-panels",
        "schemaVersion": 1,
        "coordinateSystem": "rack-logical-pixels",
        "panels": [build_panel(c) for c in registry["configurations"]],
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    text = json.dumps(generate(), indent=2) + "\n"
    path = ROOT / "shared/module-panels.v1.json"
    if args.check:
        assert (
            path.read_text() == text
        ), "Regenerate shared panels with python3 tools/panel_layouts.py"
    else:
        path.write_text(text)

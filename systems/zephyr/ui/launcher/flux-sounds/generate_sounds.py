"""FLUX Lightgrain: original UI sound family, stdlib only."""
from pathlib import Path
import math
import random
import json
import struct
import wave

root = Path(__file__).resolve().parent / 'assets' / 'sounds'
root.mkdir(parents=True, exist_ok=True)
rate = 22050
# start, duration, base frequency, amplitude; deliberately short harmonic tails.
voices = {
    'tick': (0.038, [(0, .038, 640, .16)]),
    'tap': (0.050, [(0, .050, 480, .13)]),
    'open': (0.280, [(0, .15, 480, .13), (.045, .17, 720, .10), (.095, .185, 960, .07)]),
    'close': (0.245, [(0, .12, 960, .075), (.035, .15, 720, .095), (.075, .17, 480, .12)]),
    'tune': (0.170, [(0, .095, 600, .12), (.060, .11, 900, .09)]),
}
rendered = {}
for name, (duration, notes) in voices.items():
    rng = random.Random(83)
    samples = []
    noise_low = 0
    for i in range(round(duration * rate)):
        t = i / rate
        noise_low += .22 * (rng.uniform(-1, 1) - noise_low)
        value = 0
        for start, length, freq, gain in notes:
            u = t - start
            if 0 <= u < length:
                p = u / length
                attack = .0018 if name in ('tick', 'tap') else .007
                decay = 150 if name == 'tick' else 105 if name == 'tap' else 24
                env = min(1, u / attack) * math.exp(-u * decay) * (1-p)**2
                # Dry bead for scrolling; softly opening harmonic bloom for transitions.
                phase = 2 * math.pi * freq * u
                tone = .68 * math.sin(phase) + .22 * math.sin(phase*1.5) + .10 * math.sin(phase*2)
                value += gain * env * (tone + (.18 if name == 'tick' else .025) * noise_low)
        samples.append(round(max(-1, min(1, value))*32767))
    rendered[name] = samples
    with wave.open(str(root / (name+'.wav')), 'wb') as w:
        w.setparams((1,2,rate,0,'NONE','not compressed'))
        w.writeframes(struct.pack('<'+'h'*len(samples), *samples))
(root/'README.md').write_text('# 流光 · Lightgrain 音效套装\n\n原创合成，统一为轻颗粒、短谐波、柔和展开的声音。滑动 tick 是38ms干净颗粒声；open/close 为相同谐波音色的展开/收束；tune 是两点亮起；tap 更低、更轻。没有长噪声尾巴。\n\n格式：22050Hz 单声道 PCM16 WAV。用途与触发条件见 SOUND-TRIGGERS.md。五种事件分别使用 tick.wav、tap.wav、open.wav、close.wav、tune.wav，直接在模拟 launcher 中随操作播放。\n\n运行 generate_sounds.py 可重生成，Python标准库，无第三方音频。可用于本项目固件及修改。\n', encoding='utf-8')
(root/'specs.json').write_text(json.dumps({'set':'Lightgrain-v1','sample_rate':rate,'channels':1,'bits':16,'notes':voices,'partials':[[1,.68],[1.5,.22],[2,.10]],'seed':83,'tick_noise_gain':.18,'attack':{'short':.0018,'transition':.007},'decay':{'tick':150,'tap':105,'transition':24}},indent=2),encoding='utf-8')
for name,samples in rendered.items():
    assert max(abs(s) for s in samples) < 32767
    assert any(samples)
    print(name, round(len(samples)/rate*1000),'ms, peak',round(max(abs(s) for s in samples)/32767,3))

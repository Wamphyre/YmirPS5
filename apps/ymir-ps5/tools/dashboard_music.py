#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Render Cold Orbit, YmirPS5's original dashboard loop, as stereo PCM."""

import argparse
import wave
from pathlib import Path

import numpy as np


SAMPLE_RATE = 48_000
BEAT = 60 / 80
BEATS_PER_LOOP = 32


def render():
    frames = round(BEATS_PER_LOOP * BEAT * SAMPLE_RATE)
    mix = np.zeros((frames, 2))

    def note(start, midi, duration, amplitude, pan, voice):
        length = round(duration * BEAT * SAMPLE_RATE)
        t = np.arange(length) / SAMPLE_RATE
        hz = 440 * 2 ** ((midi - 69) / 12)
        phase = 2 * np.pi * hz * t
        fade_out = np.clip((length / SAMPLE_RATE - t) / 0.09, 0, 1)
        if voice == "pad":
            chorus = np.sin(phase * 0.997) + np.sin(phase * 1.003)
            tone = 0.58 * np.sin(phase) + 0.17 * chorus + 0.08 * np.sin(phase * 2)
            envelope = np.sin(np.pi * np.arange(length) / length) ** 0.75
        elif voice == "bass":
            tone = np.sin(phase) + 0.16 * np.sin(2 * phase)
            envelope = np.minimum(t / 0.12, 1) * np.exp(-0.56 * t) * fade_out
        else:
            tone = (np.sin(phase + 0.37 * np.exp(-2.2 * t) * np.sin(2 * phase))
                    + 0.13 * np.exp(-4.4 * t) * np.sin(phase * 3.01))
            envelope = np.minimum(t / 0.016, 1) * np.exp(-2.2 * t) * fade_out

        position = (round(start * BEAT * SAMPLE_RATE) + np.arange(length)) % frames
        stereo = np.sqrt([0.5 * (1 - pan), 0.5 * (1 + pan)])
        mix[position] += (amplitude * tone * envelope)[:, None] * stereo

    # Dm9, B-flat major 7 with a raised 11th, Fadd9, Cadd9.
    harmony = (
        (38, (53, 57, 60, 64)),
        (34, (53, 57, 60, 64)),
        (41, (53, 57, 60, 67)),
        (36, (52, 55, 62, 67)),
    )
    for bar_pair, (root, chord) in enumerate(harmony):
        start = bar_pair * 8
        for midi, pan in zip(chord, (-0.45, 0.26, -0.22, 0.47)):
            note(start - 1, midi, 10, 0.024, pan, "pad")
        for offset, interval, level in ((0, 0, 0.041), (3, 12, 0.022), (4, 0, 0.037), (6.5, 7, 0.023)):
            note(start + offset, root + interval, 2.1, level, 0, "bass")

    melody = (
        (0.5, 69, 0.053), (2, 72, 0.042), (3.5, 76, 0.052), (5.25, 74, 0.031), (7, 72, 0.039),
        (8.5, 69, 0.051), (10, 64, 0.043), (11.5, 72, 0.040), (13.75, 76, 0.042), (15, 77, 0.036),
        (16.5, 76, 0.051), (18, 72, 0.039), (19.5, 79, 0.051), (21.25, 76, 0.036), (23, 72, 0.036),
        (24.5, 74, 0.044), (26, 76, 0.041), (27.5, 79, 0.045), (29.5, 74, 0.035), (31, 67, 0.031),
    )
    for index, (start, midi, level) in enumerate(melody):
        pan = -0.26 if index % 2 == 0 else 0.26
        note(start, midi, 2.1, level, pan, "bell")
        note(start + 0.75, midi, 1.7, level * 0.24, -pan * 2, "bell")
        note(start + 1.5, midi, 1.4, level * 0.08, pan * 2, "bell")

    return mix


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="stereo 48 kHz WAV file")
    parser.add_argument("--gain-db", type=float, default=0.0, help="gain in dB")
    args = parser.parse_args()

    samples = render() * 10 ** (args.gain_db / 20)
    if np.max(np.abs(samples)) >= 1:
        parser.error("the requested gain clips the audio")
    pcm = np.rint(samples * 32767).astype("<i2")
    with wave.open(str(args.output), "wb") as output:
        output.setparams((2, 2, SAMPLE_RATE, 0, "NONE", "not compressed"))
        output.writeframes(pcm.tobytes())


if __name__ == "__main__":
    main()

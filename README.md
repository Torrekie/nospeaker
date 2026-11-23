```text
────────────────────────  a90.com presents  ─────────────────────────

    _   __ ____      _____  ____   ______ ___     __ __  ______ ____
   / | / // __ \    / ___/ / __ \ / ____//   |   / //_/ / ____// __ \
  /  |/ // / / /    \__ \ / /_/ // __/  / /| |  / ,<   / __/  / /_/ /
 / /|  // /_/ /    ___/ // ____// /___ / ___ | / /| | / /___ / _, _/
/_/ |_/ \____/    /____//_/    /_____//_/  |_|/_/ |_|/_____//_/ |_|

─  haptic voice synthesis  ─────────────────────────────  trackpad  ─
```

**Mute your Mac. Your trackpad will still talk.**

There's no speaker in a MacBook trackpad, just a small haptic motor that taps the glass so you feel a click.
NO SPEAKER drives that motor up to 4,000 times a second, fast enough to make sound you can hear, and turns it
into a voice. It talks, sings, raps, and becomes an instrument under your finger.

One command. About a minute and a half. No speakers used.

<!-- demo video: drag an .mp4 here in GitHub's editor -->

## 01 / before you start

```text
[ mute your mac ]        it still talks; that's the point
[ put it on a table ]    on your lap it's much quieter
[ hands off the glass ]  until it asks you to touch it
```

**Space** skips a part. **q** or **Ctrl-C** goes to the end.

## 02 / run it

Build this C port with Xcode or the Command Line Tools (`xcode-select --install`). The script builds one
program for Apple Silicon and Intel Macs. Xcode 15.2 with the macOS 14.2 SDK can build both slices. Use
`SDKROOT` to select another installed macOS SDK if needed.

```sh
git clone https://github.com/Torrekie/nospeaker
cd nospeaker
./build.sh
./nospeaker
```

[play.sh](play.sh) installs a binary from this fork's GitHub Releases when one is available. The original
Swift release is available from [os6/nospeaker](https://github.com/os6/nospeaker/releases/latest).

## 03 / play with it

```text
./nospeaker "hello there"            say anything
./nospeaker --voice Daniel "hello"   in another macOS voice
./nospeaker --sweep                  a rising tone, 100 Hz to 4 kHz

--dark                               white on black
--plain                              no visuals, plain text
```

## 04 / needs

A Force Touch trackpad, either built in or a Magic Trackpad 2 or newer. The current build targets macOS 11.0
on Apple Silicon and OS X 10.10.3 on Intel. Those are deployment targets. Hardware playback on those
versions and on external trackpads still needs verification. The original Magic Trackpad 2 requires
OS X 10.11 or later.

MultitouchSupport is private and its behavior can vary by macOS version and trackpad. If you try this port,
include your Mac model, macOS version, trackpad and what you heard when reporting results.

## 05 / how it works

```text
say ──▶ formants ──▶ two tones ──▶ motor ──▶ glass
        per 10 ms    F1 + F2       haptics   sound
```

**Why you can hear it.** The trackpad has no speaker, but the motor moves the glass. Driven back and forth
hundreds or thousands of times a second, the glass vibrates fast enough to make sound you can hear. It's quiet
because the motor is built for short taps, not for sound.

**The motor.** `MultitouchSupport` is the private framework macOS uses for the trackpad. Its function
`MTActuationCreateFromDictionary` takes a description of a vibration, a list of sine tones with a frequency,
a strength and a length each, and `MTActuationActuate` plays it on the motor. Apple's public haptics API only
offers three fixed taps.

**The range.** Measured with the MacBook's own microphone, every semitone from 98 Hz to 3,951 Hz: how loud it
is over a quiet room, and how much of the sound is the note itself.

```text
│                                      ###
│                          #+#     +++#####        #
│                         +#+#+   .+++#####       .#
│              .       ++++#+#+   .+++#####    # #.# #
│   ++        +. . #..+++++#+#++++.+++#####+ #####.###          .#
│   +++#.     +...+#..+++++#+#++++.+++#####+ #####.####+        .#
│  ++++#...   +...+#..+++++#+#++++.+++#####+.#####.####+        .#
│ .++++#...++++...+#..+++++#+#++++.+++#####+.#####.####+  +#    .#
│..++++#...++++...+#..+++++#+#++++.+++#####+.#####.####+#.+#  .#.#
│..++++#...++++...+#..+++++#+#++++.+++#####+.#####.####+#.+#+..#.#
└─────────────────────────────────────────────────────────────────
      131         262         523         1047        2093    3951
   public notes stop at 600 Hz ─┘

across: every semitone, in Hz · up: loudness, +16 to +34 dB
# clean note   + fair   . mostly buzz
```

It's loud from about 100 Hz to 4 kHz, with a few dead spots where the glass resonates. Its loudest, cleanest
notes are around 1 kHz, which covers the most important part of a voice.

**The voice.** macOS's own voice renders the line to a temporary WAV file. Every 10 ms, linear prediction finds the voice's
two strongest resonances (formants), its loudness and its pitch. The motor plays those two resonances as tones,
restarted on every beat of the pitch, so they buzz like a voice instead of whistling. Hiss sounds (s, f, t)
become quick bursts of high tones. Two resonances beat three: in a blind test they were clearer, and louder,
because the motor's strength is split fewer ways.

It sounds robotic, and it's much easier to follow when you can read along, which is why the show types every
line as it speaks.

**The code.** One C file, [nospeaker.c](nospeaker.c), using macOS system libraries.

## 06 / caveats

- **Is it safe?** It never drives the motor past its normal maximum strength. Going past it is possible and
  louder, but the driver starts to stall, so nospeaker doesn't.
- MultitouchSupport is private and undocumented, so a macOS update could break this.

The MultitouchSupport function names were learned from the open-source Tactile project. Everything else was
measured on a MacBook.

```text
──────────────────────  thanks for listening  ───────────────────────
```

Made by [Cody Collier](https://x.com/codycollier) · [a90.com](https://a90.com) · [MIT license](LICENSE)

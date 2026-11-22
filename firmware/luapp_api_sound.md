# Sound

The gadget's speaker, for game sounds and short tunes. 4 channels play at
once; a new sound on a channel replaces what it was playing. Calls return at
once (the sound plays on its own). Sound plays only while the app is on
screen and Muse isn't talking or listening, at the gadget's volume (muted
when its speaker is off).

    sound.effect(name [, {volume=60, ch=4}])
      -- click, select, coin, jump, laser, hit, explosion, powerup, win,
      -- lose, error, tick
    sound.tone(freq, ms [, {wave="square", volume=50, to, fade=false, ch=3, loop=false}])
      -- freq 20 to 7900 Hz; to: slide to this frequency; fade: fade out
    sound.play(melody [, {bpm=120, wave="square", volume=40, ch=1, loop=false}])
    sound.stop([ch])     -- one channel, or all of them
    sound.busy([ch])     -- true while it (or any channel) plays

Waves: "square", "pulse" (thinner), "triangle" (soft), "saw" (buzzy), "sine"
(pure), "noise" (drums, explosions: freq sets the grain). Volume 0 to 100.

## Melodies

Notes separated by spaces: a letter A to G, then # (sharp) or b (flat), an
octave 0 to 8 (C4 is middle C), and :length, where 1 is a whole note, 4 a
quarter and 8 an eighth (1, 2, 4, 8, 16 or 32); a trailing . makes a note
half as long again. R is a rest. A note without an octave or length keeps
the previous note's. Up to 128 notes.

    sound.play("C4:8 E G C5:4 R:8 G4:8 C5:2", {bpm=140})
    sound.play("G4:8 B D5 G5:4. D5:8 G5:2", {wave="pulse", volume=35})
    sound.play("C3:8 G2 C3 G2", {wave="triangle", ch=2, loop=true})   -- bass loop

Channel 1 is for music, 2 for a bass line, 3 and 4 for effects (the
defaults). Write your own tunes. Keep them short and volumes moderate: it's
a small speaker.

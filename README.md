# player (1) 
A dead simple, yet usable music player: now with a few features that actually make it tolerable to use.

# fyi

"Aim to create a system that is simple, transparent, and easy to pick up, without having to give up practicality and a rich feature set."

# features (added on from [m701](https://github.com/unixextremist/m701))

- pausing/playing
- reinit replaced by looping
- rewind/fast-forward

# Checklist

- [ ] Add an internal volume control system to regulate the volume of the music from inside the player
- [x] Add Vim key support


# compiling (void) 

Dependencies: `doas pacman -S alsa-lib ffmpeg ncurses clang make`

Building: `make`

Install: `sudo/doas make install`


# License 

The original work was licensed under the ISC license, but this fork is licensed under the GPL-v3

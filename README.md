# GEMU for Nintendo GameCube/Wii
This is a port of GEMU (Generic EMUlator) for the Nintendo GameCube and Nintendo Wii. GEMU is a multi-machine emulator designed to emulate as many different machines and their peripherals as possible.

Note that not all machines available on GEMU are available on the GameCube or Wii due to the hardware limitations of the consoles. Some machines may require a GameCube controller or Wii Classic controller.

## Building

```
tools/install-devkitpro.sh            # installs needed tools / libraries
source /etc/profile.d/devkit-env.sh
make                                  # makes gemu.dol
make package                          # creates dist/apps/gemu/ for the SD card
make TARGETS="chip8"                  # pick machine families to include (default: all)
```

## Run on Dolphin
```
tools/run-dolphin.sh                  # builds, packages, and start Dolphin
tools/run-dolphin.sh game.ch8 foo.8o  # ...with these ROMs on the SD card
make run                              # ditto as the first line
tools/run-dolphin.sh --hbc            # boot the Homebrew Channel with GEMU on its SD card
```

For `--hbc` to work, Homebrew Channel needs to be installed onto your Dolphin NAND

## Running on a real Wii

Copy `dist/apps/` to the SD card root (`sd:/apps/gemu/boot.dol`). ROMs are
looked up in `sd:/gemu/<machine>/`, `usb:/gemu/<machine>/`,
`sd:/roms/<machine>/`, `usb:/roms/<machine>/` and next to `boot.dol`.

A ROM path given as the first argument (HBC `<arguments>`) boots it directly.

# Disclaimer
Like GEMU, this emulator is made with AI, mainly as an experiment to see how many different machines AI can emulate with only its datasheets, manuals, expected behaviors, etc described. I have enough programming experience to monitor the code and ensure its maintainable and not horrendous, so its not *fully* vibecoded, but still - if you are against AI software, this is not for you!
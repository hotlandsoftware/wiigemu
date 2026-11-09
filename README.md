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

## Netplay (Wii, NES)

NES games can be played online with other people through a Kaillera
server, as in GEMU on a computer - and with GEMU on a computer: a Wii and a
PC can play the same game together.

1. Set up the Wii's Internet connection in Wii Settings.
2. Load the game, press HOME, then **Netplay > Connect...**. Your name is the
   Wii's nickname to begin with (A on Name changes it, on an on-screen
   keyboard). Pick a server from the list - the ones you've used, then the
   public Kaillera lists - or type one in.
3. In the lobby, **Create Game** offers the loaded game; to join someone's,
   load the same ROM and pick it. The game's owner starts it.
4. **+** chats (to the server, or in a game to the other players). Leaving
   the game, or the NES, puts your own game back as it was, battery save
   included.

While the Netplay screen is open your game is paused - and in a netplay
game, everyone's. Not available on the GameCube (no network without a
Broadband Adapter).

`tools/kaillera_test_server.py` is a small local Kaillera server for trying
it out, and `tools/run-dolphin.sh --netplay=HOST:PORT roms/nes/smb.nes`
runs a scripted netplay game against it in Dolphin.

# Disclaimer
Like GEMU, this emulator is made with AI, mainly as an experiment to see how many different machines AI can emulate with only its datasheets, manuals, expected behaviors, etc described. I have enough programming experience to monitor the code and ensure its maintainable and not horrendous, so its not *fully* vibecoded, but still - if you are against AI software, this is not for you!
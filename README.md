# gameport-steamworks-shim

The part of [GamePort](https://github.com/gameport-project/gameport-app) that makes a game believe Steam is there.

A Steam game asks the Steamworks API (`libsteam_api.so`) who the player is, whether the game is owned, what the cloud saves are, and for a session ticket to log in to the game's own servers. On a headset or a phone there is no Steam client to answer. This library takes the place of that API inside the game and answers for it, using what GamePort leaves for the game: the Steam account that is signed in, the game's AppID, and a session ticket made by GamePort.

GamePort puts the compiled library into each game it patches; it is never part of GamePort itself.

## Based on the Goldberg Steam Emulator

This repository is a **fork of the [Goldberg Steam Emulator](https://gitlab.com/Mr_Goldberg/goldberg_emulator)** by Mr. Goldberg, licensed under the **LGPLv3** (see [LICENSE](LICENSE)). Its history is kept as it was, and our changes sit on top of it in the `gameport` branch. The original project's readme is kept as [README.upstream.md](README.upstream.md).

What GamePort changed:

- it builds and runs on Android (arm64), with the POSIX path helpers ported to Bionic;
- it answers the newer `SteamClient023` interface and a no-op `SteamTimeline`, which games built with recent SDKs ask for;
- the AppID, the data folder and the player's identity come from files that GamePort's patch leaves with the game, not from a hand-written configuration;
- the session ticket is the real one GamePort asks Steam for, handed over for each request, including the one for web services;
- an Android build script (`android/build_shim.sh`).

Because the library is LGPLv3, the source of any changes to it has to stay available: this repository is that source.

## Building

`android/build_shim.sh` builds it with the Android NDK. It needs the NDK (`ANDROID_NDK`) and the sources of protobuf (`GP_PROTOBUF_SRC`), already built for Android. Then GamePort's `scripts/stage_shim.sh` copies the result into the app. Details are in GamePort's [development notes](https://github.com/gameport-project/gameport-app/blob/main/docs/DEVELOPMENT.md).

## Not affiliated

This is an independent community project. It is not made by, or affiliated with, Valve. Only games the user owns are meant to be run with it.

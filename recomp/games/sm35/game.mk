# Super Mario Bros. 35 (title 0100277011F1A000), version 1.0.2: what the shared Makefile needs to know.
GAME_ID        := sm35
GAME_TITLE     := SM35 Recomp
GAME_BUNDLE_ID := org.sm35recomp.sm35recomp
GAME_TITLE_ID  := 0x0100277011F1A000
GAME_DISPLAY_VERSION := 1.0.2
# The app's Nextendo Network client id (public: an OAuth public client with PKCE; runtime/net/nextendo.c)
GAME_NEXTENDO_CLIENT_ID := nxc_sdpEwnQyyHwl
# The engine links the movie library (subsdk1), but the game has no movies (none in its RomFS) and never calls it: no
# runtime/hle/movie.c, no FFmpeg (its imports stay unbound host stubs)
GAME_MOVIES    := 0

# The game's glue, built into the executable with the shared runtime
GAME_SRC       := $(wildcard games/sm35/*.c)
# Guest symbols replaced by host code before recompiling (one mangled name per line; empty: none)
GAME_HLE       := games/sm35/hle_symbols.txt
# Per-instruction patches for the generator (none yet)
GAME_PATCHES   := games/sm35/patches.py
GAME_PATCH_DEPS :=

# Your own dump, extracted: the base game (v1.0.0) and the v1.0.2 update. `make sm35` reads them from here unless
# you pass other paths, e.g. make smb35 DUMP_BASE_ROMFS=/path/to/romfs. Only the update's exefs is used (an update
# ships the whole exefs); its romfs goes over the base game's.
DUMP_ROOT         ?= $(HOME)/smm-work/sm35
DUMP_BASE_ROMFS   ?= $(DUMP_ROOT)/base-romfs
DUMP_UPDATE_EXEFS ?= $(DUMP_ROOT)/v101-exefs
DUMP_UPDATE_ROMFS ?= $(DUMP_ROOT)/v101-romfs
SETUP_ARGS        ?= --base-romfs "$(DUMP_BASE_ROMFS)" --update-exefs "$(DUMP_UPDATE_EXEFS)" \
                     --update-romfs "$(DUMP_UPDATE_ROMFS)"

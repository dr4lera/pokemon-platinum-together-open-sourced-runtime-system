# libntrsystem

This separately maintained community-runtime fork starts at
cybervisi0n/libntrsystem `cd778c9d5e45869f8ecb8245b316ac20ea576958`.
It supports the user's source gameplay client; existing upstream provenance and
notices are retained. New changes were developed with OpenAI Codex assistance
and are not submitted upstream.

The desktop NCLR loader now interprets serialized 32-bit fields separately from
native pointers, borrows the exact validated palette payload and owns stable
typed descriptors until their source heap allocation is released. It no longer
copies beyond a short palette or writes native pointers into its file bytes.
Explicit sized entry points validate actual supplied extents; the inherited
unsized API still requires a complete buffer matching its declared file size.
The ARM decoding branch is preserved.

The gameplay client's `tests/run_community_palette.py` compiles this actual loader
and actual SDK structures against synthetic, guard-page-protected resources.
Those checks establish decoder and descriptor-lifetime behavior, not full game
or graphics parity. No game assets, saves or private references are included.

Drop-in replacement for NitroSystem that targets Nintendo DS, PC, and other platforms.

libntrsystem makes it easier to port Nintendo DS games to PC and other platforms by handling the logic for common libraries used by nearly all Nintendo DS games.

## Available Build Targets
Select the build target with the 'build_target' meson option.
 * arm (Nintendo DS hardware)
 * linux (x86_64 Linux PC)
 * win64 (x86_64 Windows PC)
 * nx (Switch via libnx)

The desktop palette decoder also recognizes the exact `compNum10` metadata
written by the public nitrogfx tool into the upper format halfword of Pokemon
palettes (`0x000a0004`). It normalizes that reviewed encoding to GX format4;
other unexpected format flags remain invalid. This keeps valid original sprite
palettes compatible with strict bounds validation.

# Redistributable recovery fonts

`winre-light.ttf`, `winre-semilight.ttf` and `winre-regular.ttf` are the three faces
a **public** build of the WinRE-look recovery uses in place of Segoe UI. Segoe UI
belongs to the Windows it is copied from and may not be redistributed, so the
installer keeps using it for the recovery it builds on each user's own PC. The
recovery attached to a release is built with these fonts instead.

They are compiled by `make_fonts.py` from **Selawik**, Microsoft's open-source
stand-in for Segoe UI ([microsoft/Selawik](https://github.com/microsoft/Selawik),
commit `89362e8`), licensed under the **SIL Open Font License 1.1** (`OFL.txt`):

| File | Face |
|------|------|
| `winre-light.ttf` | the Light master |
| `winre-semilight.ttf` | interpolated halfway between Light and Regular (weight 350, Segoe's Semilight) |
| `winre-regular.ttf` | the Regular master |

Compiling is a change of format, which makes these OFL *Modified Versions*: they
may not carry the Reserved Font Name "Selawik", so the family is renamed
**S9WoA Sans**. The copyright notice is unchanged, and the recovery builder copies
`OFL.txt` into the image next to the fonts. The build is reproducible: running
`make_fonts.py` on the pinned commit gives these exact files.

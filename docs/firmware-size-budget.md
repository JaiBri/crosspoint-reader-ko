# Firmware Size Budget

The released `gh_release` firmware must install onto a locked X4 in a single OTA
step. The OTA path writes the new image into the inactive app partition of the
device's current (stock Crosspoint) layout, so the firmware has to fit that
partition.

## The constraint

Stock Crosspoint 1.3.0 app partition (`upstream/release/1.3.0:partitions.csv`):

```
app0, app, ota_0, 0x10000,  0x640000   ; 6,553,600 bytes = 6.25 MB
app1, app, ota_1, 0x650000, 0x640000
```

`gh_release` `firmware.bin` must stay at or below **6,553,600 bytes**. If it is
larger, the OTA unlocker cannot flash KO directly and the user is forced through
the two-step path (OTA to Crosspoint, then SD-card flash to KO — see issue #15).

This repo's own `partitions.csv` keeps the larger `0x6A0000` (6.625 MB) app
partitions for the full-flash / SD-update path; that is independent of the OTA
size limit above.

## What gets us under the limit

Korean fonts dominate the image (~4.3 MB: KoPub Batang 14 incl. Hanja, plus
Pretendard 10 UI). They are the product and are kept in full. The two cuts that
make `firmware.bin` fit are configured in the `gh_release` env in
`platformio.ini`:

| Cut | Where | Saving |
|-----|-------|--------|
| Drop Latin/Cyrillic hyphenation tries | `-DCP_HYPHENATION_LANGS=0` (`gh_release`) | ~310 KB |
| Omit serial logging | no `-DENABLE_SERIAL_LOG` (`gh_release`) | ~27 KB |
| Ship only Korean + English UI | removed 24 `lib/I18n/translations/*.yaml` | ~184 KB |
| Disable upstream SD Card Fonts web/UI | `-DCP_DISABLE_SD_CARD_FONTS=1` + `build_src_filter` | ~7 KB |

Korean text is not Liang-hyphenated, so dropping the hyphenation tries has no
effect on Korean reading. `CP_HYPHENATION_LANGS` defaults to `1` (all languages)
for non-release builds and upstream parity; individual languages can be toggled
with `CP_HYPHEN_LANG_<EN|FR|DE|RU|ES|IT|UK|SV|PL>`. See
`lib/Epub/Epub/hyphenation/LanguageRegistry.cpp`.

The 1.3.0 merge added many upstream features (RTL, bookmarks, sup/sub, tiled
grayscale, sleep timer, footnotes, SD card fonts, +14 languages) that pushed the
image ~150 KB over budget. The decisive cut was dropping the non-Korean/English
UI languages: this fork ships **EN + KO only**. `gen_i18n.py` maps any removed
language in the v1 migration table to a fallback, and `I18n` defaults to Korean,
so a device that had saved a now-removed language simply falls back to Korean.

## Measured (version 1.3.0-ko.0)

| Build | `firmware.bin` | vs 6.25 MB |
|-------|---------------|------------|
| merge result, 26 languages, all features | 6,701,856 | +148,256 over |
| + SD Card Fonts web/UI compiled out | 6,694,496 | +140,896 over |
| + EN/KO only | 6,506,240 | 47,360 under |
| + KO custom/system SD fonts restored (`gh_release`) | 6,512,224 | 41,376 under |

The Korean custom reader font + UI system font (glyph-fallback to Pretendard) ride on upstream's
`SdCardFont` engine (`.cpfont` v4), so re-adding them cost only ~6 KB.

Headroom is ~41 KB. Any sizeable new feature will push `firmware.bin` back over
the limit. Before adding flash-resident data, rebuild `gh_release` and check
`firmware.bin` against 6,553,600 bytes. If more room is needed, the next levers
are excluding X3-only code (tilt/gyro/NTP, currently always linked) or reducing
Hanja coverage in `kopub_14_regular`.

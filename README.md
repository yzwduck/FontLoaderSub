# FontLoaderSub

OpenType font loader for subtitles (ASS/SSA), inspired by [CryptWizard's FontLoader](https://bitbucket.org/cryptw/fontloader).

Instead of drag-and-drop font files, it accepts ASS/SSA subtitles and loads corresponding font files in its directory.

## Usage

1. Move `FontLoaderSub.exe` to the root of font directory;
1. Drag-and-drop subtitles `*.ass` (or folders) onto `FontLoaderSub.exe`.

## UI

* Line 1: requested fonts from subtitles;
* Line 2: stats for font collection;
* `OK`: minimize;
* `Retry`: ignore cache and reload font metadata;
* `Close`, Esc key, Alt+F4: unload fonts and exit.

## Note

* In order to work with huge font collections, it builds the cache for fonts' metadata for fast lookup.
* Only accept ASS/SSA files under 64MB, encoded in Unicode with BOM.
* Only load the first matching font file.
* Windows 7 (or later) required.

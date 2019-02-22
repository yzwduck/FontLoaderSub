# FontLoaderSub

OpenType font loader for subtitles (ASS/SSA), inspired by [CryptWizard's FontLoader](https://bitbucket.org/cryptw/fontloader).

Instead of font files, drag-and-drop ASS/SSA subtitles and it will load corresponding font files in its directory.

## Usage

1. Move `FontLoaderSub.exe` to the root of font directory;
1. Drag-and-drop subtitles `*.ass` (or folders) onto `FontLoaderSub.exe`.

## UI

* Line 1: requested fonts from subtitles;
* Line 2: stats for font collection;
* `OK`: minimize;
* `Retry`: rebuild font cache;
* `Close`, Esc key, Alt+F4: unload fonts and exit.

## Note

* In order to work with huge font collections, font cache `fc-subs.db` will be built for fast lookup.
* Only accept ASS/SSA files under 64MB, encoded in Unicode with BOM.
* Windows 7 (or later) required.

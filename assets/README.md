# FileFlow icon identity

The light-grid mark uses three rounded corner markers and a six-by-six field of
cells. Cyan (#7DD3F0), soft green (#A8E6A1), and off-white (#E1E6E8) sit on deep teal.
Keep the marker positions and cell pattern consistent across platforms.

The canonical geometry and raster renderer live in `desktop/src/icon_art.h`.
`desktop/tools/icon.cpp` exports the desktop PNG/ICO/iconset files during builds.
To regenerate the checked-in SVG and Android vectors after changing the artwork:

```sh
cmake --build build/desktop-app --target ff-icon
build/desktop-app/desktop/ff-icon --brand-assets .
```

- **macOS:** the legacy ICNS canvas has a centered 824/1024 rounded tile, a
  transparent margin, and 16–1024 px representations. Use the bundle icon;
  calling SDL_SetWindowIcon on macOS replaces the Dock image and loses its native
  multi-resolution presentation.
- **Windows:** the ICO contains 16, 20, 24, 32, 40, 48, 64, 96, 128, and 256 px
  representations. The running window uses the same raster artwork.
- **Linux:** the window and installed hicolor launcher icon use that artwork.
  Desktop archives include the PNG and desktop entry under `share/`.
- **Android:** the same paths are centered in the adaptive icon's 108dp canvas,
  entirely within the 66dp safe circle. The OS supplies the outer mask. A separate
  opaque monochrome vector preserves the marker holes for themed icons.
- **Web:** `fileflow-icon.svg` is the matching transparent, scalable export.
  The separate web project can adopt it without redrawing the logo.

When reviewing a change, check the packaged icon and the running app, small sizes
on both light and dark backgrounds, Android circle/rounded masks, and monochrome.
Do not add an opaque canvas or pre-mask Android's foreground.

Platform references:
[Apple app icons](https://developer.apple.com/design/human-interface-guidelines/app-icons)
and [Android adaptive icons](https://developer.android.com/develop/ui/views/launch/icon_design_adaptive).

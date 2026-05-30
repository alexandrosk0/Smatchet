# Third-party licenses

Smatchet vendors or links the following third-party components. Each component's
upstream license terms apply to that component; nothing in this file overrides
those terms or the project's main [`LICENSE`](LICENSE) (MIT).

Components fetched at build time by CMake `FetchContent` (cpr, SQLiteCpp, nlohmann/json,
md4c, ImGui, GLFW, Lua, sol2, ghc::filesystem, cpp-httplib, doctest) carry their own
LICENSE files in their respective fetch directories — see `.fetchcontent-src/<name>-src/`
once the project has been configured.

The components listed below are either vendored directly into the source tree, or
shipped alongside the binary as runtime assets, so their licences are inlined here.

---

## IconFontCppHeaders — `Source/Core/ThirdParty/IconsFontAwesome6/IconsFontAwesome6.h`

Source: <https://github.com/juliettef/IconFontCppHeaders>
Vendored revision: header generated against Font Awesome 6.x metadata.

License: **zlib**

```
Copyright (c) 2017 Juliette Foucaut and Doug Binks

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

---

## Font Awesome 6 Free Solid — `assets/fonts/fa-solid-900.ttf`

Source: <https://github.com/FortAwesome/Font-Awesome>
Distribution: shipped next to the Smatchet executable via CMake POST_BUILD copy
(not committed to this repo's git history; see `assets/fonts/README.md` for the
drop-in contract).

License: **SIL Open Font License 1.1**

```
Copyright (c) 2024 Fonticons, Inc. (https://fontawesome.com)
```

Full SIL OFL 1.1 text: <https://scripts.sil.org/cms/scripts/page.php?site_id=nrsi&id=OFL>

The license permits redistribution, modification, and embedding of the font in
commercial products, subject to two conditions: (1) the font itself may not be
sold standalone (Smatchet ships it bundled with the application, not as a font
package), and (2) the reserved font names "Font Awesome" and "Font Awesome 6"
may not be used to identify derivative works.

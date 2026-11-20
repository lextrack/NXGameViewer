# Third-party code and documentation

NX Game Viewer is under the MIT licence (see `LICENSE`). It contains no code copied from
GPL projects. These projects are used or credited:

## Libraries the app links to

| Library | Licence | Used for |
|---|---|---|
| [Qt 6](https://www.qt.io/) (Core, Gui, Widgets, Multimedia, Concurrent, DBus on Linux) | LGPL-3.0 (linked dynamically) | the whole app; sound playback; reading common images and sounds; loading files in the background; showing files in the file manager |
| [FFmpeg](https://ffmpeg.org/), through Qt Multimedia's plugin | LGPL-2.1+ (loaded at run time by Qt) | decoding common sound formats (MP3, Ogg, FLAC, M4A, …) |
| [libopus](https://opus-codec.org/) | BSD-3-Clause | decoding `.nxopus` sounds |
| [bc7enc](https://github.com/richgel999/bc7enc) by Richard Geldreich, Jr. (bundled in `third_party/bc7enc`, unmodified) | MIT (dual-licensed with public domain; used under MIT, notice in `third_party/bc7enc/LICENSE`) | compressing changed texture blocks to BC7, BC1, BC3, BC4 and BC5 when replacing a texture |
| [astc-encoder](https://github.com/ARM-software/astc-encoder) by Arm Limited, version 5.2.0 (library sources bundled in `third_party/astcenc`, unmodified) | Apache-2.0 (licence text in `third_party/astcenc/LICENSE.txt`; the project has no NOTICE file) | decoding ASTC textures, and compressing changed ASTC blocks when replacing a texture |
| [zstd](https://github.com/facebook/zstd) (libzstd) | BSD-3-Clause (dual-licensed with GPL-2.0; used under BSD) | opening zstd-compressed files (`.zs`), compressing rebuilt mod files again, and reading compressed Switch games (NSZ/XCZ) |
| [zlib](https://zlib.net/) | zlib licence | reading zlib and raw-deflate entries in RPF6 archives |
| [OpenSSL](https://www.openssl.org/) (libcrypto) | Apache-2.0 (linked dynamically) | AES-128 (ECB, XTS and CTR) for decrypting the user's own Switch game dumps in the game extractor, and AES-256 for the encrypted file lists of RPF6 archives |

## Format knowledge from other projects

### hactool by SciresM — ISC

<https://github.com/SciresM/hactool>. The game extractor (`src/formats/switchfs/`) follows hactool for
how Switch game dumps are put together: the key file format and how the header key, key area keys and
titlekeks are made from the master keys and key sources (`extkeys.c`, `pki.c`); the NCA header
(decrypted with AES-XTS using big-endian sector numbers), the key generation and key area, titlekey
crypto through tickets, the section headers, and the AES-CTR counter made from the section counter
and the offset (`nca.c`, `nca.h`); the PFS0 and HFS0 partitions (`pfs0.h`, `hfs0.h`); the XCI header
and its secure partition (`xci.h`, `xci.c`); the IVFC levels, the RomFS tables and how the last level
is checked against its hashes (`ivfc.h`, `romfs.c`). The C++ is the app's own, and the AES work is done
by OpenSSL. The CNMT and NACP fields (title type and version, content records, names, publisher,
display version, icons) and the ticket layout were checked against the user's own dumps. Updates
(BKTR patches) are not read yet. The app ships no keys: they are read from a file the user dumped
from their own console.

Lichii and the yuzu family of emulators it comes from are GPL-licensed; their code was not used.

Its licence notice:

```
ISC License

Copyright (c) 2018, SciresM

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

### nsz by nicoboss and blawar — MIT

<https://github.com/nicoboss/nsz>. The game extractor reads compressed NSZ and XCZ packages
(`src/formats/switchfs/switchncz.cpp`) following the format described in nsz's `docs/formats.md` and its
block reader (`nsz/BlockDecompressorReader.py`): the NCZ file keeps the first 0x4000 bytes of the NCA,
then an `NCZSECTN` section table (offset, size, crypto type, key and counter for each section), an
optional `NCZBLOCK` table of independently compressed blocks (a block stored at full size is plain), and
zstd data that decompresses to the rest of the NCA, already decrypted. nsz is Python; the C++ is the
app's own, and decompression is done by libzstd.

### Emulator key folders — Ryujinx (MIT) and the yuzu family (GPL, reference only)

The game extractor looks for the user's own `prod.keys` and `title.keys` in the folders where Switch
emulators keep them (`src/formats/switchfs/switchkeysearch.cpp`). The folder layout was taken from the
public documentation and setup guides of [Ryujinx](https://en.wikipedia.org/wiki/Ryujinx) (MIT: a
`system` folder in its data folder, or `portable/system` next to the program) and of yuzu and its
forks Eden, Lichii, Citron, Sudachi, Suyu and Torzu (GPL: a `keys` folder in their data folder, or
`user/keys` next to the program), and from hactool's `~/.switch` convention. No emulator code was used.
Key files are recognised by their content and are only read, never copied; key values are never shown
or logged.

### KuroTools — MIT

<https://github.com/nnguyen259/KuroTools>. The app's own C++ code follows these parts of it:

- `src/formats/data/ed9bytecode.cpp`: script instruction names and operand sizes, the
  command group names, and the tagged-value encoding (from `disasm/ED9InstructionsSet.py`
  and `lib/parser.py`).
- `src/formats/data/falcomscriptreader.cpp`: the meaning of the script and function header
  fields (from `disasm/script.py` and `disasm/function.py`).
- `src/formats/models/falcommdlreader.cpp`: the MDL section, material and node layouts (from
  `mdl/MDLFile.cpp`). Versions 4 and 5, and the vertex data section, were worked out
  separately from game files.

Its licence notice:

```
MIT License

Copyright (c) 2022 Nhan Nguyen

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### parse_ed9_mdl.py by uyjulian — MIT

<https://gist.github.com/uyjulian/9a9d6395682dac55d113b503b1172009>. `src/formats/models/falcommdlreader.cpp`
and `src/formats/models/gltfwriter.cpp` follow what it documents about MDL files: the vertex stream types
(packed normals, colours with red and blue swapped, UVs, bone weights and indices), the texture
slot of each shader, the Z·Y·X order of the node rotations and the flipped V texture coordinate.
It also documents the animation section (curve types 9–14 and their key sizes), which
`src/formats/models/falcommdlreader.cpp` reads to export animations. No code was copied. The node rotations,
the bounds block after each mesh and the material transparency switches were checked against game
files; so were the node's "rotate" quaternion, how animated rotations combine with the joint
orientation, the visibility curve (type 16) and the clip start and end times after the curves.

GPL-licensed tools (eArmada8's kuro_mdl_tool and misc_kiseki) were looked at as references only.

### noclip.website by Jasper St. Pierre — MIT

<https://github.com/magcius/noclip.website>. `src/formats/models/bfresreader.cpp` follows its
`src/fres_nx/bfres.ts` and `src/fres_nx/nngfx_enum.ts` for the layout of Switch BFRES files
(versions 5 to 9): the header, resource dictionaries, memory pool, FMDL, FSKL, FVTX, FSHP and FMAT
records, the vertex attribute format numbers, and the FSKA animation and curve records (frame and
key types, curve types, scale and offset). No code was copied; the reader is the app's own C++.
These were worked out separately from game files: the order of an FSKA bone's base values (scale,
rotation, translation), which curve target offsets mean which value, the bone and curve flag bits,
the Euler rotation flag of skeletons and animations, how rigid (one-bone) and unskinned shapes are
placed by their bone, and that the skeleton's rest pose matches its inverse bind matrices.

Its licence notice:

```
MIT License

Copyright (c) 2018 Jasper St. Pierre

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The BYML layout in `src/formats/data/bymlreader.cpp` also follows noclip's `src/byml.ts` (MIT, same notice
as above): the header, the key and value string tables, and the array and dictionary nodes. The
64-bit values, binary data and hash dictionaries of newer versions come from community documentation.
No code was copied.

### AssetStudio by Perfare — MIT

<https://github.com/Perfare/AssetStudio>. `src/formats/unity/unityscene.cpp`, which turns a Unity
game object into a whole model, is a C++ port of the approach in AssetStudio's
`AssetStudioUtility/ModelConverter.cs` and `AssetStudio/Classes/AnimationClip.cs`:

- walking the transform tree from a game object, and placing static meshes (MeshFilter and
  MeshRenderer) and skinned meshes (SkinnedMeshRenderer) on its nodes;
- the skeleton: bones from `m_Bones`, or found by `m_BoneNameHashes`, which are CRC-32 hashes of
  each transform's path and of every shorter path ending at it; and the bind poses;
- mirroring Unity's left-handed space into glTF's right-handed one: X of positions negated,
  quaternions as (x, −y, −z, w), bind poses as S·B·S with S = diag(−1, 1, 1, 1);
- materials: the texture of each `m_SavedProperties.m_TexEnvs` slot;
- animation clips: legacy curves, and Mecanim clips made of streamed frames (time, key count,
  then index and four Hermite coefficients per key, keeping the value), dense frames and
  constant values, matched to curves through `m_ClipBindingConstant.genericBindings`, where a
  position, scale or Euler binding spans 3 curves and a rotation binding 4.

The C++ is the app's own and reads objects through the app's type-tree reader. Choosing a
texture by its name when a material only has generated slot names (as in Shader Graph), and
packing everything a model needs into one entry, are the app's own and were checked against
Super Mario RPG. Its licence notice:

```
MIT License

Copyright (c) 2016 Radu
Copyright (c) 2016-2020 Perfare

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### UnityPy by K0lb3 — MIT

<https://github.com/K0lb3/UnityPy>. Used as a reference, and to check results, for the Unity
readers in `src/formats/unity/`:

- `serializedfile.cpp`: the table of Unity's "common strings" (the names type trees point to
  with offsets that have the top bit set) was generated from UnityPy's bundled type database. The
  serialized-file header, type, object and external-file layouts, and how a type tree is walked
  (arrays, aligned fields, strings, `TypelessData`), follow UnityPy's `SerializedFile.py` and
  `TypeTreeHelper.py`.
- `unitymesh.cpp`: the vertex channel order, the vertex format numbers (and their older,
  pre-2019 numbering), and the 16-byte alignment between vertex streams match UnityPy's
  `MeshHelper.py`; the code was written separately and checked against Super Mario RPG's meshes.
- `unitytexture.cpp`: the Unity texture format numbers, and that Switch textures keep their GPU
  "GOBs per block" in bytes 8–11 of `m_PlatformBlob` (from UnityPy's `TextureSwizzler.py`, itself
  based on AssetsTools.NET). Unlike UnityPy, the app also undoes the block-linear layout when a
  texture uses one GOB per block: checked against game files, where those textures are stored
  padded to the block-linear size and only look right once deswizzled.
- `unitybundle.cpp`: the UnityFS header, block list and file list.

No code was copied; the app's readers are its own C++ code. Its licence notice:

```
MIT License

Copyright (c) 2019-2026 K0lb3

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### nxnative by seedds — MIT

<https://github.com/seedds/nxnative>. `src/formats/archives/rpf6reader.cpp` and
`src/formats/archives/rpf6key.cpp` follow its `dm/gfx/tools/rpf6.py`: the RPF6 header and entry
layout, the 16-round AES-256 ECB table encryption, the joaat name hashes, the RSC5 / RSC85
resource headers, the encrypted scripts, and the two ways the key is found in the game's
executable (a 32-byte window that decrypts the root entry, and the AArch64 key setup that
builds it from four 16-byte constants). The app ships no key: it is read from the user's own
executable and kept in the app's settings.

### vgmstream — ISC-style permissive licence

<https://github.com/vgmstream/vgmstream>. `src/formats/sound/nxopusreader.cpp` reads the
`.nxopus` header fields documented in its `src/meta/nxof.c`, and `src/formats/sound/bwavreader.cpp` reads
the BWAV header and channel fields documented in its `src/meta/bwav.c` (codec, sample rate, sample
counts, DSP coefficients and history, data offset, loop points). No code was copied. The DSP-ADPCM
decoder follows the publicly documented GameCube/Wii/Switch "DSP ADPCM" algorithm. The BARS sound
pack layout (`src/formats/sound/barsreader.cpp`: entry table, AMTA blocks and where version 5 keeps the
sound's name) was worked out from game files.

The CRI readers in `src/formats/cri/` follow vgmstream's documentation of CRI's formats: the
`@UTF` table layout (`src/meta/cri_utf.c`), the AFS2 sound archive (`src/meta/awb.c`), how an
ACB cue leads to its waveforms through the cue, synth, sequence, track and track event tables
(`src/meta/acb.c`), and the standard Nintendo Opus header (`src/meta/opus.c`) that
`src/formats/sound/nxopusreader.cpp` now also reads. No code was copied.

`src/formats/rage/awcreader.cpp` follows vgmstream's documentation of RAGE's AWC audio containers
(`src/meta/awc.c`, `src/meta/awc_streamfile.h`, `src/layout/blocked_awc.c`): the header flags, the
stream ids and tag table, the sound-effect and music headers, the codec numbers, the DSP header in
front of each sound, and how music interleaves its channels in blocks with a header and a DSP
header per channel. No code was copied; the DSP decoding reuses the app's own decoder.

### Community documentation

- SARC archives and Yaz0 compression (`src/formats/archives/sarcreader.cpp`, `src/formats/archives/decompress.cpp`,
  and the writers `src/formats/archives/sarcwriter.cpp`, `src/formats/archives/compress.cpp`): written from the
  public community documentation of these Nintendo formats (e.g. the ZeldaMods and Nintendo
  File Formats wikis): the name hash, the hash-sorted file table, hash collision counters and
  the Yaz0 match encoding. The Yaz0 compressor is the app's own (a hash-chain match finder).
  No code was copied.

- MSBT messages (`src/formats/data/msbtreader.cpp`): written from public community documentation of
  the LMS message formats (e.g. the Nintendo File Formats and ZeldaMods wikis): the header, the
  LBL1, ATR1 and TXT2 sections and the control tags inside the text. No code was copied.

- BCSV tables (`src/formats/data/bcsvreader.cpp`): written from public documentation of the Animal
  Crossing: New Horizons format (kinnay's Nintendo-File-Formats and Jon Barrow's BCSV write-up):
  the header, the field table and that each column's hash is the CRC32 of "name type". That the
  first 4 bytes of each record hold the record's own offset was worked out from game files. No
  column names are bundled: names come from a list the user supplies.

- BNTX textures (`src/formats/textures/bntxreader.cpp`, `src/formats/textures/bntxwriter.cpp`): the header,
  "NX" container, BRTI texture info and BRTD data layout, the format numbers and the channel
  selectors were written from public community documentation of the format (e.g. the
  Nintendo File Formats wiki), and checked against game files. Tools such as Switch Toolbox
  and BNTX-Extractor (GPL) were not used as a source, and no code was copied.

- Resource size tables (`src/formats/data/restblreader.cpp`): the RESTBL layout (version, name
  length, the CRC32-keyed entries and the named entries, where the key is the CRC32 of the romfs
  path without ".zs") follows public community documentation of the Tears of the Kingdom format
  (e.g. the ZeldaMods wiki and the MIT-licensed `restbl` tools), and was checked against game
  files. No code was copied.

- AI node files (`src/formats/data/ainbreader.cpp`): the header, command, node, parameter, plug and
  global-parameter layouts follow RootCubed's MIT-licensed ainby (`src/file_formats/ainb.cpp`,
  Copyright 2023 RootCubed). Version 0x408 (seven global-parameter slots, user-defined multi-inputs)
  was worked out from game files. No code was copied.

- Event flows (`src/formats/data/evflreader.cpp`): the flowchart, timeline, actor, event, container and
  DIC layouts follow the public documentation in the evfl project (`bfevfl_layout.md`) and the
  ZeldaMods wiki, and were checked against game files, including version 4. evfl (GPL) was used only
  as documentation of the format; no code was copied.

- Animation sequences and events (`src/formats/data/asbreader.cpp`, `src/formats/data/baevreader.cpp`):
  the command, node and BFFH/BFSI container layouts follow the public research in the TotkMods
  Research repository and the ZeldaMods EPD wiki, checked against game files. The ASB 0x41B header,
  and that BAEV animation keys are the FNV-1a hash of the clip name, were worked out from game files.
  No code was copied.

- Terrain tiles and scenes (`src/formats/data/terrainreader.cpp`): the TARB container (grid, margin,
  array count and the offset/size block pairs) and the Ocarina of Time layer table in TSCB follow the
  public research in BladesawStudios' TscbSharp (AGPL, used only as documentation of the format) and the
  ZeldaMods TSCB pages. That the tiled block is a block-linear texture with swapped GOB sectors, that
  heights are float32, and that materials are four indices plus four weights were worked out from game
  files. No code was copied.

- Nintendo fonts (`src/formats/data/nintendofont.cpp`): the BFTTF/BFOTF header and the XOR scheme
  (the key is the stored magic XOR 0x18029A7F) were worked out from game files and the public
  documentation of the Switch shared fonts. No code was copied.

- BYML and MSBT writers (`src/formats/data/bymlwriter.cpp`, `src/formats/data/msbtwriter.cpp`): written from
  the same public documentation as the readers (the ZeldaMods and Nintendo File Formats wikis): the
  sorted key and string tables, the container and value encodings, the hash dictionaries, and the
  TXT2 section with its offset table and control tags. No code was copied.

- BFRES material animations and embedded files (`src/formats/models/bfresreader.cpp`): the header
  slots of the material, bone visibility, shape and scene animation dictionaries and of the
  embedded file array follow the MIT-licensed BfresLibrary; the version 10 FMAA entry size, name,
  frame count and loop flag were worked out from game files. No code was copied.

- .NET BinaryFormatter data (`src/formats/data/nrbfreader.cpp`): written from Microsoft's published
  [MS-NRBF] specification (".NET Remoting: Binary Format Data Structure"): the record types,
  class and member type information, arrays and primitive encodings. No code was copied.

- LZ4 block decompression (`src/formats/unity/lz4block.cpp`): the app's own decoder, written from
  the LZ4 block format description in the LZ4 project's documentation.

- XTX texture layout and NVN pixel format numbers: community documentation of the
  format (e.g. AboodXD's XTX-Extractor). The Tegra X1 block-linear layout (including the
  block height each mip level uses), the box-filtered mip levels and the BC1–BC7
  decoders are written from the public format specifications.

- CRI USM movies (`src/formats/cri/usmreader.cpp`): written from the public community
  description of the USM container (the `CRID`, `@SFV` and `@SFA` chunks, their 0x20-byte
  header with the payload offset, padding, channel and data type, and the `@UTF` stream
  headers), and checked against Super Mario RPG's movies. No code was copied.

- Built-in Unity class layouts (`src/formats/unity/builtintypetrees.cpp`): the field lists
  (type trees) of Texture2D, Sprite, TextAsset, Font, Cubemap and Mesh for Unity 2021.3, as
  Unity itself writes them into asset bundles. They were read from bundles of a game built with
  that version and contain no game content. They are used only for files saved without type
  information, and only when a layout reads each object of its class exactly to its last byte.

- RAGE resources and texture dictionaries (`src/formats/rage/`): the RSC5 size flags follow the
  public community documentation of RAGE resources from the GTA IV era; the RSC85 extended size
  word, the `.wtd` dictionary and texture layouts (name, size, format code, mip count and pixel
  pointer) were worked out from Red Dead Redemption's files and checked against ImageMagick's
  DDS decoder. No code was copied. Models, fragments, map tiles and particle files carry their
  own texture dictionaries inside; the app finds them by their shape (parent pointer 0, usage
  count 1, a hash array and a texture array of the same length, and every texture readable).
  That dictionary layout is the one described in CodeX.Games.RDR1's `RSC6/Rsc6Texture.cs` (see
  the next item), which was used as a reference only.

- RAGE string tables (`src/formats/rage/ragestringtablereader.cpp`): the layout of `.sst` / `.wst`
  resources (a hash table of slots, each a chain of hash entries pointing at a UTF-8 string) and of
  `.strtbl` files (language block positions, identifiers, then per language a count and UTF-16
  strings keyed by the joaat hash of their identifier, with a scale and offset after each) was read
  in [CodeX.Games.RDR1](https://github.com/Foxxyyy/CodeX.Games.RDR1) by Foxxyyy
  (`RSC6/Rsc6StringTable.cs`, `Files/StrtblFile.cs`). That project has no licence, so it was used
  as a description of the format only and no code was copied. The language order, the slots that
  share another language's block, and the per-string note (a 16-bit kind and a length-prefixed
  label, empty in most files) were checked against Red Dead Redemption's files.

- RAGE models (`src/formats/rage/ragemodelreader.cpp`): where `.wft` fragments, `.wfd` fragment
  drawables and `.wvd` visual dictionaries keep their drawables; the drawable, level-of-detail,
  model and geometry records; the vertex and index buffers; the vertex declaration (a 16-bit
  channel mask with a 4-bit type per channel) and its channel and type numbers; the skeleton and
  bone records; the shader group and shader parameter layout (and that the parameter name hashes
  follow the data); the sampler names used for colour and normal textures; and how an extended
  (RSC85) resource's flags give the page its root object starts on. All of this was read in
  CodeX.Games.RDR1 (`RSC6/Rsc6Drawable.cs`, `RSC6/Rsc6Fragment.cs`, `Files/WftFile.cs`,
  `Files/WfdFile.cs`, `Files/WvdFile.cs`, `RPF6/Rpf6File.cs`, `RPF6/Rpf6Crypto.cs`), used as a
  reference only; no code was copied. Checked against the game's files: bone world positions built
  from each bone's translation and rotation match the skeleton's stored world matrices, skinned
  vertices index the geometry's own bone list with weights summing to 255, terrain UVs are
  signed 16-bit normalised values, and the UVs need the same vertical flip as other Switch
  formats.

- RAGE animations (`src/formats/rage/rageanimation.cpp`): the `.was` animation set and its clip
  dictionary, the clip record (name, animation, start and end time), the animation, block, track
  and chunk records, the track and format numbers, the bit-packed arrays, and the constant,
  quantised and "smallest three" quaternion channels were read in CodeX.Games.RDR1
  (`RSC6/Rcs6Animation.cs`, `Files/WasFile.cs`, `RSC6/Rsc6Data.cs`), used as a reference only; no
  code was copied. Checked against the game's files: that the tracks name bones by the same id the
  skeleton's bones carry, which channel combinations occur (one channel per component when the
  format's packing bit is set), where the rebuilt component of a "smallest three" quaternion goes,
  and that the values are in the skeleton's own axes. Every clip in the game decodes to finite
  values.

- DDS textures (`src/formats/textures/ddsreader.cpp`): written from Microsoft's public DDS file
  format documentation (the header, the pixel format block and the DX10 extension).

# Making mods with NX Game Viewer

Change a game's textures or files and load them in an emulator or on a modded console.

The game itself is never changed. Each changed file is saved in a separate **mod folder**,
at the same path it has in the game. To undo a change, delete that file from the mod folder.

---

## What you need

- **Your own game**: an NSP, XCI, NSZ or XCZ file, or a `romfs` folder dumped by your emulator.
- **An emulator or console that loads mods**: yuzu and the emulators based on it (Eden and
  others), Ryujinx, or a console running Atmosphère.
- **An image editor** that saves PNG with transparency (GIMP, Krita, Photoshop…), for textures.

---

## Quick steps

1. **Open the game.** Drag its NSP/XCI or its `romfs` folder onto the window.
   (Or use **File → Extract game…** to unpack it into a folder first.)
2. **Make a mod: Mod → New mod…** Pick your emulator and its data folder, and type a name.
   The title ID is usually filled in for you. Click **Create**.
3. **Save the texture: Extract → Picture as PNG…**
4. **Edit the PNG.** Keep the same width and height, and keep the transparency.
5. **Put it back: Mod → Replace texture with PNG…** and pick your PNG.
6. **Turn the mod on** in your emulator and start the game.

To replace a whole file (not only a texture), open it and use **Mod → Replace file…**.

To change values or messages, open a BYML (`.byml`, `.bgyml`) or MSBT file, use
**Extract → Plain JSON…**, edit the JSON in any text editor, and use
**Mod → Replace with edited JSON…**. See [Editing data and messages](#editing-data-and-messages).

---

## Where the mod folder goes

The app makes these folders for you in **New mod…**:

| Emulator | Mod folder |
|---|---|
| yuzu, Eden and similar | `<data folder>/load/<title id>/<mod name>/romfs` |
| Ryujinx | `<data folder>/mods/contents/<title id>/<mod name>/romfs` |
| Atmosphère | `<SD card>/atmosphere/contents/<title id>/romfs` |

To find the data folder, use your emulator's **File** menu (for example *Open yuzu Folder*).

The **Mod** menu shows the mod you are saving to (*Saving to: …*). Use
**Choose mod to save in…** to pick another one.

---

## Turning the mod on

- **yuzu, Eden and similar:** right-click the game → **Properties** → **Add-ons**, tick your mod.
- **Ryujinx:** right-click the game → **Manage Mods**, enable your mod.
- **Atmosphère:** copy the `atmosphere` folder to the root of the SD card (unless you made the
  mod on the SD card already).

Restart the game after each change: mods are read when it starts.

---

## Changing several things in one file

Some files hold many textures (an archive, a `__Combined.bntx`, a model). When you change a
second texture in the same file, the app asks:

- **Keep earlier changes** (usually what you want) adds the new change to the old ones.
- **Start from the game's file** throws the earlier changes away.

---

## My mods

**Mod → My mods…** lists your mods and the ones found in your emulators' folders. From there
you can:

- see which files a mod changes, and **undo** some of them;
- **pack a mod as a zip** to share it;
- **delete** a mod.

Share only the files you changed, never the game itself.

---

## Editing data and messages

**BYML / BGYML** (game data, actor parameters, maps): every value keeps the type it has in the game's
file. A number that was a decimal stays a decimal even if you write `2`, and a whole-number field
refuses `2.5`. Very large 64-bit numbers are written as text in quotes (`"18446744073709551615"`), and
binary data as base64 text. You can add keys and list items; new values get a type from what you
write (whole numbers become integers, decimals become floats). Keys of hash tables look like
`0x1234abcd`.

**MSBT** (messages): change the text of existing messages. Control tags are kept as written in the
JSON: `[0:3 0a0b]` opens a tag (group 0, type 3, parameters in hex), `[/0:3]` closes it, and `[page]`
starts a new text box. Messages you do not change are kept byte for byte. Adding or renaming
messages is not supported.

If the JSON has a mistake, the app tells you the line and column, or the path of the value
(for example `Actors[3].Scale:`) and what it should be.

---

## Resource size tables

Games like Tears of the Kingdom and Ocarina of Time keep a table of how much memory each file needs
(`System/Resource/ResourceSizeTable….rsizetable.zs`). When a change makes a file larger, the app
saves an updated copy of that table in the mod folder, raising only the entries that need it. When
you change more files later, it builds on the copy already in the mod. Undoing a change does not
lower the table again; a larger entry does no harm.

---

## What can be changed

- **Textures:** XTX and BNTX, including those inside models, fonts and archives.
  Most pixel formats work (RGBA8, BC1–BC7, ASTC…).
- **Any file** in the game's `romfs`, with **Replace file…**.
- **Data and messages:** BYML/BGYML values and MSBT message text, with **Replace with edited JSON…**.
- **Archives:** files inside SARC archives (`.sarc`, `.szs`, `.zs`, `.pack`…), even nested ones.
  The app rebuilds and recompresses them for you, with the game's zstd dictionaries when it uses them.

Not yet: cube maps and texture arrays, textures in Unity games, files inside Red Dead
Redemption's `.rpf` archives, adding MSBT messages, and editing models or sounds directly.

---

## Problems?

- **"Use a picture of the same size"**: your editor changed the size. Put it back exactly.
- **The game still shows the old texture**: check the mod is turned on, restart the game,
  and check the **Mod** menu shows the right mod. Its title ID must end in `000`, even if
  you have an update.
- **"This file is from game … but the mod folder is for game …"**: choose
  **New mod for this game…**.
- **The game crashes**: delete the changed file from the mod folder (**Mod → Open mod folder**)
  and try a smaller change.

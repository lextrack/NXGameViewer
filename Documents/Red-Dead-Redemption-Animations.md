# Exporting Red Dead Redemption models with animations

This guide shows how to save a Red Dead Redemption character or object as a 3D model
(`.glb`) with its animations, ready to open in Blender or any other glTF viewer.

---

## What you need

- **A dump of the game's files** (its `romfs` folder), made from a game you own.
- **The game's ExeFS** (the `main` file). The app needs it once, to unlock the archives.

---

## 1. Unlock the archives (only the first time)

1. Open an archive of the game, for example `fragments.rpf`.
2. The app says the archive is encrypted. Click **Find the key in the game's executable…**
3. Pick the `main` file from the game's ExeFS.

The app remembers the key. Every archive of the game opens from now on.

---

## 2. Extract the animations

Animations live in `animationres.rpf`, inside `animationres/ast/clipres/`. Each `.was` file
holds a set of clips (for example `offer_illicit_services.was`).

1. Open `animationres.rpf` and go to `animationres/ast/clipres/`.
2. Select the `.was` files you want, right-click them and choose **Extract…**
3. Pick a folder to save them in and click **Extract**. Keep **Save files that cannot be
   converted as they are** ticked: animation sets are saved as `.was` files.

Tip: click a `.was` file to see the list of clips inside it.

---

## 3. Export the model with animations

1. Open `fragments.rpf` and click a character model (`.wfd`) or an object (`.wft`).
   The model appears in the preview.
2. Click **Extract → Model with animations (.glb)…**
3. In the window that opens, click **Add files…** and pick the `.was` files you extracted.
   Only the clips that fit the model's skeleton are listed.
4. Tick the clips you want and click **Export…**
5. Choose where to save the `.glb` file.

Open the `.glb` in Blender (**File → Import → glTF 2.0**) to see the animations.

---

## Good to know

- A model without animations can be saved with **Extract → Model as glTF (.glb)…**
- To save many models, textures and sounds at once, select them (or a whole `.rpf`) and choose
  **Extract…** from the right-click menu. Models become `.glb`, textures `.png` and sounds
  (`.awc`) `.wav`.
- Textures that are shared between models (for example in `fragmenttexturelist.wtd`) are looked
  up in the same archive, and in `fragments.rpf` for models from `fragments2.rpf`.
- Some textures are kept in shared files, not in the model, so those parts come out
  without a texture.
- Cutscene animations (`.wcdt` files) and facial animations are not supported yet.

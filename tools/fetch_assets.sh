#!/usr/bin/env bash
# Fetches a small CC0 starter pack into assets/ under the conventions
# EngineRuntime::instantiateModel, applyEnvironmentFile and
# asset::loadTextureSet expect. Idempotent (re-running skips anything already
# present); a single item failing (network hiccup, upstream layout change)
# logs and continues instead of aborting the whole run. Nothing this script
# downloads is committed to the repository (see .gitignore / assets/README.md).
#
# Sources:
#   - textures: ambientCG 1K-JPG packs (CC0), normalized to our
#     albedo/normal/roughness/metalness/ao naming (loadTextureSet's contract).
#   - models: Khronos glTF-Sample-Assets (CC0), same repo DamagedHelmet.glb
#     already comes from. Poly Haven's model downloads were the original plan
#     (matching the env/ source below), but Poly Haven does NOT publish a
#     single self-contained .glb per model -- its "gltf" file variant is a
#     multi-file bundle (.gltf + .bin + loose texture files), which does not
#     match the <name>.glb convention instantiateModel resolves. Rather than
#     ship a URL that only works after a manual glTF->GLB repack, this script
#     uses Khronos's own CC0 single-file .glb samples instead. If you want a
#     specific Poly Haven model, download it from https://polyhaven.com/models,
#     export/repack it as glTF-Binary (.glb) yourself, and drop it in
#     assets/models/<name>.glb -- instantiateModel needs nothing more.
#   - env: Poly Haven 2k HDRIs (CC0), direct single-file .hdr downloads.
#   - model packs (MA milestone, fetch_pack): categorized stylized props from
#     Kenney (CC0), zips of single-file .glb models, flattened into
#     assets/models/<category>/<name>.glb with a pack.json manifest per
#     category. URLs verified with curl before shipping (see the fetch_pack
#     calls below); Kenney's own zip URLs embed a content hash that changes
#     when a pack is updated, so a 404 here means the pack was re-uploaded --
#     re-verify at https://kenney.nl/assets/<slug> and update the URL.
#     Quaternius (also CC0, also stylized) was evaluated too but its packs are
#     distributed as unversioned Google Drive folder links (e.g.
#     https://drive.google.com/drive/folders/1-Kl0L_Jg8awbh0S5T-z3zxh4mVlnxTpa
#     for "Ultimate Nature"), which cannot be fetched with a stable curl
#     command (no direct file URL, and Drive's folder API requires
#     interactive auth) -- download manually from quaternius.com/packs and
#     place the .glb files under assets/models/<category>/ with a hand-written
#     pack.json ({"category":...,"style":"stylized","source":"Quaternius
#     (<pack name>)","license":"CC0"}) alongside them if you want them
#     indexed by `viewer --thumbnails`.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ASSET_DIR="$(cd "$SCRIPT_DIR/.." && pwd)/assets"
TEXTURES_DIR="$ASSET_DIR/textures"
MODELS_DIR="$ASSET_DIR/models"
ENV_DIR="$ASSET_DIR/env"

mkdir -p "$TEXTURES_DIR" "$MODELS_DIR" "$ENV_DIR"

FETCHED=0
SKIPPED=0
FAILED=0

log() { echo "[fetch_assets] $*"; }
fail() {
    echo "[fetch_assets] FAILED: $*" >&2
    FAILED=$((FAILED + 1))
}

# ambientCG ships each material as a zip of loosely-named per-map images
# (<Id>_1K-JPG_Color.jpg, _NormalGL.jpg, _Roughness.jpg, _Metalness.jpg,
# _AmbientOcclusion.jpg -- not every map exists for every material). This
# picks the ones present and renames them to loadTextureSet's convention.
fetch_texture_set() {
    local name="$1" acg_id="$2"
    local dest="$TEXTURES_DIR/$name"
    if [ -f "$dest/albedo.jpg" ] || [ -f "$dest/albedo.png" ]; then
        log "textures/$name already present, skipping"
        SKIPPED=$((SKIPPED + 1))
        return
    fi

    local tmp
    tmp="$(mktemp -d)"
    local url="https://ambientcg.com/get?file=${acg_id}_1K-JPG.zip"
    if ! curl -fsSL -A "Mozilla/5.0 (compatible; kumo-fetch-assets)" "$url" -o "$tmp/pack.zip"; then
        fail "textures/$name: download failed ($url)"
        rm -rf "$tmp"
        return
    fi
    if ! unzip -q -o "$tmp/pack.zip" -d "$tmp/extracted"; then
        fail "textures/$name: unzip failed"
        rm -rf "$tmp"
        return
    fi

    mkdir -p "$dest.tmp"
    copy_map() {
        local suffix="$1" out="$2"
        local found
        found="$(find "$tmp/extracted" -iname "${acg_id}_1K-JPG_${suffix}.jpg" -print -quit)"
        if [ -n "$found" ]; then
            cp "$found" "$dest.tmp/$out"
        fi
    }
    copy_map "Color" "albedo.jpg"
    copy_map "NormalGL" "normal.jpg"
    copy_map "Roughness" "roughness.jpg"
    copy_map "Metalness" "metalness.jpg"
    copy_map "AmbientOcclusion" "ao.jpg"

    if [ ! -f "$dest.tmp/albedo.jpg" ]; then
        fail "textures/$name: no Color (albedo) map found in the ambientCG archive"
        rm -rf "$tmp" "$dest.tmp"
        return
    fi
    rm -rf "$dest"
    mv "$dest.tmp" "$dest"
    rm -rf "$tmp"
    log "textures/$name fetched from ambientCG $acg_id"
    FETCHED=$((FETCHED + 1))
}

fetch_model() {
    local name="$1" url="$2"
    local dest="$MODELS_DIR/$name.glb"
    if [ -f "$dest" ]; then
        log "models/$name.glb already present, skipping"
        SKIPPED=$((SKIPPED + 1))
        return
    fi
    if ! curl -fsSL "$url" -o "$dest.tmp"; then
        fail "models/$name: download failed ($url)"
        rm -f "$dest.tmp"
        return
    fi
    mv "$dest.tmp" "$dest"
    log "models/$name.glb fetched"
    FETCHED=$((FETCHED + 1))
}

# A categorized pack of single-file .glb models (Kenney's own "GLTF/GLB
# format" export): downloads `url` and, for every directory anywhere in the
# zip that directly holds at least one *.glb (Kenney's own subfolder naming
# for this varies between packs -- "GLTF format" vs "GLB format" -- so this
# searches recursively instead of hardcoding one), copies that directory's
# WHOLE contents -- not just the *.glb files -- into assets/models/<category>/.
# This matters: some packs' glb export references a sibling file by relative
# uri that the exporter did not embed (Kenney survival-kit's glb files all
# point at "Textures/colormap.png" sitting next to them in the same "GLB
# format" folder); flattening just the *.glb files would silently drop that
# dependency and break image loading. Also writes a pack.json manifest
# asset_index/`viewer --thumbnails` read for category/style/source/license.
# Idempotent: a category directory that already has a pack.json is skipped.
# Shared tail of fetch_pack/unpack_local_pack: flatten every directory that
# holds .glb files (keeping siblings like a shared Textures/ folder) into the
# category directory and write its pack.json manifest.
flatten_pack_zip() {
    local category="$1" zip="$2" style="$3" source="$4" license="$5"
    local dest="$MODELS_DIR/$category"
    local tmp
    tmp="$(mktemp -d)"
    if ! unzip -q -o "$zip" -d "$tmp/extracted"; then
        fail "models/$category: unzip failed"
        rm -rf "$tmp"
        return 1
    fi
    mkdir -p "$dest"
    while IFS= read -r glbDir; do
        [ -n "$glbDir" ] || continue
        cp -R "$glbDir"/. "$dest"/
    done < <(find "$tmp/extracted" -iname "*.glb" -exec dirname {} \; | sort -u)
    rm -rf "$tmp"

    local count
    count="$(find "$dest" -iname "*.glb" | wc -l | tr -d ' ')"
    if [ "$count" -eq 0 ]; then
        fail "models/$category: no .glb files found in the pack"
        rm -rf "$dest"
        return 1
    fi
    printf '{"category":"%s","style":"%s","source":"%s","license":"%s"}\n' \
        "$category" "$style" "$source" "$license" > "$dest/pack.json"
    log "models/$category: $count models from $source"
    FETCHED=$((FETCHED + count))
}

fetch_pack() {
    local category="$1" url="$2" style="$3" source="$4" license="$5"
    local dest="$MODELS_DIR/$category"
    if [ -f "$dest/pack.json" ]; then
        log "models/$category already present, skipping"
        SKIPPED=$((SKIPPED + 1))
        return
    fi

    local tmp
    tmp="$(mktemp -d)"
    if ! curl -fsSL "$url" -o "$tmp/pack.zip"; then
        fail "models/$category: download failed ($url)"
        rm -rf "$tmp"
        return
    fi
    flatten_pack_zip "$category" "$tmp/pack.zip" "$style" "$source" "$license" || true
    rm -rf "$tmp"
}

unpack_local_pack() {
    local category="$1" zip="$2" style="$3" source="$4" license="$5"
    if [ -f "$MODELS_DIR/$category/pack.json" ]; then
        log "models/$category already present, skipping"
        SKIPPED=$((SKIPPED + 1))
        return
    fi
    flatten_pack_zip "$category" "$zip" "$style" "$source" "$license" || true
}

fetch_env() {
    local name="$1" polyhaven_slug="$2"
    local dest="$ENV_DIR/$name.hdr"
    if [ -f "$dest" ]; then
        log "env/$name.hdr already present, skipping"
        SKIPPED=$((SKIPPED + 1))
        return
    fi
    local url="https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/2k/${polyhaven_slug}_2k.hdr"
    if ! curl -fsSL "$url" -o "$dest.tmp"; then
        fail "env/$name: download failed ($url)"
        rm -f "$dest.tmp"
        return
    fi
    mv "$dest.tmp" "$dest"
    log "env/$name.hdr fetched"
    FETCHED=$((FETCHED + 1))
}

# Textures: ambientCG 1K-JPG packs. Ground037 stands in for "sand" -- the
# obvious GroundSand005 id does not exist on ambientCG (verified 404); Ground037
# is a real sandy/dirt ground material and the closest verified match.
fetch_texture_set sand   Ground037
fetch_texture_set rock   Rock035
fetch_texture_set bark   Bark012
fetch_texture_set planks Planks012
fetch_texture_set grass  Grass004

# Models: Khronos glTF-Sample-Assets CC0 single-file .glb (see header comment
# for why this replaces the originally planned Poly Haven source).
fetch_model Avocado     "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Avocado/glTF-Binary/Avocado.glb"
fetch_model BoomBox     "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/BoomBox/glTF-Binary/BoomBox.glb"
fetch_model WaterBottle "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/WaterBottle/glTF-Binary/WaterBottle.glb"

# Urban/cyberpunk expansion (all URLs verified like the sets above).
fetch_texture_set asphalt  Asphalt012
fetch_texture_set concrete Concrete034
fetch_texture_set metal    MetalPlates006

# Env: Poly Haven 2k HDRIs — day / sunset / night, plus two city nights for
# urban scenes.
fetch_env day    kloofendal_43d_clear_puresky
fetch_env sunset venice_sunset
fetch_env night  dikhololo_night
fetch_env city_night  potsdamer_platz
fetch_env city_night2 shanghai_bund

# Model packs (MA milestone; MS expands to 8 more kits): Kenney CC0 kits,
# single-file .glb per model (URLs verified with `curl -sI`; see the header
# comment above for why Quaternius is manual-only). Every pack below passed
# the MS intake gate: a full `viewer --index` run with zero load or render
# failures. Nature Kit was evaluated and REMOVED: its glbs come from a broken
# UniGLTF export (the real scene root is missing from scenes[0].nodes, cgltf
# rejects every file as invalid_gltf), so shipping it only floods asset_list
# with unusable entries. Revisit if Kenney re-exports.
fetch_pack survival "https://kenney.nl/media/pages/assets/survival-kit/4065a8185b-1712149243/kenney_survival-kit.zip" \
    stylized "Kenney (Survival Kit)" CC0
fetch_pack furniture "https://kenney.nl/media/pages/assets/furniture-kit/440e0608a4-1677580847/kenney_furniture-kit.zip" \
    stylized "Kenney (Furniture Kit)" CC0
fetch_pack food "https://kenney.nl/media/pages/assets/food-kit/83086fa91c-1719418518/kenney_food-kit.zip" \
    stylized "Kenney (Food Kit)" CC0
fetch_pack suburban "https://kenney.nl/media/pages/assets/city-kit-suburban/2c871b7af2-1745479373/kenney_city-kit-suburban_20.zip" \
    stylized "Kenney (City Kit Suburban)" CC0
fetch_pack commercial "https://kenney.nl/media/pages/assets/city-kit-commercial/a742d900eb-1753115042/kenney_city-kit-commercial_2.1.zip" \
    stylized "Kenney (City Kit Commercial)" CC0
fetch_pack castle "https://kenney.nl/media/pages/assets/castle-kit/a395102d20-1711543616/kenney_castle-kit.zip" \
    stylized "Kenney (Castle Kit)" CC0
fetch_pack graveyard "https://kenney.nl/media/pages/assets/graveyard-kit/ba8d4b4517-1760691807/kenney_graveyard-kit_5.0.zip" \
    stylized "Kenney (Graveyard Kit)" CC0
fetch_pack holiday "https://kenney.nl/media/pages/assets/holiday-kit/3976a6496a-1733923970/kenney_holiday-kit.zip" \
    stylized "Kenney (Holiday Kit)" CC0
fetch_pack pirate "https://kenney.nl/media/pages/assets/pirate-kit/e6d4bb1525-1771333093/kenney_pirate-kit.zip" \
    stylized "Kenney (Pirate Kit)" CC0

# Manual pack channel (MS): zips that cannot be fetched with a stable URL
# (Quaternius/KayKit distribute via Drive/itch). Drop them into assets/packs/
# as <category>__<Source Name>.zip (double underscore separates the category
# from the human-readable source) and they are unpacked through the same
# layout and manifest path as fetch_pack. Idempotent like fetch_pack: a
# category that already has a pack.json is skipped.
PACKS_DIR="$ASSET_DIR/packs"
if [ -d "$PACKS_DIR" ]; then
    for zip in "$PACKS_DIR"/*.zip; do
        [ -e "$zip" ] || continue
        base="$(basename "$zip" .zip)"
        category="${base%%__*}"
        source_name="${base#*__}"
        if [ "$category" = "$base" ]; then
            fail "packs/$base.zip: name must be <category>__<Source Name>.zip"
            continue
        fi
        unpack_local_pack "$category" "$zip" stylized "$source_name" CC0
    done
fi

log "done: $FETCHED fetched, $SKIPPED skipped, $FAILED failed"
if [ "$FAILED" -gt 0 ]; then
    exit 1
fi

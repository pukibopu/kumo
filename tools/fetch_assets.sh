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

log "done: $FETCHED fetched, $SKIPPED skipped, $FAILED failed"
if [ "$FAILED" -gt 0 ]; then
    exit 1
fi

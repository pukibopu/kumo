# Poly Haven fixtures

`assets_textures.json` / `assets_hdris.json` are trimmed real captures of
`GET https://api.polyhaven.com/assets?t=textures|hdris` (a handful of assets
kept, every field left as returned). `files_asphalt_02.json` /
`files_metal_plate.json` / `files_dikhololo_night.json` are trimmed real
captures of `GET https://api.polyhaven.com/files/<id>` (only the 1k/2k/4k
resolutions and jpg/png/hdr/exr formats kept). `files_asphalt_02_doctored_host.json`
is `files_asphalt_02.json` with one map's `url` hand-edited to a non-Poly-Haven
host, for the host-whitelist rejection test.

`assets_models.json` is a trimmed real capture of `GET
https://api.polyhaven.com/assets?t=models` (verified with `curl` during the MA
milestone: `t=models` returns the same id/name/tags/categories shape as
textures/hdris, so `pickAsset` needs no model-specific test coverage beyond
proving it reads this file). `files_barrel_01.json` is a trimmed real capture
of `GET https://api.polyhaven.com/files/Barrel_01` (only the `gltf` key's 1k/2k
resolutions kept; verified the nesting is one level deeper than a texture/hdri
map: `gltf.<resolution>.gltf.{url,include}`, and that the `.gltf` file's own
buffer/image `uri`s match the `include` map's keys exactly, so downloading
`url` plus every `include` entry at its relative path reproduces a working
local glTF). `files_barrel_01_doctored_host.json` is the same file with one
`include` entry's `url` hand-edited to a non-Poly-Haven host, for
`fetchModel`'s host-whitelist rejection test.

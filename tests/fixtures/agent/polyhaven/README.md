# Poly Haven fixtures

`assets_textures.json` / `assets_hdris.json` are trimmed real captures of
`GET https://api.polyhaven.com/assets?t=textures|hdris` (a handful of assets
kept, every field left as returned). `files_asphalt_02.json` /
`files_metal_plate.json` / `files_dikhololo_night.json` are trimmed real
captures of `GET https://api.polyhaven.com/files/<id>` (only the 1k/2k/4k
resolutions and jpg/png/hdr/exr formats kept). `files_asphalt_02_doctored_host.json`
is `files_asphalt_02.json` with one map's `url` hand-edited to a non-Poly-Haven
host, for the host-whitelist rejection test.

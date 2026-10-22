# EZ-Tree model assets

The seven upstream GLBs in `models/` were retrieved from
[`dgreenheck/ez-tree`](https://github.com/dgreenheck/ez-tree/tree/main/src/app/public/models)
on 2026-10-20. They are retained byte-for-byte as source assets. Their upstream
license is in [`LICENSE`](LICENSE); the upstream texture notice is in
[`models/TEXTURE_LICENSE_NOTICE.md`](models/TEXTURE_LICENSE_NOTICE.md).

Run `python tools/prepare_eztree_models.py` from the project root to create the
runtime copies under `ohos-project/entry/src/main/resources/rawfile/models/EZTree`.
That preparation step converts embedded WebP images to PNG because the current
OHOS image decoder supports PNG/JPEG, and leaves the original GLBs unchanged.
The runtime renderer currently treats glTF `MASK` and `BLEND` materials as
alpha-cutout materials in its opaque scene pass; full translucent blending is
not implemented in the deferred GLES path.

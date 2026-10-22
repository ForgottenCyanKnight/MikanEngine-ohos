#!/usr/bin/env python3
"""Prepare the EZ-Tree GLBs for the project's lightweight glTF runtime.

The upstream GLBs are preserved byte-for-byte under third_party/ez-tree/models.
This script repacks their GLB buffer views, converts embedded WebP images to
PNG, and decodes Draco geometry for runtime copies under the OHOS rawfile
directory.
"""

from __future__ import annotations

import argparse
import io
import json
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = ROOT / "third_party" / "ez-tree" / "models"
DEFAULT_OUTPUT = ROOT / "ohos-project" / "entry" / "src" / "main" / "resources" / "rawfile" / "models" / "EZTree"
GLB_MAGIC = 0x46546C67
JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942
GRASS_BASE_COLOR_FACTOR = [0.18, 0.65, 0.12, 1.0]
GRASS_ALPHA_CUTOFF = 0.5
DRACO_DECODER = ROOT / "tools" / "ez-tree-draco" / "decode.mjs"


def read_glb(path: Path) -> tuple[dict, bytes]:
    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError(f"{path}: truncated GLB")
    magic, version, total_length = struct.unpack_from("<III", data)
    if magic != GLB_MAGIC or version != 2 or total_length != len(data):
        raise ValueError(f"{path}: invalid GLB header")

    offset = 12
    document = None
    binary = None
    while offset < len(data):
        if offset + 8 > len(data):
            raise ValueError(f"{path}: truncated GLB chunk header")
        chunk_length, chunk_type = struct.unpack_from("<II", data, offset)
        offset += 8
        chunk = data[offset : offset + chunk_length]
        if len(chunk) != chunk_length:
            raise ValueError(f"{path}: truncated GLB chunk")
        offset += chunk_length
        if chunk_type == JSON_CHUNK:
            document = json.loads(chunk.decode("utf-8").rstrip("\x00 \t\r\n"))
        elif chunk_type == BIN_CHUNK:
            binary = chunk
        else:
            raise ValueError(f"{path}: unsupported GLB chunk 0x{chunk_type:08x}")
    if document is None or binary is None:
        raise ValueError(f"{path}: missing JSON or BIN chunk")
    return document, binary


def encode_webp_as_png(encoded: bytes, path: Path, image_name: str) -> bytes:
    try:
        with Image.open(io.BytesIO(encoded)) as image:
            rgba = image.convert("RGBA")
            output = io.BytesIO()
            rgba.save(output, format="PNG", optimize=True)
            return output.getvalue()
    except Exception as error:  # Pillow reports codec-specific exception types.
        raise ValueError(f"{path}: could not decode WebP image {image_name!r}: {error}") from error


def prepare(source: Path, output: Path) -> tuple[int, int, int]:
    document, binary = read_glb(source)
    buffer_views = document.get("bufferViews", [])
    replacements: dict[int, bytes] = {}
    document_modified = False
    draco_primitive_count = sum(
        1
        for mesh in document.get("meshes", [])
        for primitive in mesh.get("primitives", [])
        if "KHR_draco_mesh_compression" in primitive.get("extensions", {})
    )

    # EXT_texture_webp stores the image index under the texture extension,
    # whereas this runtime's glTF loader consumes the core `source` member.
    # The converted PNG must remain connected to its material after conversion.
    for texture in document.get("textures", []):
        extensions = texture.get("extensions", {})
        webp = extensions.pop("EXT_texture_webp", None)
        if webp is None:
            continue
        webp_source = webp.get("source")
        if not isinstance(webp_source, int):
            raise ValueError(f"{source}: EXT_texture_webp texture has no valid source")
        core_source = texture.get("source")
        if core_source is not None and core_source != webp_source:
            raise ValueError(f"{source}: core and WebP texture sources disagree")
        texture["source"] = webp_source
        if not extensions:
            texture.pop("extensions", None)
        document_modified = True

    if not any(
        "EXT_texture_webp" in texture.get("extensions", {})
        for texture in document.get("textures", [])
    ):
        for extension_list in ("extensionsUsed", "extensionsRequired"):
            if extension_list in document:
                document[extension_list] = [
                    extension
                    for extension in document[extension_list]
                    if extension != "EXT_texture_webp"
                ]
                if not document[extension_list]:
                    document.pop(extension_list)
                document_modified = True

    # EZ-Tree's grass texture is a grayscale coverage/albedo map and its GLB
    # leaves the material factor white. Tint only the generated runtime copy;
    # the original upstream GLB remains byte-for-byte intact.
    if source.stem.lower() == "grass":
        for material in document.get("materials", []):
            pbr = material.setdefault("pbrMetallicRoughness", {})
            pbr["baseColorFactor"] = GRASS_BASE_COLOR_FACTOR
            # EZ-Tree's web renderer uses alphaTest=0.5 for this foliage.
            # Convert BLEND to a cutout because the engine's G-buffer pass is
            # opaque/deferred and does not have a general translucent-model pass.
            material["alphaMode"] = "MASK"
            material["alphaCutoff"] = GRASS_ALPHA_CUTOFF
            document_modified = True

    for image in document.get("images", []):
        if image.get("mimeType", "").lower() != "image/webp":
            continue
        view_index = image.get("bufferView")
        if not isinstance(view_index, int) or not 0 <= view_index < len(buffer_views):
            raise ValueError(f"{source}: WebP image has no valid bufferView")
        view = buffer_views[view_index]
        if view.get("buffer", 0) != 0:
            raise ValueError(f"{source}: WebP image refers to a non-GLB buffer")
        start = int(view.get("byteOffset", 0))
        length = int(view["byteLength"])
        encoded = binary[start : start + length]
        if len(encoded) != length:
            raise ValueError(f"{source}: WebP bufferView is out of range")
        converted = encode_webp_as_png(encoded, source, image.get("name", ""))
        previous = replacements.get(view_index)
        if previous is not None and previous != converted:
            raise ValueError(f"{source}: one bufferView is shared by different WebP images")
        replacements[view_index] = converted
        image["mimeType"] = "image/png"

    if not replacements and not document_modified and draco_primitive_count == 0:
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, output)
        return 0, output.stat().st_size, 0

    packed = bytearray()
    for index, view in enumerate(buffer_views):
        if view.get("buffer", 0) != 0:
            raise ValueError(f"{source}: GLB bufferView refers to a non-GLB buffer")
        while len(packed) % 4:
            packed.append(0)
        old_start = int(view.get("byteOffset", 0))
        old_length = int(view["byteLength"])
        payload = replacements.get(index)
        if payload is None:
            payload = binary[old_start : old_start + old_length]
            if len(payload) != old_length:
                raise ValueError(f"{source}: bufferView {index} is out of range")
        view["byteOffset"] = len(packed)
        view["byteLength"] = len(payload)
        packed.extend(payload)

    buffers = document.get("buffers", [])
    if len(buffers) != 1:
        raise ValueError(f"{source}: expected exactly one GLB buffer")
    buffers[0]["byteLength"] = len(packed)
    json_bytes = json.dumps(document, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    json_bytes += b" " * (-len(json_bytes) % 4)
    packed.extend(b"\x00" * (-len(packed) % 4))
    glb_length = 12 + 8 + len(json_bytes) + 8 + len(packed)
    result = bytearray(struct.pack("<III", GLB_MAGIC, 2, glb_length))
    result.extend(struct.pack("<II", len(json_bytes), JSON_CHUNK))
    result.extend(json_bytes)
    result.extend(struct.pack("<II", len(packed), BIN_CHUNK))
    result.extend(packed)

    output.parent.mkdir(parents=True, exist_ok=True)
    if draco_primitive_count:
        # Keep the intermediate outside the runtime asset name. If decoding
        # fails, the prior output remains untouched rather than leaving an
        # unusable GLB with required Draco extensions in the HAP.
        with tempfile.TemporaryDirectory(
            prefix=f".{output.stem}-draco-", dir=output.parent
        ) as temporary_directory:
            prepared = Path(temporary_directory) / "prepared.glb"
            decoded = Path(temporary_directory) / "decoded.glb"
            prepared.write_bytes(result)
            try:
                subprocess.run(
                    ["node", str(DRACO_DECODER), str(prepared), str(decoded)],
                    check=True,
                )
            except FileNotFoundError as error:
                raise RuntimeError(
                    "Node.js is required to decode Draco assets"
                ) from error
            if not decoded.is_file() or decoded.stat().st_size == 0:
                raise RuntimeError(f"{source}: Draco decoder produced no output")
            decoded_document, _ = read_glb(decoded)
            remaining_draco = any(
                "KHR_draco_mesh_compression" in primitive.get("extensions", {})
                for mesh in decoded_document.get("meshes", [])
                for primitive in mesh.get("primitives", [])
            )
            remaining_webp = any(
                "EXT_texture_webp" in texture.get("extensions", {})
                or "source" not in texture
                for texture in decoded_document.get("textures", [])
            ) or any(
                image.get("mimeType", "").lower() == "image/webp"
                for image in decoded_document.get("images", [])
            )
            if remaining_draco or remaining_webp:
                raise RuntimeError(
                    f"{source}: decoded GLB still contains unsupported geometry or texture extensions"
                )
            shutil.copyfile(decoded, output)
        output_size = output.stat().st_size
    else:
        output.write_bytes(result)
        output_size = len(result)
    return len(replacements), output_size, draco_primitive_count


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    if not args.source_dir.is_dir():
        parser.error(f"source directory does not exist: {args.source_dir}")

    sources = sorted(args.source_dir.glob("*.glb"))
    if not sources:
        parser.error(f"no GLB files found in {args.source_dir}")
    for source in sources:
        converted_count, output_size, draco_count = prepare(
            source, args.output_dir / source.name
        )
        print(
            f"{source.name}: webp_images={converted_count} "
            f"draco_primitives_decoded={draco_count} output_bytes={output_size}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

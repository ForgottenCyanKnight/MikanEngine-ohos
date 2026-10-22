import { NodeIO } from '@gltf-transform/core';
import { KHRDracoMeshCompression } from '@gltf-transform/extensions';
import draco3d from 'draco3dgltf';

const [inputPath, outputPath] = process.argv.slice(2);
if (!inputPath || !outputPath) {
  throw new Error('usage: node decode.mjs input.glb output.glb');
}

const decoder = await draco3d.createDecoderModule();
const io = new NodeIO()
  .registerExtensions([KHRDracoMeshCompression])
  .registerDependencies({ 'draco3d.decoder': decoder });

const document = await io.read(inputPath);
const dracoExtension = document
  .getRoot()
  .listExtensionsUsed()
  .find((extension) => extension.extensionName === 'KHR_draco_mesh_compression');
if (!dracoExtension) {
  throw new Error(`${inputPath}: no KHR_draco_mesh_compression extension found`);
}

// Reading decodes the compressed streams into ordinary glTF accessors. Drop
// the extension before writing so the OHOS runtime needs no Draco decoder.
dracoExtension.dispose();
await io.write(outputPath, document);

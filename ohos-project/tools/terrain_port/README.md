# OHOS 高度图地形移植

## 实现边界

主引擎参考入口：`TerrainComponent`、`TerrainRenderer`、`HeightmapLoader`、
`PhysicsSystem`，以及 `terrain.vert` / `terrain.frag`。OHOS 实现位于
`entry/src/main/cpp/application/terrain/heightmap_terrain.*`。

| 层 | 责任 |
|---|---|
| scene_definition | 读取 SceneSerializer v1 的 terrain 组件，保留高度图、图层、控制图及网格参数，并检查移动端预算 |
| heightmap_terrain | 从 rawfile 解码高度和材质；生成连续区块、世界空间法线与移动端材质贴图 |
| Vulkan | 上传共享网格与材质，使用现有 PBR 模型管线、阴影管线和独立的恒等实例绘制 |
| GLES | 上传相同网格与材质，进入现有 G-buffer、阴影与光照路径 |
| Jolt | 使用相同高度和变换规则生成独立静态碰撞网格；替换时先完整创建，再释放旧碰撞体 |

保留的主引擎约定：

- 高度图为非交错 **16-bit 灰度或灰度+Alpha PNG**，不经过 RGBA8 中转。
- 样本值除以 65535，双线性采样；局部 X/Z 范围是 `[-worldSize/2, +worldSize/2]`。
- PNG 顶行对应局部 +Z，底行对应 -Z。高度为 `sample * heightScale + heightOffset`。
- 法线从高度采样域计算，再按实体及父节点变换的逆转置矩阵变换。
- 区块边界使用整数全局格点计算相同的位置、法线和 UV，固定分辨率下不产生 LOD 接缝。
- `sculptedHeightmapPath` 优先于 `heightmapPath`；`paintedControlMapPath` 优先于 `controlMapPath`。
- RGBA 控制图对应 layer0..3；没有控制图时使用主引擎的草/泥/岩坡度阈值及 blendSharpness。
- 空高度图生成平坦地形，空图层使用中性白色；显式指定但缺失的资产会报告失败。
- 渲染可见性和 castShadow 分别控制绘制与投影，collisionEnabled 独立控制碰撞。

OHOS 使用加载阶段生成的固定区块网格，而非桌面端的顶点高度纹理采样、GPU 剔除和动态 LOD。
四层 albedo 在加载阶段按权重混合为每个地形一张 **512×512** 贴图，在线性空间混合后编码回 sRGB，
由现有模型 PBR 管线解码。保留 authored texture/material 颜色，不添加世界噪声亮度或草地重染色。
纹理使用世界 X/Z 平面采样；桌面端的反重复采样、岩石三平面和编辑器笔刷没有移植。
因此超大地形或高频细节应调整材质方案后再投入使用，不应按桌面效果等价验收。

当前预算：最多 4 个地形；每轴 chunkCount 1..16，patchResolution 2..129，
总格点间隔不超过 512；全场景不超过 110 万地形顶点；输入图像每轴 2..4096（材质允许 1），
单个编码文件最多 64 MiB。超预算明确报错，不自动截断地形。
碰撞每轴最多 collisionResolution 个采样点，并限制为不超过渲染网格分辨率；碰撞不随相机变化。
现有示例地板仍保留，地形低于地板的区域可能被地板遮挡或支撑。

## 试用示例

已打包资源：

- `entry/src/main/resources/rawfile/terrain/demo_height16.png`：65×65 的 16 位平滑山丘。
- `terrain/demo_layer0.png`、`demo_layer1.png`、`demo_layer2.png`：纯色图层示例。
- `entry/src/main/resources/rawfile/scenes/terrain_demo.json`：独立示例场景。

默认场景保留原有实体，将平台扩大到 40×40，海平面扩大到 384×384，地形扩大到 192×192。
地形使用 `terrain/archipelago_height16.png`（257×257、16 位灰度），海床 Y=-6.5，周围有 5 座独立岛屿。
中央 X/Z 各 ±22 内保持 Y≈-1.34 的平坦承托面，紧贴平台底部；22..36 米范围平滑过渡到海床。
岛屿中心位于 (-54,-43)、(54,-46)、(57,43)、(-51,48)、(0,69)，最高岛峰约 Y=12。
平地为草地，缓坡连续混合土色，陡坡混合灰色岩石；颜色来自独立材质，不叠加明暗噪声。
渲染使用 8×8 区块、每块 17×17 顶点（18496 顶点、32768 三角形），物理网格为 129×129，
共享相同高度采样。平台渲染、Jolt 碰撞和无物理回退边界同步扩大。相机远裁剪为 600。
构建默认使用该 OHOS 场景；若缓存了 MIKAN_SCENE_FILE，请清空该参数。

要切换到独立山丘示例，请在 `entry/build-profile.json5` 的
`externalNativeOptions.arguments` 末尾加入下面参数，然后通过 DevEco/Hvigor 构建：

```text
-DMIKAN_SCENE_FILE=<仓库绝对路径>/ohos-project/entry/src/main/resources/rawfile/scenes/terrain_demo.json
```

也可以在自己的导出场景实体中加入：

```json
"terrain": {
  "enabled": true,
  "heightmapPath": "terrain/demo_height16.png",
  "worldSize": [20, 20],
  "heightScale": 4,
  "heightOffset": 0,
  "chunkCount": 4,
  "patchResolution": 17,
  "collisionEnabled": true,
  "collisionResolution": 65,
  "materialTiling": 8,
  "blendSharpness": 1,
  "layer0Path": "terrain/demo_layer0.png",
  "layer1Path": "terrain/demo_layer1.png",
  "layer2Path": "terrain/demo_layer2.png"
}
```

路径相对于 HAP 的 rawfile 根，大小写保留。主引擎导出的项目资源路径需要同步到该目录，
不能直接引用 Windows 绝对路径。模型归一化不应用于地形，地形使用场景的完整尺寸与变换。

## 自动验证

在仓库根目录运行（需要 Windows Visual Studio C++ 工具和 Python）：

```powershell
.\ohos-project\tools\terrain_port\run_tests.ps1 -Python 'python.exe'
```

测试使用真实 PNG 解码器与运行时地形代码，仅替换 OHOS rawfile IO 和 SDL 错误报告。
涵盖 16 位高度、纵向方向、区块接缝、三角形朝向、控制图、平坦回退、父节点、
镜像/非均匀变换、碰撞开关和无效输入。生成的文件均位于 `.codex-runtime/`，不会进入 Git。

原生两个 ABI 的独立构建日志在本地 `.codex-runtime/heightmap-validation/`。
这些检查不等于 HAP 打包、设备运行或画面验收；设备上还应检查地形加载日志、坡地行走、
阴影、区块边界及前后台恢复。日志标签是 `SDL3_TERRAIN`。


## 固定海平面

默认场景根节点的 `water` 配置为 OHOS 扩展，使用运行时世界坐标，不再叠加地面校准偏移：

```json
"water": {
  "enabled": true,
  "height": -2.6,
  "size": 384,
  "roughness": 0.08,
  "color": [0.035, 0.18, 0.22]
}
```

`height` 是海平面 Y；`size` 是以世界原点为中心的方形边长；`color` 是 sRGB 水体颜色。
默认海平面低于平台和地形支撑台，高于外围低洼地形。普通深度测试形成岸线。
关闭 `enabled` 或省略 `water` 即不生成水面。有效范围：高度 ±10000、边长 (0,4096]、
粗糙度 [0.045,1]、颜色各通道 [0,1]。

Vulkan 和 GLES 都在不透明场景之后、Bloom/色调映射之前执行独立水面合成。
原始场景颜色和深度完整保留；射线与固定高度平面求交，并与不透明深度比较形成岸线与遮挡。
四层动态噪声法线控制天空反射，F0=0.02 的菲涅耳将反射与水底透射按视角混合。
透射使用水下光程的 Beer-Lambert 吸收（RGB 系数 0.45/0.12/0.06 每米），
加上 `color` 对应的水体散射近似。浅水保留水底颜色，深水逐渐呈现水色；水下视角也衰减透射。
细波纹随像素覆盖范围淡出，以减少远处闪烁。

Vulkan 将场景深度保留为可采样只读布局，创建独立的深度视图、水面颜色目标和描述符；
GLES 读取现有 G-buffer 深度，在独立目标合成后交换后处理输入。两者均不采样当前正在写入的附件。
关闭水面时保留原场景颜色。资源随分辨率/交换链重建释放。

当前透射使用同一屏幕射线的底色，未偏移采样或追踪折射射线；没有 SSR、岸边泡沫、几何波浪、
固体碰撞或浮力。水面不投射阴影，物理地形导入不包含水面。
编译与离线检查不能代替设备上的水底可见性、岸线、遮挡、前后台恢复及画面验收。

水面法线流速为最初多层噪声版本的 3 倍。玩家在水面范围内、脚底浸入 ≥0.85 米时，
播放 `Swim_Fwd_Loop`（移动）或 `Swim_Idle_Loop`（静止）；浸入深度低于 0.65 米时退出。
检测使用 Jolt 胶囊脚底高度，避免模型中心偏移导致误判。受击动画仍优先，出水恢复陆地状态。
这里只切换动画，保留现有重力与地形碰撞，没有水面浮力或游泳运动控制。

默认群岛资产可通过 `generate_archipelago.py --output <rawfile根目录>` 确定性重新生成。
该脚本只生成数值高度图和三张材质颜色纹理，地形、坡度混合与碰撞仍使用实际运行时代码。

#version 450
precision highp float;
precision highp int;

layout(location = 0) in vec3 vShadowWorldPos;
layout(location = 1) in flat float vDissolve;

// Same noise threshold as the forward model dissolve so the shadow of the
// dying enemy crumbles away with the body instead of lingering as a full
// silhouette.  The depth attachment is populated by fixed-function testing.
float DissolveHash(vec3 p)
{
    p = fract(p * 0.3183099 + vec3(0.1, 0.17, 0.13));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float DissolveNoise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(DissolveHash(i),
                DissolveHash(i + vec3(1.0, 0.0, 0.0)), f.x),
            mix(DissolveHash(i + vec3(0.0, 1.0, 0.0)),
                DissolveHash(i + vec3(1.0, 1.0, 0.0)), f.x), f.y),
        mix(mix(DissolveHash(i + vec3(0.0, 0.0, 1.0)),
                DissolveHash(i + vec3(1.0, 0.0, 1.0)), f.x),
            mix(DissolveHash(i + vec3(0.0, 1.0, 1.0)),
                DissolveHash(i + vec3(1.0, 1.0, 1.0)), f.x), f.y), f.z);
}

void main()
{
    if (vDissolve > 0.0) {
        if (DissolveNoise(vShadowWorldPos * 5.0) < vDissolve * 1.18 - 0.09) {
            discard;
        }
    }
}

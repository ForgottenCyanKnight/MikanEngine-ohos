#version 450
precision highp float;
precision highp int;

layout(set = 0, binding = 0) uniform samplerCube skyboxTexture;
layout(location = 0) in vec3 vDirection;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

void main()
{
    vec3 N = normalize(vDirection);
    vec3 irradiance = vec3(0.0);
    vec3 up = (abs(N.y) < 0.999) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, N));
    up = cross(N, right);
    float sampleDelta = 0.05;
    float sampleCount = 0.0;
    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta) {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta) {
            vec3 tangentSample = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;
            irradiance += pow(texture(skyboxTexture, sampleVec).rgb, vec3(2.2)) *
                cos(theta) * sin(theta);
            sampleCount += 1.0;
        }
    }
    irradiance = PI * irradiance * (1.0 / sampleCount);
    outColor = vec4(irradiance, 1.0);
}

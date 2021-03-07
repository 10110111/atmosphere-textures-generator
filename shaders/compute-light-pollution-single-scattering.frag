#version 330
#extension GL_ARB_shading_language_420pack : require

#include "const.h.glsl"
#include "single-scattering-light-pollution.h.glsl"
#include "texture-coordinates.h.glsl"

out vec4 scatteringTextureOutput;

void main()
{
    const LightPollutionTexVars vars=scatteringTexIndicesToLightPollutionTexVars(gl_FragCoord.xy-vec2(0.5));
    scatteringTextureOutput=computeSingleScatteringForLightPollution(vars.cosViewZenithAngle, vars.altitude, vars.viewRayIntersectsGround);
}

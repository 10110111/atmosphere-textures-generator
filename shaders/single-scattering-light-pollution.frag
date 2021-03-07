#version 330
#extension GL_ARB_shading_language_420pack : require

#include "const.h.glsl"
#include "common-functions.h.glsl"
#include "texture-sampling-functions.h.glsl"
#include "total-scattering-coefficient.h.glsl"

// This function omits ground luminance: it is to be applied somewhere in the calling code.
vec4 computeSingleScatteringForLightPollutionIntegrand(const float cosViewZenithAngle, const float altitude,
                                                       const float dist, const bool viewRayIntersectsGround)
{
    const float r=earthRadius+altitude;
    // Clamping only guards against rounding errors here, we don't try to handle here the case when the
    // endpoint of the view ray intentionally appears in outer space.
    const float altAtDist=clampAltitude(sqrt(sqr(dist)+sqr(r)+2*r*dist*cosViewZenithAngle)-earthRadius);

    const float horizonZenithAngle=acos(clamp(cosZenithAngleOfHorizon(altAtDist),-1.,1.));

    vec4 weightedIncomingRadiance=vec4(0);
    // We want to integrate over all the 4PI solid angle. But we know a priori that
    //  1. from true horizon to zenith there are no sources of light, so nothing contributes to single scattering there,
    //  2. incoming radiance doesn't depend on azimuth.
    // Thus we've integrated out the azimuth, which gives us the 2PI multiplier before the integral, and we only integrate
    // over zenith angles from nadir to true horizon.
    // We are using midpoint rule for quadrature.
    const float kMax=ceil(sqrt(angularIntegrationPoints)/2);
    const float incZenithAngleStep=(horizonZenithAngle-PI/2)/kMax;
    const float sinViewZenithAngle=safeSqrt(1-sqr(cosViewZenithAngle));
    for(float k=0; k<kMax; ++k)
    {
        const float incZenithAngle=PI/2+(k+0.5)*incZenithAngleStep;
        const float distToGround=distanceToGround(cos(incZenithAngle), altAtDist);
        const float dotViewInc = sin(incZenithAngle)*sinViewZenithAngle+cos(incZenithAngle)*cosViewZenithAngle;
        weightedIncomingRadiance += transmittance(cos(incZenithAngle), altAtDist, distToGround, true)
                                                                *
                                         totalScatteringCoefficient(altAtDist, dotViewInc)
                                                                *
                                                        sin(incZenithAngle);
    }

    const vec4 xmittanceToScatterer=transmittance(cosViewZenithAngle, altitude, dist, viewRayIntersectsGround);
    return 2*PI*xmittanceToScatterer*weightedIncomingRadiance;
}

// This function is basically the same as computeSingleScattering(), just calls a different implementation of integrand
vec4 computeSingleScatteringForLightPollution(const float cosViewZenithAngle, const float altitude, const bool viewRayIntersectsGround)
{
    const float integrInterval=distanceToNearestAtmosphereBoundary(cosViewZenithAngle, altitude, viewRayIntersectsGround);

    // Using trapezoid rule on a uniform grid: f0/2+f1+f2+...+f(N-2)+f(N-1)/2.
    // Initializing with sum of values at endpoints, with weight of 0.5
    const vec4 end1=computeSingleScatteringForLightPollutionIntegrand(cosViewZenithAngle, altitude, 0, viewRayIntersectsGround);
    const vec4 end2=computeSingleScatteringForLightPollutionIntegrand(cosViewZenithAngle, altitude, integrInterval, viewRayIntersectsGround);
    vec4 spectrum=(end1+end2)*0.5;

    const float dl=integrInterval/(radialIntegrationPoints-1);
    for(int n=1; n<radialIntegrationPoints-1; ++n)
    {
        const float dist=n*dl;
        spectrum += computeSingleScatteringForLightPollutionIntegrand(cosViewZenithAngle, altitude, dist, viewRayIntersectsGround);
    }
    spectrum *= dl*lightPollutionRadiance;
    return spectrum;
}

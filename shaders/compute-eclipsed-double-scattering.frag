#version 330
#extension GL_ARB_shading_language_420pack : require

#include "const.h.glsl"
#include "phase-functions.h.glsl"
#include "common-functions.h.glsl"
#include "texture-coordinates.h.glsl"
#include "radiance-to-luminance.h.glsl"
#include "eclipsed-direct-irradiance.h.glsl"
#include "texture-sampling-functions.h.glsl"
#include "single-scattering-eclipsed.h.glsl"
#include "total-scattering-coefficient.h.glsl"

in vec3 position;
out vec4 partialRadiance;

vec4 computeDoubleScatteringEclipsedDensitySample(const int directionIndex, const vec3 cameraViewDir, const vec3 scatterer,
                                                  const vec3 sunDir, const vec3 moonPos)
{
    const vec3 zenith=vec3(0,0,1);
    const float altitude=pointAltitude(scatterer);
    // XXX: Might be a good idea to increase sampling density near horizon and decrease near zenith&nadir.
    // XXX: Also sampling should be more dense near the light source, since there often is a strong forward
    //       scattering peak like that of Mie phase functions.
    // TODO:At the very least, the phase functions should be lowpass-filtered to avoid aliasing, before
    //       sampling them here.

    // Instead of iterating over all directions, we compute only one sample, for only one direction, to
    // facilitate parallelization. The summation will be done after this parallel computation of the samples.

    const float dSolidAngle = sphereIntegrationSolidAngleDifferential(eclipseAngularIntegrationPoints);
    // Direction to the source of incident ray
    const vec3 incDir = sphereIntegrationSampleDir(directionIndex, eclipseAngularIntegrationPoints);

    // NOTE: we don't recalculate sunDir as we do in computeScatteringDensity(), because it would also require
    // at least recalculating the position of the Moon. Instead we take into account scatterer's position to
    // calculate zenith direction and the direction to the incident ray.
    const vec3 zenithAtScattererPos=normalize(scatterer-earthCenter);
    const float cosIncZenithAngle=dot(incDir, zenithAtScattererPos);
    const bool incRayIntersectsGround=rayIntersectsGround(cosIncZenithAngle, altitude);

    float distToGround=0;
    vec4 transmittanceToGround=vec4(0);
    if(incRayIntersectsGround)
    {
        distToGround = distanceToGround(cosIncZenithAngle, altitude);
        transmittanceToGround = transmittance(cosIncZenithAngle, altitude, distToGround, incRayIntersectsGround);
    }

    vec4 incidentRadiance = vec4(0);
    // XXX: keep this ground-scattered radiation logic in sync with that in computeScatteringDensity().
    {
        // The point where incident light originates on the ground, with current incDir
        const vec3 pointOnGround = scatterer+incDir*distToGround;
        const vec4 groundIrradiance = calcEclipsedDirectGroundIrradiance(pointOnGround, sunDir, moonPos);
        // Radiation scattered by the ground
        const float groundBRDF = 1/PI; // Assuming Lambertian BRDF, which is constant
        incidentRadiance += transmittanceToGround*groundAlbedo*groundIrradiance*groundBRDF;
    }
    // Radiation scattered by the atmosphere
    incidentRadiance+=computeSingleScatteringEclipsed(scatterer,incDir,sunDir,moonPos,incRayIntersectsGround);

    const float dotViewInc = dot(cameraViewDir, incDir);
    return dSolidAngle * incidentRadiance * totalScatteringCoefficient(altitude, dotViewInc);
}

uniform float cameraAltitude;
uniform vec3 cameraViewDir;
uniform float sunZenithAngle;
uniform vec3 moonPositionRelativeToSunAzimuth;

void main()
{
    const vec3 sunDir=vec3(sin(sunZenithAngle), 0, cos(sunZenithAngle));
    const vec3 cameraPos=vec3(0,0,cameraAltitude);
    const bool viewRayIntersectsGround=rayIntersectsGround(cameraViewDir.z, cameraAltitude);

    const float radialIntegrInterval=distanceToNearestAtmosphereBoundary(cameraViewDir.z, cameraAltitude,
                                                                         viewRayIntersectsGround);

    const int directionIndex=int(gl_FragCoord.x);
    const float radialDistIndex=gl_FragCoord.y;

    const float dl=radialIntegrInterval/(radialIntegrationPoints-1);
    const float dist=radialDistIndex*dl;
    const vec4 scDensity=computeDoubleScatteringEclipsedDensitySample(directionIndex, cameraViewDir, cameraPos+cameraViewDir*dist,
                                                                      sunDir, moonPositionRelativeToSunAzimuth);
    const vec4 xmittance=transmittance(cameraViewDir.z, cameraAltitude, dist, viewRayIntersectsGround);
    // TODO: switch to midpoint rule here and everywhere else for radial integration: it's simpler and has two times lower error bound
    const float weight = radialDistIndex==0||radialDistIndex==radialIntegrationPoints ? 0.5 : 1; // weight by trapezoidal rule
    partialRadiance = scDensity*xmittance*weight*dl;

}

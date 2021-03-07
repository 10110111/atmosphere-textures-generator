#ifndef INCLUDE_ONCE_60F008D3_578F_4231_998C_EBB05192B4B7
#define INCLUDE_ONCE_60F008D3_578F_4231_998C_EBB05192B4B7

#include <set>
#include <map>
#include <cmath>
#include <array>
#include <vector>
#include <memory>
#include <QOpenGLShader>
#include <glm/glm.hpp>
#include "const.hpp"
#include "../common/AtmosphereParameters.hpp"

inline std::map<QString, QString> virtualSourceFiles;
inline std::map<QString, QString> virtualHeaderFiles;

inline GLuint vao, vbo;
enum FBOId
{
    FBO_FOR_TEXTURE_SAVING,
    FBO_TRANSMITTANCE,
    FBO_IRRADIANCE,
    FBO_DELTA_SCATTERING,
    FBO_SINGLE_SCATTERING,
    FBO_MULTIPLE_SCATTERING,
    FBO_ECLIPSED_DOUBLE_SCATTERING,
    FBO_LIGHT_POLLUTION,

    FBO_COUNT
};
inline GLuint fbos[FBO_COUNT];
enum TextureId
{
    TEX_TRANSMITTANCE,
    TEX_IRRADIANCE,
    TEX_DELTA_IRRADIANCE,
    TEX_DELTA_SCATTERING,
    TEX_MULTIPLE_SCATTERING,
    TEX_DELTA_SCATTERING_DENSITY,
    TEX_ECLIPSED_DOUBLE_SCATTERING,
    TEX_LIGHT_POLLUTION,

    TEX_COUNT
};
inline GLuint textures[TEX_COUNT];
// Accumulation of radiance to yield luminance
inline std::map<QString/*scatterer name*/, GLuint> accumulatedSingleScatteringTextures;

struct Options
{
    bool saveResultAsRadiance=false;
    bool dbgNoSaveTextures=false;
    bool dbgNoEDSTextures=false;
    bool dbgSaveGroundIrradiance=false;
    bool dbgSaveScatDensityOrder2FromGround=false;
    bool dbgSaveScatDensity=false;
    bool dbgSaveDeltaScattering=false;
    bool dbgSaveAccumScattering=false;
};
inline Options opts;
inline AtmosphereParameters atmo;

#endif

#ifndef INCLUDE_ONCE_D1C5F1EB_B6E7_4260_8AC3_57C35BB9B0DF
#define INCLUDE_ONCE_D1C5F1EB_B6E7_4260_8AC3_57C35BB9B0DF

#include <cmath>
#include <glm/glm.hpp>
#include <QtGui>
#include "types.hpp"
#include "util.hpp"

struct AtmosphereParameters
{
    class ParsingError : public Error
    {
        QString message;
    public:
        ParsingError(QString const& filename, int lineNum, QString const& message)
            : message(QString("%1:%2: %3").arg(filename).arg(lineNum).arg(message))
        {}
        QString errorType() const override { return QObject::tr("Parsing error"); }
        QString what() const override { return message; }
    };
    struct Scatterer
    {
        GLfloat crossSectionAt1um = NAN;
        GLfloat angstromExponent = NAN;
        QString numberDensity;
        QString phaseFunction;
        PhaseFunctionType phaseFunctionType=PhaseFunctionType::General;
        QString name;

        explicit Scatterer(QString const& name) : name(name) {}
        bool valid() const
        {
            return std::isfinite(crossSectionAt1um) &&
                   std::isfinite(angstromExponent) &&
                   !numberDensity.isEmpty() &&
                   !phaseFunction.isEmpty() &&
                   !name.isEmpty();
        }
        glm::vec4 crossSection(glm::vec4 const wavelengths) const
        {
            constexpr float refWL=1000; // nm
            return crossSectionAt1um*pow(wavelengths/refWL, glm::vec4(-angstromExponent));
        }
    };
    struct Vec4Spectrum
    {
        std::vector<glm::vec4> values;
        QString filename; // non-empty if the values were loaded from file

        bool empty() const { return values.empty(); }
        auto size() const { return values.size(); }
        decltype(auto) operator[](const std::size_t n)       { return values[n]; }
        decltype(auto) operator[](const std::size_t n) const { return values[n]; }
    };
    struct Absorber
    {
        QString numberDensity;
        QString name;
        Vec4Spectrum absorptionCrossSection;

        AtmosphereParameters const& atmo;

        Absorber(QString const& name, AtmosphereParameters const& atmo)
            : name(name)
            , atmo(atmo)
        {}
        bool valid() const
        {
            return !numberDensity.isEmpty() &&
                   absorptionCrossSection.size()==atmo.allWavelengths.size() &&
                   !name.isEmpty();
        }
        glm::vec4 crossSection(glm::vec4 const wavelengths) const
        {
            const auto i=atmo.wavelengthsIndex(wavelengths);
            return absorptionCrossSection[i];
        }
    };

    QString descriptionFileText;
    QString descriptionFileDir;
    std::vector<glm::vec4> allWavelengths;
    Vec4Spectrum solarIrradianceAtTOA;
    std::string textureOutputDir=".";
    GLint transmittanceTexW, transmittanceTexH;
    GLint irradianceTexW, irradianceTexH;
    glm::ivec4 scatteringTextureSize;
    glm::ivec2 eclipsedSingleScatteringTextureSize;
    glm::ivec4 eclipsedDoubleScatteringTextureSize;
    unsigned eclipsedDoubleScatteringNumberOfAzimuthPairsToSample;
    unsigned eclipsedDoubleScatteringNumberOfElevationPairsToSample;
    unsigned scatteringOrdersToCompute;
    GLint numTransmittanceIntegrationPoints;
    GLint radialIntegrationPoints;
    GLint angularIntegrationPoints;
    GLint eclipseAngularIntegrationPoints;
    GLfloat earthRadius;
    GLfloat atmosphereHeight;
    double earthSunDistance;
    double earthMoonDistance;
    GLfloat sunAngularRadius; // calculated from earthSunDistance
    float lengthOfHorizRayFromGroundToBorderOfAtmo; // calculated from atmosphereHeight and earthRadius
    // moonAngularRadius is calculated from earthMoonDistance and other parameters on the fly, so isn't kept here
    Vec4Spectrum groundAlbedo;
    std::vector<Scatterer> scatterers;
    std::vector<Absorber> absorbers;
    bool allTexturesAreRadiance=false;
    bool noEclipsedDoubleScatteringTextures=false;
    static constexpr unsigned pointsPerWavelengthItem=4;
    static constexpr char ALL_TEXTURES_ARE_RADIANCES_DIRECTIVE[]="all textures are radiances";
    static constexpr char NO_ECLIPSED_DOUBLE_SCATTERING_TEXTURES_DIRECTIVE[]="no eclipsed double scattering textures";


    void parse(QString const& atmoDescrFileName);
    // XXX: keep in sync with those in previewer and renderer
    auto scatTexWidth()  const { return GLsizei(scatteringTextureSize[0]); }
    auto scatTexHeight() const { return GLsizei(scatteringTextureSize[1]*scatteringTextureSize[2]); }
    auto scatTexDepth()  const { return GLsizei(scatteringTextureSize[3]); }
    unsigned wavelengthsIndex(glm::vec4 const& wavelengths) const
    {
        const auto it=std::find(allWavelengths.begin(), allWavelengths.end(), wavelengths);
        assert(it!=allWavelengths.end());
        return it-allWavelengths.begin();
    }
};

#endif

#include "shaders.hpp"

#include <set>
#include <iomanip>
#include <iostream>
#include <QApplication>
#include <QFile>
#include <QDir>

#include "data.hpp"
#include "util.hpp"

#include "config.h"

QString withHeadersIncluded(QString src, QString const& filename);

void initConstHeader(glm::vec4 const& wavelengths)
{
    QString header=1+R"(
#ifndef INCLUDE_ONCE_2B59AE86_E78B_4D75_ACDF_5DA644F8E9A3
#define INCLUDE_ONCE_2B59AE86_E78B_4D75_ACDF_5DA644F8E9A3
const float earthRadius=)"+QString::number(atmo.earthRadius)+"; // must be in meters\n"
                         "const float atmosphereHeight="+QString::number(atmo.atmosphereHeight)+"; // must be in meters\n"
                         R"(
const vec3 earthCenter=vec3(0,0,-earthRadius);

const float dobsonUnit = 2.687e20; // molecules/m^2
const float PI=3.1415926535897932;
const float km=1000;
#define sqr(x) ((x)*(x))

const float sunAngularRadius=)" + toString(atmo.sunAngularRadius) + R"(;
uniform float moonAngularRadius;
const vec4 scatteringTextureSize=)" + toString(glm::vec4(atmo.scatteringTextureSize)) + R"(;
const vec2 irradianceTextureSize=)" + toString(glm::vec2(atmo.irradianceTexW, atmo.irradianceTexH)) + R"(;
const vec2 transmittanceTextureSize=)" + toString(glm::vec2(atmo.transmittanceTexW,atmo.transmittanceTexH)) + R"(;
const vec2 eclipsedSingleScatteringTextureSize=)" + toString(glm::vec2(atmo.eclipsedSingleScatteringTextureSize)) +R"(;
const vec2 lightPollutionTextureSize=)" + toString(glm::vec2(atmo.lightPollutionTextureSize)) +R"(;
const int radialIntegrationPoints=)" + toString(atmo.radialIntegrationPoints) + R"(;
const int angularIntegrationPoints=)" + toString(atmo.angularIntegrationPoints) + R"(;
const int eclipseAngularIntegrationPoints=)" + toString(atmo.eclipseAngularIntegrationPoints) + R"(;
const int numTransmittanceIntegrationPoints=)" + toString(atmo.numTransmittanceIntegrationPoints) + R"(;
)";
    for(auto const& scatterer : atmo.scatterers)
        header += "const vec4 scatteringCrossSection_"+scatterer.name+"="+toString(scatterer.crossSection(wavelengths))+";\n";
    const auto wlI=atmo.wavelengthsIndex(wavelengths);
    header += "const vec4 groundAlbedo="+toString(atmo.groundAlbedo[wlI])+";\n";
    header += "const vec4 solarIrradianceAtTOA="+toString(atmo.solarIrradianceAtTOA[wlI])+";\n";
    header += "const vec4 lightPollutionRadiance="+toString(atmo.lightPollutionRadiance[wlI])+";\n";

    header+="#endif\n"; // close the include guard
    virtualHeaderFiles[CONSTANTS_HEADER_FILENAME]=header;
}

QString makeDensitiesFunctions()
{
    QString header;
    QString src;
    for(auto const& scatterer : atmo.scatterers)
    {
        src += "float scattererNumberDensity_"+scatterer.name+"(float altitude)\n"
               "{\n"
               +scatterer.numberDensity+
               "}\n";
        header += "float scattererNumberDensity_"+scatterer.name+"(float altitude);\n";
    }
    for(auto const& absorber : atmo.absorbers)
    {
        src += "float absorberNumberDensity_"+absorber.name+"(float altitude)\n"
               "{\n"
               +absorber.numberDensity+
               "}\n";
        header += "float absorberNumberDensity_"+absorber.name+"(float altitude);\n";
    }

    header += "vec4 scatteringCrossSection();\n"
              "float scattererDensity(float altitude);\n";

    virtualHeaderFiles[DENSITIES_HEADER_FILENAME]=header;

    return src;
}

QString makeTransmittanceComputeFunctionsSrc(glm::vec4 const& wavelengths)
{
    const QString head=1+R"(
#version 330
#extension GL_ARB_shading_language_420pack : require

#include "const.h.glsl"
#include "common-functions.h.glsl"
)";
    const QString opticalDepthFunctionTemplate=R"(
vec4 opticalDepthToAtmosphereBorder_##agentSpecies(float altitude, float cosZenithAngle, vec4 crossSection)
{
    const float integrInterval=distanceToAtmosphereBorder(cosZenithAngle, altitude);

    const float R=earthRadius;
    const float r1=R+altitude;
    const float l=integrInterval;
    const float mu=cosZenithAngle;
    /* From law of cosines: r₂²=r₁²+l²+2r₁lμ */
    const float endAltitude=-R+sqrt(sqr(r1)+sqr(l)+2*r1*l*mu);

    const float dl=integrInterval/(numTransmittanceIntegrationPoints-1);

    /* Using trapezoid rule on a uniform grid: f0/2+f1+f2+...+f(N-2)+f(N-1)/2. */
    float sum=(agent##NumberDensity_##agentSpecies(altitude)+
               agent##NumberDensity_##agentSpecies(endAltitude))/2;
    for(int n=1;n<numTransmittanceIntegrationPoints-1;++n)
    {
        const float dist=n*dl;
        const float currAlt=-R+sqrt(sqr(r1)+sqr(dist)+2*r1*dist*mu);
        sum+=agent##NumberDensity_##agentSpecies(currAlt);
    }
    return sum*dl*crossSection;
}
)";
    QString opticalDepthFunctions;
    QString computeFunction = R"(
// This assumes that ray doesn't intersect Earth
vec4 computeTransmittanceToAtmosphereBorder(float cosZenithAngle, float altitude)
{
    const vec4 depth=
)";
    for(auto const& scatterer : atmo.scatterers)
    {
        opticalDepthFunctions += QString(opticalDepthFunctionTemplate).replace("##agentSpecies",scatterer.name).replace("agent##","scatterer");
        computeFunction += "        +opticalDepthToAtmosphereBorder_"+scatterer.name+
                             "(altitude,cosZenithAngle,"+toString(scatterer.crossSection(wavelengths))+")\n";
    }
    for(auto const& absorber : atmo.absorbers)
    {
        opticalDepthFunctions += QString(opticalDepthFunctionTemplate).replace("##agentSpecies",absorber.name).replace("agent##","absorber");
        computeFunction += "        +opticalDepthToAtmosphereBorder_"+absorber.name+
                             "(altitude,cosZenithAngle,"+toString(absorber.crossSection(wavelengths))+")\n";
    }
    computeFunction += R"(      ;
    return exp(-depth);
}
)";
    return head+makeDensitiesFunctions()+opticalDepthFunctions+computeFunction;
}

QString makeScattererDensityFunctionsSrc()
{
    const QString head=1+R"(
#version 330
#extension GL_ARB_shading_language_420pack : require

#include "const.h.glsl"
)";
    return head+makeDensitiesFunctions();
}

QString makePhaseFunctionsSrc()
{
    QString src = 1+R"(
#version 330
#extension GL_ARB_shading_language_420pack : require

#include "const.h.glsl"

)";
    QString header;
    for(auto const& scatterer : atmo.scatterers)
    {
        src += "vec4 phaseFunction_"+scatterer.name+"(float dotViewSun)\n"
               "{\n"
               +scatterer.phaseFunction+
               "}\n";
        header += "vec4 phaseFunction_"+scatterer.name+"(float dotViewSun);\n";
    }
    header+="vec4 currentPhaseFunction(float dotViewSun);\n";
    virtualHeaderFiles[PHASE_FUNCTIONS_HEADER_FILENAME]=header;
    return src;
}

QString makeTotalScatteringCoefSrc()
{
    QString src=1+R"(
#version 330
#extension GL_ARB_shading_language_420pack : require

#include "const.h.glsl"
#include "densities.h.glsl"
#include "phase-functions.h.glsl"

vec4 totalScatteringCoefficient(float altitude, float dotViewInc)
{
    return
)";
    for(auto const& scatterer : atmo.scatterers)
    {
        src += "        + scatteringCrossSection_"+scatterer.name+
               " * scattererNumberDensity_"+scatterer.name+"(altitude) "
               " * phaseFunction_"+scatterer.name+"(dotViewInc)\n";
    }
    src += "        ;\n}\n";
    virtualHeaderFiles[TOTAL_SCATTERING_COEFFICIENT_HEADER_FILENAME]=
        "vec4 totalScatteringCoefficient(float altitude, float dotViewInc);\n";
    return src;
}

QString getShaderSrc(QString const& fileName, IgnoreCache ignoreCache)
{
    if(!ignoreCache)
    {
        if(const auto it=virtualSourceFiles.find(fileName); it!=virtualSourceFiles.end())
            return it->second;
        if(const auto it=virtualHeaderFiles.find(fileName); it!=virtualHeaderFiles.end())
            return it->second;
    }

    const auto appBinDir=QDir(qApp->applicationDirPath()+"/").canonicalPath();
    QString filePath=appBinDir + "/shaders/" + fileName;
    if(appBinDir==QDir(INSTALL_BINDIR).canonicalPath())
    {
        filePath=DATA_ROOT_DIR "shaders/" + fileName;
    }
    else if(appBinDir==QDir(BUILD_BINDIR "CalcMySky/").canonicalPath())
    {
        filePath=SOURCE_DIR "shaders/" + fileName;
    }
    QFile file(filePath);
    if(!file.open(QIODevice::ReadOnly))
    {
        std::cerr << "Error opening shader file \"" << filePath.toStdString() << "\"\n";
        throw MustQuit{};
    }
    return file.readAll();
}

std::unique_ptr<QOpenGLShader> compileShader(QOpenGLShader::ShaderType type, QString source, QString const& description)
{
    auto shader=std::make_unique<QOpenGLShader>(type);
    source=withHeadersIncluded(source, description);
    if(!shader->compileSourceCode(source))
    {
        std::cerr << "Failed to compile " << description.toStdString() << ":\n"
                  << shader->log().toStdString() << "\n";
        std::cerr << "Source of the shader:\n________________________________________________\n";
        const auto lineCount=source.count(QChar('\n'));
        QTextStream srcStream(&source);
        int lineNumber=1;
        for(auto line=srcStream.readLine(); !line.isNull(); line=srcStream.readLine(), ++lineNumber)
        {
            QRegExp lineChanger("^\\s*#\\s*line\\s+([0-9]+)\\b.*");
            std::cerr << std::setw(std::ceil(std::log10(lineCount))) << lineNumber << " " << line.toStdString() << "\n";
            if(lineChanger.exactMatch(line))
            {
                lineNumber = lineChanger.cap(1).toInt() - 1;
                continue;
            }
        }
        std::cerr << "________________________________________________\n";
        throw MustQuit{};
    }
    if(!shader->log().isEmpty())
    {
        std::cerr << "Warnings while compiling " << description.toStdString() << ":\n"
                  << shader->log().toStdString() << "\n";
    }
    return shader;
}

std::unique_ptr<QOpenGLShader> compileShader(QOpenGLShader::ShaderType type, QString const& filename)
{ return compileShader(type, getShaderSrc(filename), filename); }

QString withHeadersIncluded(QString src, QString const& filename)
{
    QTextStream srcStream(&src);
    int lineNumber=1;
    int headerNumber=1;
    QString newSrc;
    for(auto line=srcStream.readLine(); !line.isNull(); line=srcStream.readLine(), ++lineNumber)
    {
        if(!line.contains(QRegExp("^\\s*#include(?: \"|_if\\s*\\()")))
        {
            // Not an include line, pass it to output
            newSrc.append(line+'\n');
            continue;
        }
        if(line.contains(QRegExp("^\\s*#include_if\\([A-Za-z_][A-Za-z0-9_]*\\)")))
        {
            // Disabled include, skip it (enabled one must have the condition be literal 1)
            newSrc.append('\n');
            continue;
        }
        auto includePattern=QRegExp("^#include(?:_if\\s*\\(\\s*1\\s*(?:/\\*[^)]*\\*/)?\\))? \"([^\"]+)\"$");
        if(!includePattern.exactMatch(line))
        {
            std::cerr << filename.toStdString() << ":" << lineNumber << ": syntax error in #include directive:\n" << line << "\n";
            throw MustQuit{};
        }
        const auto includeFileName=includePattern.cap(1);
        static const char headerSuffix[]=".h.glsl";
        if(!includeFileName.endsWith(headerSuffix))
        {
            std::cerr << filename.toStdString() << ":" << lineNumber << ": file to include must have suffix \""
                      << headerSuffix << "\"\n";
            throw MustQuit{};
        }
        const auto header = getShaderSrc(includeFileName);
        newSrc.append(QString("#line 1 %1 // %2\n").arg(headerNumber++).arg(includeFileName));
        newSrc.append(header);
        newSrc.append(QString("#line %1 0 // %2\n").arg(lineNumber+1).arg(filename));
    }
    return newSrc;
}

std::set<QString> getShaderFileNamesToLinkWith(QString const& filename, int recursionDepth=0)
{
    constexpr int maxRecursionDepth=50;
    if(recursionDepth>maxRecursionDepth)
    {
        std::cerr << "Include recursion depth exceeded " << maxRecursionDepth << "\n";
        throw MustQuit{};
    }
    std::set<QString> filenames;
    auto shaderSrc=getShaderSrc(filename);
    QTextStream srcStream(&shaderSrc);
    for(auto line=srcStream.readLine(); !line.isNull(); line=srcStream.readLine())
    {
        auto includePattern=QRegExp("^#include(?:_if\\s*\\(\\s*1\\s*(?:/\\*[^)]*\\*/)?\\))? \"([^\"]+)(\\.h\\.glsl)\"$");
        if(!includePattern.exactMatch(line))
            continue;
        const auto includeFileBaseName=includePattern.cap(1);
        const auto headerFileName=includeFileBaseName+includePattern.cap(2);
        if(headerFileName == CONSTANTS_HEADER_FILENAME) // no companion source for constants header
            continue;
        if(headerFileName == RADIANCE_TO_LUMINANCE_HEADER_FILENAME) // no companion source for radiance-to-luminance conversion header
            continue;
        const auto shaderFileNameToLinkWith=includeFileBaseName+".frag";
        filenames.insert(shaderFileNameToLinkWith);
        if(shaderFileNameToLinkWith!=filename)
        {
            const auto extraFileNames=getShaderFileNamesToLinkWith(shaderFileNameToLinkWith, recursionDepth+1);
            filenames.insert(extraFileNames.begin(), extraFileNames.end());
        }
    }
    return filenames;
}

std::unique_ptr<QOpenGLShaderProgram> compileShaderProgram(QString const& mainSrcFileName,
                                                           const char* description, const UseGeomShader useGeomShader,
                                                           std::vector<std::pair<QString, QString>>* sourcesToSave)
{
    auto program=std::make_unique<QOpenGLShaderProgram>();

    auto shaderFileNames=getShaderFileNamesToLinkWith(mainSrcFileName);
    shaderFileNames.insert(mainSrcFileName);

    std::vector<std::unique_ptr<QOpenGLShader>> shaders;
    for(const auto filename : shaderFileNames)
    {
        shaders.emplace_back(compileShader(QOpenGLShader::Fragment, filename));
        program->addShader(shaders.back().get());
        if(sourcesToSave)
            sourcesToSave->push_back({filename, withHeadersIncluded(getShaderSrc(filename), filename)});
    }

    shaders.emplace_back(compileShader(QOpenGLShader::Vertex, "shader.vert"));
    program->addShader(shaders.back().get());

    if(useGeomShader)
    {
        shaders.emplace_back(compileShader(QOpenGLShader::Geometry, "shader.geom"));
        program->addShader(shaders.back().get());
    }

    if(!program->link())
    {
        // Qt prints linking errors to stderr, so don't print them again
        std::cerr << "Failed to link " << description << "\n";
        throw MustQuit{};
    }
    return program;
}


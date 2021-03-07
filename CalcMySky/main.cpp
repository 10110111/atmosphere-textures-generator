#define _USE_MATH_DEFINES // for MSVC to define M_PI etc.
#include <iostream>
#include <iterator>
#include <sstream>
#include <complex>
#include <memory>
#include <random>
#include <chrono>
#include <cmath>
#include <map>
#include <set>

#include <QOffscreenSurface>
#include <QSurfaceFormat>
#include <QApplication>
#include <QRegExp>
#include <QImage>
#include <QFile>

#include "config.h"
#include "data.hpp"
#include "util.hpp"
#include "glinit.hpp"
#include "cmdline.hpp"
#include "shaders.hpp"
#include "../common/EclipsedDoubleScatteringPrecomputer.hpp"
#include "../common/cie-xyzw-functions.hpp"
#include "../common/timing.hpp"

QOpenGLFunctions_3_3_Core gl;
using glm::ivec2;
using glm::vec2;
using glm::vec4;

glm::mat4 radianceToLuminance(const unsigned texIndex)
{
    using glm::mat4;
    const auto diag=[](GLfloat x, GLfloat y, GLfloat z, GLfloat w) { return mat4(x,0,0,0,
                                                                                 0,y,0,0,
                                                                                 0,0,z,0,
                                                                                 0,0,0,w); };
    const auto wlCount = 4*atmo.allWavelengths.size();
    // Weights for the trapezoidal quadrature rule
    const mat4 weights = wlCount==4            ? diag(0.5,1,1,0.5) :
                         texIndex==0           ? diag(0.5,1,1,1  ) :
                         texIndex+1==wlCount/4 ? diag(  1,1,1,0.5) :
                                                 diag(  1,1,1,1);
    const mat4 dlambda = weights * abs(atmo.allWavelengths.back()[3]-atmo.allWavelengths.front()[0]) / (wlCount-1.f);
    // Ref: Rapport BIPM-2019/05. Principles Governing Photometry, 2nd edition. Sections 6.2, 6.3.
    const mat4 maxLuminousEfficacy=diag(683.002f,683.002f,683.002f,1700.13f); // lm/W
    return maxLuminousEfficacy * mat4(wavelengthToXYZW(atmo.allWavelengths[texIndex][0]),
                                      wavelengthToXYZW(atmo.allWavelengths[texIndex][1]),
                                      wavelengthToXYZW(atmo.allWavelengths[texIndex][2]),
                                      wavelengthToXYZW(atmo.allWavelengths[texIndex][3])) * dlambda;
}

void saveIrradiance(const unsigned scatteringOrder, const unsigned texIndex)
{
    if(scatteringOrder==atmo.scatteringOrdersToCompute)
    {
        saveTexture(GL_TEXTURE_2D,textures[TEX_IRRADIANCE],"irradiance texture",
                    atmo.textureOutputDir+"/irradiance-wlset"+std::to_string(texIndex)+".f32",
                    {atmo.irradianceTexW, atmo.irradianceTexH});
    }

    if(!opts.dbgSaveGroundIrradiance) return;

    saveTexture(GL_TEXTURE_2D,textures[TEX_DELTA_IRRADIANCE],"irradiance texture",
                atmo.textureOutputDir+"/irradiance-delta-order"+std::to_string(scatteringOrder-1)+"-wlset"+std::to_string(texIndex)+".f32",
                {atmo.irradianceTexW, atmo.irradianceTexH});

    saveTexture(GL_TEXTURE_2D,textures[TEX_IRRADIANCE],"irradiance texture",
                atmo.textureOutputDir+"/irradiance-accum-order"+std::to_string(scatteringOrder-1)+"-wlset"+std::to_string(texIndex)+".f32",
                {atmo.irradianceTexW, atmo.irradianceTexH});
}

void saveScatteringDensity(const unsigned scatteringOrder, const unsigned texIndex)
{
    if(!opts.dbgSaveScatDensity) return;
    saveTexture(GL_TEXTURE_3D,textures[TEX_DELTA_SCATTERING_DENSITY],
                "order "+std::to_string(scatteringOrder)+" scattering density",
                atmo.textureOutputDir+"/scattering-density"+std::to_string(scatteringOrder)+"-wlset"+std::to_string(texIndex)+".f32",
                {atmo.scatteringTextureSize[0], atmo.scatteringTextureSize[1], atmo.scatteringTextureSize[2], atmo.scatteringTextureSize[3]});
}

void render3DTexLayers(QOpenGLShaderProgram& program, const std::string_view whatIsBeingDone)
{
    if(opts.dbgNoSaveTextures) return; // don't take time to do useless computations

    if(const auto err=gl.glGetError(); err!=GL_NO_ERROR)
    {
        std::cerr << "FAILED on entry to render3DTexLayers(): " << openglErrorString(err) << "\n";
        throw MustQuit{};
    }

    std::cerr << indentOutput() << whatIsBeingDone << "... ";
    for(GLsizei layer=0; layer<atmo.scatTexDepth(); ++layer)
    {
        std::ostringstream ss;
        ss << layer << " of " << atmo.scatTexDepth() << " layers done";
        std::cerr << ss.str();

        program.setUniformValue("layer",layer);
        renderQuad();
        gl.glFinish();

        // Clear previous status and reset cursor position
        const auto statusWidth=ss.tellp();
        std::cerr << std::string(statusWidth, '\b') << std::string(statusWidth, ' ')
                  << std::string(statusWidth, '\b');
    }
    if(const auto err=gl.glGetError(); err!=GL_NO_ERROR)
    {
        std::cerr << "FAILED: " << openglErrorString(err) << "\n";
        throw MustQuit{};
    }
    std::cerr << "done\n";
}

void computeTransmittance(const unsigned texIndex)
{
    const auto program=compileShaderProgram("compute-transmittance.frag", "transmittance computation shader program");

    std::cerr << indentOutput() << "Computing transmittance... ";

    gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_TRANSMITTANCE]);
    assert(fbos[FBO_TRANSMITTANCE]);
    gl.glFramebufferTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,textures[TEX_TRANSMITTANCE],0);
    checkFramebufferStatus("framebuffer for transmittance texture");

    program->bind();
    gl.glViewport(0, 0, atmo.transmittanceTexW, atmo.transmittanceTexH);
    renderQuad();

    gl.glFinish();
    std::cerr << "done\n";

    saveTexture(GL_TEXTURE_2D,textures[TEX_TRANSMITTANCE],"transmittance texture",
                atmo.textureOutputDir+"/transmittance-wlset"+std::to_string(texIndex)+".f32",
                {atmo.transmittanceTexW, atmo.transmittanceTexH});

    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);
}

void computeDirectGroundIrradiance(const unsigned texIndex)
{
    const auto program=compileShaderProgram("compute-direct-irradiance.frag", "direct ground irradiance computation shader program");

    std::cerr << indentOutput() << "Computing direct ground irradiance... ";

    gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_IRRADIANCE]);
    gl.glFramebufferTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,textures[TEX_DELTA_IRRADIANCE],0);
    gl.glFramebufferTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT1,textures[TEX_IRRADIANCE],0);
    checkFramebufferStatus("framebuffer for irradiance texture");
    setDrawBuffers({GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1});

    program->bind();

    setUniformTexture(*program,GL_TEXTURE_2D,TEX_TRANSMITTANCE,0,"transmittanceTexture");

    gl.glViewport(0, 0, atmo.irradianceTexW, atmo.irradianceTexH);
    renderQuad();

    gl.glFinish();
    std::cerr << "done\n";

    saveIrradiance(1,texIndex);
    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);
}

static constexpr char renderShaderFileName[]="render.frag";
constexpr char viewDirFuncFileName[]="calc-view-dir.frag";
constexpr char viewDirStubFunc[]="#version 330\nvec3 calcViewDir() { return vec3(0); }";
void saveZeroOrderScatteringRenderingShader(const unsigned texIndex)
{
    std::vector<std::pair<QString, QString>> sourcesToSave;
    virtualSourceFiles[viewDirFuncFileName]=viewDirStubFunc;
    virtualSourceFiles[renderShaderFileName]=getShaderSrc(renderShaderFileName,IgnoreCache{})
            .replace(QRegExp("\\b(RENDERING_ANY_ZERO_SCATTERING)\\b"), "1 /*\\1*/")
            .replace(QRegExp("\\b(RENDERING_ZERO_SCATTERING)\\b"), "1 /*\\1*/");
    const auto program=compileShaderProgram(renderShaderFileName,
                                            "zero-order scattering rendering shader program",
                                            UseGeomShader{false}, &sourcesToSave);
    for(const auto& [filename, src] : sourcesToSave)
    {
        if(filename==viewDirFuncFileName) continue;

        const auto filePath=QString("%1/shaders/zero-order-scattering/%2/%3")
                                .arg(atmo.textureOutputDir.c_str()).arg(texIndex).arg(filename);
        std::cerr << indentOutput() << "Saving shader \"" << filePath << "\"...";
        QFile file(filePath);
        if(!file.open(QFile::WriteOnly))
        {
            std::cerr << " failed: " << file.errorString().toStdString() << "\"\n";
            throw MustQuit{};
        }
        file.write(src.toUtf8());
        file.flush();
        if(file.error())
        {
            std::cerr << " failed: " << file.errorString().toStdString() << "\"\n";
            throw MustQuit{};
        }
        std::cerr << "done\n";
    }
}

void saveEclipsedZeroOrderScatteringRenderingShader(const unsigned texIndex)
{
    std::vector<std::pair<QString, QString>> sourcesToSave;
    virtualSourceFiles[viewDirFuncFileName]=viewDirStubFunc;
    virtualSourceFiles[renderShaderFileName]=getShaderSrc(renderShaderFileName,IgnoreCache{})
            .replace(QRegExp("\\b(RENDERING_ANY_ZERO_SCATTERING)\\b"), "1 /*\\1*/")
            .replace(QRegExp("\\b(RENDERING_ECLIPSED_ZERO_SCATTERING)\\b"), "1 /*\\1*/");
    const auto program=compileShaderProgram(renderShaderFileName,
                                            "eclipsed zero-order scattering rendering shader program",
                                            UseGeomShader{false}, &sourcesToSave);
    for(const auto& [filename, src] : sourcesToSave)
    {
        if(filename==viewDirFuncFileName) continue;

        const auto filePath=QString("%1/shaders/eclipsed-zero-order-scattering/%2/%3")
                                .arg(atmo.textureOutputDir.c_str()).arg(texIndex).arg(filename);
        std::cerr << indentOutput() << "Saving shader \"" << filePath << "\"...";
        QFile file(filePath);
        if(!file.open(QFile::WriteOnly))
        {
            std::cerr << " failed: " << file.errorString().toStdString() << "\"\n";
            throw MustQuit{};
        }
        file.write(src.toUtf8());
        file.flush();
        if(file.error())
        {
            std::cerr << " failed: " << file.errorString().toStdString() << "\"\n";
            throw MustQuit{};
        }
        std::cerr << "done\n";
    }
}

void saveMultipleScatteringRenderingShader(const unsigned texIndex)
{
    std::vector<std::pair<QString, QString>> sourcesToSave;
    virtualSourceFiles[viewDirFuncFileName]=viewDirStubFunc;
    const QString macroToReplace = opts.saveResultAsRadiance ? "RENDERING_MULTIPLE_SCATTERING_RADIANCE" : "RENDERING_MULTIPLE_SCATTERING_LUMINANCE";
    virtualSourceFiles[renderShaderFileName]=getShaderSrc(renderShaderFileName,IgnoreCache{}).replace(QRegExp("\\b("+macroToReplace+")\\b"), "1 /*\\1*/");
    const auto program=compileShaderProgram(renderShaderFileName,
                                            "multiple scattering rendering shader program",
                                            UseGeomShader{false}, &sourcesToSave);
    for(const auto& [filename, src] : sourcesToSave)
    {
        if(filename==viewDirFuncFileName) continue;

        const auto filePath = opts.saveResultAsRadiance ? QString("%1/shaders/multiple-scattering/%2/%3").arg(atmo.textureOutputDir.c_str()).arg(texIndex).arg(filename)
                                                        : QString("%1/shaders/multiple-scattering/%2").arg(atmo.textureOutputDir.c_str()).arg(filename);
        std::cerr << indentOutput() << "Saving shader \"" << filePath << "\"...";
        QFile file(filePath);
        if(!file.open(QFile::WriteOnly))
        {
            std::cerr << " failed: " << file.errorString().toStdString() << "\"\n";
            throw MustQuit{};
        }
        file.write(src.toUtf8());
        file.flush();
        if(file.error())
        {
            std::cerr << " failed: " << file.errorString().toStdString() << "\"\n";
            throw MustQuit{};
        }
        std::cerr << "done\n";
    }
}

void saveSingleScatteringRenderingShader(const unsigned texIndex, AtmosphereParameters::Scatterer const& scatterer, const SingleScatteringRenderMode renderMode)
{
    virtualSourceFiles[PHASE_FUNCTIONS_SHADER_FILENAME]=makePhaseFunctionsSrc()+
        "vec4 currentPhaseFunction(float dotViewSun) { return phaseFunction_"+scatterer.name+"(dotViewSun); }\n";

    if(scatterer.phaseFunctionType==PhaseFunctionType::Smooth)
        return; // Luminance will be already merged in multiple scattering texture, no need to render it separately

    std::vector<std::pair<QString, QString>> sourcesToSave;
    virtualSourceFiles[viewDirFuncFileName]=viewDirStubFunc;
    const auto renderModeDefine = renderMode==SSRM_ON_THE_FLY ? "RENDERING_SINGLE_SCATTERING_ON_THE_FLY" :
                                  scatterer.phaseFunctionType==PhaseFunctionType::General ? "RENDERING_SINGLE_SCATTERING_PRECOMPUTED_RADIANCE"
                                                                                          : "RENDERING_SINGLE_SCATTERING_PRECOMPUTED_LUMINANCE";
    virtualSourceFiles[renderShaderFileName]=getShaderSrc(renderShaderFileName,IgnoreCache{})
                                                .replace(QRegExp("\\b(RENDERING_ANY_SINGLE_SCATTERING)\\b"), "1 /*\\1*/")
                                                .replace(QRegExp("\\b(RENDERING_ANY_NORMAL_SINGLE_SCATTERING)\\b"), "1 /*\\1*/")
                                                .replace(QRegExp(QString("\\b(%1)\\b").arg(renderModeDefine)), "1 /*\\1*/");
    const auto program=compileShaderProgram(renderShaderFileName,
                                            "single scattering rendering shader program",
                                            UseGeomShader{false}, &sourcesToSave);
    for(const auto& [filename, src] : sourcesToSave)
    {
        if(filename==viewDirFuncFileName) continue;

        const auto filePath = scatterer.phaseFunctionType==PhaseFunctionType::General || renderMode==SSRM_ON_THE_FLY ?
           QString("%1/shaders/single-scattering/%2/%3/%4/%5").arg(atmo.textureOutputDir.c_str()).arg(toString(renderMode)).arg(texIndex).arg(scatterer.name).arg(filename) :
           QString("%1/shaders/single-scattering/%2/%3/%4").arg(atmo.textureOutputDir.c_str()).arg(toString(renderMode)).arg(scatterer.name).arg(filename);
        std::cerr << indentOutput() << "Saving shader \"" << filePath << "\"...";
        QFile file(filePath);
        if(!file.open(QFile::WriteOnly))
        {
            std::cerr << " failed: " << file.errorString().toStdString() << "\"\n";
            throw MustQuit{};
        }
        file.write(src.toUtf8());
        file.flush();
        if(file.error())
        {
            std::cerr << " failed: " << file.errorString().toStdString() << "\"\n";
            throw MustQuit{};
        }
        std::cerr << "done\n";
    }
}

void saveEclipsedSingleScatteringRenderingShader(const unsigned texIndex, AtmosphereParameters::Scatterer const& scatterer, const SingleScatteringRenderMode renderMode)
{
    virtualSourceFiles.erase(SINGLE_SCATTERING_ECLIPSED_FILENAME); // Need to refresh it after computeEclipsedDoubleScattering()
    virtualSourceFiles[PHASE_FUNCTIONS_SHADER_FILENAME]=makePhaseFunctionsSrc()+
        "vec4 currentPhaseFunction(float dotViewSun) { return phaseFunction_"+scatterer.name+"(dotViewSun); }\n";

    std::vector<std::pair<QString, QString>> sourcesToSave;
    static constexpr char renderShaderFileName[]="render.frag";
    const auto renderModeDefine = renderMode==SSRM_ON_THE_FLY ? "RENDERING_ECLIPSED_SINGLE_SCATTERING_ON_THE_FLY" :
                                  scatterer.phaseFunctionType==PhaseFunctionType::General ? "RENDERING_ECLIPSED_SINGLE_SCATTERING_PRECOMPUTED_RADIANCE"
                                                                                          : "RENDERING_ECLIPSED_SINGLE_SCATTERING_PRECOMPUTED_LUMINANCE";
    virtualSourceFiles[renderShaderFileName]=getShaderSrc(renderShaderFileName,IgnoreCache{})
                                                .replace(QRegExp("\\b(RENDERING_ANY_SINGLE_SCATTERING)\\b"), "1 /*\\1*/")
                                                .replace(QRegExp("\\b(RENDERING_ANY_ECLIPSED_SINGLE_SCATTERING)\\b"), "1 /*\\1*/")
                                                .replace(QRegExp(QString("\\b(%1)\\b").arg(renderModeDefine)), "1 /*\\1*/");
    const auto program=compileShaderProgram(renderShaderFileName,
                                            "single scattering rendering shader program",
                                            UseGeomShader{false}, &sourcesToSave);
    for(const auto& [filename, src] : sourcesToSave)
    {
        if(filename==viewDirFuncFileName) continue;

        const auto filePath = scatterer.phaseFunctionType==PhaseFunctionType::General || renderMode==SSRM_ON_THE_FLY ?
            QString("%1/shaders/single-scattering-eclipsed/%2/%3/%4/%5").arg(atmo.textureOutputDir.c_str()).arg(toString(renderMode)).arg(texIndex).arg(scatterer.name).arg(filename) :
            QString("%1/shaders/single-scattering-eclipsed/%2/%3/%4").arg(atmo.textureOutputDir.c_str()).arg(toString(renderMode)).arg(scatterer.name).arg(filename);
        std::cerr << indentOutput() << "Saving shader \"" << filePath << "\"...";
        QFile file(filePath);
        if(!file.open(QFile::WriteOnly))
        {
            std::cerr << " failed: " << file.errorString().toStdString() << "\"\n";
            throw MustQuit{};
        }
        file.write(src.toUtf8());
        file.flush();
        if(file.error())
        {
            std::cerr << " failed: " << file.errorString().toStdString() << "\"\n";
            throw MustQuit{};
        }
        std::cerr << "done\n";
    }
}

void saveEclipsedSingleScatteringComputationShader(const unsigned texIndex, AtmosphereParameters::Scatterer const& scatterer)
{
    // Not removing SINGLE_SCATTERING_ECLIPSED_FILENAME, since it's refreshed in saveEclipsedSingleScatteringRenderingShader()
    virtualSourceFiles[PHASE_FUNCTIONS_SHADER_FILENAME]=makePhaseFunctionsSrc()+
        "vec4 currentPhaseFunction(float dotViewSun) { return phaseFunction_"+scatterer.name+"(dotViewSun); }\n";

    std::vector<std::pair<QString, QString>> sourcesToSave;
    static constexpr char renderShaderFileName[]="compute-eclipsed-single-scattering.frag";
    virtualSourceFiles[renderShaderFileName]=getShaderSrc(renderShaderFileName,IgnoreCache{})
        .replace(QRegExp(QString("\\b(%1)\\b").arg(scatterer.phaseFunctionType==PhaseFunctionType::General ? "COMPUTE_RADIANCE" : "COMPUTE_LUMINANCE")), "1 /*\\1*/");
    const auto program=compileShaderProgram(renderShaderFileName,
                                            "single scattering rendering shader program",
                                            UseGeomShader{false}, &sourcesToSave);
    for(const auto& [filename, src] : sourcesToSave)
    {
        if(filename==viewDirFuncFileName) continue;

        const auto filePath = QString("%1/shaders/single-scattering-eclipsed/precomputation/%2/%3/%4")
                                    .arg(atmo.textureOutputDir.c_str())
                                    .arg(texIndex)
                                    .arg(scatterer.name)
                                    .arg(filename);
        std::cerr << indentOutput() << "Saving shader \"" << filePath << "\"...";
        QFile file(filePath);
        if(!file.open(QFile::WriteOnly))
        {
            std::cerr << " failed: " << file.errorString().toStdString() << "\"\n";
            throw MustQuit{};
        }
        file.write(src.toUtf8());
        file.flush();
        if(file.error())
        {
            std::cerr << " failed: " << file.errorString().toStdString() << "\"\n";
            throw MustQuit{};
        }
        std::cerr << "done\n";
    }
}

void saveEclipsedDoubleScatteringRenderingShader(const unsigned texIndex)
{
    virtualSourceFiles.erase(DOUBLE_SCATTERING_ECLIPSED_FILENAME);

    std::vector<std::pair<QString, QString>> sourcesToSave;
    static constexpr char renderShaderFileName[]="render.frag";
    virtualSourceFiles[renderShaderFileName]=getShaderSrc(renderShaderFileName,IgnoreCache{})
        .replace(QRegExp("\\b(RENDERING_ECLIPSED_DOUBLE_SCATTERING_PRECOMPUTED_RADIANCE)\\b"), "1 /*\\1*/");
    const auto program=compileShaderProgram(renderShaderFileName,
                                            "double scattering rendering shader program",
                                            UseGeomShader{false}, &sourcesToSave);
    for(const auto& [filename, src] : sourcesToSave)
    {
        if(filename==viewDirFuncFileName) continue;

        const auto filePath = QString("%1/shaders/double-scattering-eclipsed/precomputed/%2/%3").arg(atmo.textureOutputDir.c_str())
                                                                                                .arg(texIndex)
                                                                                                .arg(filename);
        std::cerr << indentOutput() << "Saving shader \"" << filePath << "\"...";
        QFile file(filePath);
        if(!file.open(QFile::WriteOnly))
        {
            std::cerr << " failed: " << file.errorString().toStdString() << "\"\n";
            throw MustQuit{};
        }
        file.write(src.toUtf8());
        file.flush();
        if(file.error())
        {
            std::cerr << " failed: " << file.errorString().toStdString() << "\"\n";
            throw MustQuit{};
        }
        std::cerr << "done\n";
    }
}

void accumulateSingleScattering(const unsigned texIndex, AtmosphereParameters::Scatterer const& scatterer)
{
    gl.glBlendFunc(GL_ONE, GL_ONE);
    gl.glEnable(GL_BLEND);
    auto& targetTexture=accumulatedSingleScatteringTextures[scatterer.name];
    if(!targetTexture)
    {
        gl.glGenTextures(1, &targetTexture);
        gl.glBindTexture(GL_TEXTURE_3D,targetTexture);
        gl.glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        gl.glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        gl.glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        gl.glTexParameteri(GL_TEXTURE_3D,GL_TEXTURE_WRAP_R,GL_CLAMP_TO_EDGE);
        setupTexture(targetTexture, atmo.scatTexWidth(),atmo.scatTexHeight(),atmo.scatTexDepth());
        gl.glDisable(GL_BLEND);
    }
    gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_SINGLE_SCATTERING]);
    gl.glFramebufferTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0, targetTexture,0);
    checkFramebufferStatus("framebuffer for accumulation of single scattering radiance");

    const auto program=compileShaderProgram("copy-scattering-texture.frag",
                                            "scattering texture copy-blend shader program",
                                            UseGeomShader{});
    program->bind();
    setUniformTexture(*program,GL_TEXTURE_3D,TEX_DELTA_SCATTERING,0,"tex");
    program->setUniformValue("radianceToLuminance", toQMatrix(radianceToLuminance(texIndex)));
    render3DTexLayers(*program, "Blending single scattering layers into accumulator texture");

    gl.glDisable(GL_BLEND);
    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);

    if(texIndex+1==atmo.allWavelengths.size() && scatterer.phaseFunctionType!=PhaseFunctionType::Smooth)
    {
        saveTexture(GL_TEXTURE_3D,targetTexture, "single scattering texture",
                    atmo.textureOutputDir+"/single-scattering/"+scatterer.name.toStdString()+"-xyzw.f32",
                    {atmo.scatteringTextureSize[0], atmo.scatteringTextureSize[1], atmo.scatteringTextureSize[2], atmo.scatteringTextureSize[3]});
    }
}

void computeSingleScattering(const unsigned texIndex, AtmosphereParameters::Scatterer const& scatterer)
{
    gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_DELTA_SCATTERING]);
    gl.glFramebufferTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0, textures[TEX_DELTA_SCATTERING],0);
    checkFramebufferStatus("framebuffer for first scattering");

    gl.glViewport(0, 0, atmo.scatTexWidth(), atmo.scatTexHeight());

    const auto src=makeScattererDensityFunctionsSrc()+
                    "float scattererDensity(float alt) { return scattererNumberDensity_"+scatterer.name+"(alt); }\n"+
                    "vec4 scatteringCrossSection() { return "+toString(scatterer.crossSection(atmo.allWavelengths[texIndex]))+"; }\n";
    virtualSourceFiles[DENSITIES_SHADER_FILENAME]=src;
    const auto program=compileShaderProgram("compute-single-scattering.frag",
                                            "single scattering computation shader program",
                                            UseGeomShader{});
    program->bind();
    setUniformTexture(*program,GL_TEXTURE_2D,TEX_TRANSMITTANCE,0,"transmittanceTexture");

    render3DTexLayers(*program, "Computing single scattering layers");

    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);

    switch(scatterer.phaseFunctionType)
    {
    case PhaseFunctionType::General:
        saveTexture(GL_TEXTURE_3D,textures[TEX_DELTA_SCATTERING], "single scattering texture",
                    atmo.textureOutputDir+"/single-scattering/"+std::to_string(texIndex)+"/"+scatterer.name.toStdString()+".f32",
                    {atmo.scatteringTextureSize[0], atmo.scatteringTextureSize[1], atmo.scatteringTextureSize[2], atmo.scatteringTextureSize[3]});
        break;
    case PhaseFunctionType::Achromatic:
    case PhaseFunctionType::Smooth:
        accumulateSingleScattering(texIndex, scatterer);
        break;
    }

    saveSingleScatteringRenderingShader(texIndex, scatterer, SSRM_ON_THE_FLY);
    saveSingleScatteringRenderingShader(texIndex, scatterer, SSRM_PRECOMPUTED);
    saveEclipsedSingleScatteringRenderingShader(texIndex, scatterer, SSRM_ON_THE_FLY);
    saveEclipsedSingleScatteringRenderingShader(texIndex, scatterer, SSRM_PRECOMPUTED);
    saveEclipsedSingleScatteringComputationShader(texIndex, scatterer);
}

void computeIndirectIrradianceOrder1(unsigned texIndex, unsigned scattererIndex);
void computeScatteringDensityOrder2(const unsigned texIndex)
{
    constexpr unsigned scatteringOrder=2;

    virtualSourceFiles[DENSITIES_SHADER_FILENAME]=makeScattererDensityFunctionsSrc();
    std::unique_ptr<QOpenGLShaderProgram> program;
    {
        // Make a stub for current phase function. It's not used for ground radiance, but we need it to avoid linking errors.
        virtualSourceFiles[PHASE_FUNCTIONS_SHADER_FILENAME]=makePhaseFunctionsSrc()+
            "vec4 currentPhaseFunction(float dotViewSun) { return vec4(3.4028235e38); }\n";

        // Doing replacements instead of using uniforms is meant to
        //  1) Improve performance by statically avoiding branching
        //  2) Ease debugging by clearing the list of really-used uniforms (this can be printed by dumpActiveUniforms())
        virtualSourceFiles[COMPUTE_SCATTERING_DENSITY_FILENAME]=getShaderSrc(COMPUTE_SCATTERING_DENSITY_FILENAME,IgnoreCache{})
                                                    .replace(QRegExp("\\bRADIATION_IS_FROM_GROUND_ONLY\\b"), "true")
                                                    .replace(QRegExp("\\bSCATTERING_ORDER\\b"), QString::number(scatteringOrder));
        // recompile the program
        program=compileShaderProgram(COMPUTE_SCATTERING_DENSITY_FILENAME,
                                     "scattering density computation shader program", UseGeomShader{});
    }

    gl.glViewport(0, 0, atmo.scatTexWidth(), atmo.scatTexHeight());

    program->bind();

    gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_MULTIPLE_SCATTERING]);
    gl.glFramebufferTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,textures[TEX_DELTA_SCATTERING_DENSITY],0);
    checkFramebufferStatus("framebuffer for scattering density");

    setUniformTexture(*program,GL_TEXTURE_2D,TEX_TRANSMITTANCE   ,0,"transmittanceTexture");
    setUniformTexture(*program,GL_TEXTURE_2D,TEX_DELTA_IRRADIANCE,1,"irradianceTexture");

    render3DTexLayers(*program, "Computing scattering density layers for radiation from the ground");

    if(opts.dbgSaveScatDensityOrder2FromGround)
    {
        saveTexture(GL_TEXTURE_3D,textures[TEX_DELTA_SCATTERING_DENSITY],
                    "order 2 scattering density from ground texture",
                    atmo.textureOutputDir+"/scattering-density2-from-ground-wlset"+std::to_string(texIndex)+".f32",
                    {atmo.scatteringTextureSize[0], atmo.scatteringTextureSize[1], atmo.scatteringTextureSize[2], atmo.scatteringTextureSize[3]});
    }

    gl.glBlendFunc(GL_ONE, GL_ONE);
    for(unsigned scattererIndex=0; scattererIndex<atmo.scatterers.size(); ++scattererIndex)
    {
        const auto& scatterer=atmo.scatterers[scattererIndex];
        std::cerr << indentOutput() << "Processing scatterer \""+scatterer.name.toStdString()+"\":\n";
        OutputIndentIncrease incr;

        // Current phase function is updated in the single scattering computation while saving the rendering shader
        computeSingleScattering(texIndex, scatterer);

        gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_MULTIPLE_SCATTERING]);

        {
            virtualSourceFiles[COMPUTE_SCATTERING_DENSITY_FILENAME]=getShaderSrc(COMPUTE_SCATTERING_DENSITY_FILENAME,IgnoreCache{})
                                                    .replace(QRegExp("\\bRADIATION_IS_FROM_GROUND_ONLY\\b"), "false")
                                                    .replace(QRegExp("\\bSCATTERING_ORDER\\b"), QString::number(scatteringOrder));
            // recompile the program
            program=compileShaderProgram(COMPUTE_SCATTERING_DENSITY_FILENAME,
                                                        "scattering density computation shader program", UseGeomShader{});
        }
        program->bind();

        setUniformTexture(*program,GL_TEXTURE_3D,TEX_DELTA_SCATTERING,1,"firstScatteringTexture");

        gl.glEnable(GL_BLEND);
        render3DTexLayers(*program, "Computing scattering density layers");

        // Disables blending before returning
        computeIndirectIrradianceOrder1(texIndex, scattererIndex);
    }
    gl.glDisable(GL_BLEND);
    saveScatteringDensity(scatteringOrder,texIndex);
    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);
}

void computeScatteringDensity(const unsigned scatteringOrder, const unsigned texIndex)
{
    assert(scatteringOrder>2);

    gl.glViewport(0, 0, atmo.scatTexWidth(), atmo.scatTexHeight());
    gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_MULTIPLE_SCATTERING]);
    gl.glFramebufferTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,textures[TEX_DELTA_SCATTERING_DENSITY],0);

    virtualSourceFiles[COMPUTE_SCATTERING_DENSITY_FILENAME]=getShaderSrc(COMPUTE_SCATTERING_DENSITY_FILENAME,IgnoreCache{})
                                                    .replace(QRegExp("\\bRADIATION_IS_FROM_GROUND_ONLY\\b"), "false")
                                                    .replace(QRegExp("\\bSCATTERING_ORDER\\b"), QString::number(scatteringOrder));
    // recompile the program
    const std::unique_ptr<QOpenGLShaderProgram> program=compileShaderProgram(COMPUTE_SCATTERING_DENSITY_FILENAME,
                                                                             "scattering density computation shader program",
                                                                             UseGeomShader{});
    program->bind();

    setUniformTexture(*program,GL_TEXTURE_2D,TEX_TRANSMITTANCE   ,0,"transmittanceTexture");
    setUniformTexture(*program,GL_TEXTURE_2D,TEX_DELTA_IRRADIANCE,1,"irradianceTexture");
    setUniformTexture(*program,GL_TEXTURE_3D,TEX_DELTA_SCATTERING,2,"multipleScatteringTexture");

    render3DTexLayers(*program, "Computing scattering density layers");
    saveScatteringDensity(scatteringOrder,texIndex);
    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);
}

void computeIndirectIrradianceOrder1(const unsigned texIndex, const unsigned scattererIndex)
{
    constexpr unsigned scatteringOrder=2;

    gl.glViewport(0, 0, atmo.irradianceTexW, atmo.irradianceTexH);

    gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_IRRADIANCE]);
    gl.glBlendFunc(GL_ONE, GL_ONE);
    if(scattererIndex==0)
        gl.glDisablei(GL_BLEND, 0); // First scatterer overwrites delta-irradiance-texture
    else
        gl.glEnablei(GL_BLEND, 0); // Second and subsequent scatterers blend into delta-irradiance-texture
    gl.glEnablei(GL_BLEND, 1); // Total irradiance is always accumulated

    const auto& scatterer=atmo.scatterers[scattererIndex];

    virtualSourceFiles[PHASE_FUNCTIONS_SHADER_FILENAME]=makePhaseFunctionsSrc()+
        "vec4 currentPhaseFunction(float dotViewSun) { return phaseFunction_"+scatterer.name+"(dotViewSun); }\n";

    virtualSourceFiles[COMPUTE_INDIRECT_IRRADIANCE_FILENAME]=getShaderSrc(COMPUTE_INDIRECT_IRRADIANCE_FILENAME,IgnoreCache{})
                                                .replace(QRegExp("\\bSCATTERING_ORDER\\b"), QString::number(scatteringOrder-1));
    std::unique_ptr<QOpenGLShaderProgram> program=compileShaderProgram(COMPUTE_INDIRECT_IRRADIANCE_FILENAME,
                                                                       "indirect irradiance computation shader program");
    program->bind();
    setUniformTexture(*program,GL_TEXTURE_3D,TEX_DELTA_SCATTERING,0,"firstScatteringTexture");

    std::cerr << indentOutput() << "Computing indirect irradiance... ";
    renderQuad();
    gl.glFinish();
    std::cerr << "done\n";

    gl.glDisable(GL_BLEND);
    saveIrradiance(scatteringOrder,texIndex);
    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);
}

void computeIndirectIrradiance(const unsigned scatteringOrder, const unsigned texIndex)
{
    assert(scatteringOrder>2);
    gl.glViewport(0, 0, atmo.irradianceTexW, atmo.irradianceTexH);

    gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_IRRADIANCE]);
    gl.glBlendFunc(GL_ONE, GL_ONE);
    gl.glDisablei(GL_BLEND, 0); // Overwrite delta-irradiance-texture
    gl.glEnablei(GL_BLEND, 1); // Accumulate total irradiance

    virtualSourceFiles[COMPUTE_INDIRECT_IRRADIANCE_FILENAME]=getShaderSrc(COMPUTE_INDIRECT_IRRADIANCE_FILENAME,IgnoreCache{})
                                                .replace(QRegExp("\\bSCATTERING_ORDER\\b"), QString::number(scatteringOrder-1));
    std::unique_ptr<QOpenGLShaderProgram> program=compileShaderProgram(COMPUTE_INDIRECT_IRRADIANCE_FILENAME,
                                                                       "indirect irradiance computation shader program");
    program->bind();
    setUniformTexture(*program,GL_TEXTURE_3D,TEX_DELTA_SCATTERING,0,"multipleScatteringTexture");

    std::cerr << indentOutput() << "Computing indirect irradiance... ";
    renderQuad();
    gl.glFinish();
    std::cerr << "done\n";

    gl.glDisable(GL_BLEND);
    saveIrradiance(scatteringOrder,texIndex);
    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);
}

void mergeSmoothSingleScatteringTexture()
{
    gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_MULTIPLE_SCATTERING]);
    for(const auto& scatterer : atmo.scatterers)
    {
        if(scatterer.phaseFunctionType!=PhaseFunctionType::Smooth)
            continue;
        virtualSourceFiles[PHASE_FUNCTIONS_SHADER_FILENAME]=makePhaseFunctionsSrc()+
            "vec4 currentPhaseFunction(float dotViewSun) { return phaseFunction_"+scatterer.name+"(dotViewSun); }\n";
        const auto program=compileShaderProgram("merge-smooth-single-scattering-texture.frag",
                                                "single scattering texture merge shader program",
                                                UseGeomShader{});
        program->bind();
        gl.glBlendFunc(GL_ONE, GL_ONE);
        gl.glEnable(GL_BLEND);
        setUniformTexture(*program,GL_TEXTURE_3D,accumulatedSingleScatteringTextures[scatterer.name],0,"tex");
        render3DTexLayers(*program, "Blending single scattering data for scatterer \""+scatterer.name.toStdString()+
                                    "\" into multiple scattering texture");
    }
    gl.glDisable(GL_BLEND);
    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);
}

void accumulateMultipleScattering(const unsigned scatteringOrder, const unsigned texIndex)
{
    // We didn't render to the accumulating texture when computing delta scattering to avoid holding
    // more than two 4D textures in VRAM at once.
    // Now it's time to do this by only holding the accumulator and delta scattering texture in VRAM.
    gl.glActiveTexture(GL_TEXTURE0);
    gl.glBlendFunc(GL_ONE, GL_ONE);
    if(scatteringOrder>2 || (texIndex>0 && !opts.saveResultAsRadiance))
        gl.glEnable(GL_BLEND);
    else
        gl.glDisable(GL_BLEND);
    gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_MULTIPLE_SCATTERING]);
    gl.glFramebufferTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0, textures[TEX_MULTIPLE_SCATTERING],0);
    checkFramebufferStatus("framebuffer for accumulation of multiple scattering data");

    const auto program=compileShaderProgram("copy-scattering-texture.frag",
                                            "scattering texture copy-blend shader program",
                                            UseGeomShader{});
    program->bind();
    if(!opts.saveResultAsRadiance)
        program->setUniformValue("radianceToLuminance", toQMatrix(radianceToLuminance(texIndex)));
    setUniformTexture(*program,GL_TEXTURE_3D,TEX_DELTA_SCATTERING,0,"tex");
    render3DTexLayers(*program, "Blending multiple scattering layers into accumulator texture");
    gl.glDisable(GL_BLEND);

    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);

    if(opts.dbgSaveAccumScattering)
    {
        saveTexture(GL_TEXTURE_3D,textures[TEX_MULTIPLE_SCATTERING],
                    "multiple scattering accumulator texture",
                    atmo.textureOutputDir+"/multiple-scattering-to-order"+std::to_string(scatteringOrder)+"-wlset"+std::to_string(texIndex)+".f32",
                    {atmo.scatteringTextureSize[0], atmo.scatteringTextureSize[1], atmo.scatteringTextureSize[2], atmo.scatteringTextureSize[3]});
    }
    if(scatteringOrder==atmo.scatteringOrdersToCompute && (texIndex+1==atmo.allWavelengths.size() || opts.saveResultAsRadiance))
    {
        mergeSmoothSingleScatteringTexture();

        const auto filename = opts.saveResultAsRadiance ?
            atmo.textureOutputDir+"/multiple-scattering-wlset"+std::to_string(texIndex)+".f32" :
            atmo.textureOutputDir+"/multiple-scattering-xyzw.f32";
        saveTexture(GL_TEXTURE_3D,textures[TEX_MULTIPLE_SCATTERING],
                    "multiple scattering accumulator texture", filename,
                    {atmo.scatteringTextureSize[0], atmo.scatteringTextureSize[1], atmo.scatteringTextureSize[2], atmo.scatteringTextureSize[3]});
    }
}

void computeMultipleScatteringFromDensity(const unsigned scatteringOrder, const unsigned texIndex)
{
    gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_MULTIPLE_SCATTERING]);
    gl.glFramebufferTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0, textures[TEX_DELTA_SCATTERING],0);
    checkFramebufferStatus("framebuffer for delta multiple scattering");

    gl.glViewport(0, 0, atmo.scatTexWidth(), atmo.scatTexHeight());

    {
        const auto program=compileShaderProgram("compute-multiple-scattering.frag",
                                                "multiple scattering computation shader program",
                                                UseGeomShader{});
        program->bind();

        setUniformTexture(*program,GL_TEXTURE_2D,TEX_TRANSMITTANCE,0,"transmittanceTexture");
        setUniformTexture(*program,GL_TEXTURE_3D,TEX_DELTA_SCATTERING_DENSITY,1,"scatteringDensityTexture");

        render3DTexLayers(*program, "Computing multiple scattering layers");

        if(opts.dbgSaveDeltaScattering)
        {
            saveTexture(GL_TEXTURE_3D,textures[TEX_DELTA_SCATTERING],
                        "delta scattering texture",
                        atmo.textureOutputDir+"/delta-scattering-order"+std::to_string(scatteringOrder)+"-wlset"+std::to_string(texIndex)+".f32",
                        {atmo.scatteringTextureSize[0], atmo.scatteringTextureSize[1], atmo.scatteringTextureSize[2], atmo.scatteringTextureSize[3]});
        }
    }
    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);
    accumulateMultipleScattering(scatteringOrder, texIndex);
}

void computeMultipleScattering(const unsigned texIndex)
{
    // Due to interleaving of calculations of first scattering for each scatterer with the
    // second-order scattering density and irradiance we have to do this iteration separately.
    {
        std::cerr << indentOutput() << "Working on scattering orders 1 and 2:\n";
        OutputIndentIncrease incr;

        computeScatteringDensityOrder2(texIndex);
        computeMultipleScatteringFromDensity(2,texIndex);
    }
    saveEclipsedDoubleScatteringRenderingShader(texIndex);
    for(unsigned scatteringOrder=3; scatteringOrder<=atmo.scatteringOrdersToCompute; ++scatteringOrder)
    {
        std::cerr << indentOutput() << "Working on scattering order " << scatteringOrder << ":\n";
        OutputIndentIncrease incr;

        computeScatteringDensity(scatteringOrder,texIndex);
        computeIndirectIrradiance(scatteringOrder,texIndex);
        computeMultipleScatteringFromDensity(scatteringOrder,texIndex);
    }
}

// XXX: keep in sync with the GLSL version in texture-coordinates.frag
float unitRangeTexCoordToCosSZA(const float texCoord)
{
    const float distMin=atmo.atmosphereHeight;
    const float distMax=atmo.lengthOfHorizRayFromGroundToBorderOfAtmo;
    // TODO: choose a more descriptive name, same as in cosSZAToUnitRangeTexCoord()
    const float A=atmo.earthRadius/(distMax-distMin);
    // TODO: choose a more descriptive name, same as in cosSZAToUnitRangeTexCoord()
    const float a=(A-A*texCoord)/(1+A*texCoord);
    const float distFromGroundToTopAtmoBorder=distMin+std::min(a,A)*(distMax-distMin);
    return distFromGroundToTopAtmoBorder==0 ? 1 :
        clampCosine((sqr(atmo.lengthOfHorizRayFromGroundToBorderOfAtmo)-sqr(distFromGroundToTopAtmoBorder)) /
                    (2*atmo.earthRadius*distFromGroundToTopAtmoBorder));
}

std::unique_ptr<QOpenGLShaderProgram> saveEclipsedDoubleScatteringComputationShader(const unsigned texIndex)
{
    QString scatCoefDef="vec4 totalScatteringCoefficient=vec4(0);\n";
    for(const auto& scatterer : atmo.scatterers)
    {
        scatCoefDef += "    totalScatteringCoefficient += "
                         " scattererNumberDensity_" + scatterer.name + "(altAtDist)"
                       " * scatteringCrossSection_" + scatterer.name +
                       " * phaseFunction_" + scatterer.name + "(dotViewSun)"
                       ";\n";
    }
    virtualSourceFiles[SINGLE_SCATTERING_ECLIPSED_FILENAME]=getShaderSrc(SINGLE_SCATTERING_ECLIPSED_FILENAME,IgnoreCache{})
                                                    .replace(QRegExp("\\bCOMPUTE_TOTAL_SCATTERING_COEFFICIENT;"), scatCoefDef)
                                                    .replace(QRegExp("\\b(ALL_SCATTERERS_AT_ONCE_WITH_PHASE_FUNCTION)\\b"), "1 /*\\1*/");
    std::vector<std::pair<QString, QString>> sourcesToSave;
    auto program=compileShaderProgram(COMPUTE_ECLIPSED_DOUBLE_SCATTERING_FILENAME,
                                      "eclipsed double scattering computation shader program",
                                      UseGeomShader{false}, &sourcesToSave);
    for(const auto& [filename, src] : sourcesToSave)
    {
        if(filename==viewDirFuncFileName) continue;

        const auto filePath = QString("%1/shaders/double-scattering-eclipsed/precomputation/%2/%3").arg(atmo.textureOutputDir.c_str())
                                    .arg(texIndex).arg(filename);
        std::cerr << indentOutput() << "Saving shader \"" << filePath << "\"...";
        QFile file(filePath);
        if(!file.open(QFile::WriteOnly))
        {
            std::cerr << " failed: " << file.errorString().toStdString() << "\"\n";
            throw MustQuit{};
        }
        file.write(src.toUtf8());
        file.flush();
        if(file.error())
        {
            std::cerr << " failed: " << file.errorString().toStdString() << "\"\n";
            throw MustQuit{};
        }
        std::cerr << "done\n";
    }
    return program;
}

void computeEclipsedDoubleScattering(const unsigned texIndex)
{
    const auto program=saveEclipsedDoubleScatteringComputationShader(texIndex);

    if(opts.dbgNoEDSTextures) return;

    std::cerr << indentOutput() << "Computing eclipsed double scattering... ";
    const auto time0=std::chrono::steady_clock::now();

    using namespace glm;
    using std::acos;

    const unsigned texSizeByViewAzimuth = atmo.eclipsedDoubleScatteringTextureSize[0];
    const unsigned texSizeByViewElevation = atmo.eclipsedDoubleScatteringTextureSize[1];
    const unsigned texSizeBySZA = atmo.eclipsedDoubleScatteringTextureSize[2];
    const unsigned texSizeByAltitude = atmo.eclipsedDoubleScatteringTextureSize[3];

    gl.glBindFramebuffer(GL_FRAMEBUFFER, fbos[FBO_ECLIPSED_DOUBLE_SCATTERING]);
    gl.glFramebufferTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,textures[TEX_ECLIPSED_DOUBLE_SCATTERING],0);
    checkFramebufferStatus("framebuffer for eclipsed double scattering");
    program->bind();
    int unusedTextureUnitNum=0;
    setUniformTexture(*program,GL_TEXTURE_2D,TEX_TRANSMITTANCE,unusedTextureUnitNum++,"transmittanceTexture");

    EclipsedDoubleScatteringPrecomputer precomputer(*program, gl,
                                                    textures[TEX_ECLIPSED_DOUBLE_SCATTERING], unusedTextureUnitNum,
                                                    atmo, texSizeByViewAzimuth, texSizeByViewElevation, texSizeBySZA, texSizeByAltitude);

	gl.glBindVertexArray(vao);
    for(unsigned szaIndex=0; szaIndex<texSizeBySZA; ++szaIndex)
    {
        const double cosSunZenithAngle=unitRangeTexCoordToCosSZA(float(szaIndex)/(texSizeBySZA-1));
        const double sunZenithAngle=acos(cosSunZenithAngle);
        for(unsigned altIndex=0; altIndex<texSizeByAltitude; ++altIndex)
        {
            std::ostringstream ss;
            ss << szaIndex*texSizeByAltitude+altIndex << " of " << texSizeBySZA*texSizeByAltitude << " samples done";
            std::cerr << ss.str();

            // Using the same encoding for altitude as in scatteringTex4DCoordsToTexVars()
            const float distToHorizon = float(altIndex)/(texSizeByAltitude-1)*atmo.lengthOfHorizRayFromGroundToBorderOfAtmo;
            // Rounding errors can result in altitude>max, breaking the code after this calculation, so we have to clamp.
            // To avoid too many zeros that would make log interpolation problematic, we clamp the bottom value at 1 m. The same at the top.
            const float cameraAltitude=clamp(sqrt(sqr(distToHorizon)+sqr(atmo.earthRadius))-atmo.earthRadius, 1.f, atmo.atmosphereHeight-1);

            precomputer.compute(altIndex, szaIndex, cameraAltitude, sunZenithAngle, sunZenithAngle, 0);

            // Clear previous status and reset cursor position
            const auto statusWidth=ss.tellp();
            std::cerr << std::string(statusWidth, '\b') << std::string(statusWidth, ' ')
                      << std::string(statusWidth, '\b');
        }
    }
	gl.glBindVertexArray(0);

    const auto time1=std::chrono::steady_clock::now();
    std::cerr << "done in " << formatDeltaTime(time0, time1) << "\n";

    const auto path=atmo.textureOutputDir+"/eclipsed-double-scattering-wlset"+std::to_string(texIndex)+".f32";
    std::cerr << "Saving eclipsed double scattering texture to \"" << path << "\"... ";
    QFile out(QString::fromStdString(path));
    if(!out.open(QFile::WriteOnly))
    {
        std::cerr << "failed to open file: " << out.errorString().toStdString() << "\n";
        throw MustQuit{};
    }
    for(const uint16_t size : {atmo.eclipsedDoubleScatteringTextureSize[0],atmo.eclipsedDoubleScatteringTextureSize[1],
                               atmo.eclipsedDoubleScatteringTextureSize[2],atmo.eclipsedDoubleScatteringTextureSize[3]})
        out.write(reinterpret_cast<const char*>(&size), sizeof size);
    const auto& texture=precomputer.texture();
    out.write(reinterpret_cast<const char*>(texture.data()), texture.size()*sizeof texture[0]);
    out.close();
    if(out.error())
    {
        std::cerr << "failed to write file: " << out.errorString().toStdString() << "\n";
        throw MustQuit{};
    }
    std::cerr << "done\n";
}

void computeLightPollutionSingleScattering(const unsigned texIndex)
{
    std::cerr << indentOutput() << "Computing light pollution single scattering... ";

    gl.glBindFramebuffer(GL_FRAMEBUFFER,fbos[FBO_LIGHT_POLLUTION]);
    gl.glFramebufferTexture(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0, textures[TEX_LIGHT_POLLUTION],0);
    checkFramebufferStatus("framebuffer for light pollution");

    gl.glViewport(0, 0, atmo.lightPollutionTextureSize[0], atmo.lightPollutionTextureSize[1]);

    const auto src=makeScattererDensityFunctionsSrc();
    virtualSourceFiles[DENSITIES_SHADER_FILENAME]=src;
    const auto program=compileShaderProgram("compute-light-pollution-single-scattering.frag",
                                            "shader program to compute single scattering of light pollution");
    program->bind();
    setUniformTexture(*program,GL_TEXTURE_2D,TEX_TRANSMITTANCE,0,"transmittanceTexture");
    renderQuad();

    gl.glFinish();
    std::cerr << "done\n";

    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);
}

void computeLightPollutionMultipleScattering(const unsigned texIndex)
{
    // TODO: implement
}

int main(int argc, char** argv)
{
    [[maybe_unused]] UTF8Console utf8console;

    qInstallMessageHandler(qtMessageHandler);
    QApplication app(argc, argv);
    app.setApplicationName("CalcMySky");
    app.setApplicationVersion(APP_VERSION);

    try
    {
        handleCmdLine();

        if(opts.saveResultAsRadiance)
            for(auto& scatterer : atmo.scatterers)
                scatterer.phaseFunctionType=PhaseFunctionType::General;

        if(atmo.textureOutputDir.length() && atmo.textureOutputDir.back()=='/')
            atmo.textureOutputDir.pop_back(); // Make the paths a bit nicer (without double slashes)
        for(const auto& scatterer : atmo.scatterers)
        {
            for(unsigned texIndex=0; texIndex<atmo.allWavelengths.size(); ++texIndex)
            {
                createDirs(atmo.textureOutputDir+"/shaders/single-scattering-eclipsed/precomputation/"+
                           std::to_string(texIndex)+"/"+scatterer.name.toStdString());
                createDirs(atmo.textureOutputDir+"/shaders/single-scattering-eclipsed/"+singleScatteringRenderModeNames[SSRM_ON_THE_FLY]+"/"+
                           std::to_string(texIndex)+"/"+scatterer.name.toStdString());
                if(scatterer.phaseFunctionType!=PhaseFunctionType::Smooth)
                {
                    createDirs(atmo.textureOutputDir+"/shaders/single-scattering/"+singleScatteringRenderModeNames[SSRM_ON_THE_FLY]+"/"+
                               std::to_string(texIndex)+"/"+scatterer.name.toStdString());
                }
                if(scatterer.phaseFunctionType==PhaseFunctionType::General)
                {
                    createDirs(atmo.textureOutputDir+"/shaders/single-scattering/"+singleScatteringRenderModeNames[SSRM_PRECOMPUTED]+"/"+
                               std::to_string(texIndex)+"/"+scatterer.name.toStdString());
                    createDirs(atmo.textureOutputDir+"/shaders/single-scattering-eclipsed/"+singleScatteringRenderModeNames[SSRM_PRECOMPUTED]+"/"+
                               std::to_string(texIndex)+"/"+scatterer.name.toStdString());
                }
            }
            if(scatterer.phaseFunctionType==PhaseFunctionType::Achromatic)
            {
                createDirs(atmo.textureOutputDir+"/shaders/single-scattering/"+singleScatteringRenderModeNames[SSRM_PRECOMPUTED]+"/"+
                           scatterer.name.toStdString());
            }
            if(scatterer.phaseFunctionType!=PhaseFunctionType::General)
            {
                createDirs(atmo.textureOutputDir+"/shaders/single-scattering-eclipsed/"+singleScatteringRenderModeNames[SSRM_PRECOMPUTED]+"/"+
                           scatterer.name.toStdString());
            }
        }
        for(unsigned texIndex=0; texIndex<atmo.allWavelengths.size(); ++texIndex)
        {
            createDirs(atmo.textureOutputDir+"/shaders/zero-order-scattering/"+std::to_string(texIndex));
            createDirs(atmo.textureOutputDir+"/shaders/eclipsed-zero-order-scattering/"+std::to_string(texIndex));
            createDirs(atmo.textureOutputDir+"/shaders/double-scattering-eclipsed/precomputed/"+std::to_string(texIndex));
            createDirs(atmo.textureOutputDir+"/shaders/double-scattering-eclipsed/precomputation/"+std::to_string(texIndex));
            createDirs(atmo.textureOutputDir+"/single-scattering/"+std::to_string(texIndex));
        }
        createDirs(atmo.textureOutputDir+"/shaders/multiple-scattering/");
        if(opts.saveResultAsRadiance)
            for(unsigned texIndex=0; texIndex<atmo.allWavelengths.size(); ++texIndex)
                createDirs(atmo.textureOutputDir+"/shaders/multiple-scattering/"+std::to_string(texIndex));

        {
            std::cerr << "Writing parameters to output description file...";
            const auto target=atmo.textureOutputDir+"/params.atmo";
            QFile file(target.c_str());
            if(!file.open(QFile::WriteOnly))
            {
                std::cerr << " FAILED to open \"" << target << "\": " << file.errorString() << "\n";
                throw MustQuit{};
            }
            QTextStream out(&file);
            if(opts.saveResultAsRadiance)
                out << AtmosphereParameters::ALL_TEXTURES_ARE_RADIANCES_DIRECTIVE << "\n";
            if(opts.dbgNoEDSTextures)
                out << AtmosphereParameters::NO_ECLIPSED_DOUBLE_SCATTERING_TEXTURES_DIRECTIVE << "\n";
            out << atmo.descriptionFileText;
            out.flush();
            file.close();
            if(file.error())
            {
                std::cerr << " FAILED to write to \"" << target << "\": " << file.errorString() << "\n";
                throw MustQuit{};
            }
            std::cerr << " done\n";
        }

        QSurfaceFormat format;
        format.setMajorVersion(3);
        format.setMinorVersion(3);
        format.setProfile(QSurfaceFormat::CoreProfile);

        QOpenGLContext context;
        context.setFormat(format);
        context.create();
        if(!context.isValid())
        {
            std::cerr << "Failed to create OpenGL "
                << format.majorVersion() << '.'
                << format.minorVersion() << " context\n";
            return 1;
        }

        QOffscreenSurface surface;
        surface.setFormat(format);
        surface.create();
        if(!surface.isValid())
        {
            std::cerr << "Failed to create OpenGL "
                << format.majorVersion() << '.'
                << format.minorVersion() << " offscreen surface\n";
            return 1;
        }

        context.makeCurrent(&surface);

        if(!gl.initializeOpenGLFunctions())
        {
            std::cerr << "Failed to initialize OpenGL "
                << format.majorVersion() << '.'
                << format.minorVersion() << " functions\n";
            return 1;
        }

        init();

        const auto timeBegin=std::chrono::steady_clock::now();

        for(unsigned texIndex=0;texIndex<atmo.allWavelengths.size();++texIndex)
        {
            std::cerr << "Working on wavelengths " << atmo.allWavelengths[texIndex][0] << ", "
                                                   << atmo.allWavelengths[texIndex][1] << ", "
                                                   << atmo.allWavelengths[texIndex][2] << ", "
                                                   << atmo.allWavelengths[texIndex][3] << " nm"
                         " (set " << texIndex+1 << " of " << atmo.allWavelengths.size() << "):\n";
            OutputIndentIncrease incr;

            initConstHeader(atmo.allWavelengths[texIndex]);
            virtualSourceFiles[COMPUTE_TRANSMITTANCE_SHADER_FILENAME]=
                makeTransmittanceComputeFunctionsSrc(atmo.allWavelengths[texIndex]);
            virtualSourceFiles[PHASE_FUNCTIONS_SHADER_FILENAME]=makePhaseFunctionsSrc();
            virtualSourceFiles[TOTAL_SCATTERING_COEFFICIENT_SHADER_FILENAME]=makeTotalScatteringCoefSrc();
            virtualHeaderFiles[RADIANCE_TO_LUMINANCE_HEADER_FILENAME]="const mat4 radianceToLuminance=" +
                                                                        toString(radianceToLuminance(texIndex)) + ";\n";

            saveZeroOrderScatteringRenderingShader(texIndex);
            saveEclipsedZeroOrderScatteringRenderingShader(texIndex);

            {
                std::cerr << indentOutput() << "Computing parts of scattering order 1:\n";
                OutputIndentIncrease incr;

                computeTransmittance(texIndex);
                // We'll use ground irradiance to take into account the contribution of light scattered by the ground to the
                // sky color. Irradiance will also be needed when we want to draw the ground itself.
                computeDirectGroundIrradiance(texIndex);
            }

            computeLightPollutionSingleScattering(texIndex);
            computeLightPollutionMultipleScattering(texIndex);
            saveTexture(GL_TEXTURE_2D,textures[TEX_LIGHT_POLLUTION],"light pollution texture",
                        atmo.textureOutputDir+"/light-pollution-wlset"+std::to_string(texIndex)+".f32",
                        {atmo.lightPollutionTextureSize[0], atmo.lightPollutionTextureSize[1]});

            computeMultipleScattering(texIndex);
            if(opts.saveResultAsRadiance)
                saveMultipleScatteringRenderingShader(texIndex);

            computeEclipsedDoubleScattering(texIndex);

        }
        if(!opts.saveResultAsRadiance)
            saveMultipleScatteringRenderingShader(-1);

        const auto timeEnd=std::chrono::steady_clock::now();
        std::cerr << "Finished in " << formatDeltaTime(timeBegin, timeEnd) << "\n";
    }
    catch(ParsingError const& ex)
    {
        std::cerr << ex.what() << "\n";
        return 1;
    }
    catch(ShowMySky::Error const& ex)
    {
        std::cerr << QObject::tr("Error: %1\n").arg(ex.what());
        return 1;
    }
    catch(MustQuit& ex)
    {
        return ex.exitCode;
    }
    catch(std::exception const& ex)
    {
        std::cerr << "Fatal error: " << QString::fromLocal8Bit(ex.what()) << '\n';
        return 111;
    }
}

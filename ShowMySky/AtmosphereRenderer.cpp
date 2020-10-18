#include "AtmosphereRenderer.hpp"

#include <set>
#include <cmath>
#include <array>
#include <vector>
#include <cstring>
#include <cassert>
#include <iterator>
#include <iostream>
#include <filesystem>
#include <QFile>
#include <QRegularExpression>

#include "util.hpp"
#include "../common/const.hpp"
#include "../common/util.hpp"
#include "ToolsWidget.hpp"

namespace fs=std::filesystem;

namespace
{

// XXX: keep in sync with those in CalcMySky
GLsizei scatTexWidth(glm::ivec4 sizes) { return sizes[0]; }
GLsizei scatTexHeight(glm::ivec4 sizes) { return sizes[1]*sizes[2]; }
GLsizei scatTexDepth(glm::ivec4 sizes) { return sizes[3]; }

float unitRangeToTexCoord(const float u, const int texSize)
{
    return (0.5+(texSize-1)*u)/texSize;
}

auto newTex(QOpenGLTexture::Target target)
{
    return std::make_unique<QOpenGLTexture>(target);
}

}

void AtmosphereRenderer::makeBayerPatternTexture()
{
    bayerPatternTexture_.setMinificationFilter(QOpenGLTexture::Nearest);
    bayerPatternTexture_.setMagnificationFilter(QOpenGLTexture::Nearest);
    bayerPatternTexture_.setWrapMode(QOpenGLTexture::Repeat);
    bayerPatternTexture_.bind();
	static constexpr float bayerPattern[8*8] =
	{
		// 8x8 Bayer ordered dithering pattern.
		0/64.f, 32/64.f,  8/64.f, 40/64.f,  2/64.f, 34/64.f, 10/64.f, 42/64.f,
		48/64.f, 16/64.f, 56/64.f, 24/64.f, 50/64.f, 18/64.f, 58/64.f, 26/64.f,
		12/64.f, 44/64.f,  4/64.f, 36/64.f, 14/64.f, 46/64.f,  6/64.f, 38/64.f,
		60/64.f, 28/64.f, 52/64.f, 20/64.f, 62/64.f, 30/64.f, 54/64.f, 22/64.f,
		3/64.f, 35/64.f, 11/64.f, 43/64.f,  1/64.f, 33/64.f,  9/64.f, 41/64.f,
		51/64.f, 19/64.f, 59/64.f, 27/64.f, 49/64.f, 17/64.f, 57/64.f, 25/64.f,
		15/64.f, 47/64.f,  7/64.f, 39/64.f, 13/64.f, 45/64.f,  5/64.f, 37/64.f,
		63/64.f, 31/64.f, 55/64.f, 23/64.f, 61/64.f, 29/64.f, 53/64.f, 21/64.f
	};
	gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, 8, 8, 0, GL_RED, GL_FLOAT, bayerPattern);
}

QVector3D AtmosphereRenderer::rgbMaxValue() const
{
    switch(tools_->ditheringMode())
	{
		default:
		case DitheringMode::Disabled:
			return QVector3D(0,0,0);
		case DitheringMode::Color666:
			return QVector3D(63,63,63);
		case DitheringMode::Color565:
			return QVector3D(31,63,31);
		case DitheringMode::Color888:
			return QVector3D(255,255,255);
		case DitheringMode::Color101010:
			return QVector3D(1023,1023,1023);
	}
}

void AtmosphereRenderer::updateAltitudeTexCoords(const float altitudeCoord, double* floorAltIndexOut)
{
    const auto altTexIndex = altitudeCoord==1 ? numAltIntervalsIn4DTexture_-1 : altitudeCoord*numAltIntervalsIn4DTexture_;
    const auto floorAltIndex = std::floor(altTexIndex);
    const auto fractAltIndex = altTexIndex-floorAltIndex;

    staticAltitudeTexCoord_ = unitRangeToTexCoord(fractAltIndex, 2);

    if(floorAltIndexOut) *floorAltIndexOut=floorAltIndex;
}

void AtmosphereRenderer::loadTexture4D(QString const& path, const float altitudeCoord)
{
    if(const auto err=gl.glGetError(); err!=GL_NO_ERROR)
    {
        throw DataLoadError{tr("GL error on entry to loadTexture4D(\"%1\"): %2")
                            .arg(path).arg(openglErrorString(err).c_str())};
    }
    std::cerr << "Loading texture from \"" << path << "\"... ";
    QFile file(path);
    if(!file.open(QFile::ReadOnly))
        throw DataLoadError{tr("Failed to open file \"%1\": %2").arg(path).arg(file.errorString())};

    uint16_t sizes[4];
    {
        const qint64 sizeToRead=sizeof sizes;
        if(file.read(reinterpret_cast<char*>(sizes), sizeToRead) != sizeToRead)
        {
            throw DataLoadError{tr("Failed to read header from file \"%1\": %2")
                                .arg(path).arg(file.errorString())};
        }
    }
    std::cerr << "dimensions from header: " << sizes[0] << "×" << sizes[1] << "×" << sizes[2] << "×" << sizes[3] << "... ";

    if(const qint64 expectedFileSize = sizeof(GLfloat)*4*uint64_t(sizes[0])*sizes[1]*sizes[2]*sizes[3] + file.pos();
       expectedFileSize != file.size())
    {
        throw DataLoadError{tr("Size of file \"%1\" (%2 bytes) doesn't match image dimensions %3×%4×%5×%6 from file header.\nThe expected size is %7 bytes.")
                            .arg(path).arg(file.size()).arg(sizes[0]).arg(sizes[1]).arg(sizes[2]).arg(sizes[3]).arg(expectedFileSize)};
    }

    numAltIntervalsIn4DTexture_ = sizes[3]-1;
    double floorAltIndex;
    updateAltitudeTexCoords(altitudeCoord, &floorAltIndex);

    loadedAltitudeURTexCoordRange_[0] = floorAltIndex/numAltIntervalsIn4DTexture_;
    loadedAltitudeURTexCoordRange_[1] = (floorAltIndex+1)/numAltIntervalsIn4DTexture_;

    const auto subpixelReadOffset = 4*uint64_t(sizes[0])*sizes[1]*sizes[2]*uint64_t(floorAltIndex);
    sizes[3]=2;
    const auto subpixelCountToRead = 4*uint64_t(sizes[0])*sizes[1]*sizes[2]*sizes[3];

    const std::unique_ptr<GLfloat[]> subpixels(new GLfloat[subpixelCountToRead]);
    {
        const qint64 offset=file.pos()+subpixelReadOffset*sizeof subpixels[0];
        std::cerr << "skipping to offset " << offset << "... ";
        if(!file.seek(offset))
        {
            throw DataLoadError{tr("Failed to seek to offset %1 in file \"%2\": %3")
                                .arg(offset).arg(path).arg(file.errorString())};
        }
        const qint64 sizeToRead=subpixelCountToRead*sizeof subpixels[0];
        const auto actuallyRead=file.read(reinterpret_cast<char*>(subpixels.get()), sizeToRead);
        if(actuallyRead != sizeToRead)
        {
            const auto error = actuallyRead==-1 ? tr("Failed to read texture data from file \"%1\": %2").arg(path).arg(file.errorString())
                                                : tr("Failed to read texture data from file \"%1\": requested %2 bytes, read %3").arg(path).arg(sizeToRead).arg(actuallyRead);
            throw DataLoadError{error};
        }
    }
    const glm::ivec4 size(sizes[0],sizes[1],sizes[2],sizes[3]);
    gl.glTexImage3D(GL_TEXTURE_3D,0,GL_RGBA32F,scatTexWidth(size),scatTexHeight(size),scatTexDepth(size),
                    0,GL_RGBA,GL_FLOAT,subpixels.get());
    if(const auto err=gl.glGetError(); err!=GL_NO_ERROR)
    {
        throw DataLoadError{tr("GL error in loadTexture4D(\"%1\") after glTexImage3D() call: %2")
                            .arg(path).arg(openglErrorString(err).c_str())};
    }

    std::cerr << "done\n";
}

glm::ivec2 AtmosphereRenderer::loadTexture2D(QString const& path)
{
    if(const auto err=gl.glGetError(); err!=GL_NO_ERROR)
    {
        throw DataLoadError{tr("GL error on entry to loadTexture2D(\"%1\"): %2")
                            .arg(path).arg(openglErrorString(err).c_str())};
    }
    std::cerr << "Loading texture from \"" << path << "\"... ";
    QFile file(path);
    if(!file.open(QFile::ReadOnly))
        throw DataLoadError{tr("Failed to open file \"%1\": %2").arg(path).arg(file.errorString())};

    uint16_t sizes[2];
    {
        const qint64 sizeToRead=sizeof sizes;
        if(file.read(reinterpret_cast<char*>(sizes), sizeToRead) != sizeToRead)
        {
            throw DataLoadError{tr("Failed to read header from file \"%1\": %2")
                                .arg(path).arg(file.errorString())};
        }
    }
    const auto subpixelCount = 4*uint64_t(sizes[0])*sizes[1];
    std::cerr << "dimensions from header: " << sizes[0] << "×" << sizes[1] << "... ";

    if(const qint64 expectedFileSize = subpixelCount*sizeof(GLfloat)+file.pos();
       expectedFileSize != file.size())
    {
        throw DataLoadError{tr("Size of file \"%1\" (%2 bytes) doesn't match image dimensions %3×%4 from file header.\nThe expected size is %5 bytes.")
                            .arg(path).arg(file.size()).arg(sizes[0]).arg(sizes[1]).arg(expectedFileSize)};
    }

    const std::unique_ptr<GLfloat[]> subpixels(new GLfloat[subpixelCount]);
    {
        const qint64 sizeToRead=subpixelCount*sizeof subpixels[0];
        const auto actuallyRead=file.read(reinterpret_cast<char*>(subpixels.get()), sizeToRead);
        if(actuallyRead != sizeToRead)
        {
            const auto error = actuallyRead==-1 ? tr("Failed to read texture data from file \"%1\": %2").arg(path).arg(file.errorString())
                                                : tr("Failed to read texture data from file \"%1\": requested %2 bytes, read %3").arg(path).arg(sizeToRead).arg(actuallyRead);
            throw DataLoadError{error};
        }
    }
    gl.glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,sizes[0],sizes[1],0,GL_RGBA,GL_FLOAT,subpixels.get());
    if(const auto err=gl.glGetError(); err!=GL_NO_ERROR)
    {
        throw DataLoadError{tr("GL error in loadTexture2D(\"%1\") after glTexImage2D() call: %2")
                            .arg(path).arg(openglErrorString(err).c_str())};
    }
    std::cerr << "done\n";
    return {sizes[0], sizes[1]};
}

void AtmosphereRenderer::loadTextures(const CountStepsOnly countStepsOnly)
{
    while(gl.glGetError()!=GL_NO_ERROR);

    gl.glActiveTexture(GL_TEXTURE0);

    for(unsigned wlSetIndex=0; wlSetIndex<params_.wavelengthSetCount; ++wlSetIndex)
    {
        if(countStepsOnly)
        {
            ++totalLoadingStepsToDo_;
            continue;
        }

        auto& tex=*transmittanceTextures_.emplace_back(newTex(QOpenGLTexture::Target2D));
        tex.setMinificationFilter(QOpenGLTexture::Linear);
        tex.setWrapMode(QOpenGLTexture::ClampToEdge);
        tex.bind();
        loadTexture2D(QString("%1/transmittance-wlset%2.f32").arg(pathToData_).arg(wlSetIndex));
        tick(++loadingStepsDone_);
    }

    for(unsigned wlSetIndex=0; wlSetIndex<params_.wavelengthSetCount; ++wlSetIndex)
    {
        if(countStepsOnly)
        {
            ++totalLoadingStepsToDo_;
            continue;
        }

        auto& tex=*irradianceTextures_.emplace_back(newTex(QOpenGLTexture::Target2D));
        tex.setMinificationFilter(QOpenGLTexture::Linear);
        tex.setWrapMode(QOpenGLTexture::ClampToEdge);
        tex.bind();
        loadTexture2D(QString("%1/irradiance-wlset%2.f32").arg(pathToData_).arg(wlSetIndex));
        tick(++loadingStepsDone_);
    }

    reloadScatteringTextures(countStepsOnly);

    makeBayerPatternTexture();

    assert(gl.glGetError()==GL_NO_ERROR);
}

double AtmosphereRenderer::altitudeUnitRangeTexCoord() const
{
    const auto h = tools_->altitude();
    const auto H = params_.atmosphereHeight;
    const auto R = params_.earthRadius;
    return std::sqrt(h*(h+2*R) / ( H*(H+2*R) ));
}

void AtmosphereRenderer::reloadScatteringTextures(const CountStepsOnly countStepsOnly)
{
    const auto texFilter = tools_->textureFilteringEnabled() ? QOpenGLTexture::Linear : QOpenGLTexture::Nearest;
    const auto altCoord = altitudeUnitRangeTexCoord();

    multipleScatteringTextures_.clear();
    if(const auto filename=pathToData_+"/multiple-scattering-xyzw.f32"; QFile::exists(filename))
    {
        if(countStepsOnly)
        {
            ++totalLoadingStepsToDo_;
        }
        else
        {
            auto& tex=*multipleScatteringTextures_.emplace_back(newTex(QOpenGLTexture::Target3D));
            tex.setMinificationFilter(texFilter);
            tex.setMagnificationFilter(texFilter);
            tex.setWrapMode(QOpenGLTexture::ClampToEdge);
            tex.bind();
            loadTexture4D(filename, altCoord);
            tick(++loadingStepsDone_);
        }
    }
    else
    {
        for(unsigned wlSetIndex=0; wlSetIndex<params_.wavelengthSetCount; ++wlSetIndex)
        {
            if(countStepsOnly)
            {
                ++totalLoadingStepsToDo_;
                continue;
            }

            auto& tex=*multipleScatteringTextures_.emplace_back(newTex(QOpenGLTexture::Target3D));
            tex.setMinificationFilter(texFilter);
            tex.setMagnificationFilter(texFilter);
            tex.setWrapMode(QOpenGLTexture::ClampToEdge);
            tex.bind();
            loadTexture4D(QString("%1/multiple-scattering-wlset%2.f32").arg(pathToData_).arg(wlSetIndex), altCoord);
            tick(++loadingStepsDone_);
        }
    }

    singleScatteringTextures_.clear();
    for(const auto& [scattererName, phaseFuncType] : params_.scatterers)
    {
        auto& texturesPerWLSet=singleScatteringTextures_[scattererName];
        switch(phaseFuncType)
        {
        case PhaseFunctionType::General:
            for(unsigned wlSetIndex=0; wlSetIndex<params_.wavelengthSetCount; ++wlSetIndex)
            {
                if(countStepsOnly)
                {
                    ++totalLoadingStepsToDo_;
                    continue;
                }

                auto& texture=*texturesPerWLSet.emplace_back(newTex(QOpenGLTexture::Target3D));
                texture.setMinificationFilter(texFilter);
                texture.setMagnificationFilter(texFilter);
                texture.setWrapMode(QOpenGLTexture::ClampToEdge);
                texture.bind();
                loadTexture4D(QString("%1/single-scattering/%2/%3.f32").arg(pathToData_).arg(wlSetIndex).arg(scattererName), altCoord);
                tick(++loadingStepsDone_);
            }
            break;
        case PhaseFunctionType::Achromatic:
        {
            if(countStepsOnly)
            {
                ++totalLoadingStepsToDo_;
                continue;
            }

            auto& texture=*texturesPerWLSet.emplace_back(newTex(QOpenGLTexture::Target3D));
            texture.setMinificationFilter(texFilter);
            texture.setMagnificationFilter(texFilter);
            texture.setWrapMode(QOpenGLTexture::ClampToEdge);
            texture.bind();
            loadTexture4D(QString("%1/single-scattering/%2-xyzw.f32").arg(pathToData_).arg(scattererName), altCoord);
            tick(++loadingStepsDone_);
            break;
        }
        case PhaseFunctionType::Smooth:
            break;
        }
    }
}

void AtmosphereRenderer::loadShaders(const CountStepsOnly countStepsOnly)
{
    constexpr char commonVertexShaderSrc[]=R"(
#version 330
in vec3 vertex;
out vec3 position;
void main()
{
    position=vertex;
    gl_Position=vec4(position,1);
}
)";
    const QByteArray viewDirShaderSrc=1+R"(
#version 330
in vec3 position;
uniform float zoomFactor;
const float PI=3.1415926535897932;
vec3 calcViewDir()
{
    vec2 pos=position.xy/zoomFactor;
    return vec3(cos(pos.x*PI)*cos(pos.y*(PI/2)),
                sin(pos.x*PI)*cos(pos.y*(PI/2)),
                sin(pos.y*(PI/2)));
})";

    singleScatteringPrograms_.clear();
    for(int renderMode=0; renderMode<SSRM_COUNT; ++renderMode)
    {
        auto& programsPerScatterer=*singleScatteringPrograms_.emplace_back(std::make_unique<std::map<QString,std::vector<ShaderProgPtr>>>());

        for(const auto& [scattererName,phaseFuncType] : params_.scatterers)
        {
            auto& programs=programsPerScatterer[scattererName];
            if(phaseFuncType==PhaseFunctionType::General || (phaseFuncType!=PhaseFunctionType::Smooth && renderMode==SSRM_ON_THE_FLY))
            {
                for(unsigned wlSetIndex=0; wlSetIndex<params_.wavelengthSetCount; ++wlSetIndex)
                {
                    if(countStepsOnly)
                    {
                        ++totalLoadingStepsToDo_;
                        continue;
                    }

                    const auto scatDir=QString("%1/shaders/single-scattering/%2/%3/%4").arg(pathToData_)
                                                                                       .arg(singleScatteringRenderModeNames[renderMode])
                                                                                       .arg(wlSetIndex)
                                                                                       .arg(scattererName);
                    std::cerr << "Loading shaders from " << scatDir.toStdString() << "...\n";
                    auto& program=*programs.emplace_back(std::make_unique<QOpenGLShaderProgram>());

                    for(const auto& shaderFile : fs::directory_iterator(fs::u8path(scatDir.toStdString())))
                        addShaderFile(program,QOpenGLShader::Fragment,shaderFile.path());

                    addShaderCode(program,QOpenGLShader::Fragment,"viewDir function shader",viewDirShaderSrc);

                    addShaderCode(program, QOpenGLShader::Vertex, tr("vertex shader for scatterer \"%1\"").arg(scattererName),
                                  commonVertexShaderSrc);

                    link(program, tr("shader program for scatterer \"%1\"").arg(scattererName));
                    tick(++loadingStepsDone_);
                }
            }
            else if(phaseFuncType==PhaseFunctionType::Achromatic)
            {
                if(countStepsOnly)
                {
                    ++totalLoadingStepsToDo_;
                    continue;
                }

                const auto scatDir=QString("%1/shaders/single-scattering/%2/%3").arg(pathToData_)
                                                                                .arg(singleScatteringRenderModeNames[renderMode])
                                                                                .arg(scattererName);
                std::cerr << "Loading shaders from " << scatDir.toStdString() << "...\n";
                auto& program=*programs.emplace_back(std::make_unique<QOpenGLShaderProgram>());
                for(const auto& shaderFile : fs::directory_iterator(fs::u8path(scatDir.toStdString())))
                    addShaderFile(program,QOpenGLShader::Fragment,shaderFile.path());

                addShaderCode(program,QOpenGLShader::Fragment,"viewDir function shader",viewDirShaderSrc);

                addShaderCode(program, QOpenGLShader::Vertex, tr("vertex shader for scatterer \"%1\"").arg(scattererName),
                              commonVertexShaderSrc);

                link(program, tr("shader program for scatterer \"%1\"").arg(scattererName));
                tick(++loadingStepsDone_);
            }
        }
    }

    eclipsedSingleScatteringPrograms_.clear();
    for(int renderMode=SSRM_ON_THE_FLY; renderMode<SSRM_COUNT; ++renderMode)
    {
        auto& programsPerScatterer=*eclipsedSingleScatteringPrograms_.emplace_back(std::make_unique<ScatteringProgramsMap>());

        for(const auto& [scattererName,phaseFuncType] : params_.scatterers)
        {
            auto& programs=programsPerScatterer[scattererName];
            if(phaseFuncType==PhaseFunctionType::General || renderMode==SSRM_ON_THE_FLY)
            {
                for(unsigned wlSetIndex=0; wlSetIndex<params_.wavelengthSetCount; ++wlSetIndex)
                {
                    if(countStepsOnly)
                    {
                        ++totalLoadingStepsToDo_;
                        continue;
                    }

                    const auto scatDir=QString("%1/shaders/single-scattering-eclipsed/%2/%3/%4").arg(pathToData_)
                                                                                                .arg(singleScatteringRenderModeNames[renderMode])
                                                                                                .arg(wlSetIndex)
                                                                                                .arg(scattererName);
                    std::cerr << "Loading shaders from " << scatDir.toStdString() << "...\n";
                    auto& program=*programs.emplace_back(std::make_unique<QOpenGLShaderProgram>());

                    for(const auto& shaderFile : fs::directory_iterator(fs::u8path(scatDir.toStdString())))
                        addShaderFile(program,QOpenGLShader::Fragment,shaderFile.path());

                    addShaderCode(program,QOpenGLShader::Fragment,"viewDir function shader",viewDirShaderSrc);

                    addShaderCode(program, QOpenGLShader::Vertex, tr("vertex shader for scatterer \"%1\"").arg(scattererName),
                                  commonVertexShaderSrc);

                    link(program, tr("shader program for scatterer \"%1\"").arg(scattererName));
                    tick(++loadingStepsDone_);
                }
            }
            else
            {
                if(countStepsOnly)
                {
                    ++totalLoadingStepsToDo_;
                    continue;
                }

                const auto scatDir=QString("%1/shaders/single-scattering-eclipsed/%2/%3").arg(pathToData_)
                                                                                            .arg(singleScatteringRenderModeNames[renderMode])
                                                                                            .arg(scattererName);
                std::cerr << "Loading shaders from " << scatDir.toStdString() << "...\n";
                auto& program=*programs.emplace_back(std::make_unique<QOpenGLShaderProgram>());

                for(const auto& shaderFile : fs::directory_iterator(fs::u8path(scatDir.toStdString())))
                    addShaderFile(program,QOpenGLShader::Fragment,shaderFile.path());

                addShaderCode(program,QOpenGLShader::Fragment,"viewDir function shader",viewDirShaderSrc);

                addShaderCode(program, QOpenGLShader::Vertex, tr("vertex shader for scatterer \"%1\"").arg(scattererName),
                              commonVertexShaderSrc);

                link(program, tr("shader program for scatterer \"%1\"").arg(scattererName));
                tick(++loadingStepsDone_);
            }
        }
    }

    eclipsedSingleScatteringPrecomputationPrograms_=std::make_unique<ScatteringProgramsMap>();
    for(const auto& [scattererName,phaseFuncType] : params_.scatterers)
    {
        auto& programs=(*eclipsedSingleScatteringPrecomputationPrograms_)[scattererName];
        for(unsigned wlSetIndex=0; wlSetIndex<params_.wavelengthSetCount; ++wlSetIndex)
        {
            if(countStepsOnly)
            {
                ++totalLoadingStepsToDo_;
                continue;
            }

            const auto scatDir=QString("%1/shaders/single-scattering-eclipsed/precomputation/%3/%4").arg(pathToData_)
                                                                                                    .arg(wlSetIndex)
                                                                                                    .arg(scattererName);
            std::cerr << "Loading shaders from " << scatDir.toStdString() << "...\n";
            auto& program=*programs.emplace_back(std::make_unique<QOpenGLShaderProgram>());

            for(const auto& shaderFile : fs::directory_iterator(fs::u8path(scatDir.toStdString())))
                addShaderFile(program,QOpenGLShader::Fragment,shaderFile.path());

            addShaderCode(program, QOpenGLShader::Vertex, tr("vertex shader for scatterer \"%1\"").arg(scattererName),
                          commonVertexShaderSrc);

            link(program, tr("shader program for scatterer \"%1\"").arg(scattererName));
            tick(++loadingStepsDone_);
        }
    }

    if(countStepsOnly)
    {
        ++totalLoadingStepsToDo_;
    }
    else
    {
        luminanceToScreenRGB_=std::make_unique<QOpenGLShaderProgram>();
        addShaderCode(*luminanceToScreenRGB_, QOpenGLShader::Fragment, tr("luminanceToScreenRGB fragment shader"), 1+R"(
#version 330
uniform float exposure;
uniform sampler2D luminanceXYZW;
in vec2 texCoord;
out vec4 color;

uniform vec3 rgbMaxValue;
uniform sampler2D bayerPattern;

const vec3 gamma=vec3(2.2);
vec3 toLinearRGB(vec3 sRGB)
{
    return pow(sRGB,gamma);
}
vec3 toSRGB(vec3 linearRGB)
{
    return pow(linearRGB,1/gamma);
}

vec3 dither(const vec3 inColor)
{
    if(rgbMaxValue.r==0.) return inColor;
    vec3 bayer=texture2D(bayerPattern,gl_FragCoord.xy/8.).rrr;

    vec3 lowerSRGB = floor(inColor*rgbMaxValue);
    vec3 upperSRGB = ceil (inColor*rgbMaxValue);
    upperSRGB += 1-(upperSRGB-lowerSRGB); // Make sure we don't divide by 0 for exact sRGB values of inColor

    vec3 lower = toLinearRGB(lowerSRGB/rgbMaxValue);
    vec3 upper = toLinearRGB(upperSRGB/rgbMaxValue);

    vec3 delta = upper-lower; // Linear-RGB difference between nearest representable colors
    vec3 extra = toLinearRGB(inColor) - lower; // Extra (linear-RGB) intensity to add to lower to get inColor

    return toSRGB(lower + delta*step(bayer, extra/delta));
}

void main()
{
    const mat3 XYZ2sRGBl=mat3(vec3(3.2406,-0.9689,0.0557),
                              vec3(-1.5372,1.8758,-0.204),
                              vec3(-0.4986,0.0415,1.057));
    vec3 XYZ=texture(luminanceXYZW, texCoord).xyz;
    vec3 rgb=XYZ2sRGBl*XYZ;
    vec3 srgb=toSRGB(rgb*exposure);
    color=vec4(dither(srgb),1);
}
)");
        addShaderCode(*luminanceToScreenRGB_, QOpenGLShader::Vertex, tr("luminanceToScreenRGB vertex shader"), 1+R"(
#version 330
in vec3 vertex;
out vec2 texCoord;
void main()
{
    texCoord=(vertex.xy+vec2(1))/2;
    gl_Position=vec4(vertex,1);
}
)");
        link(*luminanceToScreenRGB_, tr("luminanceToScreenRGB shader program"));
        tick(++loadingStepsDone_);
    }

    multipleScatteringPrograms_.clear();
    if(QFile::exists(pathToData_+"/shaders/multiple-scattering/0/"))
    {
        for(unsigned wlSetIndex=0; wlSetIndex<params_.wavelengthSetCount; ++wlSetIndex)
        {
            if(countStepsOnly)
            {
                ++totalLoadingStepsToDo_;
                continue;
            }

            auto& program=*multipleScatteringPrograms_.emplace_back(std::make_unique<QOpenGLShaderProgram>());
            const auto wlDir=QString("%1/shaders/multiple-scattering/%2").arg(pathToData_).arg(wlSetIndex);
            std::cerr << "Loading shaders from " << wlDir.toStdString() << "...\n";
            for(const auto& shaderFile : fs::directory_iterator(fs::u8path(wlDir.toStdString())))
                addShaderFile(program, QOpenGLShader::Fragment, shaderFile.path());
            addShaderCode(program,QOpenGLShader::Fragment,"viewDir function shader",viewDirShaderSrc);
            addShaderCode(program, QOpenGLShader::Vertex, tr("vertex shader for multiple scattering"), commonVertexShaderSrc);
            link(program, tr("multiple scattering shader program"));
            tick(++loadingStepsDone_);
        }
    }
    else
    {
        if(countStepsOnly)
        {
            ++totalLoadingStepsToDo_;
        }
        else
        {
            auto& program=*multipleScatteringPrograms_.emplace_back(std::make_unique<QOpenGLShaderProgram>());
            const auto wlDir=pathToData_+"/shaders/multiple-scattering/";
            std::cerr << "Loading shaders from " << wlDir.toStdString() << "...\n";
            for(const auto& shaderFile : fs::directory_iterator(fs::u8path(wlDir.toStdString())))
                addShaderFile(program, QOpenGLShader::Fragment, shaderFile.path());
            addShaderCode(program,QOpenGLShader::Fragment,"viewDir function shader",viewDirShaderSrc);
            addShaderCode(program, QOpenGLShader::Vertex, tr("vertex shader for multiple scattering"), commonVertexShaderSrc);
            link(program, tr("multiple scattering shader program"));
            tick(++loadingStepsDone_);
        }
    }

    zeroOrderScatteringPrograms_.clear();
    for(unsigned wlSetIndex=0; wlSetIndex<params_.wavelengthSetCount; ++wlSetIndex)
    {
        if(countStepsOnly)
        {
            ++totalLoadingStepsToDo_;
            continue;
        }

        auto& program=*zeroOrderScatteringPrograms_.emplace_back(std::make_unique<QOpenGLShaderProgram>());
        const auto wlDir=QString("%1/shaders/zero-order-scattering/%2").arg(pathToData_).arg(wlSetIndex);
        std::cerr << "Loading shaders from " << wlDir.toStdString() << "...\n";
        for(const auto& shaderFile : fs::directory_iterator(fs::u8path(wlDir.toStdString())))
            addShaderFile(program, QOpenGLShader::Fragment, shaderFile.path());
        addShaderCode(program,QOpenGLShader::Fragment,"viewDir function shader",viewDirShaderSrc);
        addShaderCode(program, QOpenGLShader::Vertex, tr("vertex shader for zero-order scattering"), commonVertexShaderSrc);
        link(program, tr("zero-order scattering shader program"));
        tick(++loadingStepsDone_);
    }

    if(countStepsOnly)
    {
        ++totalLoadingStepsToDo_;
    }
    else
    {
        viewDirectionGetterProgram_=std::make_unique<QOpenGLShaderProgram>();
        auto& program=*viewDirectionGetterProgram_;
        addShaderCode(program, QOpenGLShader::Vertex, tr("vertex shader for view direction getter"), commonVertexShaderSrc);
        addShaderCode(program, QOpenGLShader::Fragment, tr("viewDir function shader"), viewDirShaderSrc);
        addShaderCode(program, QOpenGLShader::Fragment, tr("fragment shader for view direction getter"), 1+R"(
#version 330

in vec3 position;
out vec3 viewDir;

vec3 calcViewDir();
void main()
{
    viewDir=calcViewDir();
}
)");
        link(program, tr("view direction getter shader program"));
        tick(++loadingStepsDone_);
    }
}

void AtmosphereRenderer::setupBuffers()
{
    gl.glGenVertexArrays(1, &vao_);
    gl.glBindVertexArray(vao_);
    gl.glGenBuffers(1, &vbo_);
    gl.glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    const GLfloat vertices[]=
    {
        -1, -1,
         1, -1,
        -1,  1,
         1,  1,
    };
    gl.glBufferData(GL_ARRAY_BUFFER, sizeof vertices, vertices, GL_STATIC_DRAW);
    constexpr GLuint attribIndex=0;
    constexpr int coordsPerVertex=2;
    gl.glVertexAttribPointer(attribIndex, coordsPerVertex, GL_FLOAT, false, 0, 0);
    gl.glEnableVertexAttribArray(attribIndex);
    gl.glBindVertexArray(0);
}

glm::dvec3 AtmosphereRenderer::cameraPosition() const
{
    return glm::dvec3(0,0,tools_->altitude());
}

glm::dvec3 AtmosphereRenderer::sunDirection() const
{
    return glm::dvec3(std::cos(tools_->sunAzimuth())*std::sin(tools_->sunZenithAngle()),
                      std::sin(tools_->sunAzimuth())*std::sin(tools_->sunZenithAngle()),
                      std::cos(tools_->sunZenithAngle()));
}

glm::dvec3 AtmosphereRenderer::moonPosition() const
{
    const auto moonDir=glm::dvec3(std::cos(tools_->moonAzimuth())*std::sin(tools_->moonZenithAngle()),
                                  std::sin(tools_->moonAzimuth())*std::sin(tools_->moonZenithAngle()),
                                  std::cos(tools_->moonZenithAngle()));
    return cameraPosition()+moonDir*cameraMoonDistance();
}

glm::dvec3 AtmosphereRenderer::moonPositionRelativeToSunAzimuth() const
{
    const auto moonDir=glm::dvec3(std::cos(tools_->moonAzimuth() - tools_->sunAzimuth())*std::sin(tools_->moonZenithAngle()),
                                  std::sin(tools_->moonAzimuth() - tools_->sunAzimuth())*std::sin(tools_->moonZenithAngle()),
                                  std::cos(tools_->moonZenithAngle()));
    return cameraPosition()+moonDir*cameraMoonDistance();
}

double AtmosphereRenderer::cameraMoonDistance() const
{
    using namespace std;
    const auto hpR=tools_->altitude()+params_.earthRadius;
    const auto moonElevation=M_PI/2-tools_->moonZenithAngle();
    return -hpR*sin(moonElevation)+sqrt(sqr(params_.earthMoonDistance)-0.5*sqr(hpR)*(1+cos(2*moonElevation)));
}

double AtmosphereRenderer::moonAngularRadius() const
{
    return moonRadius/cameraMoonDistance();
}

auto AtmosphereRenderer::getPixelSpectralRadiance(QPoint const& pixelPos) const -> SpectralRadiance
{
    if(radianceRenderBuffers_.empty()) return {};
    if(pixelPos.x()<0 || pixelPos.y()<0 || pixelPos.x()>=viewportSize_.width() || pixelPos.y()>=viewportSize_.height())
        return {};

    constexpr auto wavelengthsPerPixel=4;
    SpectralRadiance output;
    for(const auto wl : params_.wavelengths)
        output.wavelengths.emplace_back(wl);
    gl.glBindFramebuffer(GL_FRAMEBUFFER, mainFBO_);
    gl.glReadBuffer(GL_COLOR_ATTACHMENT1);
    for(unsigned wlSetIndex=0; wlSetIndex<params_.wavelengthSetCount; ++wlSetIndex)
    {
        gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_RENDERBUFFER, radianceRenderBuffers_[wlSetIndex]);
        GLfloat data[wavelengthsPerPixel]={NAN,NAN,NAN,NAN};
        gl.glReadPixels(pixelPos.x(), viewportSize_.height()-pixelPos.y()-1, 1,1, GL_RGBA, GL_FLOAT, data);
        for(unsigned i=0; i<wavelengthsPerPixel; ++i)
            output.radiances.emplace_back(data[i]);
    }
    assert(output.wavelengths.size()==output.radiances.size());

    const auto dir=getViewDirection(pixelPos);
    output.azimuth=dir.azimuth;
    output.elevation=dir.elevation;

    return output;
}

auto AtmosphereRenderer::getViewDirection(QPoint const& pixelPos) const -> Direction
{
    gl.glBindVertexArray(vao_);
    viewDirectionGetterProgram_->bind();
    viewDirectionGetterProgram_->setUniformValue("zoomFactor", tools_->zoomFactor());
    gl.glBindFramebuffer(GL_FRAMEBUFFER, viewDirectionFBO_);
    gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    gl.glBindVertexArray(0);
    GLfloat viewDir[3]={NAN,NAN,NAN};
    gl.glReadPixels(pixelPos.x(), viewportSize_.height()-pixelPos.y()-1, 1,1, GL_RGB, GL_FLOAT, viewDir);

    const float azimuth = 180/M_PI * (viewDir[0]!=0 || viewDir[1]!=0 ? std::atan2(viewDir[1], viewDir[0]) : 0);
    const float elevation = 180/M_PI * std::asin(viewDir[2]);

    return Direction{azimuth, elevation};
}

void AtmosphereRenderer::clearRadianceFrames()
{
    if(radianceRenderBuffers_.empty()) return;

    for(unsigned wlSetIndex=0; wlSetIndex<params_.wavelengthSetCount; ++wlSetIndex)
    {
        gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_RENDERBUFFER, radianceRenderBuffers_[wlSetIndex]);
        gl.glDrawBuffers(2, std::array<GLenum,2>{GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1}.data());
        gl.glClearBufferfv(GL_COLOR, 1, std::array<GLfloat,4>{0,0,0,0}.data());
    }
}

bool AtmosphereRenderer::canGrabRadiance() const
{
    const auto& sst=singleScatteringTextures_;
    const bool haveNoLuminanceOnlySingleScatteringTextures = std::find_if(sst.begin(), sst.end(), [=](auto const& texSet)
                                                                          { return texSet.second.size()==1; }) == sst.end();
    return haveNoLuminanceOnlySingleScatteringTextures && multipleScatteringTextures_.size()==params_.wavelengthSetCount;
}

void AtmosphereRenderer::renderZeroOrderScattering()
{
    for(unsigned wlSetIndex=0; wlSetIndex<params_.wavelengthSetCount; ++wlSetIndex)
    {
        if(!radianceRenderBuffers_.empty())
            gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_RENDERBUFFER, radianceRenderBuffers_[wlSetIndex]);
        auto& prog=*zeroOrderScatteringPrograms_[wlSetIndex];
        prog.bind();
        prog.setUniformValue("cameraPosition", toQVector(cameraPosition()));
        prog.setUniformValue("zoomFactor", tools_->zoomFactor());
        prog.setUniformValue("sunDirection", toQVector(sunDirection()));
        transmittanceTextures_[wlSetIndex]->bind(0);
        prog.setUniformValue("transmittanceTexture", 0);
        irradianceTextures_[wlSetIndex]->bind(1);
        prog.setUniformValue("irradianceTexture",1);

        gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
}


void AtmosphereRenderer::precomputeEclipsedSingleScattering()
{
    // TODO: avoid redoing it if Sun elevation and Moon elevation and relative azimuth haven't changed
    for(const auto& [scattererName, phaseFuncType] : params_.scatterers)
    {
        auto& textures=eclipsedSingleScatteringPrecomputationTextures_[scattererName];
        const auto& programs=eclipsedSingleScatteringPrecomputationPrograms_->at(scattererName);
        gl.glDisablei(GL_BLEND, 0); // First wavelength set overwrites old contents, regardless of subsequent blending modes
        const bool needBlending = phaseFuncType==PhaseFunctionType::Achromatic || phaseFuncType==PhaseFunctionType::Smooth;
        for(unsigned wlSetIndex=0; wlSetIndex<params_.wavelengthSetCount; ++wlSetIndex)
        {
            auto& prog=*programs[wlSetIndex];
            prog.bind();
            prog.setUniformValue("altitude", float(tools_->altitude()));
            prog.setUniformValue("moonAngularRadius", float(moonAngularRadius()));
            prog.setUniformValue("moonPositionRelativeToSunAzimuth", toQVector(moonPositionRelativeToSunAzimuth()));
            prog.setUniformValue("sunZenithAngle", float(tools_->sunZenithAngle()));
            transmittanceTextures_[wlSetIndex]->bind(0);
            prog.setUniformValue("transmittanceTexture", 0);

            auto& tex = needBlending ? *textures.front() : *textures[wlSetIndex];
            gl.glBindFramebuffer(GL_FRAMEBUFFER, eclipseSingleScatteringPrecomputationFBO_);
            gl.glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex.textureId(),0);
            checkFramebufferStatus(gl, "Eclipsed single scattering precomputation FBO");
            gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            if(needBlending)
                gl.glEnablei(GL_BLEND, 0);
        }
    }
    gl.glBindFramebuffer(GL_FRAMEBUFFER,mainFBO_);
    gl.glEnablei(GL_BLEND, 0);
}

void AtmosphereRenderer::renderSingleScattering()
{
    if(tools_->usingEclipseShader())
        precomputeEclipsedSingleScattering();

    const auto renderMode = tools_->onTheFlySingleScatteringEnabled() ? SSRM_ON_THE_FLY : SSRM_PRECOMPUTED;
    for(const auto& [scattererName,phaseFuncType] : params_.scatterers)
    {
        if(!scatterersEnabledStates_.at(scattererName))
            continue;

        if(renderMode==SSRM_ON_THE_FLY)
        {
            if(tools_->usingEclipseShader())
            {
                for(unsigned wlSetIndex=0; wlSetIndex<params_.wavelengthSetCount; ++wlSetIndex)
                {
                    if(!radianceRenderBuffers_.empty())
                        gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_RENDERBUFFER, radianceRenderBuffers_[wlSetIndex]);

                    auto& prog=*eclipsedSingleScatteringPrograms_[renderMode]->at(scattererName)[wlSetIndex];
                    prog.bind();
                    prog.setUniformValue("cameraPosition", toQVector(cameraPosition()));
                    prog.setUniformValue("moonAngularRadius", float(moonAngularRadius()));
                    prog.setUniformValue("moonPosition", toQVector(moonPosition()));
                    prog.setUniformValue("zoomFactor", tools_->zoomFactor());
                    prog.setUniformValue("sunDirection", toQVector(sunDirection()));
                    transmittanceTextures_[wlSetIndex]->bind(0);
                    prog.setUniformValue("transmittanceTexture", 0);

                    gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                }
            }
            else
            {
                if(phaseFuncType==PhaseFunctionType::Smooth)
                    continue;

                for(unsigned wlSetIndex=0; wlSetIndex<params_.wavelengthSetCount; ++wlSetIndex)
                {
                    if(!radianceRenderBuffers_.empty())
                        gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_RENDERBUFFER, radianceRenderBuffers_[wlSetIndex]);

                    auto& prog=*singleScatteringPrograms_[renderMode]->at(scattererName)[wlSetIndex];
                    prog.bind();
                    prog.setUniformValue("cameraPosition", toQVector(cameraPosition()));
                    prog.setUniformValue("zoomFactor", tools_->zoomFactor());
                    prog.setUniformValue("sunDirection", toQVector(sunDirection()));
                    transmittanceTextures_[wlSetIndex]->bind(0);
                    prog.setUniformValue("transmittanceTexture", 0);

                    gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                }
            }
        }
        else if(phaseFuncType==PhaseFunctionType::General)
        {
            if(tools_->usingEclipseShader())
            {
                for(unsigned wlSetIndex=0; wlSetIndex<params_.wavelengthSetCount; ++wlSetIndex)
                {
                    if(!radianceRenderBuffers_.empty())
                        gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_RENDERBUFFER, radianceRenderBuffers_[wlSetIndex]);

                    auto& prog=*eclipsedSingleScatteringPrograms_[renderMode]->at(scattererName)[wlSetIndex];
                    prog.bind();
                    prog.setUniformValue("cameraPosition", toQVector(cameraPosition()));
                    prog.setUniformValue("zoomFactor", tools_->zoomFactor());
                    prog.setUniformValue("sunDirection", toQVector(sunDirection()));
                    {
                        auto& tex=*eclipsedSingleScatteringPrecomputationTextures_.at(scattererName)[wlSetIndex];
                        const auto texFilter = tools_->textureFilteringEnabled() ? QOpenGLTexture::Linear : QOpenGLTexture::Nearest;
                        tex.setMinificationFilter(texFilter);
                        tex.setMagnificationFilter(texFilter);
                        tex.bind(0);
                        prog.setUniformValue("eclipsedScatteringTexture", 0);
                    }

                    gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                }
            }
            else
            {
                for(unsigned wlSetIndex=0; wlSetIndex<params_.wavelengthSetCount; ++wlSetIndex)
                {
                    if(!radianceRenderBuffers_.empty())
                        gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_RENDERBUFFER, radianceRenderBuffers_[wlSetIndex]);

                    auto& prog=*singleScatteringPrograms_[renderMode]->at(scattererName)[wlSetIndex];
                    prog.bind();
                    prog.setUniformValue("cameraPosition", toQVector(cameraPosition()));
                    prog.setUniformValue("zoomFactor", tools_->zoomFactor());
                    prog.setUniformValue("sunDirection", toQVector(sunDirection()));
                    {
                        auto& tex=*singleScatteringTextures_.at(scattererName)[wlSetIndex];
                        const auto texFilter = tools_->textureFilteringEnabled() ? QOpenGLTexture::Linear : QOpenGLTexture::Nearest;
                        tex.setMinificationFilter(texFilter);
                        tex.setMagnificationFilter(texFilter);
                        tex.bind(0);
                        prog.setUniformValue("scatteringTexture", 0);
                        prog.setUniformValue("staticAltitudeTexCoord", staticAltitudeTexCoord_);
                    }

                    gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                }
            }
        }
        else if(phaseFuncType==PhaseFunctionType::Achromatic && !tools_->usingEclipseShader())
        {
            auto& prog=*singleScatteringPrograms_[renderMode]->at(scattererName).front();
            prog.bind();
            prog.setUniformValue("cameraPosition", toQVector(cameraPosition()));
            prog.setUniformValue("zoomFactor", tools_->zoomFactor());
            prog.setUniformValue("sunDirection", toQVector(sunDirection()));
            {
                auto& tex=*singleScatteringTextures_.at(scattererName).front();
                const auto texFilter = tools_->textureFilteringEnabled() ? QOpenGLTexture::Linear : QOpenGLTexture::Nearest;
                tex.setMinificationFilter(texFilter);
                tex.setMagnificationFilter(texFilter);
                tex.bind(0);
            }
            prog.setUniformValue("scatteringTexture", 0);
            prog.setUniformValue("staticAltitudeTexCoord", staticAltitudeTexCoord_);

            gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        else if(tools_->usingEclipseShader())
        {
            auto& prog=*eclipsedSingleScatteringPrograms_[renderMode]->at(scattererName).front();
            prog.bind();
            prog.setUniformValue("cameraPosition", toQVector(cameraPosition()));
            prog.setUniformValue("zoomFactor", tools_->zoomFactor());
            prog.setUniformValue("sunDirection", toQVector(sunDirection()));
            {
                auto& tex=*eclipsedSingleScatteringPrecomputationTextures_.at(scattererName).front();
                const auto texFilter = tools_->textureFilteringEnabled() ? QOpenGLTexture::Linear : QOpenGLTexture::Nearest;
                tex.setMinificationFilter(texFilter);
                tex.setMagnificationFilter(texFilter);
                tex.bind(0);
                prog.setUniformValue("eclipsedScatteringTexture", 0);
            }

            gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    }
}

void AtmosphereRenderer::renderMultipleScattering()
{
    const auto texFilter = tools_->textureFilteringEnabled() ? QOpenGLTexture::Linear : QOpenGLTexture::Nearest;
    if(multipleScatteringTextures_.size()==1)
    {
        auto& prog=*multipleScatteringPrograms_.front();
        prog.bind();
        prog.setUniformValue("cameraPosition", toQVector(cameraPosition()));
        prog.setUniformValue("zoomFactor", tools_->zoomFactor());
        prog.setUniformValue("sunDirection", toQVector(sunDirection()));

        auto& tex=*multipleScatteringTextures_.front();
        tex.setMinificationFilter(texFilter);
        tex.setMagnificationFilter(texFilter);
        tex.bind(0);
        prog.setUniformValue("scatteringTexture", 0);
        prog.setUniformValue("staticAltitudeTexCoord", staticAltitudeTexCoord_);
        gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    else
    {
        for(unsigned wlSetIndex=0; wlSetIndex<params_.wavelengthSetCount; ++wlSetIndex)
        {
            if(!radianceRenderBuffers_.empty())
                gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_RENDERBUFFER, radianceRenderBuffers_[wlSetIndex]);

            auto& prog=*multipleScatteringPrograms_[wlSetIndex];
            prog.bind();
            prog.setUniformValue("cameraPosition", toQVector(cameraPosition()));
            prog.setUniformValue("zoomFactor", tools_->zoomFactor());
            prog.setUniformValue("sunDirection", toQVector(sunDirection()));

            auto& tex=*multipleScatteringTextures_[wlSetIndex];
            tex.setMinificationFilter(texFilter);
            tex.setMagnificationFilter(texFilter);
            tex.bind(0);
            prog.setUniformValue("scatteringTexture", 0);
            prog.setUniformValue("staticAltitudeTexCoord", staticAltitudeTexCoord_);
            gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    }
}

void AtmosphereRenderer::draw()
{
    if(!readyToRender_) return;
    // Don't try to draw while we're still loading something. We can come here in
    // this state when e.g. progress reporting code results in a resize event.
    if(totalLoadingStepsToDo_!=0) return;

    const auto altCoord=altitudeUnitRangeTexCoord();
    if(altCoord < loadedAltitudeURTexCoordRange_[0] || altCoord > loadedAltitudeURTexCoordRange_[1])
    {
        currentActivity_=tr("Reloading textures due to altitude getting out of the currently loaded layers...");
        totalLoadingStepsToDo_=0;
        reloadScatteringTextures(CountStepsOnly{true});
        loadingStepsDone_=0;
        reloadScatteringTextures(CountStepsOnly{false});
        reportLoadingFinished();
    }
    else
    {
        updateAltitudeTexCoords(altCoord);
    }

    GLint targetFBO=-1;
    gl.glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &targetFBO);

    gl.glBindVertexArray(vao_);
    {
        gl.glBindFramebuffer(GL_FRAMEBUFFER,mainFBO_);
        if(canGrabRadiance())
        {
            clearRadianceFrames(); // also calls glDrawBuffers
            gl.glEnablei(GL_BLEND, 1);
        }
        gl.glClearColor(0,0,0,0);
        gl.glClear(GL_COLOR_BUFFER_BIT);
        gl.glEnablei(GL_BLEND, 0);
        {
            gl.glBlendFunc(GL_ONE, GL_ONE);
            if(tools_->zeroOrderScatteringEnabled())
                renderZeroOrderScattering();
            if(tools_->singleScatteringEnabled())
                renderSingleScattering();
            if(tools_->multipleScatteringEnabled())
                renderMultipleScattering();
        }
        gl.glDisablei(GL_BLEND, 0);

        gl.glBindFramebuffer(GL_FRAMEBUFFER,targetFBO);
        {
            luminanceToScreenRGB_->bind();
            mainFBOTexture_.bind(0);
            luminanceToScreenRGB_->setUniformValue("luminanceXYZW", 0);
            bayerPatternTexture_.bind(1);
            luminanceToScreenRGB_->setUniformValue("bayerPattern", 1);
            luminanceToScreenRGB_->setUniformValue("rgbMaxValue", rgbMaxValue());
            luminanceToScreenRGB_->setUniformValue("exposure", tools_->exposure());
            gl.glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    }
    gl.glBindVertexArray(0);
}

void AtmosphereRenderer::setupRenderTarget()
{
    gl.glGenFramebuffers(1,&mainFBO_);
    mainFBOTexture_.setMinificationFilter(QOpenGLTexture::Nearest);
    mainFBOTexture_.setMagnificationFilter(QOpenGLTexture::Nearest);
    mainFBOTexture_.setWrapMode(QOpenGLTexture::ClampToEdge);

    if(canGrabRadiance())
    {
        assert(radianceRenderBuffers_.empty());
        radianceRenderBuffers_.resize(params_.wavelengthSetCount);
        gl.glGenRenderbuffers(radianceRenderBuffers_.size(), radianceRenderBuffers_.data());

        gl.glGenFramebuffers(1, &viewDirectionFBO_);
        gl.glGenRenderbuffers(1, &viewDirectionRenderBuffer_);
        gl.glBindFramebuffer(GL_FRAMEBUFFER, viewDirectionFBO_);
        gl.glBindRenderbuffer(GL_RENDERBUFFER, viewDirectionRenderBuffer_);
        gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA32F, 1, 1); // dummy size just to initialize the renderbuffer
        gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, viewDirectionRenderBuffer_);
        gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        gl.glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }

    gl.glGenFramebuffers(1,&eclipseSingleScatteringPrecomputationFBO_);
    eclipsedSingleScatteringPrecomputationTextures_.clear();
    for(const auto& [scattererName, phaseFuncType] : params_.scatterers)
    {
        auto& textures=eclipsedSingleScatteringPrecomputationTextures_[scattererName];
        for(unsigned wlSetIndex=0; wlSetIndex<params_.wavelengthSetCount; ++wlSetIndex)
        {
            auto& tex=*textures.emplace_back(newTex(QOpenGLTexture::Target2D));
            tex.setMinificationFilter(QOpenGLTexture::Linear);
            tex.setMagnificationFilter(QOpenGLTexture::Linear);
            // relative azimuth
            tex.setWrapMode(QOpenGLTexture::DirectionS, QOpenGLTexture::Repeat);
            // cosVZA
            tex.setWrapMode(QOpenGLTexture::DirectionT, QOpenGLTexture::ClampToEdge);
            tex.bind();
            const auto width=params_.eclipseSingleScatteringTextureSizeForRelAzimuth;
            const auto height=params_.eclipseSingleScatteringTextureSizeForCosVZA;
            gl.glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);

            if(phaseFuncType==PhaseFunctionType::Achromatic || phaseFuncType==PhaseFunctionType::Smooth)
                break;
        }
    }

    GLint viewport[4];
    gl.glGetIntegerv(GL_VIEWPORT, viewport);
    const int width=viewport[2], height=viewport[3];
    resizeEvent(width,height);
}

AtmosphereRenderer::AtmosphereRenderer(QOpenGLFunctions_3_3_Core& gl, QString const& pathToData,
                                       Parameters const& params, ToolsWidget* tools)
    : gl(gl)
    , tools_(tools)
    , params_(params)
    , pathToData_(pathToData)
    , bayerPatternTexture_(QOpenGLTexture::Target2D)
    , mainFBOTexture_(QOpenGLTexture::Target2D)
{
}

void AtmosphereRenderer::loadData()
{
    readyToRender_=false;
    loadingStepsDone_=0;
    totalLoadingStepsToDo_=0;

    clearResources();

    for(const auto& [scattererName,_] : params_.scatterers)
        scatterersEnabledStates_[scattererName]=true;

    currentActivity_=tr("Loading textures and shaders...");
    // The longest actions should be progress-tracked
    loadShaders(CountStepsOnly{true});
    loadTextures(CountStepsOnly{true});
    // Now they can be actually executed, reporting the progress
    loadShaders(CountStepsOnly{false});
    loadTextures(CountStepsOnly{false});
    reportLoadingFinished();

    setupRenderTarget();
    setupBuffers();

    if(multipleScatteringPrograms_.size() != multipleScatteringTextures_.size())
    {
        throw DataLoadError{tr("Numbers of multiple scattering shader programs and textures don't match: %1 vs %2")
                              .arg(multipleScatteringPrograms_.size())
                              .arg(multipleScatteringTextures_.size())};
    }

    tools_->setCanGrabRadiance(canGrabRadiance());
    readyToRender_=true;
}

void AtmosphereRenderer::tick(const int loadingStepsDone)
{
    emit loadProgress(currentActivity_, loadingStepsDone, totalLoadingStepsToDo_);
}

void AtmosphereRenderer::reportLoadingFinished()
{
    currentActivity_.clear();
    totalLoadingStepsToDo_=0;
    loadingStepsDone_=0;
    tick(0);
}

AtmosphereRenderer::~AtmosphereRenderer()
{
    clearResources();
}

void AtmosphereRenderer::clearResources()
{
    if(vbo_)
    {
        gl.glDeleteBuffers(1, &vbo_);
        vbo_=0;
    }
    if(vao_)
    {
        gl.glDeleteVertexArrays(1, &vao_);
        vao_=0;
    }
    if(mainFBO_)
    {
        gl.glDeleteFramebuffers(1, &mainFBO_);
        mainFBO_=0;
    }
    if(eclipseSingleScatteringPrecomputationFBO_)
    {
        gl.glDeleteFramebuffers(1, &eclipseSingleScatteringPrecomputationFBO_);
        eclipseSingleScatteringPrecomputationFBO_=0;
    }
    if(!radianceRenderBuffers_.empty())
        gl.glDeleteRenderbuffers(radianceRenderBuffers_.size(), radianceRenderBuffers_.data());
}

void AtmosphereRenderer::resizeEvent(const int width, const int height)
{
    viewportSize_=QSize(width,height);
    if(!mainFBO_) return;

    mainFBOTexture_.bind();
    gl.glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    gl.glBindFramebuffer(GL_FRAMEBUFFER,mainFBO_);
    gl.glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,mainFBOTexture_.textureId(),0);
    checkFramebufferStatus(gl, "Atmosphere renderer FBO");
    gl.glBindFramebuffer(GL_FRAMEBUFFER,0);

    if(!radianceRenderBuffers_.empty())
    {
        for(unsigned wlSetIndex=0; wlSetIndex<params_.wavelengthSetCount; ++wlSetIndex)
        {
            gl.glBindRenderbuffer(GL_RENDERBUFFER, radianceRenderBuffers_[wlSetIndex]);
            gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA32F, width, height);
        }
        gl.glBindRenderbuffer(GL_RENDERBUFFER, viewDirectionRenderBuffer_);
        gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA32F, width, height);
    }
}

void AtmosphereRenderer::mouseMove(const int x, const int y)
{
    constexpr double scale=500;
    switch(dragMode_)
    {
    case DragMode::Sun:
    {
        const auto oldZA=tools_->sunZenithAngle(), oldAz=tools_->sunAzimuth();
        tools_->setSunZenithAngle(std::clamp(oldZA - (prevMouseY_-y)/scale, 0., M_PI));
        tools_->setSunAzimuth(std::clamp(oldAz - (prevMouseX_-x)/scale, -M_PI, M_PI));
        break;
    }
    default:
        break;
    }
    prevMouseX_=x;
    prevMouseY_=y;
}

void AtmosphereRenderer::setScattererEnabled(QString const& name, const bool enable)
{
    scatterersEnabledStates_[name]=enable;
    emit needRedraw();
}

void AtmosphereRenderer::reloadShaders()
{
    currentActivity_=tr("Reloading shaders...");
    totalLoadingStepsToDo_=0;
    loadShaders(CountStepsOnly{true});
    loadingStepsDone_=0;
    loadShaders(CountStepsOnly{false});
    reportLoadingFinished();
}

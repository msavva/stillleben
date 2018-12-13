// Complete render pass for complex scenes
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#ifndef STILLLEBEN_RENDER_PASS_H
#define STILLLEBEN_RENDER_PASS_H

#include <Magnum/GL/MultisampleTexture.h>
#include <Magnum/GL/Renderbuffer.h>
#include <Magnum/GL/RectangleTexture.h>
#include <Magnum/GL/Mesh.h>

#include <memory>

namespace sl
{

class RenderShader;
class ResolveShader;
class Scene;

class RenderPass
{
public:
    RenderPass();
    ~RenderPass();

    struct Result
    {
        Magnum::GL::RectangleTexture rgb;
        Magnum::GL::RectangleTexture objectCoordinates;
        Magnum::GL::RectangleTexture classIndex;
        Magnum::GL::RectangleTexture instanceIndex;
        Magnum::GL::RectangleTexture validMask;
    };

    std::shared_ptr<Result> render(Scene& scene);
private:
    unsigned int m_msaa_factor = 4;
    Magnum::GL::MultisampleTexture2D m_msaa_rgb;
    Magnum::GL::MultisampleTexture2D m_msaa_depth;
    Magnum::GL::MultisampleTexture2D m_msaa_objectCoordinates;
    Magnum::GL::MultisampleTexture2D m_msaa_classIndex;
    Magnum::GL::MultisampleTexture2D m_msaa_instanceIndex;

    std::unique_ptr<RenderShader> m_shaderTextured;
    std::unique_ptr<RenderShader> m_shaderUniform;

    std::unique_ptr<ResolveShader> m_resolveShader;

    Magnum::GL::Mesh m_quadMesh;
};

}

#endif
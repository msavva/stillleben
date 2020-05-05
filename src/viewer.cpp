// Interactive scene viewer
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#include <stillleben/viewer.h>

#include <stillleben/context.h>
#include <stillleben/render_pass.h>
#include <stillleben/scene.h>
#include <stillleben/mesh.h>

#include "utils/arc_ball.h"
#include "utils/x11_events.h"

#include "shaders/viewer/viewer_shader.h"

#include <Corrade/Utility/Debug.h>
#include <Corrade/Utility/System.h>
#include <Corrade/Utility/FormatStl.h>

#include <Corrade/Containers/StaticArray.h>

#include <Magnum/GL/DefaultFramebuffer.h>
#include <Magnum/GL/Renderer.h>
#include <Magnum/GL/TextureFormat.h>

#include <Magnum/MeshTools/Compile.h>
#include <Magnum/Primitives/Square.h>
#include <Magnum/Trade/MeshData.h>

#include <Magnum/ImGuiIntegration/Context.h>
#include <Magnum/ImGuiIntegration/Context.hpp>
#include <Magnum/ImGuiIntegration/Integration.h>
#include <Magnum/ImGuiIntegration/Widgets.h>
#include <imgui.h>

// TODO: remove this
#include <Egl.h>

#include <EGL/egl.h>
/* undef Xlib nonsense to avoid conflicts */
#undef None
#undef Complex

#ifdef HAVE_XCURSOR
#include <X11/Xcursor/Xcursor.h>
#endif

/* EGL returns visual ID as int, but Xorg expects long unsigned int */
#ifdef __unix__
typedef VisualID VisualId;
#else
typedef EGLInt VisualId;
#endif

/* Mask for X events */
#define INPUT_MASK KeyPressMask|KeyReleaseMask|ButtonPressMask|ButtonReleaseMask|PointerMotionMask|StructureNotifyMask

using namespace Magnum;
using namespace Magnum::Math::Literals;
using namespace sl::utils;

namespace
{
    constexpr const char* renderTypeLabel(sl::RenderPass::Type type)
    {
        using namespace sl;
        switch(type)
        {
            case RenderPass::Type::PBR: return "PBR";
            case RenderPass::Type::Phong: return "Phong";
            case RenderPass::Type::Flat: return "Flat";
        }
        return {};
    }
}

namespace sl
{

class Viewer::Private
{
public:
    enum class Flag: unsigned int {
        Exit = 1 << 0
    };

    typedef Containers::EnumSet<Flag> Flags;
    CORRADE_ENUMSET_FRIEND_OPERATORS(Flags)

    enum class Cursor : unsigned int
    {
        Arrow,
        TextInput,
        ResizeNS,
        ResizeWE,
        ResizeNWSE,
        ResizeNESW,
        Hand,

        NumCursors
    };


    explicit Private(const std::shared_ptr<Context>& ctx)
     : ctx{ctx}
     , renderer{std::make_unique<RenderPass>(RenderPass::Type::Phong, false)}
    {}

    void redraw(Magnum::UnsignedInt cnt = 1)
    {
        redrawCount = std::max(redrawCount, cnt);
    }

    void mousePressEvent(MouseEvent& event)
    {
        redraw(5);

        if(imgui.handleMousePressEvent(event)) return;
    }

    void mouseReleaseEvent(MouseEvent& event)
    {
        redraw(5);

        if(arcBallHovered)
        {
            if(event.button() == MouseEvent::Button::WheelUp)
            {
                arcBall->zoom(-0.1 * arcBall->viewDistance());
                return;
            }
            else if(event.button() == MouseEvent::Button::WheelDown)
            {
                arcBall->zoom(0.1 * arcBall->viewDistance());
                return;
            }
        }

        if(arcBallActive)
        {
            stopArcBall();
        }

        if(imgui.handleMouseReleaseEvent(event)) return;
    }

    void mouseMoveEvent(MouseMoveEvent& event)
    {
        redraw();

        if(arcBallActive)
        {
            Vector2i pos{(Vector2{event.position()} - arcBallOffset) / arcBallScale};
            if(event.buttons() & InputEvent::Button::Middle)
                arcBall->translate(pos);
            else
                arcBall->rotate(pos);
            return;
        }

        if(imgui.handleMouseMoveEvent(event)) return;
    }

    void keyPressEvent(KeyEvent& event)
    {
    }
    void keyReleaseEvent(KeyEvent& event)
    {
    }

    void setCursor([[maybe_unused]] Cursor cursor)
    {
#if HAVE_XCURSOR
        XDefineCursor(display, window, cursors[(int)cursor]);
#endif
    }

    void startArcBall(const Vector2& offset, float scale)
    {
        arcBallActive = true;
        arcBallOffset = offset;
        arcBallScale = scale;

        Vector2i pos{(Vector2{ImGui::GetMousePos()} - offset) / scale};
        arcBall->initTransformation(pos);

        XGrabPointer(display, window, False, PointerMotionMask | ButtonPressMask | ButtonReleaseMask, GrabModeAsync, GrabModeAsync, window, 0, CurrentTime);
    }

    void stopArcBall()
    {
        XUngrabPointer(display, CurrentTime);
        arcBallActive = false;
    }

    std::shared_ptr<Context> ctx;
    std::unique_ptr<RenderPass> renderer;
    std::shared_ptr<Scene> scene;
    std::shared_ptr<RenderPass::Result> result;

    Display* display{};
    Window window{};
    Atom deleteWindow{};

    EGLSurface surface{};

    Flags flags{};
    Magnum::UnsignedInt redrawCount = 0;

    Magnum::GL::Framebuffer framebuffer{Magnum::NoCreate};
    Magnum::GL::Texture2D textureRGB{Magnum::NoCreate};
    Magnum::GL::Texture2D textureNormal{Magnum::NoCreate};
    Magnum::GL::Texture2D textureInstance{Magnum::NoCreate};
    Magnum::GL::Texture2D textureClass{Magnum::NoCreate};
    Magnum::GL::Texture2D textureCoordinates{Magnum::NoCreate};
    Magnum::Vector2i windowSize{};

    Magnum::ImGuiIntegration::Context imgui{Magnum::NoCreate};

    std::unique_ptr<sl::utils::ArcBall> arcBall;
    bool arcBallActive = false;
    Vector2 arcBallOffset;
    float arcBallScale;
    bool arcBallHovered = false;

#if HAVE_XCURSOR
    Corrade::Containers::StaticArray<static_cast<unsigned int>(Cursor::NumCursors), ::Cursor> cursors;
#endif

    Magnum::GL::Mesh quad{Magnum::NoCreate};
    ViewerShader shader{Magnum::NoCreate};

    RenderPass::Type renderType = RenderPass::Type::PBR;
    bool enableSSAO = true;
    bool drawPhysics = false;
    bool drawSimplified = false;

    bool showInstances = true;
};

Viewer::Viewer(const std::shared_ptr<Context>& ctx)
 : m_d{std::make_unique<Private>(ctx)}
{
}

Viewer::~Viewer()
{
    if(m_d->surface)
        eglDestroySurface(eglGetCurrentDisplay(), m_d->surface);

    if(m_d->window)
        XDestroyWindow(m_d->display, m_d->window);
}

void Viewer::setScene(const std::shared_ptr<Scene>& scene)
{
    m_d->scene = scene;
}

std::shared_ptr<Scene> Viewer::scene() const
{
    return m_d->scene;
}

void Viewer::setup()
{
    if(!m_d->scene)
        throw std::logic_error{"Need to call Viewer::setScene() first"};

    m_d->windowSize = {1280, 720};

    // X11 stuff for creating a new window
    {
        // Get default X display
        m_d->display = reinterpret_cast<Display*>(m_d->ctx->x11Display());

        VisualId visualId = m_d->ctx->visualID();

        /* Get visual info */
        XVisualInfo *visInfo, visTemplate;
        int visualCount;
        visTemplate.visualid = visualId;
        visInfo = XGetVisualInfo(m_d->display, VisualIDMask, &visTemplate, &visualCount);
        if(!visInfo)
        {
            throw std::runtime_error{
                Corrade::Utility::formatString("Viewer: cannot get X visual using visualid {}", visualId)
            };
        }

        /* Create X Window */
        Window root = RootWindow(m_d->display, DefaultScreen(m_d->display));
        XSetWindowAttributes attr;
        attr.background_pixel = 0;
        attr.border_pixel = 0;
        attr.colormap = XCreateColormap(m_d->display, root, visInfo->visual, AllocNone);
        attr.event_mask = 0;
        unsigned long mask = CWBackPixel|CWBorderPixel|CWColormap|CWEventMask;
        m_d->window = XCreateWindow(m_d->display, root, 20, 20, m_d->windowSize.x(), m_d->windowSize.y(), 0, visInfo->depth, InputOutput, visInfo->visual, mask, &attr);
        XSetStandardProperties(m_d->display, m_d->window, "stillleben", nullptr, 0, nullptr, 0, nullptr);
        XFree(visInfo);

        /* Be notified about closing the window */
        m_d->deleteWindow = XInternAtom(m_d->display, "WM_DELETE_WINDOW", True);
        XSetWMProtocols(m_d->display, m_d->window, &m_d->deleteWindow, 1);
    }

    // Create the EGL surface connected to the window
    m_d->surface = eglCreateWindowSurface(
        eglGetCurrentDisplay(), m_d->ctx->eglConfig(), m_d->window, nullptr
    );
    if(!m_d->surface)
        throw std::runtime_error{std::string{"Cannot create window surface:"} + Platform::Implementation::eglErrorString(eglGetError())};

    // Make current
    eglMakeCurrent(eglGetCurrentDisplay(), m_d->surface, m_d->surface, eglGetCurrentContext());

    // Capture exposure, keyboard and mouse button events
    XSelectInput(m_d->display, m_d->window, INPUT_MASK);

    Magnum::GL::defaultFramebuffer.setViewport({{}, m_d->windowSize});

    m_d->imgui = Magnum::ImGuiIntegration::Context{m_d->windowSize};

    /* Set up proper blending to be used by ImGui. There's a great chance
       you'll need this exact behavior for the rest of your scene. If not, set
       this only for the drawFrame() call. */
    GL::Renderer::setBlendEquation(GL::Renderer::BlendEquation::Add,
        GL::Renderer::BlendEquation::Add);
    GL::Renderer::setBlendFunction(GL::Renderer::BlendFunction::SourceAlpha,
        GL::Renderer::BlendFunction::OneMinusSourceAlpha);

    // Setup our intermediate framebuffer
    auto size = m_d->scene->viewport();
    m_d->framebuffer = Magnum::GL::Framebuffer{{{}, size}};
    for(auto* tex : {&m_d->textureRGB, &m_d->textureNormal, &m_d->textureInstance, &m_d->textureClass, &m_d->textureCoordinates})
    {
        *tex = Magnum::GL::Texture2D{};
        (*tex)
            .setMagnificationFilter(GL::SamplerFilter::Linear)
            .setMinificationFilter(GL::SamplerFilter::Linear, GL::SamplerMipmap::Linear)
            .setWrapping(GL::SamplerWrapping::ClampToEdge)
            .setMaxAnisotropy(GL::Sampler::maxMaxAnisotropy())
            .setStorage(Math::log2(size.max())+1, GL::TextureFormat::RGBA8, size);
    }

    m_d->framebuffer.attachTexture(GL::Framebuffer::ColorAttachment{0}, m_d->textureRGB, 0);
    m_d->framebuffer.attachTexture(GL::Framebuffer::ColorAttachment{1}, m_d->textureNormal, 0);
    m_d->framebuffer.attachTexture(GL::Framebuffer::ColorAttachment{2}, m_d->textureInstance, 0);
    m_d->framebuffer.attachTexture(GL::Framebuffer::ColorAttachment{3}, m_d->textureClass, 0);
    m_d->framebuffer.attachTexture(GL::Framebuffer::ColorAttachment{4}, m_d->textureCoordinates, 0);

    m_d->quad = Magnum::MeshTools::compile(Magnum::Primitives::squareSolid());
    m_d->shader = ViewerShader{m_d->scene};

    // Compute camPosition, viewCenter & FoV from pose + projection matrix
    // We will position the "ball" at the mean depth of the objects in
    // the scene.
    Vector3 camPosition = m_d->scene->cameraPose().translation();

    Vector3 meanPosition{};
    if(!m_d->scene->objects().empty())
    {
        for(auto& obj : m_d->scene->objects())
            meanPosition += obj->pose().translation();
        meanPosition /= m_d->scene->objects().size();
    }

    // to camera space
    meanPosition = m_d->scene->cameraPose().invertedRigid().transformPoint(meanPosition);

    // in camera space
    Vector3 viewCenter{0.0f, 0.0f, meanPosition.z()};

    // to world
    viewCenter = m_d->scene->cameraPose().transformPoint(viewCenter);

    Vector3 upDir = m_d->scene->cameraPose().rotation() * Vector3{0.0f, -1.0f, 0.0f};
    Magnum::Deg fov = 2.0f * Math::atan(1.0f / m_d->scene->camera().projectionMatrix()[0][0]);
    m_d->arcBall = std::make_unique<utils::ArcBall>(
        camPosition, viewCenter, upDir, fov,
        m_d->scene->viewport()
    );

#if HAVE_XCURSOR
    // Load cursors
    // Why yes, I quite like pain.
    for(int i = 0; i < static_cast<int>(Private::Cursor::NumCursors); ++i)
    {
        switch(static_cast<Private::Cursor>(i))
        {
            case Private::Cursor::Arrow:
                m_d->cursors[i] = XcursorLibraryLoadCursor(m_d->display, "arrow");
                break;
            case Private::Cursor::TextInput:
                m_d->cursors[i] = XcursorLibraryLoadCursor(m_d->display, "text");
                break;
            case Private::Cursor::Hand:
                m_d->cursors[i] = XcursorLibraryLoadCursor(m_d->display, "openhand");
                break;
            case Private::Cursor::ResizeNS:
                m_d->cursors[i] = XcursorLibraryLoadCursor(m_d->display, "size_ver");
                break;
            case Private::Cursor::ResizeWE:
                m_d->cursors[i] = XcursorLibraryLoadCursor(m_d->display, "size_hor");
                break;
            case Private::Cursor::ResizeNESW:
                m_d->cursors[i] = XcursorLibraryLoadCursor(m_d->display, "size_bdiag");
                break;
            case Private::Cursor::ResizeNWSE:
                m_d->cursors[i] = XcursorLibraryLoadCursor(m_d->display, "size_fdiag");
                break;
            case Private::Cursor::NumCursors:
                break;
        }
    }
#endif

    m_d->redraw();
}

void Viewer::draw()
{
    m_d->arcBall->updateTransformation();
    m_d->scene->setCameraPose(m_d->arcBall->transformationMatrix());

    if(m_d->renderer->type() != m_d->renderType)
        m_d->renderer = std::make_unique<RenderPass>(m_d->renderType, false);

    m_d->renderer->setSSAOEnabled(m_d->enableSSAO);
    m_d->renderer->setDrawPhysicsEnabled(m_d->drawPhysics);
    m_d->renderer->setDrawSimplifiedEnabled(m_d->drawSimplified);

    m_d->result = m_d->renderer->render(*m_d->scene, m_d->result);

    // Run the viewer shader to visualize results
    m_d->framebuffer.bind();
    m_d->framebuffer.mapForDraw({
        {ViewerShader::ColorOutput, GL::Framebuffer::ColorAttachment{0}},
        {ViewerShader::NormalOutput, GL::Framebuffer::ColorAttachment{1}},
        {ViewerShader::InstanceIndexOutput, GL::Framebuffer::ColorAttachment{2}},
        {ViewerShader::ClassIndexOutput, GL::Framebuffer::ColorAttachment{3}},
        {ViewerShader::CoordinateOutput, GL::Framebuffer::ColorAttachment{4}}
    });

    m_d->shader.setData(*m_d->result);

    m_d->shader.draw(m_d->quad);

    m_d->textureRGB.generateMipmap();
    m_d->textureNormal.generateMipmap();
    m_d->textureInstance.generateMipmap();
    m_d->textureClass.generateMipmap();
    m_d->textureCoordinates.generateMipmap();

    Magnum::GL::defaultFramebuffer.bind();
    Magnum::GL::defaultFramebuffer.mapForDraw(Magnum::GL::DefaultFramebuffer::DrawAttachment::Back);
    Magnum::GL::defaultFramebuffer.clear(GL::FramebufferClear::Color);

    if(!m_d->scene)
        return;

    m_d->imgui.newFrame();

    Vector2 srcSize{m_d->scene->viewport()};
    const int MENU_BAR_WIDTH = 200;
    Vector2 qSize = Vector2{(m_d->windowSize - Vector2i{MENU_BAR_WIDTH, 0})/ 2};

    m_d->arcBallHovered = false;

    auto fitImage = [&](Magnum::GL::Texture2D& tex){
        auto available = Vector2{ImGui::GetWindowContentRegionMax()} - Vector2{ImGui::GetWindowContentRegionMin()};

        float scale = (available / srcSize).min();
        Vector2 imgSize = scale * srcSize;

        // Center
        Vector2 off = (available - imgSize)/2;
        ImGui::SetCursorPos(ImVec2{Vector2{ImGui::GetCursorPos()} + off});

        auto loc = Vector2{ImGui::GetCursorScreenPos()};
        Magnum::ImGuiIntegration::imageButton(tex, imgSize, {{0.0, 1.0}, {1.0, 0.0}}, 0);
        if(ImGui::IsItemClicked(0) || ImGui::IsItemClicked(1) || ImGui::IsItemClicked(2))
        {
            m_d->startArcBall(loc, scale);
        }
        if(ImGui::IsItemHovered())
            m_d->arcBallHovered = true;
    };

    ImGui::PushStyleVar(ImGuiStyleVar_WindowTitleAlign, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));

    {
        ImGui::SetNextWindowPos(ImVec2{0,0});
        ImGui::SetNextWindowSize(ImVec2{qSize});
        ImGui::Begin("RGB", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
        fitImage(m_d->textureRGB);
        ImGui::End();
    }
    {
        ImGui::SetNextWindowPos(ImVec2{qSize.x(),0});
        ImGui::SetNextWindowSize(ImVec2{qSize});
        ImGui::Begin("Normals", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
        fitImage(m_d->textureNormal);
        ImGui::End();
    }
    {
        ImGui::SetNextWindowPos(ImVec2{0,qSize.y()});
        ImGui::SetNextWindowSize(ImVec2{qSize});
        ImGui::Begin("Segmentation", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

        {
            auto loc = ImGui::GetCursorPos();

            ImGui::PushClipRect(ImVec2{0,0}, ImVec2{Vector2{m_d->windowSize}}, false);
            ImGui::SetCursorScreenPos(ImVec2{10, qSize.y()});
            ImGui::Checkbox("Instances", &m_d->showInstances);
            ImGui::SetCursorPos(loc);
            ImGui::PopClipRect();
        }

        if(m_d->showInstances)
            fitImage(m_d->textureInstance);
        else
            fitImage(m_d->textureClass);
        ImGui::End();
    }
    {
        ImGui::SetNextWindowPos(ImVec2{qSize});
        ImGui::SetNextWindowSize(ImVec2{qSize});
        ImGui::Begin("Coordinates", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
        fitImage(m_d->textureCoordinates);
        ImGui::End();
    }

    // Enable padding again
    ImGui::PopStyleVar();

    {
        ImGui::SetNextWindowPos(ImVec2{2*qSize.x(), 0});
        ImGui::SetNextWindowSize(ImVec2(MENU_BAR_WIDTH, m_d->windowSize.y()));
        ImGui::Begin("Menu", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

        if(ImGui::CollapsingHeader("Rendering"))
        {
            ImGui::Text("Shading:"); ImGui::SameLine();
            if(ImGui::BeginCombo("Shading model", renderTypeLabel(m_d->renderType)))
            {
                for(auto type : {RenderPass::Type::PBR, RenderPass::Type::Phong, RenderPass::Type::Flat})
                {
                    bool selected = (m_d->renderType == type);
                    if(ImGui::Selectable(renderTypeLabel(type), selected))
                        m_d->renderType = type;
                    if(selected)
                        ImGui::SetItemDefaultFocus();
                }

                ImGui::EndCombo();
            }

            ImGui::Checkbox("SSAO", &m_d->enableSSAO);
        }

        if(ImGui::CollapsingHeader("Debug"))
        {
            ImGui::Checkbox("Draw simplified meshes", &m_d->drawSimplified);
            ImGui::Checkbox("Draw physics", &m_d->drawPhysics);
        }

        ImGui::End();
    }

    ImGui::PopStyleVar();

    // Update cursor
    m_d->imgui.updateApplicationCursor(*m_d);

    /* Set appropriate states. If you only draw ImGui, it is sufficient to
       just enable blending and scissor test in the constructor. */
    GL::Renderer::enable(GL::Renderer::Feature::Blending);
    GL::Renderer::enable(GL::Renderer::Feature::ScissorTest);
    GL::Renderer::disable(GL::Renderer::Feature::FaceCulling);
    GL::Renderer::disable(GL::Renderer::Feature::DepthTest);

    m_d->imgui.drawFrame();

    /* Reset state. Only needed if you want to draw something else with
       different state after. */
    GL::Renderer::enable(GL::Renderer::Feature::DepthTest);
    GL::Renderer::enable(GL::Renderer::Feature::FaceCulling);
    GL::Renderer::disable(GL::Renderer::Feature::ScissorTest);
    GL::Renderer::disable(GL::Renderer::Feature::Blending);

    eglSwapBuffers(eglGetCurrentDisplay(), m_d->surface);
}

void Viewer::run()
{
    setup();

    // Show window
    XMapWindow(m_d->display, m_d->window);

    while(mainLoopIteration()) {}

    // Switch back to headless EGL state
    eglMakeCurrent(eglGetCurrentDisplay(), EGL_NO_SURFACE, EGL_NO_SURFACE, eglGetCurrentContext());
}

bool Viewer::mainLoopIteration()
{
    XEvent event;

    // Closed window
    if(XCheckTypedWindowEvent(m_d->display, m_d->window, ClientMessage, &event) &&
        Atom(event.xclient.data.l[0]) == m_d->deleteWindow)
    {
        return false;
    }

    while(XCheckWindowEvent(m_d->display, m_d->window, INPUT_MASK, &event)) {
        switch(event.type) {
            /* Window resizing */
            case ConfigureNotify: {
                Vector2i size(event.xconfigure.width, event.xconfigure.height);
                if(size != m_d->windowSize)
                {
                    m_d->windowSize = size;
                    Magnum::GL::defaultFramebuffer.setViewport({{}, size});
                    m_d->imgui.relayout(size);
                    m_d->redraw();
                }
            } break;

            /* Key/mouse events */
            case KeyPress:
            case KeyRelease: {
                KeyEvent e(static_cast<KeyEvent::Key>(XLookupKeysym(&event.xkey, 0)), static_cast<InputEvent::Modifier>(event.xkey.state), {event.xkey.x, event.xkey.y});
                event.type == KeyPress ? m_d->keyPressEvent(e) : m_d->keyReleaseEvent(e);
            } break;
            case ButtonPress:
            case ButtonRelease: {
                MouseEvent e(static_cast<MouseEvent::Button>(event.xbutton.button), static_cast<InputEvent::Modifier>(event.xkey.state), {event.xbutton.x, event.xbutton.y});
                event.type == ButtonPress ? m_d->mousePressEvent(e) : m_d->mouseReleaseEvent(e);
            } break;

            /* Mouse move events */
            case MotionNotify: {
                MouseMoveEvent e(static_cast<InputEvent::Modifier>(event.xmotion.state), {event.xmotion.x, event.xmotion.y});
                m_d->mouseMoveEvent(e);
            } break;
        }
    }

    if(m_d->redrawCount > 0)
    {
        m_d->redrawCount--;
        draw();
    }
    else Corrade::Utility::System::sleep(5);

    return !(m_d->flags & Private::Flag::Exit);
}

void Viewer::view(const std::shared_ptr<Context>& ctx, const std::shared_ptr<sl::Scene>& scene)
{
    Viewer viewer{ctx};
    viewer.setScene(scene);
    viewer.run();
}

}

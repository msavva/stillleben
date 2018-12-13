// Represents a scene composed of multiple objects
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#ifndef STILLLEBEN_SCENE_H
#define STILLLEBEN_SCENE_H

#include <stillleben/math.h>
#include <stillleben/common.h>

#include <Magnum/SceneGraph/Camera.h>

#include <memory>
#include <vector>

namespace sl
{

class Context;
class Object;

class Scene
{
public:
    Scene(const std::shared_ptr<Context>& ctx, const ViewportSize& size);
    Scene(const Scene& other) = delete;
    Scene(Scene&& other) = delete;
    ~Scene();

    //! @name Camera and viewport settings
    //@{
    void setCameraPose(const PoseMatrix& pose);
    PoseMatrix cameraPose() const;

    void setCameraIntrinsics(float fx, float fy, float cx, float cy);

    ViewportSize viewport() const
    { return m_camera->viewport(); }

    Magnum::SceneGraph::Camera3D& camera()
    { return *m_camera; }
    //@}

    //! @name Object placement
    //@{
    float minimumDistanceForObjectDiameter(float diameter) const;
    //@}

    void addObject(const std::shared_ptr<Object>& object);
    const std::vector<std::shared_ptr<Object>>& objects() const
    { return m_objects; }

private:
    std::shared_ptr<Context> m_ctx;

    Scene3D m_scene;
    Object3D m_cameraObject;
    Magnum::SceneGraph::Camera3D* m_camera = nullptr;

    std::vector<std::shared_ptr<Object>> m_objects;
};

}

#endif
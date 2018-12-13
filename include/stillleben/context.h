// stillleben context
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#ifndef STILLLEBEN_CONTEXT_H
#define STILLLEBEN_CONTEXT_H

#include <memory>

namespace Corrade
{
    namespace PluginManager
    {
        template<class> class Manager;
    }
}

namespace Magnum
{
    namespace Trade
    {
        class AbstractImporter;
    }
}

namespace sl
{

class Mesh;

class Context
{
public:
    using Ptr = std::shared_ptr<Context>;

    Context(const Context& other) = delete;
    Context(const Context&& other) = delete;

    static Ptr Create();
    static Ptr CreateCUDA(unsigned int device = 0);

    bool makeCurrent();

    std::shared_ptr<Mesh> loadMesh(const std::string& path);

    std::shared_ptr<Corrade::PluginManager::Manager<Magnum::Trade::AbstractImporter>> importerPluginManager();
private:
    class Private;

    Context();

    std::unique_ptr<Private> m_d;
};

}

#endif
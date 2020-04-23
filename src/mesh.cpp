// Mesh
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#include <stillleben/mesh.h>
#include <stillleben/context.h>
#include <stillleben/contrib/ctpl_stl.h>
#include <stillleben/mesh_tools/simplify_mesh.h>
#include <stillleben/mesh_tools/tangents.h>
#include <stillleben/physx.h>

#include <Corrade/Containers/ArrayView.h>

#include <Corrade/Utility/Configuration.h>
#include <Corrade/Utility/DebugStl.h>
#include <Corrade/Utility/Directory.h>
#include <Corrade/Utility/Format.h>
#include <Corrade/Utility/FormatStl.h>
#include <Corrade/Utility/MurmurHash2.h>
#include <Corrade/Utility/String.h>

#include <Magnum/GL/Mesh.h>
#include <Magnum/GL/TextureFormat.h>
#include <Magnum/GL/Texture.h>
#include <Magnum/ImageView.h>
#include <Magnum/Math/ConfigurationValue.h>
#include <Magnum/Math/Functions.h>
#include <Magnum/Math/Algorithms/Svd.h>
#include <Magnum/Mesh.h>
#include <Magnum/MeshTools/Compile.h>
#include <Magnum/MeshTools/Interleave.h>
#include <Magnum/PixelFormat.h>
#include <Magnum/SceneGraph/MatrixTransformation3D.h>
#include <Magnum/SceneGraph/SceneGraph.h>
#include <Magnum/SceneGraph/Scene.h>
#include <Magnum/SceneGraph/Object.h>
#include <Magnum/Shaders/Generic.h>
#include <Magnum/Trade/ImageData.h>

#include <Magnum/Trade/MeshObjectData3D.h>
#include <Magnum/Trade/ObjectData3D.h>
#include <Magnum/Trade/PhongMaterialData.h>
#include <Magnum/Trade/SceneData.h>
#include <Magnum/Trade/TextureData.h>
#include <Magnum/Image.h>

#include <sstream>
#include <fstream>

#include "shaders/render_shader.h"
#include "physx_impl.h"
#include "utils/os.h"

using namespace Magnum;

namespace sl
{

namespace
{
    /**
     * @brief Create PhysX collision shape from Trade::MeshData3D
     * */
    Corrade::Containers::Optional<PhysXOutputBuffer> cookForPhysX(physx::PxCooking& cooking, const Containers::Array<Vector3>& vertices, const Containers::Array<UnsignedInt>& indices)
    {
        Corrade::Containers::Optional<PhysXOutputBuffer> out;

        out.emplace();

        static_assert(sizeof(decltype(*vertices.data())) == sizeof(physx::PxVec3));

        physx::PxConvexMeshDesc meshDesc;
        meshDesc.points.count = vertices.size();
        meshDesc.points.stride = sizeof(physx::PxVec3);
        meshDesc.points.data = vertices.data();
        meshDesc.flags = physx::PxConvexFlag::eCOMPUTE_CONVEX;

        physx::PxConvexMeshCookingResult::Enum result;
        bool status = cooking.cookConvexMesh(meshDesc, *out, &result);
        if(!status)
        {
            Error{} << "PhysX cooking failed, ignoring mesh";
            return {};
        }

        return out;
    }

    using MeshHash = Corrade::Utility::MurmurHash2;

    template<class T>
    MeshHash::Digest hashArray(const Corrade::Containers::Array<T>& vec)
    {
        MeshHash hash{}; // use default seed
        const auto& view = Corrade::Containers::arrayCast<const char>(vec);
        return hash(view.data(), view.size());
    }

    Corrade::Containers::Optional<std::vector<uint8_t>> readCacheFile(const std::string& cacheFile, const std::string& sourceFile, const Magnum::Trade::MeshData& meshData)
    {
        auto readHash = [](std::istream& stream) -> Corrade::Containers::Optional<MeshHash::Digest>{
            std::array<char, MeshHash::DigestSize> hashBytes;
            stream.read(hashBytes.data(), hashBytes.size());
            if(stream.gcount() != hashBytes.size())
                return {};

            return MeshHash::Digest::fromByteArray(hashBytes.data());
        };

        if(!Corrade::Utility::Directory::exists(cacheFile))
            return {};

        if(os::modificationTime(cacheFile) <= os::modificationTime(sourceFile))
        {
            Debug{} << "Cache file is stale";
            return {};
        }

        std::ifstream stream(cacheFile, std::ios::binary);
        if(!stream)
            return {};

        auto cacheVertexHash = readHash(stream);
        auto cacheIndicesHash = readHash(stream);

        auto vertexHash = hashArray(meshData.positions3DAsArray(0));
        if(vertexHash != cacheVertexHash)
        {
            Debug{} << "Vertex hash does not match: cached=" << cacheVertexHash << ", actual=" << vertexHash;
            return {};
        }

        auto indicesHash = hashArray(meshData.indicesAsArray());
        if(indicesHash != cacheIndicesHash)
        {
            Debug{} << "Index hash does not match: cached=" << cacheIndicesHash << ", actual=" << indicesHash;
            return {};
        }

        // copies all data into buffer
        // TODO: Is this inefficient?
        std::vector<uint8_t> buffer(std::istreambuf_iterator<char>(stream), {});

        return buffer;
    }
}

Mesh::Mesh(const std::string& filename, const std::shared_ptr<Context>& ctx)
 : m_ctx{ctx}
 , m_filename{filename}
{
}

Mesh::Mesh(sl::Mesh && other) = default;

Mesh::~Mesh()
{
}

void Mesh::load(std::size_t maxPhysicsTriangles, bool visual, bool physics)
{
    // If we don't load the visuals, hide Magnum importer warnings
    std::stringstream warnings;
    Warning redirect(visual ? &std::cerr : &warnings);

    openFile();

    if(physics)
        loadPhysics(maxPhysicsTriangles);

    if(visual)
        loadVisual();
}

void Mesh::openFile()
{
    if(m_opened)
        return;

    Corrade::PluginManager::Manager<Magnum::Trade::AbstractImporter> manager(m_ctx->importerPluginPath());
    Pointer<Magnum::Trade::AbstractImporter> importer;

    bool haveTinyGltf = false;

    // Load a scene importer plugin
    if(Utility::String::endsWith(m_filename, ".gltf") || Utility::String::endsWith(m_filename, ".glb"))
    {
        importer = manager.loadAndInstantiate("TinyGltfImporter");
        if(!importer)
            std::abort();

        haveTinyGltf = true;
    }
    else
    {
        importer = manager.loadAndInstantiate("AssimpImporter");
        if(!importer)
            std::abort();

        // Set up postprocess options if using AssimpImporter
        auto group = importer->configuration().group("postprocess");
        if(group)
        {
            group->setValue("JoinIdenticalVertices", true);
            group->setValue("Triangulate", true);
            group->setValue("GenSmoothNormals", true);
            group->setValue("PreTransformVertices", true);
            group->setValue("SortByPType", true);
            group->setValue("GenUVCoords", true);
            group->setValue("TransformUVCoords", true);
        }
    }

    // Load file
    {
        if(!importer->openFile(m_filename))
        {
            importer.reset();
            throw LoadException("Could not load file: " + m_filename);
        }
    }

    // Load pretransform, if available
    loadPretransform(m_filename + ".pretransform");

    // Load scene data
    if(importer->defaultScene() != -1)
    {
        m_sceneData = importer->scene(importer->defaultScene());
    }

    // Load objects
    m_objectData = ObjectDataArray{importer->object3DCount()};
    for(UnsignedInt i = 0; i < importer->object3DCount(); ++i)
        m_objectData[i] = importer->object3D(i);

    // Load meshes
    m_meshData = Array<Optional<Magnum::Trade::MeshData>>{importer->meshCount()};
    m_meshFlags = Containers::Array<MeshFlags>{m_meshData.size()};
    for(UnsignedInt i = 0; i < importer->meshCount(); ++i)
    {
        auto mesh = importer->mesh(i);

        if(!mesh)
            continue;

        if(mesh->primitive() != MeshPrimitive::Triangles || !mesh->isIndexed())
        {
            Warning{} << "Ignoring non-triangle (or non-indexed) sub-mesh" << i << "/" << importer->meshCount();
        }

        // All the following code makes the assumption that the mesh has the
        // following attributes: Position, Normal, Color, VertexIndex
        // So make sure our meshData has these fields.

        // Vertex indices start at 1
        // FIXME: This is useless for multi-object meshes.
        Array<UnsignedInt> vertexIndex(mesh->vertexCount());
        for(std::size_t j = 0; j < vertexIndex.size(); ++j)
            vertexIndex[j] = j + 1;

        Array<Trade::MeshAttributeData> extraAttributes{Containers::InPlaceInit, {
            Trade::MeshAttributeData{Trade::MeshAttribute::ObjectId, Containers::arrayView(vertexIndex)}
        }};

        Array<Color4> white;
        if(mesh->hasAttribute(Trade::MeshAttribute::Color))
            m_meshFlags[i] |= MeshFlag::HasVertexColors;
        else
        {
            white = Array<Color4>{Containers::DirectInit, mesh->vertexCount(), 1.0f, 1.0f, 1.0f, 1.0f};

            Containers::arrayAppend(extraAttributes,
                Trade::MeshAttributeData{Trade::MeshAttribute::Color, Containers::arrayView(white)}
            );
        }

        // For TinyGltf, we can load tangents as well (will hopefully soon be available in the Magnum importer)
        Array<Vector3> tangents;
        if(haveTinyGltf)
        {
            if(auto tangentData = extractTangents(*importer, *mesh))
            {
                tangents = std::move(*tangentData);
                Containers::arrayAppend(extraAttributes,
                    Trade::MeshAttributeData{Trade::MeshAttribute::Tangent, Containers::arrayView(tangents)}
                );
            }
        }

        auto interleavedMesh = MeshTools::interleave(std::move(*mesh), extraAttributes);

        // Make sure everything is in the format we expect
        if(interleavedMesh.attributeFormat(Trade::MeshAttribute::Position) != VertexFormat::Vector3)
        {
            Warning{} << "Unsupported vertex format" << interleavedMesh.attributeFormat(Trade::MeshAttribute::Position);
            continue;
        }

        if(interleavedMesh.attributeFormat(Trade::MeshAttribute::Normal) != VertexFormat::Vector3)
        {
            Warning{} << "Unsupported normal format" << interleavedMesh.attributeFormat(Trade::MeshAttribute::Normal);
            continue;
        }

        if(interleavedMesh.attributeFormat(Trade::MeshAttribute::Color) != VertexFormat::Vector4)
        {
            Warning{} << "Unsupported color format" << interleavedMesh.attributeFormat(Trade::MeshAttribute::Color);
            continue;
        }

        if(interleavedMesh.attributeFormat(Trade::MeshAttribute::ObjectId) != VertexFormat::UnsignedInt)
        {
            Warning{} << "Unsupported vertex ID format" << interleavedMesh.attributeFormat(Trade::MeshAttribute::ObjectId);
            continue;
        }

        m_meshData[i] = std::move(interleavedMesh);
    }

    // Load textures
    m_textureData = Array<Optional<Magnum::Trade::TextureData>>{importer->textureCount()};
    for(UnsignedInt i = 0; i < importer->textureCount(); ++i)
        m_textureData[i] = importer->texture(i);

    // Load images
    m_imageData = ImageDataArray{importer->image2DCount()};
    for(UnsignedInt i = 0; i < importer->image2DCount(); ++i)
        m_imageData[i] = importer->image2D(i);

    // Load materials.
    m_materials = MaterialArray{importer->materialCount()};
    for(UnsignedInt i = 0; i != importer->materialCount(); ++i)
    {
        auto materialData = importer->material(i);
        if(!materialData || materialData->type() != Trade::MaterialType::Phong)
        {
            Warning{} << "Cannot load material, skipping";
            continue;
        }

        m_materials[i] = PBRMaterialData::parse(
            *static_cast<Trade::PhongMaterialData*>(materialData.get()),
            haveTinyGltf
        );
    }

    // Compute bounding box
    {
        // Inspect the scene if available
        if(m_sceneData)
        {
            // Recursively inspect all children
            for(UnsignedInt objectId : m_sceneData->children3D())
                updateBoundingBox(Matrix4{}, objectId);
        }
        else if(!m_meshData.empty() && m_meshData[0])
        {
            updateBoundingBox(Matrix4{}, 0);
        }
        else
            Warning{} << "Could not estimate bounding box";
    }

    m_opened = true;
}

void Mesh::loadPhysics(std::size_t maxPhysicsTriangles)
{
    if(m_physicsLoaded)
        return;

    if(m_meshData.empty())
        throw std::runtime_error{"No mesh found"};

    // Simplify meshes if possible
    m_physXBuffers = CookedPhysXMeshArray{m_meshData.size()};
    m_physXMeshes = PhysXMeshArray{m_meshData.size()};
    for(UnsignedInt i = 0; i != m_meshData.size(); ++i)
    {
        auto& meshData = m_meshData[i];
        if(!meshData || meshData->primitive() != MeshPrimitive::Triangles)
        {
            Warning{} << "Cannot load the mesh, skipping";
            continue;
        }

        Corrade::Containers::Optional<PhysXOutputBuffer> buf;

        // We want to cache the simplified & cooked PhysX mesh.
        std::string cacheFile = Corrade::Utility::formatString("{}.mesh{}", m_filename, i);

        auto buffer = readCacheFile(cacheFile, m_filename, *meshData);

        if(buffer)
        {
            buf = PhysXOutputBuffer{std::move(*buffer)};
        }
        else
        {
            Debug{} << "Simplifying mesh and writing cache file...";
            Array<Vector3> vertices = meshData->positions3DAsArray();
            Array<UnsignedInt> indices = meshData->indicesAsArray();

            if(indices.size()/3 > maxPhysicsTriangles)
            {
                // simplify in-place
                mesh_tools::QuadricEdgeSimplification<Magnum::Vector3> simplification{
                    indices, vertices
                };

                simplification.simplify(maxPhysicsTriangles);
            }

            buf = cookForPhysX(m_ctx->physxCooking(), vertices, indices);

            os::AtomicFileStream ostream(cacheFile);

            // Write hashes
            MeshHash::Digest vertexHash = hashArray(meshData->positions3DAsArray());
            vertexHash.byteArray();
            ostream.write(vertexHash.byteArray(), MeshHash::DigestSize);

            MeshHash::Digest indicesHash = hashArray(meshData->indicesAsArray());
            ostream.write(indicesHash.byteArray(), MeshHash::DigestSize);

            ostream.write(reinterpret_cast<char*>(buf->data()), buf->size());
        }

        if(buf)
        {
            physx::PxDefaultMemoryInputData stream(buf->data(), buf->size());
            m_physXMeshes[i] = PhysXHolder<physx::PxConvexMesh>{
                m_ctx->physxPhysics().createConvexMesh(stream)
            };
        }

        m_physXBuffers[i] = std::move(buf);
    }

    m_physicsLoaded = true;
}

void Mesh::loadVisual()
{
    if(m_visualLoaded)
        return;

    if(m_meshData.empty())
        throw std::runtime_error{"No mesh found"};

    // Load all textures. Textures that fail to load will be NullOpt.
    m_textures = Containers::Array<Containers::Optional<GL::Texture2D>>{m_textureData.size()};
    for(UnsignedInt i = 0; i != m_textureData.size(); ++i)
    {
        const auto& textureData = m_textureData[i];
        if(!textureData || textureData->type() != Trade::TextureData::Type::Texture2D)
        {
            Warning{} << "Cannot load texture properties, skipping";
            continue;
        }

        const auto& imageData = m_imageData[textureData->image()];
        GL::TextureFormat format;
        if(imageData && imageData->format() == PixelFormat::RGB8Unorm)
            format = GL::TextureFormat::RGB8;
        else if(imageData && imageData->format() == PixelFormat::RGBA8Unorm)
            format = GL::TextureFormat::RGBA8;
        else
        {
            Warning{} << "Cannot load texture image, skipping";
            continue;
        }

        // Configure the texture
        GL::Texture2D texture;
        texture
            .setMagnificationFilter(textureData->magnificationFilter())
            .setMinificationFilter(textureData->minificationFilter(), textureData->mipmapFilter())
            .setWrapping(textureData->wrapping().xy())
            .setStorage(Math::log2(imageData->size().max()) + 1, format, imageData->size())
            .setSubImage(0, {}, *imageData)
            .generateMipmap();

        m_textures[i] = std::move(texture);
    }

    // Load all meshes. Meshes that fail to load will be NullOpt.
    m_meshes = MeshArray{m_meshData.size()};
    m_vertexBuffers = Array<GL::Buffer>{m_meshData.size()};
    m_indexBuffers = Array<GL::Buffer>{m_meshData.size()};
    for(UnsignedInt i = 0; i != m_meshData.size(); ++i)
    {
        const auto& meshData = m_meshData[i];
        if(!meshData || !meshData->hasAttribute(Trade::MeshAttribute::Normal) || meshData->primitive() != MeshPrimitive::Triangles)
        {
            Warning{} << "Cannot load the mesh, skipping";
            continue;
        }

        m_indexBuffers[i].setData(meshData->indexData(), GL::BufferUsage::StaticDraw);
        m_vertexBuffers[i].setData(meshData->vertexData(), GL::BufferUsage::StaticDraw);
        m_meshes[i] = std::make_shared<GL::Mesh>(
            MeshTools::compile(*meshData, m_indexBuffers[i], m_vertexBuffers[i])
        );
        m_meshes[i]->addVertexBuffer(m_vertexBuffers[i],
            meshData->attributeOffset(Trade::MeshAttribute::ObjectId),
            meshData->attributeStride(Trade::MeshAttribute::ObjectId),
            RenderShader::VertexIndex{}
        );
        if(meshData->hasAttribute(Trade::MeshAttribute::Tangent))
        {
            m_meshes[i]->addVertexBuffer(m_vertexBuffers[i],
                meshData->attributeOffset(Trade::MeshAttribute::Tangent),
                meshData->attributeStride(Trade::MeshAttribute::Tangent),
                Shaders::Generic3D::Tangent{}
            );
        }
    }

    // Nobody needs these anymore
    m_textureData = {};
    m_imageData = {};

    m_visualLoaded = true;
}

void Mesh::updateVertexPositions(
    const Corrade::Containers::ArrayView<int>& verticesIndex,
    const Corrade::Containers::ArrayView<Magnum::Vector3>& positionsUpdate
)
{
    updateVertexPositionsAndColors(verticesIndex, positionsUpdate, {});
}

void Mesh::updateVertexColors(
    const Corrade::Containers::ArrayView<int>& verticesIndex,
    const Corrade::Containers::ArrayView<Magnum::Color4>& colorsUpdate
)
{
    updateVertexPositionsAndColors(verticesIndex, {}, colorsUpdate);
}

void Mesh::recomputeNormals()
{
    if(m_meshData.size() != 1)
        throw Exception{"Mesh::recomputeNormals() assumes a single sub-mesh"};

    const auto& vertices = meshPoints();
    auto numVertices = vertices.size();

    const auto& indices = meshFaces();
    auto numIndices = indices.size();

    std::vector<std::vector<int>> vertexFacesMap(numVertices); // Faces a vertex is associated with.
    std::vector<float> facesArea(numIndices/3);
    std::vector<Vector3> facesNormal(numIndices/3);

    for(std::size_t i = 0; i < numIndices; i+=3)
    {
        auto face = i / 3;
        auto& vertexOne = indices[i ];
        auto& vertexTwo = indices[i+1];
        auto& vertexThree = indices[i+2];
        vertexFacesMap[vertexOne].push_back(face);
        vertexFacesMap[vertexTwo].push_back(face);
        vertexFacesMap[vertexThree].push_back(face);

        // area of the face
        Vector3 v1 = vertices[vertexOne];
        Vector3 v2 = vertices[vertexTwo];
        Vector3 v3 = vertices[vertexThree];

        auto v1v2 = v1 - v2;
        auto v1v3 = v1 - v3;

        Vector3 crossProduct = Math::cross(v1v2, v1v3);

        float area = crossProduct.length();
        facesArea[face] = area;

        Vector3 normal = crossProduct.normalized();

        facesNormal[face] = normal;
    }

    // compute new normals weight
    auto normals = meshNormals();
    for(std::size_t i = 0; i < numVertices; ++i)
    {
        Vector3 normal = Vector3(0., 0., 0.);

        for(const auto& face : vertexFacesMap[i])
            normal += facesNormal[face] * facesArea[face];

        normal = normal.normalized();

        normals[i] = normal;
    }
}

void Mesh::recompileMesh()
{
    if(m_meshData.size() != 1)
        throw Exception{"Mesh::recompileMesh() assumes a single sub-mesh"};

    m_vertexBuffers.front().setData(
        m_meshData.front()->vertexData(), GL::BufferUsage::DynamicDraw
    );
}

void Mesh::updateVertexPositionsAndColors(
    const Corrade::Containers::ArrayView<int>& verticesIndex,
    const Corrade::Containers::ArrayView<Magnum::Vector3>& positionsUpdate,
    const Corrade::Containers::ArrayView<Magnum::Color4>& colorsUpdate
)
{
    if(m_meshData.size() != 1)
        throw Exception{"Mesh::updateVertexPositionsAndColors() assumes a single sub-mesh"};

    // update
    if(!positionsUpdate.empty())
    {
        auto vertices = meshPoints();
        for(std::size_t vi = 0; vi < verticesIndex.size(); ++vi)
        {
            Vector3& point = vertices[verticesIndex[vi] - 1];
            Vector3 update = positionsUpdate[vi];
            point = point + update;
        }

        // recompute normals
        recomputeNormals();
    }

    if(!colorsUpdate.empty())
    {
        auto colors = meshColors();
        for(std::size_t vi = 0; vi < verticesIndex.size(); ++vi)
        {
            Color4& color = colors[verticesIndex[vi] - 1];
            Color4 cUpdate = colorsUpdate[vi];
            color = color + cUpdate;
        }
    }

    recompileMesh();
}

void Mesh::setVertexPositions(
    const Corrade::Containers::ArrayView<Magnum::Vector3>& newVertices
)
{
    if(m_meshData.size() != 1)
        throw Exception{"Mesh::setVertexPositions() assumes a single sub-mesh"};

    auto vertices = meshPoints();

    if(vertices.size() != newVertices.size())
        throw std::invalid_argument{"Number of new vertices should match the existing mesh vertices"};

    for(std::size_t i = 0; i < vertices.size(); ++i)
        vertices[i] = newVertices[i];

    recomputeNormals();
    recompileMesh();
}

void Mesh::setVertexColors(
    const Corrade::Containers::ArrayView<Magnum::Color4>& newColors
)
{
    if(m_meshData.size() != 1)
        throw Exception{"Mesh::setVertexColors() assumes a single sub-mesh"};

    auto& meshData = m_meshData.front();

    if(!meshData->hasAttribute(Trade::MeshAttribute::Color))
    {
        throw Exception("MeshData does not contain colors attribute."
            "This could happend if the mesh file does not contain per vertex coloring.");
    }

    auto colors = meshColors();

    if(colors.size() != newColors.size())
        throw std::invalid_argument{"Number of new vertices should match the existing mesh vertices for vertex color update"};

    for(std::size_t i = 0; i < colors.size(); ++i)
        colors[i] = newColors[i];

    recompileMesh();
}

void Mesh::loadPretransform(const std::string& filename)
{
    std::ifstream stream(filename);

    // If there is no file, we silently ignore that and use identity
    // pretransform.
    if(!stream)
        return;

    Matrix4 pretransform;
    for(int i = 0; i < 4; ++i)
    {
        std::string line;
        if(!std::getline(stream, line))
        {
            Error{} << "Short pretransform file" << filename;
            return;
        }

        std::stringstream ss(line);
        ss.imbue(std::locale::classic());

        for(int j = 0; j < 4; ++j)
        {
            if(!(ss >> pretransform[j][i]))
            {
                Error{} << "Could not read number from pretransform file" << filename;
                return;
            }
        }
    }

    setPretransform(pretransform);
}

namespace
{
    std::shared_ptr<Mesh> loadHelper(
        const std::shared_ptr<Context>& ctx,
        const std::string& filename,
        bool visual, bool physics,
        std::size_t maxPhysicsTriangles)
    {
        // If we don't load the visuals, hide Magnum importer warnings
        std::stringstream warnings;
        Warning redirect(visual ? &std::cerr : &warnings);

        auto mesh = std::make_shared<Mesh>(filename, ctx);

        mesh->openFile();

        if(physics)
            mesh->loadPhysics(maxPhysicsTriangles);

        return mesh;
    }
}

std::vector<std::shared_ptr<Mesh>> Mesh::loadThreaded(
    const std::shared_ptr<Context>& ctx,
    const std::vector<std::string>& filenames,
    bool visual, bool physics,
    std::size_t maxPhysicsTriangles)
{
    using Future = std::future<std::shared_ptr<sl::Mesh>>;

    ctpl::thread_pool pool(std::thread::hardware_concurrency());

    std::vector<Future> results;
    for(const auto& filename : filenames)
    {
        results.push_back(pool.push(std::bind(&loadHelper,
            ctx, filename, visual, physics, maxPhysicsTriangles
        )));
    }

    std::vector<std::shared_ptr<sl::Mesh>> ret;
    for(auto& future : results)
    {
        auto mesh = future.get(); // may throw if we had a load error

        if(visual)
            mesh->loadVisual();

        ret.push_back(std::move(mesh));
    }

    return ret;
}

void Mesh::updateBoundingBox(const Magnum::Matrix4& parentTransform, unsigned int meshObjectIdx)
{
    const auto& objectData = m_objectData[meshObjectIdx];
    if(!objectData)
        return;

    Magnum::Matrix4 transform = parentTransform * objectData->transformation();

    if(objectData->instanceType() == Trade::ObjectInstanceType3D::Mesh && objectData->instance() != -1 && m_meshData[objectData->instance()])
    {
        for(const auto& point : meshPoints())
        {
            auto transformed = transform.transformPoint(point);

            m_bbox.min().x() = std::min(m_bbox.min().x(), transformed.x());
            m_bbox.min().y() = std::min(m_bbox.min().y(), transformed.y());
            m_bbox.min().z() = std::min(m_bbox.min().z(), transformed.z());

            m_bbox.max().x() = std::max(m_bbox.max().x(), transformed.x());
            m_bbox.max().y() = std::max(m_bbox.max().y(), transformed.y());
            m_bbox.max().z() = std::max(m_bbox.max().z(), transformed.z());
        }
    }

    // Recurse
    for(std::size_t idx : objectData->children())
        updateBoundingBox(transform, idx);
}

void Mesh::centerBBox()
{
    m_pretransformRigid.translation() = -m_pretransformRigid.rotationScaling() * m_bbox.center();
    updatePretransform();
}

void Mesh::scaleToBBoxDiagonal(float targetDiagonal, Scale mode)
{
    float diagonal = m_bbox.size().length();

    float scale = targetDiagonal / diagonal;

    switch(mode)
    {
        case Scale::Exact:
            m_scale = scale;
            break;
        case Scale::OrderOfMagnitude:
            m_scale = std::pow(10, std::round(std::log10(scale)));
            break;
    }

    updatePretransform();
}

void Mesh::updatePretransform()
{
    m_pretransform = Matrix4::scaling(Vector3(m_scale)) * m_pretransformRigid;
}

void Mesh::setPretransform(const Magnum::Matrix4& m)
{
    // We need to separate the transformation matrix into a homogenous scaling
    // and a rotation+translation.

    Matrix3x3 u{Math::NoInit};
    Vector3 w{Math::NoInit};
    Matrix3x3 v{Math::NoInit};
    std::tie(u, w, v) = Math::Algorithms::svd(m.rotationScaling());

    float minScale = w.min();
    float maxScale = w.max();

    if(maxScale - minScale > 1e-5f)
    {
        Error{} << "Scaling is not uniform:" << w;
        throw std::invalid_argument("Scaling is not uniform");
    }

    m_scale = (maxScale + minScale) / 2.0f;
    m_pretransformRigid = Matrix4::from(u*v.transposed(), (1.0f / m_scale) * m.translation());

    updatePretransform();
}

Magnum::Range3D Mesh::bbox() const
{
    Vector3 lower = m_pretransform.transformPoint(m_bbox.min());
    Vector3 upper = m_pretransform.transformPoint(m_bbox.max());

    return Range3D{lower, upper};
}

void Mesh::setClassIndex(unsigned int index)
{
    if(index > std::numeric_limits<uint16_t>::max())
        throw std::invalid_argument("Mesh::setClassIndex(): out of range");

    m_classIndex = index;
}

void Mesh::serialize(Corrade::Utility::ConfigurationGroup& group)
{
    group.setValue("filename", m_filename);
    group.setValue("classIndex", m_classIndex);
    group.setValue("scale", m_scale);
    group.setValue("rigidPretransform", m_pretransformRigid);
}

void Mesh::deserialize(const Corrade::Utility::ConfigurationGroup& group)
{
    m_filename = group.value("filename");

    openFile();

    if(group.hasValue("classIndex"))
        setClassIndex(group.value<unsigned int>("classIndex"));

    if(group.hasValue("scale"))
        m_scale = group.value<float>("scale");

    if(group.hasValue("rigidPretransform"))
        m_pretransformRigid = group.value<Magnum::Matrix4>("rigidPretransform");
}

}

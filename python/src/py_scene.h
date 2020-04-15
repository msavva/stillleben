// sl::Scene binding
// Author: Max Schwarz <max.schwarz@ais.uni-bonn.de>

#ifndef SL_PY_SCENE_H
#define SL_PY_SCENE_H

#include <torch/extension.h>

namespace sl
{
namespace python
{
namespace Scene
{

void init(py::module& m);

}
}
}

#endif

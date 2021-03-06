#!/usr/bin/env python

import pathlib
SL_PATH = pathlib.Path(__file__).parent.parent.absolute()

def retrieve_ibl():
    import requests, zipfile, io

    print('Downloading IBL light map...')
    url = 'http://www.hdrlabs.com/sibl/archive/downloads/Circus_Backstage.zip'
    r = requests.get(url)
    z = zipfile.ZipFile(io.BytesIO(r.content))
    z.extractall(SL_PATH / 'examples')
    print('done.')

if not (SL_PATH / 'examples' / 'Circus_Backstage').exists():
    retrieve_ibl()

import stillleben as sl
import torch
import random
from PIL import Image

sl.init() # use sl.init_cuda() for CUDA interop

# Load a mesh (the constructor accepts pathlib paths)
mesh = sl.Mesh(SL_PATH / 'tests' / 'stanford_bunny' / 'scene.gltf')

# Meshes can come in strange dimensions - rescale to something reasonable
mesh.scale_to_bbox_diagonal(0.5)

# Create a scene with a few bunnies
scene = sl.Scene((1920,1080))

for i in range(20):
    obj = sl.Object(mesh)

    # Override the metallic/roughness parameters so that it gets interesting
    obj.metallic = random.random()
    obj.roughness = random.random()
    scene.add_object(obj)

# Let them fall in a heap
scene.simulate_tabletop_scene()

# Setup lighting
scene.light_map = sl.LightMap(SL_PATH / 'examples' / 'Circus_Backstage' / 'Circus_Backstage.ibl')

# Display a plane & set background color
scene.background_plane_size = torch.tensor([3.0, 3.0])
scene.background_color = torch.tensor([0.1, 0.1, 0.1, 1.0])

# Display interactive viewer
sl.view(scene)

# Render a frame
renderer = sl.RenderPass()
result = renderer.render(scene)
print('Resulting RGB frame:', result.rgb().shape)
print('Resulting segmentation frame:', result.instance_index().shape)

# Save as JPEG
Image.fromarray(result.rgb()[:,:,:3].cpu().numpy()).save('rgb.jpeg')

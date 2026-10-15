import bpy
import bmesh
import os
import subprocess
import shutil

output_dir = r"C:\Users\Dario\Documents\VMT-Previewer\models_build"
os.makedirs(output_dir, exist_ok=True)

studiomdl_exe = r"E:\SteamLibrary\steamapps\common\Source SDK Base 2013 Singleplayer\bin\studiomdl.exe"
game_dir = r"E:\SteamLibrary\steamapps\common\Source SDK Base 2013 Singleplayer\hl2"
target_models_dir = r"C:\Users\Dario\Documents\VMT-Previewer\models"

def clear_all():
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete()

def export_mesh_to_smd(obj, filepath, mat_name="preview_material"):
    mesh = obj.to_mesh()
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bmesh.ops.triangulate(bm, faces=bm.faces)
    bm.to_mesh(mesh)
    bm.free()
    
    uv_layer = mesh.uv_layers.active.data if mesh.uv_layers.active else None

    tri_count = len(mesh.polygons)
    with open(filepath, "w") as f:
        f.write("version 1\n")
        f.write("nodes\n")
        f.write('0 "root" -1\n')
        f.write("end\n")
        f.write("skeleton\n")
        f.write("time 0\n")
        f.write("0 0.000000 0.000000 0.000000 0.000000 0.000000 0.000000\n")
        f.write("end\n")
        f.write("triangles\n")
        
        for poly in mesh.polygons:
            for loop_idx in poly.loop_indices:
                loop = mesh.loops[loop_idx]
                vert = mesh.vertices[loop.vertex_index]
                
                x = vert.co.x
                y = vert.co.y
                z = vert.co.z
                
                nx = vert.normal.x
                ny = vert.normal.y
                nz = vert.normal.z
                
                if uv_layer:
                    u = uv_layer[loop_idx].uv.x
                    v = uv_layer[loop_idx].uv.y
                else:
                    u = 0.0
                    v = 0.0
                
                f.write(f"{mat_name}\n")
                f.write(f"0 {x:.6f} {y:.6f} {z:.6f} {nx:.6f} {ny:.6f} {nz:.6f} {u:.6f} {v:.6f}\n")
                
        f.write("end\n")
    obj.to_mesh_clear()
    print(f"Exported {filepath} with {tri_count} triangles.")

def compile_model(model_name, smd_filename):
    qc_path = os.path.join(output_dir, f"{model_name}.qc")
    with open(qc_path, "w") as f:
        f.write(f'$modelname "{model_name}.mdl"\n')
        f.write(f'$body "body" "{smd_filename}"\n')
        f.write('$staticprop\n')
        f.write('$surfaceprop "default"\n')
        f.write('$cdmaterials ""\n')
        f.write(f'$sequence "idle" "{smd_filename}"\n')
    
    cmd = [studiomdl_exe, "-nop4", "-game", game_dir, qc_path]
    res = subprocess.run(cmd, cwd=output_dir, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"Error compiling {model_name}: {res.stderr}\n{res.stdout}")
        return False
    print(f"Compiled {model_name} successfully!")
    
    # studiomdl outputs to <game_dir>/models/<model_name>.*
    compiled_base = os.path.join(game_dir, "models", model_name)
    for ext in [".mdl", ".vvd", ".dx90.vtx"]:
        src = compiled_base + ext
        dst = os.path.join(target_models_dir, f"{model_name}{ext}")
        if os.path.exists(src):
            shutil.copy2(src, dst)
            print(f"Copied {src} -> {dst}")
        else:
            print(f"WARNING: Missing {src}")
    return True

# 1. Cube
clear_all()
bpy.ops.mesh.primitive_cube_add(size=2.0, location=(0, 0, 0))
cube_obj = bpy.context.active_object
cube_obj.name = "Cube"
cube_smd = os.path.join(output_dir, "cube_model.smd")
export_mesh_to_smd(cube_obj, cube_smd)
compile_model("cube_model", "cube_model.smd")

# 2. Sphere
clear_all()
bpy.ops.mesh.primitive_uv_sphere_add(radius=1.0, location=(0, 0, 0))
sphere_obj = bpy.context.active_object
sphere_obj.name = "Sphere"
sphere_smd = os.path.join(output_dir, "shpere_model.smd")
export_mesh_to_smd(sphere_obj, sphere_smd)
compile_model("shpere_model", "shpere_model.smd")
compile_model("sphere_model", "shpere_model.smd")

# 3. Cylinder
clear_all()
bpy.ops.mesh.primitive_cylinder_add(radius=1.0, depth=2.0, location=(0, 0, 0))
cyl_obj = bpy.context.active_object
cyl_obj.name = "Cylinder"
cyl_smd = os.path.join(output_dir, "cylinder_model.smd")
export_mesh_to_smd(cyl_obj, cyl_smd)
compile_model("cylinder_model", "cylinder_model.smd")

print("All models compiled and copied successfully!")

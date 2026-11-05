"""Run in UE's Python console with /Game/Tests/PRTGI_RegionalValidation open.

Configures three rooms and starts the PC runtime solver. Does not save the map
or write precomputed GI. Move Classic_Box_Small_Cube, then inspect Runtime.Status.
"""
import unreal

world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
if world.get_path_name().split(".")[0] != "/Game/Tests/PRTGI_RegionalValidation":
    raise RuntimeError("Open /Game/Tests/PRTGI_RegionalValidation before running this script.")

actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
groups = {
    "PRT_ClassicBox": "Classic_Box_",
    "PRT_ColorBox": "Color_Box_",
    "PRT_ShinyBox": "Shiny_Box_",
}
volumes = [a for a in actors if isinstance(a, unreal.PRTProbeVolume)]
meshes = [a for a in actors if isinstance(a, unreal.StaticMeshActor)]
for label in groups:
    if sum(v.get_actor_label() == label for v in volumes) != 1:
        raise RuntimeError("Missing or duplicate volume: " + label)

for volume in volumes:
    label = volume.get_actor_label()
    volume.set_editor_property("enabled", label in groups)
    if label in groups:
        members = [a for a in meshes if a.get_actor_label().startswith(groups[label])
                   or a.get_actor_label().startswith("Environment_")]
        volume.set_editor_property("runtime_included_actors", members)
        volume.set_editor_property("runtime_participant_tags", [])
        unreal.log("{}: {} participants".format(label, len(members)))

commands = [
    "r.PRTGI.Runtime 0", "PRTGI.SetMode PRT",
    "r.PRTGI.Runtime.Rooms 1", "r.PRTGI.Runtime.SurfelCache 1",
    "r.PRTGI.Runtime.ReuseTransport 1", "r.PRTGI.Runtime.RegionalTransport 1",
    "r.PRTGI.Runtime.KeepLightingWhileUpdating 1", "r.PRTGI.Runtime.KeepLightingOnGeometryChange 1",
    "r.PRTGI.Runtime.RegionalIrradiance 1", "r.PRTGI.Runtime.IrradianceBudgetMB 16",
    "r.PRTGI.Runtime.ProgressiveProbes 1",
    "r.PRTGI.Runtime.TransportBudgetMB 64", "r.PRTGI.Runtime.CacheIterations 5",
    "r.PRTGI.Runtime.CacheRays 64", "r.PRTGI.Runtime.CacheBatch 256",
    "r.PRTGI.Runtime.CacheBatchesPerSubmit 8", "r.PRTGI.Runtime.CacheRelaxation 1",
    "r.PRTGI.Runtime.Rays 256", "r.PRTGI.Runtime.ProbesPerBatch 32",
    "PRTGI.Runtime.Prepare", "r.PRTGI.Runtime 1",
]
for command in commands:
    unreal.SystemLibrary.execute_console_command(world, command)
unreal.log("Regional validation started. Wait for all three rooms to publish before moving furniture.")

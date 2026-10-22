using System;
using Resonate.CSharp;
using Resonate.CSharp.Bindings;

[assembly: ResonateBehaviour]

namespace Resonate.Demo.Game;

/* A sample behaviour: every second it records a box into the frame's command
   buffer, and the engine plays it at the sync point. The states live in the
   world (components), never in this assembly. */
public static unsafe class GameBehaviour
{
    private const uint kSpawnEveryFrames = 60U;
    private const uint kCubeMesh = 1U;

    [UpdateIn(ResonateStage.Update)]
    public static void Tick(SystemContext* frame)
    {
        if (frame->FrameIndex == 0 || frame->FrameIndex % kSpawnEveryFrames != 0)
        {
            return;
        }

        HostApi* api = (HostApi*)frame->HostApi;
        ulong entity = 0;
        if (api->Spawn((IntPtr)api, &entity) != 0)
        {
            return;
        }

        Resonate.Hierarchy.LocalTransform transform = default;
        transform.Position = new Resonate.CSharp.Math.Vec3
        {
            X = -6.0F + (float)(frame->FrameIndex / kSpawnEveryFrames % 9U) * 1.5F,
            Y = 0.5F,
            Z = -2.0F,
        };
        /* A C# struct defaults to a zero quaternion; the identity is w = 1. */
        transform.Rotation.W = 1.0F;
        transform.Scale = new Resonate.CSharp.Math.Vec3
        {
            X = 1.0F,
            Y = 1.0F,
            Z = 1.0F,
        };
        Add(api, entity, Resonate.Hierarchy.LocalTransform.SchemaNameUtf8, &transform);

        Resonate.Hierarchy.WorldTransform world = default;
        world.Matrix.M00 = 1.0F;
        world.Matrix.M11 = 1.0F;
        world.Matrix.M22 = 1.0F;
        world.Matrix.M33 = 1.0F;
        Add(api, entity, Resonate.Hierarchy.WorldTransform.SchemaNameUtf8, &world);

        Resonate.Render.MeshInstance mesh = default;
        mesh.Mesh = kCubeMesh;
        mesh.Color = new Resonate.CSharp.Math.Vec3
        {
            X = 0.4F,
            Y = 0.8F,
            Z = 0.5F,
        };
        Add(api, entity, Resonate.Render.MeshInstance.SchemaNameUtf8, &mesh);
    }

    private static int Add(HostApi* api, ulong entity, ReadOnlySpan<byte> name, void* data)
    {
        fixed (byte* component = name)
        {
            return api->AddComponent((IntPtr)api, entity, component, (IntPtr)data);
        }
    }
}

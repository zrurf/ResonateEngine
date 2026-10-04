# Resonate Engine

An open-source game engine. Modern C++, built with [xmake](https://xmake.io).
Licensed under Apache-2.0 with the LLVM exception (see [LICENSE](LICENSE)).

## Layout

```
packages/pal          platform abstraction: chrono, event, io, memory, sync, thread
packages/core         containers, memory, math, jobs
packages/ecs          the authoritative state: archetype storage, blobs, queries, commands
packages/hierarchy    the scene tree: parent/children components, tree commands, transform propagation
packages/module       plugin ABI: the C contract and the C++ authoring layer
packages/render       the render domain's interfaces: the target and the device
packages/window       window and input (SDL3), statically linked
packages/physics      the physics domain's interface
packages/audio        the audio domain's interface
packages/ui           the UI domain's interface
packages/runtime      frame loop, module host, the run's world, asset registry, application entry
packages/startup      the run the launcher and the editor share
packages/modules/*    the plugins themselves: physics, audio, render, ui
packages/editor       editor binary
packages/launcher     game binary
packages/tools/*      host tools: the schema compiler (C#) and, later, its siblings
content/*             content modules: schema declarations (.rschema), assets later
schema/               file-shape contracts: JSON Schema for the manifests, XSD for the schema language
scripts/              xmake task scripts (module-check, schema, format)
tests/                Catch2 v3
```

An interface belongs to the package that owns its domain, not to the plugin that
happens to implement it: `packages/render` declares the render target and the
render device, `packages/modules/render` implements the device. A second
implementation therefore depends on the interface package and never on the first
implementation. Where an implementation is not substitutable — the window — it
stays in the same package as its interface.

## Building

```sh
xmake f -m debug      # or -m release
xmake                 # libraries, plugins and binaries
xmake test            # also builds the test target, which xmake skips by default
xmake module-check    # also runs automatically before each build
xmake schema          # also runs automatically before a target that consumes generated types
xmake schema-lock     # records the accepted shape of every versioned type (the version locks)
xmake format          # clang-format over the tree
```

Requires xmake 3.1.1 or newer and, on Windows, Visual Studio 2026 for the
`clang-cl` toolchain. Building the schema compiler needs the .NET 11 SDK; only
targets that include generated schema types pull it in (`packages/tools/schemac`).

## Schema types

Component types are declared, not written by hand: a content module holds XML
declarations under `content/<module>/schemas/*.rschema`, and the build compiles
them into `build/gen/<module>/components.gen.h` — plain structs in the module's
namespace, their ECS traits, and the frozen layout as `static_assert`s — plus
`build/gen/<module>.schema.json`, the canonical metadata the editor and the
other language forms will be generated from. A schema error fails the build.

A declaration is `enum`, `bitmask`, `struct`, `component` or `node`; a field
names a built-in scalar, `entity`, `blob` (a handle to a variable-length buffer
in the ECS blob store), a core math type (`vec2`, `vec3`, `vec4`, `quat`,
`mat4`), or a type the same module declares. What a type is addressed by outside
C++ is its module-qualified name (`resonate.gameplay.Health`): two modules may
own a type of the same name and they stay distinct. A component above version 1
declares the migrations to it, and the owner implements each one — the generated
entry point takes its address, so a missing implementation fails to link
wherever the entry is used.

A version is a promise to whatever was saved under it, and the version lock
(`schemas.lock.json`, beside each module's declarations) is what keeps the
promise honest: the build fails when a version goes backwards, when a version's
shape changes without a bump, or when a versioned type appears or disappears —
anything the lock does not already record. `xmake schema-lock` records the
current shape on purpose; commit it with the change that needed it.

The compiler is a library (`Resonate.Tools.Schemac`) with a thin command-line
host (`packages/tools/schemac/cli`), so the editor will be able to compile and
check schemas in process rather than shelling out to the binary.

The shape of a declaration file itself is declared in
`schema/formats/schema/rschema.xsd`. The build hands it to the compiler, which
checks every declaration against it as well, so the contract an editor shows and
the checks the compiler makes cannot drift apart. Editors that validate from an
XSD (VS Code's XML extension) need the association configured once — there is no
in-document way to point at it, since the XML subset allows neither processing
instructions nor namespaces:

```json
"xml.fileAssociations": [
    { "pattern": "**/*.rschema", "systemId": "schema/formats/schema/rschema.xsd" }
]
```

## Scene tree

`packages/hierarchy` owns the engine's own content module: entities linked by
the `Parent`/`Children` components form the scene tree, edited through recorded
commands (`hierarchy::setParent`, `clearParent`, `destroyEntity`) that play at
the frame's sync points and are validated there — a cycle or a node past
`kMaxDepth` is refused with a report, and never applied. The propagation system
composes every node's `LocalTransform` into its `WorldTransform`, one depth
level at a time; `startup` registers it for the launcher and the editor.

Variable-length state lives outside the chunks in the ECS blob store
(`World::createBlob` and its siblings): a component keeps only the handle, and
the entity's blobs die with it. A `Children` list is the first user; inventories
and skeleton poses are what it is shaped for. A domain records its own
structural commands — the tree edits are the first — through
`CommandBuffer::record`, so law 2 holds without the ECS knowing what they mean.

The launcher and the editor both come up through `resonate::startup::run`. Plugins
are scanned in `plugins` beside the executable, or in the first ancestor that has
one so a build tree works; `--plugin-dir` and `RESONATE_PLUGIN_PATH` override it.

The window is a component of a run rather than a requirement of one. `--headless`
leaves it out entirely and publishes an *offscreen* canvas instead, which is what a
dedicated server or an offscreen renderer wants; `--title`, `--width` and
`--height` describe the frame size in either mode. A module draws on
`Resonate.Render.Canvas`, so the same render module attaches in both — and it
never links the window package or SDL to do it.

## Writing a module

```cpp
#include <resonate/module/module.hpp>

class PhysicsModule final : public resonate::Module
{
    resonate::Status onAttach(resonate::Host& host) override
    {
        window_ = host.query<ResonateWindow>();
        return window_ ? RESONATE_OK : RESONATE_E_MISSING;
    }

    void onDetach() override { window_ = {}; }

    resonate::CapabilityRef<ResonateWindow> window_;
};

RESONATE_DEFINE_MODULE(PhysicsModule, "resonate.physics", "Physics", "0.1.0")
```

A module has an `id` (reverse-DNS, unique, what everything looks it up by) and a
`name` (display only). It publishes capabilities and consumes the ones its
manifest declares. There is no event bus: requests are direct calls through a
resolved interface, notifications are pulled from a message stream, and
same-frame events use a signal owned by the service that raises them.

Both of those facilities are reached through the `Host` handle — `Signal`,
`MessageWriter`, `MessageReader` — and never through a symbol the host exports:
the host allocates what a module creates and reclaims whatever it leaves behind
at `onDetach`. A plugin therefore links the ABI headers and nothing else, which
is what lets it be built by a different compiler than the host.

## Module contracts

A module's contract is written in four places and reconciled on every build, so
a module that declares one dependency set and links another fails the build
rather than the load:

| Location | Read by |
| --- | --- |
| `resonate.module.toml` | the loader at startup, and editor tooling |
| `src/module_descriptor.cpp` | the module, in C++ |
| the capability header | whoever defines the capability |
| `packages/modules/xmake.lua` | the build, when it links the module |

Manifests are TOML. Their shape is declared in `schema/formats/module/` (JSON
Schema, picked up by taplo in editors) and enforced at build time by the
checker, which also asserts what a schema cannot see.

## Visibility

- `include/` is public and propagates to dependents.
- `src/` is private to its target.
- `packages/module` is two targets: `Resonate.Module.Abi` carries the
  plugin-facing headers, and `Resonate.Module` carries the host
  implementation plus `host/`, which is on no plugin's include path.
- `packages/render` is two as well: `Render.Abi` carries the interfaces a plugin
  links, and `Render` carries the host-side offscreen canvas. Every domain that a
  plugin implements splits the same way, so a plugin reaches an interface without
  linking an implementation or a platform library.
- `packages/core` is four: `Core.Memory`, `Core.Container`, `Core.Math` and
  `Core.Job`, so a package that needs only math does not link a container
  allocator it never calls.

## Naming

`RESONATE_` for macros, `Resonate` for C types, `resonate_` for C functions,
`resonate` for the C++ namespace. Module ids are lowercase reverse-DNS,
capability ids are PascalCase and hash to their 128-bit id with XXH3-128, so
renaming one is an ABI break. A C type is its capability id with the dots removed:
`Resonate.Render.Device` is declared as `ResonateRenderDevice`.

## License
[Apache-2.0 with the LLVM exception](LICENSE)
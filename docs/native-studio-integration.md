# Native Crowdy Studio integration

CrowdyCPP supplies the headless Studio state machine and narrow native
integration contracts. Engines own the window, panes, text editor, language
service, renderer, and input system.

## Editor boundary

Implement `crowdy::studio::ICrowdyStudioEditorAdapter`. The adapter receives
only:

- synchronized in-memory project, personal-library, and common-file buffers;
- the selected file and ordered open-file set;
- callbacks for project-buffer edits, diagnostics, open, close, and failure;
- relayout and disposal notifications.

`CrowdyStudioEditorBridge` connects those callbacks to the same
`CrowdyStudioController::updateFile`, `openFile`, `closeFile`, and
`setLocalDiagnostics` paths used by a human UI. Reference buffers are marked
read-only. The interface has no filesystem, shell, GraphQL, network, or
`CrowdyClient` handle.

```cpp
class EngineEditor final
    : public crowdy::studio::ICrowdyStudioEditorAdapter {
 public:
  crowdy::studio::CrowdyStudioEditorMode mode() const noexcept override;
  void setCallbacks(
      crowdy::studio::CrowdyStudioEditorCallbacks callbacks) override;
  void synchronize(
      const crowdy::studio::CrowdyStudioEditorSnapshot& snapshot) override;
  void relayout() override;
  void dispose() noexcept override;
};
```

Editor callbacks run on the engine thread chosen by the adapter. Invoke them
on the game/UI thread because `CrowdyStudioController` is single-threaded.
Callbacks retained after `dispose()` are fenced.

## Complete assembly

`CrowdyClient::createCrowdyStudioIntegration` constructs owned project and
runtime adapters, the Studio runtime/controller, the editor bridge and the
layout controller in dependency-safe order. The runtime is
`CrowdyStudioModRuntime` (0.50.0): the SERVER target builds and deploys as the
grid's ck-exec mod over `client.exec()`, Invoke calls a mod endpoint, and the
CLIENT target builds as that mod's CLIENT half, which is attached, consented to
as its author and handed to `clientRuntime` as the module
`exec().modClientArtifactBytes` checked against its digest. An Invoke waits
for its reply (up to 30 s) while draining the client's dispatcher, so call it
from the maintenance phase, like the other blocking runtime work. Since 0.34.0 there is no agent
in this assembly: the Studio agent is the in-browser DeepSeek Harness, which
CrowdyJS docks beside the web editor and a game can host in a dock of its own
(the Unreal SDK does). Its policy, consent, usage, live-modding check and model
list are on `client.crowdyStudioAgent()` (see [native agent API](native-agent-api.md)).

## A draft test from the game thread (0.86.0)

The assembly's runtime blocks, so it runs from the maintenance lane. A game
that runs a draft test or a live deploy from its game thread, as a Studio dock
does, uses `CrowdyStudioModRunner` instead: `client.createCrowdyStudioModRunner()`,
then `run({.scope = {appId, gridId}, .projectId = id, .expectedRevisionId = revision},
onDone, onProgress)`. It reads the project and refuses one past the expected
revision, builds that read's files (CLIENT half first), deploys and switches on
the SERVER half (or the mod a CLIENT-only project rides), attaches the CLIENT
half and consents to it as its author, and reports each target's build log,
diagnostics, digest and version. Every callback arrives from `client.poll()`.
It does not start the CLIENT half in an engine runtime: the attached half is
served to its author like any other, through `exec().gridClientMods` and
`modClientArtifactBytes`. See the README's "A Studio dock in a game".

`crowdy::studio::createCrowdyStudioStarterProjectAsync` creates a new project
with CrowdyJS's starter files without blocking, and
`CrowdyStudioController::createProject(CrowdyStudioNewProjectOptions)` does the
same from the maintenance lane through the runtime's mod starter.

```cpp
crowdy::studio::CrowdyStudioIntegrationOptions options;
options.studio = {.appId = appId, .gridId = gridId};
options.editor = std::make_shared<EngineEditor>();
options.clientRuntime = engineClientArtifactRuntime;
options.layoutStorage = engineSettingsStore;
options.playerHost = &enginePlayerHost;  // observation only; outlives assembly
options.crypto = engineCryptoOwner;    // shared ownership

auto studio =
    client.createCrowdyStudioIntegration(std::move(options));
studio->initialize();

while (running) {
  studio->poll();  // nonblocking platform callbacks
  if (engineStudioMaintenancePhase) {
    studio->runStudioMaintenance();  // save/HTTP/compile polling may block
  }
}
```

`poll()` pumps the client/platform dispatcher and nothing else. It never runs
Studio autosave, monitor HTTP, compile polling, or sleep.
`runStudioMaintenance()` is the explicit serialized maintenance lane: it drains
work queued with `schedule()` and then runs the controller's own maintenance
tick. Call it from a deliberately blocking engine phase, or from a worker only
when all Studio controller access is serialized onto that worker; never run it
concurrently with editor/controller access.

Keep `CrowdyClient` open while the assembly uses its shared HTTP/GraphQL
dispatcher. Destroying the client first remains memory-safe but closes those
shared transports. An externally injected raw crypto provider is not assumed
owned: pass `options.crypto` so the assembly retains it.

Lower-level hosts can call `CrowdyStudioIntegration::create` with owned
`ICrowdyStudioProjectProvider` and `ICrowdyStudioRuntime` implementations.
The existing direct constructors remain available.

[`examples/native_studio_shell.cpp`](../examples/native_studio_shell.cpp) is a
credential-free, engine-neutral wiring example. It uses an in-memory editor and
layout store, an observation-only player host, explicit nonblocking and
maintenance lanes, and ordered disposal without granting DOM, filesystem, or
raw GraphQL authority to an adapter.

## Native Studio tools (removed in 0.34.0)

The 11 native Studio tools (`studio.context.get` ... `runtime.stop`), their
controller host adapter, the native tool dispatcher and the shared
`crowdyjs-studio-host-tools.v1.json` fixture executed those tools for the
Crowdy Agent orchestrator. They are gone with it. The in-browser agent asks the
Studio page for draft tests, deploys (with a page-side player confirmation),
screenshots and project switches over its own bridge; there is no native
counterpart to implement.

## Concrete layout and wallet ownership

The integration owns `StudioLayoutController`. Engines inject layout storage
and use `layout()` / `layoutSnapshot()` for typed state. The `playerHost`
pointer, when given, is handed back from `playerHost()` and never called.

`CrowdyClient::createCrowdyStudioIntegration()` also installs the owned
read-only PlayerWallet adapter when `observePlayerWallet` is true (the
default). Supply `walletProvider` to override it or set the flag false to omit
wallet observation. Wallet read failures only clear the optional snapshot;
they never block editing, saving, compilation, or deployment.


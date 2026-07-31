# Lua Rules Migration Plan

## Goal

Replace the unreleased `--pup-triggers` rule-file feature in `ppuc` with a Lua-based rules system, and add Blockly plus direct Lua editing support to the Drupal `config-tool`.

The old custom rule syntax does not need backward compatibility. Existing rule files may be converted manually or by Codex on demand.

## Decisions

- Use `--rules <path>` as the new CLI option.
- Remove `--pup-triggers`.
- Keep rules optional.
- Store rules as a separate Lua file, not embedded in the game YAML.
- Include rules and Blockly workspace data in config-tool tar.gz exports so a project can be imported and continued in another config-tool instance.
- Add a config-tool button to download the rules file directly.
- Support direct Lua editing in config-tool.
- Blockly should generate Lua directly.
- Use camelCase for public Lua API names.
- Use a namespaced Lua API under `ppuc`.
- Use explicit event handler names.
- On Lua runtime error, log the error and quit cleanly.
- Support all targeted platforms from the implementation: macOS, Linux, and Windows.

## Lua Script Shape

Rules are ordinary Lua scripts defining handlers on the `ppuc` namespace.

```lua
function ppuc.onSwitchChanged(number, state)
  if ppuc.switchRising(13) and ppuc.lampState(42) then
    ppuc.pupTrigger("P", 100, 1)
  end

  if ppuc.stateActive("ballSave") and ppuc.switchRising(9) then
    ppuc.suppressSwitch(9)
    ppuc.pulseCoil(7, 120)
  end
end

function ppuc.onLampChanged(number, state)
  if ppuc.lampRising(23) and not ppuc.attractMode() then
    ppuc.speechTrigger(60010)
  end
end
```

## Event Handlers

- `ppuc.onSwitchChanged(number, state)`
- `ppuc.onLampChanged(number, state)`
- `ppuc.onCoilChanged(number, state)`
- `ppuc.onBallChanged(ball)`
- `ppuc.onPlayerChanged(player)`
- `ppuc.onRulesUpdate()`

`ppuc.onRulesUpdate()` should run once per main-loop tick when rules are loaded. Any handler error should set a fatal error state so the main loop can restore outputs, disconnect cleanly, and exit.

## Lua API

State reads:

```lua
ppuc.switchState(number)
ppuc.lampState(number)
ppuc.coilState(number)
ppuc.currentBall()
ppuc.currentPlayer()
ppuc.attractMode()
```

Edge checks:

```lua
ppuc.switchRising(number)
ppuc.switchFalling(number)
ppuc.lampRising(number)
ppuc.lampFalling(number)
ppuc.coilRising(number)
ppuc.coilFalling(number)
```

Switch groups:

```lua
ppuc.switchGroupState(name)
ppuc.switchGroupRising(name)
ppuc.switchGroupFalling(name)
```

Named state and trigger history:

```lua
ppuc.setState(name)
ppuc.setState(name, durationMs)
ppuc.clearState(name)
ppuc.stateActive(name)
ppuc.triggerHistory(id)
ppuc.triggerHistory(id, windowMs)
ppuc.triggerSequence(windowMs, id1, id2, id3)
```

Outputs and integrations:

```lua
ppuc.pupTrigger(source, id, value)
ppuc.speechTrigger(id)
ppuc.effectTrigger(id, value)
ppuc.suppressSwitch(number)
ppuc.pulseCoil(number, durationMs)
ppuc.blinkLamp(number, onMs, offMs)
ppuc.stopBlinkLamp(number)
```

## Runtime Refactoring

1. Add Lua as a cross-platform dependency to the `ppuc` build.
2. Create `LuaRulesEngine`.
3. Move rule actions out of `PUPTriggerEngine`-specific types into neutral rules/interceptor callbacks.
4. Replace `PUPTriggerEngine` usage in `ppuc/src/ppuc.cpp`.
5. Add `--rules <path>` and `Rules = <path>` ini support.
6. Remove `--pup-triggers` and `PUPTriggers`.
7. Delete the old rule parser and old rule examples.
8. Add `examples/rules.lua`.
9. Update README and related docs.

`LuaRulesEngine` should own:

- Lua state.
- Switch, lamp, and coil state caches.
- Current-event edge state.
- Current ball and player values.
- Attract-mode state.
- Named transient states.
- Trigger history and sequence matching.
- Switch suppression state.
- Fatal error state and diagnostic message.

The engine should keep the same practical integration points as the current rule engine:

- process switch changes and return whether the switch should be forwarded to PinMAME
- receive lamp changes
- receive coil changes
- receive ball/player changes
- receive attract-mode changes
- run periodic update work

## Lua Safety

The Lua environment should be constrained:

- no filesystem access by default
- no process execution
- no dynamic library loading
- no package loading unless explicitly needed later
- protected calls for every script load and handler call
- useful file and line diagnostics
- optional instruction-count hook to prevent runaway scripts

Startup load errors should prevent the game from starting. Runtime errors should be logged and terminate `ppuc-pinmame` cleanly.

## Config-Tool Plan

1. Add game-level storage for Lua rules source.
2. Add game-level storage for Blockly workspace data.
3. Add a rules editor screen or tab.
4. Provide two editing modes:
   - Blockly visual editor
   - raw Lua editor
5. Blockly generates Lua directly using the namespaced camelCase API.
6. Add a button to download only the rules Lua file.
7. Update tar.gz export to include:
   - generated YAML
   - `rules.lua`
   - Blockly workspace data
8. Update tar.gz import to restore Lua and Blockly data.
9. Add basic validation UX where practical, but keep `ppuc` as the authoritative runtime validator.

## Validation

`ppuc` validation should cover:

- rules file load errors
- runtime handler errors
- switch rising/falling behavior
- lamp rising/falling behavior
- coil rising/falling behavior
- switch group checks
- named state expiry
- trigger history and sequence matching
- PUP trigger dispatch
- speech trigger dispatch
- board effect trigger dispatch
- switch suppression close/open behavior
- coil pulse restore behavior
- lamp blink restore behavior

Config-tool validation should cover:

- Lua and Blockly fields save correctly
- raw Lua can be downloaded directly
- tar.gz export contains all project-continuation data
- tar.gz import restores rules and Blockly workspace data

## Rollout

1. Implement `LuaRulesEngine` and CLI support in `ppuc`.
2. Port examples to Lua.
3. Remove old rule parser and old CLI names.
4. Add config-tool raw Lua storage and download/export/import support.
5. Add Blockly editor and generators.
6. Update documentation.
7. Validate on macOS, Linux, and Windows builds.


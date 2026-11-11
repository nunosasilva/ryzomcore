# Port Ryzom to Vulkan

The port is ongoing here: ~/Projects/ryzom/github/atys-live/ryzomcore
There's a complete client here: ~/Projects/ryzom/ryzom_live

Use code a competent programmer can understand and review.

Commit and push when you advance.

Use AGENTS_todo.md as a handoff file, so that resuming current work is a "tail -30 AGENTS_todo.md" away.

## This machine

You can use all the CPU you want, but keep in mind that RAM is limited: 32GB and I use it with KDE and browsers and etc. So, target about 8GB usage during heavy work, like compiling.

## Platforms

Here we only target Linux, but Ryzom runs on Windows, Mac and Linux. For now we will not test outside Linux, but we don't want to make the port more difficult for the other platforms.

## live-server run parameters (user-supplied; low-value account, no secrecy needed)

| key | value |
|---|---|
| login | `mats007` |
| password | `passDOSma` |
| character slot | **1** — the second slot; slot 0 is the first |
| character name | `Megana` |

To enter the game without going through the login screens: ryzom_client username password slot 1 --nopatch 1

The game requires a 30 second logout progress bar. Even if you "kill -9" the game, the character will still be online for 30 seconds "server side". And if you did "kill -9" you'll need to wait for the network timeout + 30 seconds.
The workaround for the network timeout + 30 seconds is to login again. That results in "already logged in" error, but the 30s countdown starts at this point.

## Tools
gamescope can be used to get screenshots and still have full HW acceleration. (This box has a radeon GPU)
xdotool can be used to send mouse or key events to ryzom
Xvfb also available

## Viewport rule

**Ryzom needs roughly 1024×800 before all UI elements fit.** The mechanism:

- the interface is authored against a **1024×768** basis — `ryzom/client/src/interface_v3/interface_manager.cpp:1083`
  and `:2314` call `setInterfaceScale(1.0f, 1024, 768)`;
- `CViewRenderer::updateInterfaceScale()` (`nel/src/gui/view_renderer.cpp:121`) only shrinks when
  `_InterfaceBaseW/H > 0`: `_InterfaceScale = userScale * min(ScreenW/1024, ScreenH/768)`;
- but **`InterfaceScaleAuto = false` by default** (`ryzom/client/src/client_cfg.cpp:319`), so the base
  stays 0, nothing scales, and widgets are drawn at fixed pixel size → below ~1024×800 they overflow,
  overlap or get clipped. A small screenshot then shows a *broken UI*, not a broken renderer.

Consequences: capture at **1280×800** (margin over the 1024×800 minimum) for anything UI-bearing —
login screen, in-game bars, inventory. For a deliberate small-viewport test, set
`InterfaceScaleAuto=true` in the client config first, otherwise the result is meaningless. Never
judge 3D content from a viewport under 1024 wide.

# Local `#!` LED Command Protocol

This document describes the local text-command protocol implemented for the heartbeat LED feature on this firmware fork.

The protocol is consumed by the node itself. It is not part of the stock Meshtastic config UI and does not require a custom app. Commands are sent as ordinary Meshtastic text messages beginning with `#!`.

## Scope

- Transport: ordinary Meshtastic text messages
- Prefix: `#!`
- Processing: local to the receiving node
- Persistence: stored locally on the node
- Propagation: none required
- Current runtime target: heartbeat strips, with `led1` mapped to the first half and `led2` mapped to the second half of the 14-pixel chain

## High-level behavior

- Messages beginning with `#!` are intercepted and parsed locally.
- Supported verbs are `set`, `get`, `clear`, and `help`, plus the standalone top-level commands `gift` and `animate` (see below).
- Supported scopes are `default`, `ch`, and `dm`. `node` is accepted as a legacy alias for `default`.
- Default settings are global fallbacks.
- Channel settings are per-channel LED overrides.
- Channel commands use the incoming message channel by default unless an explicit channel number is supplied.
- Successful mutations are persisted immediately.
- Responses are concise text strings intended to be easy to parse by a human or script.

## Data model

```c
typedef struct {
    uint32_t led1_color;   // 0xRRGGBB
    uint32_t led2_color;   // 0xRRGGBB
    bool configured;
} ChannelLedConfig;

typedef struct {
    uint32_t node_led1_color;   // 0xRRGGBB
    uint32_t node_led2_color;   // 0xRRGGBB
    uint16_t idle_bpm;
    uint32_t idle_delay_ms;
    ChannelLedConfig channels[8];
} CustomLedConfig;
```

Semantics:

- `node_led1_color` and `node_led2_color` store the defaults.
- `channels[n]` contains a per-channel override.
- `channels[n].configured=false` means no override is active for that channel.
- If a channel override is not configured, rendering falls back to the default colors.
- `idle_bpm` and `idle_delay_ms` are default.

## Current defaults

Current firmware defaults are:

- `node_led1_color = #0000FF`
- `node_led2_color = #FF0000`
- `idle_bpm = 80`
- `idle_delay = 0`
- all channel overrides unset

## Channels

Valid channel indices are `0..7`.

For channel-scoped commands, the target channel is resolved as:

```c
target_channel = explicit_channel_present ? explicit_channel : resolved_incoming_channel;
```

If a channel-scoped command does not include an explicit channel and the parser is not given a resolved incoming channel, the response is:

```text
ERR no active channel
```

## Colors

Accepted color forms:

- named color, case-insensitive
- hex color in exact `#RRGGBB` form

Supported named colors:

- `red`
- `orange`
- `yellow`
- `green`
- `blue`
- `indigo`
- `violet`
- `purple`
- `pink`
- `white`
- `warmwhite`
- `cyan`
- `magenta`
- `teal`
- `lime`
- `amber`
- `gold`
- `off`

Fixed mappings:

- `red` → `#FF0000`
- `orange` → `#FF8000`
- `yellow` → `#FFFF00`
- `green` → `#00FF00`
- `blue` → `#0000FF`
- `indigo` → `#4B0082`
- `violet` → `#8F00FF`
- `purple` → `#8000FF`
- `pink` → `#FF4080`
- `white` → `#FFFFFF`
- `warmwhite` → `#FFF0D0`
- `cyan` → `#00FFFF`
- `magenta` → `#FF00FF`
- `teal` → `#008080`
- `lime` → `#80FF00`
- `amber` → `#FFBF00`
- `gold` → `#FFD700`
- `off` → `#000000`

Rules:

- Command keywords are case-insensitive.
- Named colors are case-insensitive.
- Hex colors must match exactly `#RRGGBB`.
- Invalid hex formats return `ERR invalid color`.

## LED color assignment

There are two logical LED areas:

- `led1`
- `led2`

Color assignment rules:

- If one color is supplied, both `led1` and `led2` are set to that color.
- If two colors are supplied, the first sets `led1` and the second sets `led2`.

This applies to both:

- default LED settings
- channel-level LED settings

## Grammar

Top-level form:

```text
#! <verb> <scope> <target> [args...]
```

Supported forms:

```text
#! set default led <c1> [c2]
#! get default
#! get default led
#! set default idle_bpm <n>
#! get default idle_bpm
#! set default idle_delay <ms>
#! get default idle_delay

#! set ch [n] led <c1> [c2]
#! get ch
#! get ch [n]
#! get ch [n] led
#! clear ch [n] led

#! help
#! help colors
```

## Supported commands

### Default LED colors

Set:

```text
#! set default led <color>
#! set default led <color1> <color2>
```

Get:

```text
#! get default led
```

Examples:

```text
#! set default led green
#! set default led red blue
#! get default led
```

### Default idle BPM

Set:

```text
#! set default idle_bpm <value>
```

Get:

```text
#! get default idle_bpm
```

Range:

- valid `1..600`

### Default idle delay

Set:

```text
#! set default idle_delay <ms>
```

Get:

```text
#! get default idle_delay
```

Range:

- valid `0..600000`

### Default aggregate get

Get all default settings:

```text
#! get default
```

### Channel LED override

Set on active/resolved channel:

```text
#! set ch led <color>
#! set ch led <color1> <color2>
```

Set on explicit channel:

```text
#! set ch <n> led <color>
#! set ch <n> led <color1> <color2>
```

Get active/resolved channel:

```text
#! get ch
#! get ch led
```

Get explicit channel:

```text
#! get ch <n>
#! get ch <n> led
```

Clear active/resolved channel override:

```text
#! clear ch led
```

Clear explicit channel override:

```text
#! clear ch <n> led
```

Clear semantics:

- clears the stored channel LED colors
- sets `configured=false`
- does not change defaults

## Response format

### Success responses

Examples:

```text
OK default led led1=#0000FF led2=#0000FF
OK ch=3 led led1=#FF8000 led2=#8000FF
OK default idle_bpm=30
OK default idle_delay=1000
OK ch=3 led cleared
```

### Get responses

Examples:

```text
default led led1=#0000FF led2=#FF0000
default idle_bpm=80
default idle_delay=0
default led led1=#0000FF led2=#FF0000 idle_bpm=80 idle_delay=0
ch=3 led led1=#FF8000 led2=#8000FF configured=true
ch=3 led led1=#0000FF led2=#FF0000 configured=false
```

Notes:

- Channel `get` responses always return the effective colors for that channel.
- If no override exists, the colors shown are the defaults and `configured=false`.

### Error responses

The parser returns these concise errors:

```text
ERR unknown command
ERR invalid channel
ERR invalid color
ERR too many colors
ERR missing value
ERR invalid idle_bpm
ERR invalid idle_delay
ERR no active channel
```

## Help output

`#! help` returns:

```text
#! set default led <c1> [c2]
#! get default [led|idle_bpm|idle_delay]
#! set default idle_bpm <n>
#! set default idle_delay <ms>
#! set ch [n] led <c1> [c2]
#! get ch [n] [led]
#! clear ch [n] led
#! help colors
```

`#! help colors` returns:

```text
colors: red orange yellow green blue indigo violet purple pink white warmwhite cyan magenta teal lime amber gold off or #RRGGBB
```

## Parser behavior

The parser flow is:

1. Confirm the payload starts with `#!`
2. Tokenize on spaces
3. Parse command keywords case-insensitively
4. Parse `verb`
5. Parse `scope`
6. For `ch`, detect optional numeric channel token
7. Parse target
8. Parse remaining arguments
9. Validate values
10. Apply changes
11. Persist on successful mutation
12. Return ACK, ERR, or query response text

## Transport behavior

Two receive paths are implemented:

- local app text path via `/Users/nate/dev/firmware/src/mesh/MeshService.cpp`
- decoded incoming text message path via `/Users/nate/dev/firmware/src/modules/LocalLedCommandModule.cpp`

Behavior:

- Local app commands are intercepted before mesh transmit.
- A local-only reply is sent back to the attached client.
- The original `#!` command is consumed and not transmitted over LoRa.
- Over-air `#!` commands are processed locally on receipt.
- Over-air commands are consumed and not treated as normal chat content by the local node.
- No ACK or ERR is propagated back over the mesh.

## Persistence

The configuration is stored locally in:

- `/prefs/custom_led.bin`

The store is loaded before modules start so LED runtime state is available during boot.

Persistence rules:

- successful `set default ...` persists immediately
- successful `set ch ...` persists immediately
- successful `clear ch ...` persists immediately

This store is intentionally separate from the native Meshtastic config protobufs and UI.

## Runtime mapping

Current heartbeat-strip runtime behavior:

- single 14-pixel NeoPixel chain
- data pin on `GPIO47`
- LED power control on `GPIO48`
- `led1` maps to the first 7 logical pixels
- `led2` maps to the last 7 logical pixels

Additional runtime tuning currently in the firmware:

- base output scale is reduced from full intensity
- selected pixels have extra per-pixel brightness modifiers

These runtime brightness modifiers are not currently exposed through the `#!` protocol.

## Example session

```text
#! get default
default led led1=#0000FF led2=#FF0000 idle_bpm=80 idle_delay=0

#! set ch led amber
OK ch=0 led led1=#FFBF00 led2=#FFBF00

#! get ch
ch=0 led led1=#FFBF00 led2=#FFBF00 configured=true

#! clear ch led
OK ch=0 led cleared

#! get ch
ch=0 led led1=#0000FF led2=#FF0000 configured=false
```

## LED patterns

In addition to a solid color, the default (node-level, always-on) heartbeat can run one of several
ambient patterns:

- `solid` (default)
- `rainbow` - hue cycles continuously
- `sparkle` - random per-pixel flicker against the base color
- `strobe` - fast on/off flash
- `chase` - a lit pixel travels around the strip

```text
#! set default pattern <solid|rainbow|sparkle|strobe|chase>
#! get default pattern
```

Like all `set default ...` commands, this is persisted and, when received over the air, applied to
the *receiving* node - so it can be sent to another badge exactly like a color change. Patterns are
currently only supported at the `default` scope; per-channel and per-DM overrides remain color-only.

## Gift

`gift` plays a one-time ~3 second animated LED burst on the *receiving* node without touching its
saved configuration - unlike every other mutating command in this protocol, it is never persisted.

```text
#! gift <solid|rainbow|sparkle|strobe|chase> [c1] [c2]
```

Colors are optional. If omitted, the receiving node gifts itself its own currently configured
colors in the requested pattern. `gift` requires both ends to be running this firmware; a peer on
unmodified firmware will not recognize it (see Compatibility below).

## Animate

`animate` reproduces a `rainbow` or `blink` effect on a peer using nothing but a timed sequence of
ordinary `set default color` commands - so, unlike `gift`, it works against a badge running
unmodified upstream firmware, since the peer never needs to understand anything beyond the color
command that already existed.

```text
#! animate rainbow            (must be sent inside a direct message; targets that peer)
#! animate rainbow ch [n]     (opt-in broadcast to channel n, or the resolved/current channel)
#! animate blink [color]
#! animate blink ch [n] [color]
```

- `rainbow` steps through 12 complementary color pairs (`led1`/`led2` are set 180 degrees apart on
  the color wheel) roughly one step every 10 seconds, then lands on the firmware's documented
  default (`blue`/`red`).
- `blink` alternates a chosen color (default white) with off for 6 steps, then lands the same way.
- Broadcasting is opt-in only (`ch` must be typed explicitly) since it changes the default color on
  every listening badge, not just one peer.
- There is no over-the-air way to read back a peer's prior color, so `animate` cannot restore
  whatever custom color a target had before running it - it always lands on the standard default.
- Implementation: `RemoteAnimatorThread` in `LocalLedConfig.cpp`, which builds a short list of
  legacy-compatible command strings and sends each as its own packet via `router->allocForSending()`
  / `service->sendToMesh()`, spaced `kStepIntervalMs` apart. Only one animation can be in flight at a
  time; a second `animate` while one is running returns `ERR animation already in progress`.

## Compatibility with unmodified badges

The `#!` transport has no version negotiation - only the literal text is sent over the air, and each
node parses/acts on it with its own local copy of the command parser. Consequences:

- `set default/ch/dm color`, `idle_bpm`, `idle_delay`, `notify_pulses`, `send_pulses`: understood by
  any version of this protocol, including the pre-`pattern`/`gift`/`animate` firmware.
- `pattern` and `gift`: require the receiving badge to also be running this change. Sent to an older
  badge, they fall through to the existing `ERR unknown command` path, which is consumed silently
  (no visible chat spam) but has no effect and produces no over-the-air reply.
- `animate`: works against any badge, since it never sends its own name over the air - it only ever
  transmits ordinary `set default color` commands the peer already understands.

## Implementation references

- parser: `/Users/nate/dev/firmware/src/led/LocalLedCommandParser.cpp`
- config store: `/Users/nate/dev/firmware/src/led/LocalLedConfig.cpp`
- app intercept: `/Users/nate/dev/firmware/src/mesh/MeshService.cpp`
- over-air module: `/Users/nate/dev/firmware/src/modules/LocalLedCommandModule.cpp`
- heartbeat runtime: `/Users/nate/dev/firmware/src/HeartbeatPixelThread.cpp`

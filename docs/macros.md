# Macros

The Raspberry Pi server includes a built-in macro system for recording and replaying button sequences.

Macros are parsed and executed **on the Raspberry Pi server**, so playback continues even if the PC client disconnects mid-sequence.

Macros are only available for PC clients and PC webapp clients.

---

## Macro Grammar

Each macro command has a **list of input tokens** followed by a **duration in milliseconds**:

```
INPUT_TOKENS DURATION_MS
```

### Examples

| Command | Effect |
|---------|--------|
| `WAIT 100` | Release all inputs for 100ms (idle) |
| `A 50` | Press and hold A for 50ms |
| `B 200` | Press and hold B for 200ms |
| `R+LSTICK_LEFT 450` | Hold R bumper + left stick left for 450ms |
| `LOOP 5` | Repeat the preceding block 5 times |

### WAIT

Releases all inputs for the specified duration. Useful for delays between actions.

```
A 50
WAIT 100
B 50
```

### LOOP

Repeats the commands since the previous `LOOP` (or the start of the macro). The count is capped at 1,000,000 expanded steps.

```
A 50
B 50
LOOP 10 
#repeats A+B block 10 times
```

```
X 50
LOOP 10
Y 50
LOOP 5
# X is pressed for 500 ms and then Y is pressed for 250 ms
```

---

## Input Tokens

### Face Buttons

| Token | Aliases |
|-------|---------|
| `A` | `BTN_A` |
| `B` | `BTN_B` |
| `X` | `BTN_X` |
| `Y` | `BTN_Y` |

### Shoulder / Trigger Buttons

| Token | Aliases |
|-------|---------|
| `L` | `BTN_L` |
| `R` | `BTN_R` |
| `ZL` | `BTN_ZL` |
| `ZR` | `BTN_ZR` |

### System Buttons

| Token | Aliases |
|-------|---------|
| `MINUS` | `-` |
| `PLUS` | `+` |
| `HOME` | `GUIDE` |
| `CAPTURE` | `SHARE` |

### Analog Sticks

**Stick clicks:**
| Token | Aliases |
|-------|---------|
| `LSTICK` | `LS` |
| `RSTICK` | `RS` |

**Stick directions:**
| Token | Aliases |
|-------|---------|
| `LSTICK_UP` | `LS_UP` |
| `LSTICK_DOWN` | `LS_DOWN` |
| `LSTICK_LEFT` | `LS_LEFT` |
| `LSTICK_RIGHT` | `LS_RIGHT` |
| `RSTICK_UP` | `RS_UP` |
| `RSTICK_DOWN` | `RS_DOWN` |
| `RSTICK_LEFT` | `RS_LEFT` |
| `RSTICK_RIGHT` | `RS_RIGHT` |

### D-Pad

| Token | Aliases |
|-------|---------|
| `DPAD_UP` | `DUP`, `UP` |
| `DPAD_DOWN` | `DDOWN`, `DOWN` |
| `DPAD_LEFT` | `DLEFT`, `LEFT` |
| `DPAD_RIGHT` | `DRIGHT`, `RIGHT` |

---

## Multi-Token Commands

Combine multiple inputs with `+`, `,`, or `|` to press them simultaneously:

```
A+B 100               ; A and B together for 100ms
R+LSTICK_LEFT 450     ; R bumper + left stick left for 450ms
ZL+ZR+MINUS 200       ; ZL, ZR, and MINUS together
```

---

## Macro Formats

Macros can be uploaded to the server in several formats:

### Raw Command String

```
WAIT 100; A 50; B 50; LOOP 5
```

### JSON Object (single string)

```json
{
  "name": "My Combo",
  "commands": "WAIT 100; A 50; B 50; LOOP 5"
}
```

### JSON Object (array of strings)

```json
{
  "name": "My Combo",
  "commands": ["WAIT 100", "A 50", "B 50", "LOOP 5"]
}
```

### Bare JSON Array

```json
["WAIT 100", "A 50", "B 50", "LOOP 5"]
```

---

## Upload Methods

### UDP One-Shot

Send the entire macro in a single UDP datagram with magic `0x4E534D43` (`NSMC`). Maximum size: 50 MB.

### UDP Chunked

For large macros, use chunked upload with magic `0x4E534D4B` (`NSMK`). Each chunk is up to 1200 bytes. Set the `FLAG_LAST` (0x01) byte on the final chunk. Supports up to 50 MB total.

### WebSocket

The embedded web server also accepts chunked macro uploads via WebSocket.

---

## Client Usage

### CLI / GUI Clients

Load and upload a macro file:

```bash
ns-client --cli <pi-ip> --macro mymacro.json
```

---

## Limits

| Limit | Value |
|-------|-------|
| Max JSON size | 50 MB |
| Max chunk size (UDP) | 1200 bytes |
| Max LOOP expansion | 1,000,000 steps |
| Concurrent macros | 4 (one per sub-pad) |
| Min duration per step | 1 ms |

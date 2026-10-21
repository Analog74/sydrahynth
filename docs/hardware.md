# Hardware notes

## Firmware pack (`.dat`, "A5Pack")

    0x00  "A5Pack\0"
    0x07  model tag ("DR"), NUL terminated
    ...   copyright text, zero padded to 0x5B
    0x5B  sections to EOF: char version[20]; char name[20]; u32 size_le; u8 data[size]

Sections in 2.2.0: `A5 code` (main firmware, ARM vectors at 0), `A5 Reserve1`,
`A5 Reserve2`, `A5 selfcheck`, `gd32 code` and `stm32 code` (panel MCU firmware,
Cortex-M, vector table for flash at 0x08010000).

There is no wave ROM in the pack: the plugin builds one from the firmware
(`hydra::buildWaveRom`) and caches a 0x3E00000-byte file.

## Main CPU

A single ARM core (CP15 reports a Cortex-A5), emulated with Dynarmic's A32 JIT.
Reset: PC=0, CPSR=0x1D3. There is no MMU translation: the plugin only logs
SCTLR/TTBR writes, and the firmware runs identity mapped.

Dynarmic only models user mode. Privileged instructions are claimed via the
`IsSystemInstruction` hook (patches/) and emulated in `arm::SystemState`:
CPS, SRS, RFE, MRS/MSR, LDM/STM `^`, data processing with S=1 and Rd=PC,
and VMRS/VMSR for FPEXC/FPSID/MVFR.

## Address map (`hydra::Machine::Machine`)

| Base         | Size    | Device |
|--------------|---------|--------|
| `0x00000000` | 64 MiB  | DRAM, `A5 code` loaded at 0 |
| `0x40000000` | 128 MiB | NOR flash. `A5 code` copy at +0x20000; 0x110-byte system settings at +0x7E00000; user flash image at +0x7000000 (16 MiB) |
| `0x80000000` | 64 KiB  | stub `sys80` |
| `0x82000000`, `0x85000000`, `0x8C000000` | 64 KiB | stubs |
| `0x86000000` | 512 KiB | stub `blk86` |
| `0x90000000` | 1 MiB   | DSP block (`hydra::DspBlock`) |
| `0xA0000000` | 1 MiB   | SRAM (boot stacks live here) |
| `0xF1000000` | 4 KiB   | timer (`A5Timer`) |
| `0xF3000000` | 4 KiB   | SPI controller → 16 MiB SPI flash, JEDEC ID `EF4018` (W25Q128) |
| `0xF4000000` | 4 KiB   | GPIO; I2C buses A and B bit-banged through it |
| `0xF5000000` | 3×0x200 | UART0, UART1, UART2 |
| `0xF6000000` | 4 KiB   | tone generator (`ToneGenerator`) |
| `0xF7000000` | 4 KiB   | ADC |
| `0xF9000000` | 4 KiB   | DSP control |
| `0xFA000000` | 4 KiB   | key scanner: button matrix, bit id % 11 of the word at +0x28 + 4 * (id / 11) |
| `0xFF000000` | 4 KiB   | interrupt controller (`A5Intc`) |
| other `0xF0`–`0xFE` blocks | 64 KiB | stubs |

## Clocks and interrupts

- CPU: 400 MHz, counted as 1 instruction = 1 tick. Timer: 67.7376 MHz
  (timer ticks = CPU ticks × 0.169344), which is 1536 timer ticks per 44.1 kHz sample.
  The plugin advances the DSP block once per 0x600 timer ticks.
- The plugin runs the CPU in slices of at most 2000 instructions, cut short at
  the next timer expiry. During WFI it advances time without running code.
- Interrupt controller (`0xFF000000`): `+4` enable, `+8` write-1-to-clear pending,
  `+0xC`/`+0x10` plain registers, `+0x14` pending & enable. IRQ while that is
  non-zero and CPSR.I is clear.
- Timer (`0xF1000000`): 6 periodic channels, 0x10 apart: `+0` control (bit 0
  enable; a 0→1 edge arms it), `+4` period, `+8` plain, `+0xC` remaining ticks.
  Channel n raises interrupt line 2+n. Firmware 2.2.0 programs ch0 = 0xC0000
  (~86 Hz, 512 samples), ch1 = 0x600 (44.1 kHz, one sample) and ch5 = 0x1089A (~1 kHz).

## GPIO and I2C

- GPIO (`0xF4000000`): 8 ports, 0xC apart: `+0` data, `+4` direction (1 = out),
  `+8` input. Inputs reset to all-ones.
- Port 0 bit-bangs two I2C buses: A uses SCL bit 25 and SDA bit 23; B uses SCL
  bit 27 and SDA bit 26. A line is high unless it is an output driving 0. The
  slave's SDA comes back on the same input bit.
- Each bus has an effects DSP (`FxDsp`) at address 0x51: bus A drives effects
  DSP A, bus B DSP B. Protocol: a 15-bit word address (low byte first), then
  32-bit little-endian words with auto-increment. 0x0000–0x0FFF is data,
  0x1000–0x1FFF is program, 0x2000 is control (setting bit 0 resets the core and
  runs the program). Firmware 2.2.0 clears and reads back the data memory, loads
  the program (2560 words at 0x1000) and the initial data (980 words at 0x0018),
  then starts each DSP.

## Effects DSPs

The same DSP core as the main block (`src/fx_dsp.h`): data memory 0x000–0xFDF
with 32 registers at 0xFE0–0xFFF (register 0: delay RAM write address, storing
register 2; register 1: read address, loading register 3; the program also uses
registers 4–9 and 0x15, which have no modelled side effects); a 2M-word delay
RAM; io/ext space 0x0000–0x0FFF = program memory, the rest a 64K-word RAM;
eight ports. Jump idle detection is on.

Per sample, at the start of the DSP block's sample (before the tone generator
renders), the chain runs on the DSP block's previous outputs:

1. DSP A: ports 2/3 = dry L/R (DSP block ports 64/65) >> 2, ports 4/5 = DSP
   block ports 70/71; sample interrupt; 6000 instructions.
2. DSP B: ports 2/3 = A's ports 0/1, ports 4/5 = DSP block ports 66/67; sample
   interrupt; 6000 instructions.
3. Output L/R = B's ports 6/7. While either DSP is stopped, the output is the
   dry signal.

DSP block ports 72/73 get the dry signal (the panel's oscilloscope). Ports 68/69
are unused. With a patch loaded, the output carries the patch's effects (e.g. a
reverb tail after note-off) at about a third of the dry level.

## Main DSP block (`0x90000000`)

- 8 cores. Core n's program is at `0x90000000 + n*0x4000`; the firmware uploads
  3072 words (0x3000 bytes) per core.
- Byte layout: `0x00000–0x1FFFF` core-local memory (0x4000 bytes per core);
  `0x20000–0x7EFFF` shared RAM; `0x7F000–0x7FFFF` mailboxes, 32 words per core
  counting down from the top (core = `(~offset >> 7) & 0x1F`, word =
  `(offset >> 2) & 0x1F`; core 0 is at `0x7FF80`, core 7 at `0x7FC00`).
- `dspctl` (`0xF9000000`): `+0` bits 0–7 run enables (cleared to halt, set to
  start), bits 8–15 "attention" requests, bits 16–23 unknown flags; `+4` and `+8`
  hold a nibble per core (both 8).
- Boot sequence: halt all cores, copy 3072 program words into each core, patch
  each core's word 0 to `0xAE064BC0`, clear mailbox 0, set run bits 0xFF, then
  wait for each core's mailbox 0 to read 0x55AA and clear it.
- Host commands, handled by each core's attention interrupt (vector 1): mailbox
  0 command, 1 target address, 2 count (≤ 16), data staged in mailbox words
  3–18. The core announces 0x55AA, waits for a command and acknowledges by
  changing word 0. Commands: C0 io→io, C1 staging→io (shared RAM), C2
  data→io, C3 staging→private data memory, C9 sync/end. Firmware 2.2.0 sends
  them only at boot.
- DSP ISA: see `src/dsp_core.h`. Verified against the reference with
  `tools/oracle` (decoder on 1.57M words, execution on 4M random instructions).
- Each sample: the tone generator renders 48 buses into the port buffer, every
  running core gets the sample interrupt (line 2) and 3072 instructions. About
  1170 of them run; the rest is an idle loop, detected and skipped.
- Emulation: `src/dsp_jit.*` compiles each core's program to x86-64 by basic
  blocks (interpreter fallback for rare instructions, while an interrupt is
  pending and at the end of a run's budget); it is bit-exact with the
  interpreter (`HYDRA_DSP_JIT=0`), checked by comparing `hydraboot --capture`
  output. The program is static while playing (its memory is written ~31000
  times at boot, twice when a patch is sent), so a write simply drops all
  translations. Hot instructions by dynamic count: Mul 15%, StoreRegInc 7%,
  Load 7%, LoadRegInc 6%, Add 6%, Sub 6%, Loop 5%, Store 5%.
- Voice architecture: core n is voice n. It reads its three oscillators from
  tone generator buses (2n, 2n+1), (16+2n, 17+2n), (32+2n, 33+2n), buffers 8
  samples and processes them in its main loop (filters, amp). Voices are summed
  into a 128-frame stereo ring at io 0x9766 (write pointers per core at io
  0x975C–0x9763; core 0 overwrites, others accumulate). Core 7 reads the ring
  (read pointer io 0x9759), clamps to 24 bits and writes ports 0x40/0x41; cores
  3, 5 and 4 write ports 0x42–0x47. Ports 64–71 feed the effects DSPs (see
  "Effects DSPs").
- Voice parameters come from the CPU (see "Voice control" below): core 0 takes
  43 targets from shared io `0x8214 + 8k + lane` (lane = voice) every 8 samples
  and ramps private data words 0x2D+k toward them (step at 0x58+k, a 64th of
  the distance). k = 37 is the amp level (data 0x52); k = 1 a squared level
  (data 0x2E).

## NOR flash

Spansion S29GL01GS (IDs 0x0001 / 0x227E / 0x2228 / 0x2201), 16-bit, 128 KiB
sectors. Standard AMD command set: unlock `AA@555`, `55@2AA` (word addresses),
then `A0` (program word), `25`, count−1, data…, `29` (buffered program), `80`
→ unlock → `30@sector` / `10@555` (erase), `90` (autoselect); `F0` resets;
`98@55` is CFI. On first boot, firmware 2.2.0 erases and rewrites the two
settings sectors at `0x47E00000`.

System settings: at boot the firmware copies 272 bytes from `0x47E00000`. The
first 176 bytes are 11 pages × 8 menu values (int16); 0x00FF means "unset, use
the menu default", and nothing checks for 0xFFFF. With erased flash every system
setting is −1: master tune 65535 Hz (looked up out of range, −1357 cents),
program change reception off, and so on. A factory device must hold 0x00FF there,
so `Machine` writes 0x00FF into a blank table (or a value from
`Machine::Config::systemSettings`). The fields after the table are range-checked
and fall back to defaults by themselves, except the index of the last patch
(offset 0xDA): at boot the firmware loads that bank slot unchecked, so 0xFFFF
loads zeros (no envelope triggers, silence). `Machine` sets it to 0 (A001)
whenever it is erased, also in restored flash.

Boot with valid settings (version 220): settings, then the bank cache in RAM
(0x6C50E2 + 2786 per slot, filled from flash), then the last patch's slot is
replayed as parameter messages (`0x77320`: index < 799 from the 1.x part at +26,
the rest from the 2.x extension at +1766), e.g. the envelope trigger sources
(page ':' 96-115, value >> 3; 1 = key).

The table, page by page (entry 0-7, menu name and default; "-" unused):

| page | entries |
|---|---|
| 0 | Transpse 0, Tuning 440 (Hz), O'Scope 1, Protect 1, Lcontrst 64, Rcontrst 64, Light 0, LiteShow 5 |
| 1 | KnobMode 0, Knob Spd 1, TempoLck 0, MacroBtn 0, SafeEdit 0, -, FxBypass, Microtun |
| 2 | Velocity 0, Vel Crv 2, AT Delay 100, AT Fade 50, AfterTch 1, AT Crv 2, ATOffset 0, ATReleas 0 |
| 3 | Clk Sync 0, Local 1, MIDI TX 1, MIDI RX 0, SusPedal 2, ExpPedal 0, ExpPedal 0, ExpCurve 1 |
| 4 | At TX 2, -, SusPedTX 1, ExpPedTX 1, -, ModWhlRX 1, -, MPE 0 |
| 5 | Param TX 0, Param RX 0, Send, Send All, Overflow 0, ARP TX 0, PgmChgTX 0, PgmChgRX 0 |
| 6 | CV Range 0, RefNote 60, CVOffset 0, -, GateType 0, GateVolt 0, -, - |
| 7 | Clk CV 0, Clk Rate 0, Clk Div 0, ClkOffst 0, -, -, -, - |
| 8 | IM1Range, IM2Range, OM1Range, OM2Range, IM1Offst, IM2Offst, OM1Offst, OM2Offst (all 0) |
| 9-10 | SelfTest, Factory, OS Ver, Serial# (not settings) |

The menu entries live at runtime in 72-byte records (name at +0, maximum at +28,
value at +32, default at +56), reached through the table at `[0x49E5D0] + 12 ×
(page + 113)`.

## Tone generator (`0xF6000000`)

`+0` address (bit 31 selects the second half of the voices), `+4` data (write
stores at the address; read returns latched data), `+8` write 5 latches the
read-back, and reads always return status `0x10100`. `+0xC`, `+0x14` and `+0x20`
are plain registers. The plugin decodes voice registers: `0x000–0x3FF`, step 4,
fields 0–3; `0xC00–0xFFF`; `0x1000–0x17FF`, step 8, fields 1–4.

## UARTs (`0xF5000000 + n*0x200`)

`+0` read pops a received byte (0 if empty), write transmits (instantly; there
is no TX busy bit); `+4` line settings; `+8` interrupt enable; `+0xC` status =
`min(rx count, 31) << 4 | (rx waiting ? 0x0C : 0)`; `+0x10` baud divisor.
Interrupt line `9 + 2n` is raised while `status & enable & 0x0C`. The plugin
re-raises it after every read while data remains.

Wiring in the plugin:
- UART0: MIDI. Host MIDI is fed in sample-timed (400e6 / 44100 = 9070.29 CPU
  ticks per sample) and split into chunks of at most 7 bytes.
- UART1: panel MCU USART. Bytes go both ways between it and the STM32.
- UART2: not connected.

## Panel MCU

STM32F103 (high density, 72 MHz = CPU ticks × 0.18). "stm32 code" is
programmed at flash 0x08010000 (the bootloader is not in the pack) and runs
from its own vector table there. Emulated in `src/cortex_m3.*` (Cortex-M3 on
Dynarmic in Thumb mode; system instructions, exception entry/return, NVIC,
SysTick are ours) and `src/stm32.*` (RCC, GPIO, AFIO/EXTI, timers, DMA, SPI,
USART, ADC).

- Displays: FSMC bank 1. Main OLED command/data at 0x60A00000/0x60A20000; four
  small displays at 0x60E0/0x60F0/0x61E0/0x61F0 0000 (+0x20000 data); writes at
  0x6000/0x6020/0x6040/0x6060 0000 are output latches (ignored). The
  controllers are SSD1305/SH1106-like 132x64 parts (`src/oled.*`): the main
  display shows RAM columns 4-131, the small ones 2-129. The firmware draws in
  vertical addressing mode (pages first, then columns) inside windows set with
  0x21/0x22, and renders the main screen itself from a screen-state frame
  (type 0x07) the main CPU sends; the small displays from type 0x08 (encoder
  names and values).
- Inputs (`src/front_panel.*`): ADC1 reads its channel behind a multiplexer
  selected by GPIOC 5-7 (input = channel * 8 + mux). The eight endless encoders
  are dual-wiper pots on channels 0/1 (wiper A at mux 0/2/1/3 of channel 0 for
  encoders 1-4 and channel 1 for 5-8, wiper B four positions further); the
  filter knobs are channel 2 mux 0/4 and channel 5 mux 0, the arpeggiator
  switches resistor ladders on channel 2 mux 3/5/7 and channel 5 mux 3.
- Encoder ring LEDs: an active-low bitmap at SRAM 0x20000101-0x20000110.
- RGB LED chains (pads): WS2812 chains fed by DMA1 into TIM3 CCR3, TIM3 CCR4,
  TIM1 CCR4 and TIM3 CCR1; 24 PWM words per LED (duty >= 14 is a one), GRB.
- On the main side: the button matrix is the key scanner at 0xFA000000, the
  data encoder a quadrature pair on GPIO port 1 inputs 6 (A) and 4 (B), the
  pads pressure inputs of the main ADC (channel 3/2/0 per row, mux
  0,4,2,6,1,5,3,7 per column; 250-1023 while pressed), and the button LED
  brightness bytes (96) live in RAM at 0x150E19 in the panel view's numbering
  (OSC 1 = 0 … POST-FX = 25, FILTER 1/2 = 26/27, TAP = 29, ARP ON/LATCH/
  TRIPLET = 30-32, SAVE = 33, BROWSE = 34, the encoder buttons 50-57, MOD
  MATRIX = 58; the LFO buttons 14-18 pulse at their LFO's rate). An earlier
  base of 0x150E14 lit every LED five places off.
- SPI2 is the slave link to the main CPU, receiving by interrupt and sending by
  DMA1 channel 5 (paced by the master's transfers). Frames start with 0x80/0x81;
  the first non-control byte gives the length (+7, at most 0xC0). After each
  frame the firmware toggles PB10.

## Panel MCU link

Besides UART1, the panel MCU is connected by:
- SPI: the main CPU's SPI controller (`0xF3000000`) exchanges bytes with the
  STM32's SPI peripheral (`hydra::PanelLink::transfer` runs the M3 core until
  its SPI is ready, then exchanges a byte). The same controller also reaches
  the W25Q128 SPI flash.
- Two handshake lines: the MCU's GPIOB output register bits 10/11 (PB10/PB11,
  taken from ODR regardless of pin mode) drive main GPIO port 0 input bits 4
  (0x10) and 5 (0x20). PA9 is held high by the board.
- UART1 traffic from the main CPU at boot is AA 00 00 01 01 repeated, which
  looks like the STM32 bootloader protocol; the application firmware never sets
  up USART1, so it is not answered (the reference does not answer it either).
- The main CPU waits for the handshake before streaming SPI to the panel.
- Emulation of the link: an SPI byte takes no time for the main CPU, so it
  sends ~60000 bytes per emulated second. Before each byte the panel catches
  up; if it has not read the previous byte, it runs on (up to 2000 cycles, in
  50-cycle steps as in the reference) until it does. The panel's DR read ends
  the run at once and the 50-cycle step is completed, which keeps the
  reference's timing at a fraction of the cost. While the panel's reply
  streams out by DMA (channel 5 feeding SPI2's DR) the master's bytes only
  clock it and the panel never reads them, so there is nothing to wait for;
  the reference waited the full 2000 cycles per byte there (its link
  "overruns", ~15000 per second), which also ran the panel ahead of the main
  CPU's clock (syncPanel only ever catches it up). Without that wait the link
  carries ~300 instead of ~180 frames per second. `HYDRA_LINK_WAIT_REPLY=1`
  restores the reference's behaviour, `HYDRA_LINK_WAIT=n` sets the wait
  limit for bytes the panel does read (longer limits lose fewer bytes but
  stall the master in the panel's SPI handler). The remaining lost bytes
  (~30 per second) arrive while the panel's SPI2 or DMA1 handler runs.
- Display frame budget: the main CPU's display task (0x2fd4c, started at boot)
  loops over ten phases, queuing one frame per phase (LED frame type 06,
  screen frame type 07 in two phases, parameter frames 04/0d …) and yielding
  (round-robin among same-priority tasks); the screen frame is rebuilt every
  turn. Other tasks queue frames too: type 0a (7 bytes, a modulated value
  for the display, at most every 10 ms) comes from the modulation engine and
  fires ~35 times per second while a voice with a displayed modulation is
  held. So the waveform's redraw rate depends on what else is on the link:
  with a held note the emulator settles, per boot, into one of a few steady
  states (which voice got the note decides), between ~17 and ~55 main display
  redraws per second with the current link, ~7 to ~32 with the reference's
  slower link. The hardware's SPI clock, and so its frame budget, is unknown.
  `emurun` with `HYDRA_SOAK` prints frames per type, content changes per
  type, the panel's redraws and the display task's turns (`HYDRA_SOAK_TASK`).
- ADC scan versus its handler: the DMA1 channel 1 handler (0x8013edc) stops
  TIM2, filters the buffer (0x8016450), restarts TIM2 and then sets the
  multiplexer pattern for the next buffer (index at 0x200000F5; after index 7
  it stops TIM2 and flags the main loop). On the hardware no conversion runs
  in between. Our DMA model completes the circular transfer every 4500 cycles
  regardless, and ~100 times a second a completion fell inside the handler,
  before its multiplexer step: that buffer, converted at one position, was
  then filed under the next, and the encoders' decoded positions jumped (the
  small displays' values fluctuated, more with some patches). The channel's
  completion now waits while its own handler is active (`Dma::holdWhileHandler`,
  `HYDRA_NO_ADC_HOLD=1` switches it off); spontaneous value changes over six
  patches went from 2-14 per 30 s to none, the redraw rates are unchanged.
  A TIM2-driven gate (no completions while TIM2 is stopped) also fixes it but
  stops the free-running scan after each pass until the main loop restarts
  TIM2 (`0x200069B5` = 0 and one SPI frame), which changed the firmware's
  link traffic so much (LED and pad frames 06/0d at 140/210 per second) that
  the screen frame starved. Timestamps from peripheral callbacks inside a JIT
  run are stale by up to a slice (4096 cycles), so rules based on them miss.
- ADC scan pace: the panel's main cost is filtering the scan's buffers
  (0x8016502 and following, a third of its instructions). Our DMA model, like
  the reference's, converts at 90 cycles per item (a pass in 0.5 ms; the
  hardware's TIM2 trigger gives 16 ms). The ADC channel now runs at 180 cycles
  per item (`Dma::channelCyclesPerItem`, `HYDRA_ADC_DMA_CYCLES`): emurun
  3.24 x instead of 2.95 x real time with the faster link, encoder and knob
  behaviour identical (scripted turns give bit-identical screens at 90 and
  180), waveform redraws 34-70 per second with a held note. 270 gains nothing
  more; the full hardware pace starves the main CPU's knob polling (see above).
- Boot timeline (emulated): the effects DSPs run at 3.4 s (their programs go
  over a bit-banged I2C, 0xa100-0xa700, 200-iteration delay loops per half
  bit); the main CPU's init (0x2a504, called by the init task 0x86b0, which
  then sets the "ready" flag 0x47001C) ends at 0.74 s. The panel shows its
  start-screen animation meanwhile: TIM7's handler builds the frames
  (0x8011304; when done it sets 0x200069B6), TIM6's handler (0x801a9e0, every
  5 ms) shows one of 33 frames per 41 ticks and, once both are done, clears
  the boot flag 0x200069B5 and stops both timers. Until then the panel keeps
  its handshake lines high, and the main CPU's SPI task (0x15f6c, which waits
  for PB10/PB11 low) sends nothing: the link starts at 7.1 s. The emulator
  goes live 0.3 s after the first link byte (and after its warm-up note), so
  that code is translated beforehand: 3.3 s of wall time instead of 4.1 s
  with the former fixed 10 s. Tried and dropped: delay loops of the I2C
  upload taking no time (the DSPs then run at 0.2 s, but the start screen
  still ends at 7.1 s and the denser emulated seconds cost more), and running
  TIM6/TIM7 faster during the start screen (the animation never finishes:
  its frame builder does not keep up). `HYDRA_BOOT_TRACE=1` prints the
  phases; emurun's `HYDRA_BOOT_TEST=<seconds>` pulls 128-frame blocks right
  after going live and reports underruns.
- Display frame budget, continued: the main CPU's display task offers ~1400
  frames per second to a one-frame send buffer (0x5DC9BE, flag 0x47AE6C;
  sub_239B4 drops a frame while it is full), and the SPI task (0x15f6c)
  sends it when the panel pulls PB10 low. The panel does that in its main
  loop after parsing a frame, but the loop first finishes refreshing the
  WS2812 LED chains by DMA (0x801b9xx), which at the LEDs' real 800 kHz takes
  ~2 ms: ~340 frames per second get through, and which ones beats with the
  task's timing, so the waveform's redraw rate depended on the patch (18-74
  per second over 16 patches). The LED DMA channels now run at 60 cycles per
  item (42-81 redraws per second, the main thread ~6 % busier; 20 cycles give
  78-119 but cost 17 %). The hardware presumably shows the same effect.
- The panel's clock is derived from the main CPU's ticks; the conversion must
  not multiply ticks by 72 MHz in 64 bits: that overflowed after 640 s of
  emulated time and stopped advancing the panel, which froze the displays and
  encoders after 10 minutes 40 seconds (`emurun` with `HYDRA_SOAK=<seconds>`
  runs a long panel soak test that catches this).
- The panel firmware waits for DMA transfers by polling DMA1's ISR; three
  unchanged reads in quick succession let the time up to the next event pass
  at once (`HYDRA_NO_POLL_IDLE=1` switches that off). Its delay function
  (0x801bd70) sets SysTick to n × 9000 cycles and polls COUNTFLAG; that poll
  idles the same way. Its main loop (0x801c0a4) is interrupt-driven (~100k
  interrupts per second from the link, timers and DMA), not a busy loop:
  idling it between interrupts gained nothing. The panel's ADC scan runs
  through DMA1 (1.25 µs per conversion in our DMA model) and its results are
  filtered as trimmed means of 10 samples (0x8016502), which is most of its
  remaining work.
- The panel's ADC scan, as the firmware sets it up: ADC1 converts a 5-channel
  sequence on every TIM2 compare-2 event (CR2 EXTTRIG, EXTSEL 3; TIM2 runs at
  1 MHz with ARR 199, CCR2 50: one trigger per 200 µs) into a 50-sample
  circular DMA buffer (10 scans per multiplexer position). The DMA1 channel 1
  interrupt (0x8013edc) stops TIM2, filters the buffer (trimmed means of 10,
  0x8016502), restarts TIM2 and steps the multiplexer (GPIOC 5-7); after the
  eighth position it stops TIM2 and sets a flag, and the main loop (0x801c52a,
  which runs once per SPI frame from the main CPU, woken by 0x20000003 from
  the SPI2 handler) restarts TIM2 for the next pass. On the hardware a pass
  therefore takes 16 ms (62 per second). Our DMA model, like the reference's,
  moves one item per 90 cycles regardless of triggers, so the buffer fills
  every 62.5 µs and a pass takes 0.5 ms: 32 times the hardware's scan rate,
  which is about a third of the panel's cost. Modelling the trigger-driven
  scan exactly (TIM2 compare events, DMA items per conversion) gave correct
  knob values but was slower overall: the main CPU's firmware polls the panel
  for its knob replies, which then come 8 times less often, so the link
  carried 2.3 times the bytes and the main thread's time rose from 298 to
  440 ms per emulated second (and the encoder simulation's 64 units per
  10 ms would have to be slowed to a pass). The 90 cycles stay.
- The panel MCU is run up to the main CPU's time before every interaction
  (link, UART1, handshake pins) and otherwise once it lags more than 50 µs,
  so that it runs in fewer, longer stretches (Machine::syncPanelLazily).

## Wave ROM

Built from the firmware by `src/wave_rom.cpp` (byte-identical to the
reference's builder, checked with `tools/oracle/waverom_diff.py`) and loaded
into NOR flash at 0x200000: 219 single-cycle waves from "A5 code" offset
0xDBD5C, each rendered band-limited at 17 levels (4096-sample cycles).

## Tone generator voices

See `src/tone_generator.h` for the register map. Voices loop single cycles of
the wave ROM (sample index = flash byte offset / 2). Reading a voice's start
register returns its live position and play flag; the firmware relies on that.

## Patch knob ring and other WS2812 LEDs

The panel's four WS2812 chains (DMA1 channels 2, 3, 4, 6) carry more than the
pads: chain 1 LEDs 0-3 always share one colour, the patch's (the colour byte
at +4 of a patch body, chosen on the Save page; the firmware converts it to
RGB, sent dim, e.g. 23 00 0a), which lights the ring around the patch knob;
with no patch loaded it is blue. Chain 1 LEDs 4-6, chain 0 LEDs 0-8 and
chain 3 change too but their purpose is unknown (not shown in the view).

## Buttons

The Desktop's 55 panel buttons are a matrix read by the main CPU through the
block at 0xFA000000: the firmware (0x14660, a 5 ms timer callback) writes a
row's register index to +0 (0xB1, 0xB3 ... 0xBF for rows 0-7) and reads the
row from +4, of which it keeps the low 8 bits (1 = pressed); 0x14480 debounces
and turns bit j of row i into button i * 11 + j. +8 is configuration (0x1F at
boot). The event-FIFO path at +0 (0x14824, bit 8 = event pending) is never
called in firmware 2.2.0 (the keyboard model's, presumably). An earlier model
put the rows at +0x28 only, which the firmware never reads: no button worked.

## SPI flash, ADC

- SPI controller at 0xF3000000 with a W25Q128 (`src/spi.h`). Firmware 2.2.0
  does not access it during boot or playing.
- ADC at 0xF7000000: 8 channels × 8 mux positions (mux = GPIO port 1 bits
  23–25); data reads return value | 0x400; a conversion raises line 27. The
  firmware scans it continuously: it starts a conversion and polls the busy bit
  (+0x84 bit 0) until the interrupt handler has read the result (which clears
  it). The 24 conversions of a scan are the pads (channels 3/2/0 at the 8 mux
  positions); each is guarded by a 1 ms timeout on timer channel 4 (line 6).
  The reference plugin raises the interrupt at once (no conversion time), so
  the firmware's main loop, which the scan paces, runs as fast as the host
  allows. We let a conversion take 20 µs (Machine::Config::adcConversionMicros,
  `HYDRA_ADC_US` for experiments) and the CPU sleep while it polls a running
  conversion with interrupts enabled. That sets the main loop to ~1400
  scans per second (hardware-like; each pad still sampled 1400 times a second)
  and saves about 60% of the main CPU's instructions compared to 0 µs: the loop
  rebuilds the panel's LED frame (86 bytes plus checksum) and screen frame
  every iteration, and the ADC interrupt is the last entry of the firmware's
  interrupt priority table, so its dispatcher walks 28 entries (~400
  instructions) for every conversion. 40 µs would let the timeout fire; 30 µs
  does not but leaves little margin for interrupt load (e.g. SysEx uploads).
  The note-on latency (MIDI to audio) is 7.6 ms with 20 µs, 6.7 ms with 5 µs.

## Patches and SysEx

- `.hydra` banks are ZIPs of `<NN><name>.patch` (2786-byte 2.x bodies), NN the
  slot in hex (00-7f). A body starts with the version (u16, 220 = 2.2.0, older
  bodies are converted at boot), the slot index (u16; librarian files carry
  0x280 + slot, the firmware rewrites it to bank × 128 + slot), the category
  (u8) and the 16-character name (NUL/space padded); the 1.x parameters follow
  at +26, the 2.x extension ("ETCD…") at +1762.
- SysEx format and upload sequence: `src/sysex.h`. Start, save and end take a
  second byte 00 (`18 00`, `14 00`, `1A 00`); without it firmware 2.2.0 ignores
  them, and the patch never reaches the sound engine. Replies: 19 (start), 17
  per chunk, 07 for the last chunk, 1B (end); the save gets none.
- An upload is confirmed by `07 00 <bank> <slot>` after the last chunk (the
  plugin waits for exactly this; `14 00` never gets a reply, also not in the
  plugin). All five banks are stored in NOR as one array of 1762-byte records at
  `0x47E91E00 + 1762 × (bank × 128 + slot)` (the slot field rewritten) plus the
  remaining 1024 bytes of a 2.x body ("ETCD…") at `0x47920000 + 1024 × index`.
  At boot the firmware copies all 640 records into a DRAM bank cache at
  `0x6C50E2 + 2786 × index` (and 384 more slots from `0x47720000`, filled with
  "Init" when blank), which uploads and saves keep up to date; the sector
  `0x47E80000` also holds a journal of 26-byte patch headers that the plugin
  does not need. The current patch is a 2786-byte body at `0xB498E2` (zeros
  while none is loaded, e.g. after selecting a blank slot: the display then
  shows no name). The emulator's `readPatches()` reads the names and the
  current index from these two places. An upload to the current program (A001
  after boot) also makes it the current sound; other slots must be selected.
- `Emulator::uploadPatches` sends a bank like a librarian, unpaced: 10 ms after
  each chunk and 60 ms after the start, save and end messages (plus 320 µs per
  byte), which takes 0.57 s per patch. Verified for bank A by selecting each
  patch afterwards (bank B lands in sectors 47ec/47ee).
- Program change reception is off by default (system setting PgmChgRX), so
  MIDI program changes are ignored until it is switched on; then `C0 <slot>`
  loads the stored patch. The bank comes from bank select **LSB** (CC 32 =
  0-4 for A-E); CC 0 (MSB) is ignored, so a program change after CC 0 = 2
  selects bank A's slot (an earlier note here blamed a blank display on bank
  B: that was an empty bank A slot).
- With blank NOR the firmware runs a sound made of 0xFF bytes: all envelope
  times "instant", loop count 0x7F, an empty mod matrix. That envelope cycles
  attack→decay→restart every tick with output 0, so every voice is silent
  until a real patch has been uploaded.
- After loading a patch the firmware sends All Notes Off and NRPN parameter
  dumps on MIDI out.

## Voice control (CPU side, firmware 2.2.0)

The CPU runs envelopes, LFOs and the mod matrix at control rate and writes the
results to the DSP; addresses are A5 code/DRAM addresses.

- Voice state: 8 voices, 34472 bytes apart, from 0x554B80. Envelope e (1–5)
  of voice v is a 6328-byte struct at `0x583808 + 6328e + 34472v`: word 17
  state (0 off, 1 delay, 2 attack, 3 hold, 4 decay, 5 sustain, 6 release), 18
  phase (a stage is 211680000 units), 19/21/22/28 attack/hold/decay/release
  rate (−1 = instant), 23 sustain, 29 loop count, 33 output (1.0 =
  0x1000000), curve tables from word 38. Updated by the routine at 0x9AE78.
- Mod matrix: 112 slots of 26 bytes at 0x574C36 (source, destination, depth
  per voice …); slots 0–31 come from the patch, the rest are fixed routings,
  e.g. slot 37: source 2 (ENV1) → destination 48 (amp). Evaluated by 0xA2DE0
  into 385 per-voice destination words at `0xB77928 + 1540v`.
- 0xA07A0 maps destinations to DSP parameters (table at 0xB7B2D0) and writes
  `base + modulation` per voice to shared RAM `0x90020000 + 32·slot + 4·lane`
  (= io 0x8000 + 8·slot + lane), about every 32 samples.
- Oscillator pitch (0x9DABC → 0x953C8), in 1/256 semitone: note (15360 + 256 ×
  MIDI note) + oscillator tuning + (global pitch modulation − 0x2000) / 5 + …;
  0x953C8 turns it into the tone generator increment via a semitone table and
  a cent table. Global pitch modulation is destination 132: `2 × (base +
  modulation)`, base = 4096 + 6.4 × master tune offset in cents, set from the
  system setting "Master Tune" (Hz, cents table at 0xD24B8 indexed by Hz − 380)
  as parameter page 0x41, index 4.
- Controller slots (step, target, current) at 0x5800BC + 12i: pitch bend
  first (MIDI pitch bend writes it and 0x58017C, centred at 0x2000).

## Emulator threading and timing

- Main CPU: one instruction per tick at 400 MHz; RAM pages are mapped into
  Dynarmic's page table, everything else uses callbacks. A block that runs
  into a count-down delay loop (`MOVS Rt, Rc; SUB Rc, Rc, #1; BNE`) ends at its
  head; the machine then advances the loop arithmetically within the slice
  (registers, flags and 3 ticks per iteration as if executed). Interrupts are
  checked at slice boundaries (at most 2000 ticks apart).
- DSP block: per sample all cores finish before the next sample;
  `DspBlock::setThreads` spreads them over threads.
- `Machine::Config::dspThread`: the DSP side (cores, tone generator, effects
  feed) runs on its own thread up to the sample due at each slice start, while
  the CPU runs the slice; CPU accesses to DSP control and the tone generator
  wait for it, while the DSP block's RAM and mailboxes are shared with the
  running cores without waiting, as on the hardware. Effects then run on their own thread too, with I2C
  accesses queued by sample. Parameter writes may land a sample later or
  earlier than single-threaded, so runs are not bit-identical.
- The DSP block's interrupts (line 15 every 32 samples, line 14 every 64) are
  raised by the CPU side at a fixed emulated time, 64 samples after they are
  due (`Machine::deliverDspInterrupts`): each slice ends at the next one, and
  there the CPU waits until the DSP side has run up to it. The firmware counts them (its control rate; the arpeggiator
  steps every 86 at 120 BPM). Raising what the DSP side had reached meanwhile
  instead came late by a varying amount and, when it caught up, several at once,
  which the interrupt controller merges: the arpeggio wavered and slowed with
  the host's speed (126-129 instead of 124.8 ms per step on a Core Ultra
  laptop, steps up to 13 ms off). The wait also bounds the DSP side's lag to
  one block; without a bound a DSP side slower than the CPU (one DSP thread,
  few or busy cores, the unpaced boot) fell behind without limit, and since
  MIDI arrives in CPU time a note was heard that much later (5 s with one DSP
  thread, found by a Windows port's headless test). Raising them without the
  delay made the CPU wait at each for the DSP side's last samples (2.7 instead
  of 3.0 x real time, 1.2 instead of 1.4 x on two cores). The steps still vary
  by one control period (1.45 ms) either way, in step with the DSP side too
  (`HYDRA_DSP_SYNC`).
- `DspBlock::setThreads` takes the workers' starting generation before it
  launches them: a worker that started late used to take the first sample as
  already seen, and `advanceToSample` waited for it forever (on one core it
  hung at once; `tools/dsp_start_race.cpp` reproduces it).

## Boot status (firmware 2.2.0)

The firmware boots completely: the effects DSPs run at 3.4 s of emulated
time, the main CPU's init ends at 0.74 s, and the panel's start-screen
animation ends at 7.1 s, after which the panel and the main CPU exchange
frames (timeline: "Boot timeline" under Panel MCU link). The emulator plays a
quiet warm-up note meanwhile and goes live 0.3 s after the first link byte.
In the steady state the main loop runs with timer interrupts at 86 Hz,
44.1 kHz, ~1 kHz and one more channel (line 6). Patches play at the right
pitch with their effects; all panel controls (buttons, encoders, knobs, arp
switches, pads), displays and lights work.

## Not emulated / not studied

- CV / Gate inputs and outputs: the system settings for them are known (see
  NOR flash), the jacks are not modelled.
- USB MIDI: the plugin uses the instrument's serial MIDI input only.
- MIDI from the instrument to the host: the firmware's MIDI output (e.g.
  controller messages when the panel is used, arpeggiator notes) is not
  passed on; the plugin declares no MIDI output.
- The pads' polyphonic aftertouch towards the host (it reaches the firmware).

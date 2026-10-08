# Brainscape DWT measurement pass (Daisy Seed)

- **brainscape_bench (engine code ITCM)** (log bench-bench-ITCM.log): archive `4f4ddaa3583e46f2`, firmware a91aa8ad64c7, clock 480000000 Hz, measurement overhead 1 cycles
  caches: I on, D on (CCR 0x00070200); SDRAM MPU region RASR 0x03030033; QSPI CR 0x01400001 CCR 0x1F18EDEB; mem* in ITCM
  device: bench image (brainscape_bench), Daisy Seed 1.2 / rev7 (PCM3060; PD5 strap), BOOT_QSPI
  firmware a91aa8ad64c7, libDaisy v9.0.0 08087203debc646d3710ec8caa35bac6cf20ab5e, arm-none-eabi-gcc 10.3.1 20210824 (release)
  engine archive sha256 4f4ddaa3583e46f29b84a008f5a80ad0938cacf05f2e961f70b1595ebf69da58, sound revision 1, engine code in ITCM
  sysclk 480000000 Hz, boot region QSPI flash, bootloader Daisy bootloader >= v6.1, FPSCR 0x00000010, FPDSCR 0x00000000
  I-cache on, D-cache on, 3 MPU region(s), QSPI CR 0x01400001 CCR 0x1F18EDEB, mem* in ITCM
- **brainscape_bench_xip (engine code XIP)** (log bench-bench-XIP.log): archive `4f4ddaa3583e46f2`, firmware a91aa8ad64c7, clock 480000000 Hz, measurement overhead 1 cycles
  caches: I on, D on (CCR 0x00070200); SDRAM MPU region RASR 0x03030033; QSPI CR 0x01400001 CCR 0x1F18EDEB; mem* in QSPI
  device: bench image (brainscape_bench_xip), Daisy Seed 1.2 / rev7 (PCM3060; PD5 strap), BOOT_QSPI
  firmware a91aa8ad64c7, libDaisy v9.0.0 08087203debc646d3710ec8caa35bac6cf20ab5e, arm-none-eabi-gcc 10.3.1 20210824 (release)
  engine archive sha256 4f4ddaa3583e46f29b84a008f5a80ad0938cacf05f2e961f70b1595ebf69da58, sound revision 1, engine code in XIP
  sysclk 480000000 Hz, boot region QSPI flash, bootloader Daisy bootloader >= v6.1, FPSCR 0x00000010, FPDSCR 0x00000000
  I-cache on, D-cache on, 3 MPU region(s), QSPI CR 0x01400001 CCR 0x1F18EDEB, mem* in QSPI
- **brainscape_bench_hooks (engine code ITCM, hooks)** (log bench-bench-ITCM-hooks.log): archive `da7b4f2e9b44aef7`, firmware a91aa8ad64c7, clock 480000000 Hz, measurement overhead 1 cycles
  caches: I on, D on (CCR 0x00070200); SDRAM MPU region RASR 0x03030033; QSPI CR 0x01400001 CCR 0x1F18EDEB; mem* in ITCM
  device: bench image (brainscape_bench_hooks), Daisy Seed 1.2 / rev7 (PCM3060; PD5 strap), BOOT_QSPI
  firmware a91aa8ad64c7, libDaisy v9.0.0 08087203debc646d3710ec8caa35bac6cf20ab5e, arm-none-eabi-gcc 10.3.1 20210824 (release)
  engine archive sha256 4f4ddaa3583e46f29b84a008f5a80ad0938cacf05f2e961f70b1595ebf69da58, sound revision 1, engine code in ITCM
  sysclk 480000000 Hz, boot region QSPI flash, bootloader Daisy bootloader >= v6.1, FPSCR 0x00000010, FPDSCR 0x00000000
  I-cache on, D-cache on, 3 MPU region(s), QSPI CR 0x01400001 CCR 0x1F18EDEB, mem* in ITCM

Budget: 480000 cycles per 48-frame block = 10,000 cycles per sample (480 MHz / 48 kHz). Interrupts are off around each measured call (and the cold-cache maintenance).

## Memory (grain-engine.md §7; from bench-bench-ITCM.log)

| tier          | PlanMemory (maxBlockSize 48) | arena    | where         |
|---------------|------------------------------|----------|---------------|
| Hot           | 16768                        | 24576    | DTCM          |
| Warm          | 129680                       | 139264   | AXI SRAM (D1) |
| Bulk          | 17545216                     | 17825792 | SDRAM         |
| Engine object | 6656                         | 7168     | DTCM          |

| linker section | start      | bytes    |
|----------------|------------|----------|
| image          | 0x90040000 | 220692   |
| text           | 0x9004ADB0 | 174600   |
| data           | 0x2402A400 | 1572     |
| bss            | 0x2402AA28 | 37792    |
| dtcmArenas     | 0x20000000 | 33792    |
| axiArenas      | 0x24000000 | 173056   |
| d2Dma          | 0x30000000 | 16384    |
| sdramArenas    | 0xC0000000 | 18941792 |
| heap           | 0xC1210760 | 48167072 |
| hot            | 0x20002400 | 24576    |
| engine         | 0x20000800 | 7168     |
| warm           | 0x24008400 | 139264   |
| bulk           | 0xC0110760 | 17825792 |

## FPU latency, normal vs subnormal (profile §4.2 test a, §8.3 Q1)

| log                        | dependent chain                                                 | cycles/instr FZ=0 | same program at FZ=1 (subnormal operands flushed to 0) |
|----------------------------|-----------------------------------------------------------------|-------------------|--------------------------------------------------------|
| bench-bench-ITCM.log       | vmul normal                                                     | 3.00              | 3.00                                                   |
| bench-bench-ITCM.log       | vmul subnormal operand and result                               | 3.00              | 3.00                                                   |
| bench-bench-ITCM.log       | vmul pair, normal throughout                                    | 3.00              | 3.00                                                   |
| bench-bench-ITCM.log       | vmul pair, every other result subnormal (underflow)             | 3.01              | 3.00                                                   |
| bench-bench-ITCM.log       | vadd normal (+0)                                                | 3.00              | 3.00                                                   |
| bench-bench-ITCM.log       | vadd subnormal operand and result (+0)                          | 3.00              | 3.00                                                   |
| bench-bench-ITCM.log       | vadd/vsub pair, normal throughout                               | 3.01              | 3.00                                                   |
| bench-bench-ITCM.log       | vadd/vsub pair, subnormal operands and results (nonzero addend) | 3.00              | 3.00                                                   |
| bench-bench-ITCM.log       | vdiv normal                                                     | 18.00             | 18.00                                                  |
| bench-bench-ITCM.log       | vdiv subnormal dividend and result                              | 18.00             | 4.00                                                   |
| bench-bench-ITCM.log       | vsqrt normal                                                    | 14.00             | 14.00                                                  |
| bench-bench-ITCM.log       | vsqrt subnormal operand                                         | 14.00             | 1.00                                                   |
| bench-bench-ITCM.log       | vmul.f64 normal                                                 | 7.00              | 7.00                                                   |
| bench-bench-XIP.log        | vmul normal                                                     | 3.00              | 3.01                                                   |
| bench-bench-XIP.log        | vmul subnormal operand and result                               | 3.00              | 3.00                                                   |
| bench-bench-XIP.log        | vmul pair, normal throughout                                    | 3.00              | 3.00                                                   |
| bench-bench-XIP.log        | vmul pair, every other result subnormal (underflow)             | 3.00              | 3.00                                                   |
| bench-bench-XIP.log        | vadd normal (+0)                                                | 3.00              | 3.00                                                   |
| bench-bench-XIP.log        | vadd subnormal operand and result (+0)                          | 3.00              | 3.00                                                   |
| bench-bench-XIP.log        | vadd/vsub pair, normal throughout                               | 3.00              | 3.00                                                   |
| bench-bench-XIP.log        | vadd/vsub pair, subnormal operands and results (nonzero addend) | 3.00              | 3.00                                                   |
| bench-bench-XIP.log        | vdiv normal                                                     | 18.00             | 18.00                                                  |
| bench-bench-XIP.log        | vdiv subnormal dividend and result                              | 18.00             | 4.00                                                   |
| bench-bench-XIP.log        | vsqrt normal                                                    | 14.00             | 14.00                                                  |
| bench-bench-XIP.log        | vsqrt subnormal operand                                         | 14.00             | 1.00                                                   |
| bench-bench-XIP.log        | vmul.f64 normal                                                 | 7.00              | 7.00                                                   |
| bench-bench-ITCM-hooks.log | vmul normal                                                     | 3.01              | 3.01                                                   |
| bench-bench-ITCM-hooks.log | vmul subnormal operand and result                               | 3.00              | 3.00                                                   |
| bench-bench-ITCM-hooks.log | vmul pair, normal throughout                                    | 3.00              | 3.00                                                   |
| bench-bench-ITCM-hooks.log | vmul pair, every other result subnormal (underflow)             | 3.00              | 3.00                                                   |
| bench-bench-ITCM-hooks.log | vadd normal (+0)                                                | 3.00              | 3.00                                                   |
| bench-bench-ITCM-hooks.log | vadd subnormal operand and result (+0)                          | 3.00              | 3.00                                                   |
| bench-bench-ITCM-hooks.log | vadd/vsub pair, normal throughout                               | 3.00              | 3.00                                                   |
| bench-bench-ITCM-hooks.log | vadd/vsub pair, subnormal operands and results (nonzero addend) | 3.00              | 3.00                                                   |
| bench-bench-ITCM-hooks.log | vdiv normal                                                     | 18.00             | 18.00                                                  |
| bench-bench-ITCM-hooks.log | vdiv subnormal dividend and result                              | 18.00             | 4.00                                                   |
| bench-bench-ITCM-hooks.log | vsqrt normal                                                    | 14.00             | 14.00                                                  |
| bench-bench-ITCM-hooks.log | vsqrt subnormal operand                                         | 14.00             | 1.00                                                   |
| bench-bench-ITCM-hooks.log | vmul.f64 normal                                                 | 7.00              | 7.00                                                   |

Subnormal penalty at FZ = 0 (bench-bench-ITCM.log): vmul with subnormal operands 1.00x; an underflowing vmul pair 1.00x; a subnormal vadd/vsub pair (nonzero addend) 1.00x the normal latency. The FZ = 1 column runs the same instructions with the operands flushed to zero, so it times zero arithmetic, not subnormal arithmetic under FZ.

Subnormal penalty at FZ = 0 (bench-bench-XIP.log): vmul with subnormal operands 1.00x; an underflowing vmul pair 1.00x; a subnormal vadd/vsub pair (nonzero addend) 1.00x the normal latency. The FZ = 1 column runs the same instructions with the operands flushed to zero, so it times zero arithmetic, not subnormal arithmetic under FZ.

Subnormal penalty at FZ = 0 (bench-bench-ITCM-hooks.log): vmul with subnormal operands 1.00x; an underflowing vmul pair 1.00x; a subnormal vadd/vsub pair (nonzero addend) 1.00x the normal latency. The FZ = 1 column runs the same instructions with the operands flushed to zero, so it times zero arithmetic, not subnormal arithmetic under FZ.

## The deterministic flush idiom (profile §4.3, §8.3 Q5)

| log                        | chains                            | form                           | cycles/update | flush cost/site | x 45 sites/sample | of budget |
|----------------------------|-----------------------------------|--------------------------------|---------------|-----------------|-------------------|-----------|
| bench-bench-ITCM.log       | 8 independent (ILP hides latency) | none                           | 2.14          | 0.00            | 0                 | 0.0%      |
| bench-bench-ITCM.log       | 8 independent (ILP hides latency) | bit test (engine, FlushTiny.h) | 4.16          | 2.02            | 91                | 0.9%      |
| bench-bench-ITCM.log       | 8 independent (ILP hides latency) | two compares                   | 12.29         | 10.15           | 457               | 4.6%      |
| bench-bench-ITCM.log       | 1 recursive (latency)             | none                           | 6.05          | 0.00            | 0                 | 0.0%      |
| bench-bench-ITCM.log       | 1 recursive (latency)             | bit test (engine, FlushTiny.h) | 10.08         | 4.03            | 181               | 1.8%      |
| bench-bench-ITCM.log       | 1 recursive (latency)             | two compares                   | 14.04         | 7.99            | 359               | 3.6%      |
| bench-bench-XIP.log        | 8 independent (ILP hides latency) | none                           | 2.15          | 0.00            | 0                 | 0.0%      |
| bench-bench-XIP.log        | 8 independent (ILP hides latency) | bit test (engine, FlushTiny.h) | 4.16          | 2.01            | 90                | 0.9%      |
| bench-bench-XIP.log        | 8 independent (ILP hides latency) | two compares                   | 12.28         | 10.14           | 456               | 4.6%      |
| bench-bench-XIP.log        | 1 recursive (latency)             | none                           | 6.02          | 0.00            | 0                 | 0.0%      |
| bench-bench-XIP.log        | 1 recursive (latency)             | bit test (engine, FlushTiny.h) | 10.01         | 3.99            | 179               | 1.8%      |
| bench-bench-XIP.log        | 1 recursive (latency)             | two compares                   | 14.04         | 8.02            | 361               | 3.6%      |
| bench-bench-ITCM-hooks.log | 8 independent (ILP hides latency) | none                           | 2.15          | 0.00            | 0                 | 0.0%      |
| bench-bench-ITCM-hooks.log | 8 independent (ILP hides latency) | bit test (engine, FlushTiny.h) | 4.16          | 2.01            | 91                | 0.9%      |
| bench-bench-ITCM-hooks.log | 8 independent (ILP hides latency) | two compares                   | 12.28         | 10.14           | 456               | 4.6%      |
| bench-bench-ITCM-hooks.log | 1 recursive (latency)             | none                           | 6.02          | 0.00            | 0                 | 0.0%      |
| bench-bench-ITCM-hooks.log | 1 recursive (latency)             | bit test (engine, FlushTiny.h) | 10.03         | 4.01            | 181               | 1.8%      |
| bench-bench-ITCM-hooks.log | 1 recursive (latency)             | two compares                   | 14.04         | 8.02            | 361               | 3.6%      |

The engine's 45 sites per sample (the profile's derived count) sit on recursions, so the per-sample cost lies between the 8-chain figure (a lower bound: independent updates overlap the flush) and the 1-chain figure (an upper bound: every flush on the critical path). Both are estimates from a measured per-site cost.

## Restart and loads against the SDRAM's own floor (profile §5.8, §8.3 Q9)

| log                        | operation               | state                                            | cycles   | ms    |
|----------------------------|-------------------------|--------------------------------------------------|----------|-------|
| bench-bench-ITCM.log       | Restart                 | after 2 s of rendering                           | 22645866 | 47.18 |
| bench-bench-ITCM.log       | Restart                 | again, nothing rendered since (skips the clears) | 9279     | 0.02  |
| bench-bench-ITCM.log       | LoadPreset(Exact)       | after 2 s of rendering                           | 22640272 | 47.17 |
| bench-bench-ITCM.log       | Reset                   | after 2 s of rendering (real-time subset)        | 20487    | 0.04  |
| bench-bench-ITCM.log       | ClearHistory            | after rendering                                  | 22622276 | 47.13 |
| bench-bench-ITCM.log       | raw clear, 16 MiB SDRAM | memset (platform/MemFunctions.c)                 | 21586568 | 44.97 |
| bench-bench-ITCM.log       | raw clear, 16 MiB SDRAM | STM loop, 8 registers (hardware floor)           | 21585334 | 44.97 |
| bench-bench-ITCM.log       | Init                    | canonical config, maxBlockSize 48                | 24065117 | 50.14 |
| bench-bench-XIP.log        | Restart                 | after 2 s of rendering                           | 22693847 | 47.28 |
| bench-bench-XIP.log        | Restart                 | again, nothing rendered since (skips the clears) | 36089    | 0.08  |
| bench-bench-XIP.log        | LoadPreset(Exact)       | after 2 s of rendering                           | 22697766 | 47.29 |
| bench-bench-XIP.log        | Reset                   | after 2 s of rendering (real-time subset)        | 63786    | 0.13  |
| bench-bench-XIP.log        | ClearHistory            | after rendering                                  | 22627629 | 47.14 |
| bench-bench-XIP.log        | raw clear, 16 MiB SDRAM | memset (platform/MemFunctions.c)                 | 21588261 | 44.98 |
| bench-bench-XIP.log        | raw clear, 16 MiB SDRAM | STM loop, 8 registers (hardware floor)           | 21586476 | 44.97 |
| bench-bench-XIP.log        | Init                    | canonical config, maxBlockSize 48                | 24184858 | 50.39 |
| bench-bench-ITCM-hooks.log | Restart                 | after 2 s of rendering                           | 22646752 | 47.18 |
| bench-bench-ITCM-hooks.log | Restart                 | again, nothing rendered since (skips the clears) | 10001    | 0.02  |
| bench-bench-ITCM-hooks.log | LoadPreset(Exact)       | after 2 s of rendering                           | 22639207 | 47.17 |
| bench-bench-ITCM-hooks.log | Reset                   | after 2 s of rendering (real-time subset)        | 18340    | 0.04  |
| bench-bench-ITCM-hooks.log | ClearHistory            | after rendering                                  | 22622370 | 47.13 |
| bench-bench-ITCM-hooks.log | raw clear, 16 MiB SDRAM | memset (platform/MemFunctions.c)                 | 21585320 | 44.97 |
| bench-bench-ITCM-hooks.log | raw clear, 16 MiB SDRAM | STM loop, 8 registers (hardware floor)           | 21586756 | 44.97 |
| bench-bench-ITCM-hooks.log | Init                    | canonical config, maxBlockSize 48                | 24066357 | 50.14 |

bench-bench-ITCM.log: a dirty Restart takes 47.2 ms = 47 audio blocks of 1 ms, so an Exact load mutes the wet path that long (the profile estimated 45-160 ms). Clearing 16 MiB of SDRAM alone takes 45.0 ms with the firmware's memset and 45.0 ms with an 8-register STM loop (the core's floor without DMA). Q9's other half, the restart watermark (§5.8), does not exist yet.

bench-bench-XIP.log: a dirty Restart takes 47.3 ms = 47 audio blocks of 1 ms, so an Exact load mutes the wet path that long (the profile estimated 45-160 ms). Clearing 16 MiB of SDRAM alone takes 45.0 ms with the firmware's memset and 45.0 ms with an 8-register STM loop (the core's floor without DMA). Q9's other half, the restart watermark (§5.8), does not exist yet.

bench-bench-ITCM-hooks.log: a dirty Restart takes 47.2 ms = 47 audio blocks of 1 ms, so an Exact load mutes the wet path that long (the profile estimated 45-160 ms). Clearing 16 MiB of SDRAM alone takes 45.0 ms with the firmware's memset and 45.0 ms with an 8-register STM loop (the core's floor without DMA). Q9's other half, the restart watermark (§5.8), does not exist yet.

## Cycles per 48-frame block (grain-engine.md §8, profile §7.2)

| config                                                   | cache | log                        | mean c/smp | mean   | p99 c/smp | p99    | p99.9 c/smp | p99.9  | max c/smp | max    | max at block |  |
|----------------------------------------------------------|-------|----------------------------|------------|--------|-----------|--------|-------------|--------|-----------|--------|--------------|--|
| default                                                  | warm  | bench-bench-ITCM.log       | 1902       | 19.0%  | 3664      | 36.6%  | 4096        | 41.0%  | 4138      | 41.4%  | 9834         |  |
| default                                                  | cold  | bench-bench-ITCM.log       | 1948       | 19.5%  | 3708      | 37.1%  | 4136        | 41.4%  | 4183      | 41.8%  | 9834         |  |
| nominal                                                  | warm  | bench-bench-ITCM.log       | 7774       | 77.7%  | 9789      | 97.9%  | 9885        | 98.8%  | 9915      | 99.1%  | 2933         |  |
| nominal                                                  | cold  | bench-bench-ITCM.log       | 7911       | 79.1%  | 9854      | 98.5%  | 9952        | 99.5%  | 10028     | 100.3% | 7434         |  |
| pess_render                                              | warm  | bench-bench-ITCM.log       | 9062       | 90.6%  | 12699     | 127.0% | 13254       | 132.5% | 13554     | 135.5% | 1482         |  |
| pess_render                                              | cold  | bench-bench-ITCM.log       | 9085       | 90.9%  | 12753     | 127.5% | 13316       | 133.2% | 13616     | 136.2% | 1482         |  |
| pess_births                                              | warm  | bench-bench-ITCM.log       | 11958      | 119.6% | 15300     | 153.0% | 16092       | 160.9% | 16847     | 168.5% | 5007         |  |
| pess_births                                              | cold  | bench-bench-ITCM.log       | 12044      | 120.4% | 15386     | 153.9% | 16168       | 161.7% | 16897     | 169.0% | 5007         |  |
| pess_events: sweeps every block, triggers, freeze, loads | warm  | bench-bench-ITCM.log       | 8922       | 89.2%  | 11812     | 118.1% | 12965       | 129.6% | 14631     | 146.3% | 8010         |  |
| pess_events: sweeps every block, triggers, freeze, loads | cold  | bench-bench-ITCM.log       | 8992       | 89.9%  | 11879     | 118.8% | 13142       | 131.4% | 14786     | 147.9% | 8010         |  |
| corpus:tail_post_fb                                      | warm  | bench-bench-ITCM.log       | 3165       | 31.7%  | 4947      | 49.5%  | 5391        | 53.9%  | 5444      | 54.4%  | 421          |  |
| corpus:tail_post_fb                                      | cold  | bench-bench-ITCM.log       | 3185       | 31.8%  | 4950      | 49.5%  | 5374        | 53.7%  | 5432      | 54.3%  | 3957         |  |
| corpus:pitch_reverse_spray                               | warm  | bench-bench-ITCM.log       | 5482       | 54.8%  | 8008      | 80.1%  | 8225        | 82.3%  | 8464      | 84.6%  | 5429         |  |
| corpus:pitch_reverse_spray                               | cold  | bench-bench-ITCM.log       | 5525       | 55.2%  | 8037      | 80.4%  | 8246        | 82.5%  | 8479      | 84.8%  | 5429         |  |
| corpus:max_delay_spray_rev_up24                          | warm  | bench-bench-ITCM.log       | 2501       | 25.0%  | 4301      | 43.0%  | 4766        | 47.7%  | 4830      | 48.3%  | 4261         |  |
| corpus:max_delay_spray_rev_up24                          | cold  | bench-bench-ITCM.log       | 2521       | 25.2%  | 4320      | 43.2%  | 4783        | 47.8%  | 4826      | 48.3%  | 4213         |  |
| corpus:dense_1ms                                         | warm  | bench-bench-ITCM.log       | 7997       | 80.0%  | 10805     | 108.1% | 11327       | 113.3% | 11863     | 118.6% | 5007         |  |
| corpus:dense_1ms                                         | cold  | bench-bench-ITCM.log       | 8118       | 81.2%  | 10898     | 109.0% | 11414       | 114.1% | 11975     | 119.7% | 5007         |  |
| default                                                  | warm  | bench-bench-XIP.log        | 2046       | 20.5%  | 4236      | 42.4%  | 4502        | 45.0%  | 4750      | 47.5%  | 1557         |  |
| default                                                  | cold  | bench-bench-XIP.log        | 3305       | 33.0%  | 5905      | 59.0%  | 6157        | 61.6%  | 6419      | 64.2%  | 9834         |  |
| nominal                                                  | warm  | bench-bench-XIP.log        | 8840       | 88.4%  | 11040     | 110.4% | 11169       | 111.7% | 11255     | 112.5% | 5845         |  |
| nominal                                                  | cold  | bench-bench-XIP.log        | 10719      | 107.2% | 12850     | 128.5% | 12978       | 129.8% | 13068     | 130.7% | 6330         |  |
| pess_render                                              | warm  | bench-bench-XIP.log        | 10247      | 102.5% | 14138     | 141.4% | 14786       | 147.9% | 14978     | 149.8% | 3402         |  |
| pess_render                                              | cold  | bench-bench-XIP.log        | 12078      | 120.8% | 16009     | 160.1% | 16675       | 166.7% | 17072     | 170.7% | 1482         |  |
| pess_births                                              | warm  | bench-bench-XIP.log        | 13267      | 132.7% | 16801     | 168.0% | 17624       | 176.2% | 18359     | 183.6% | 5007         |  |
| pess_births                                              | cold  | bench-bench-XIP.log        | 15167      | 151.7% | 18742     | 187.4% | 19653       | 196.5% | 20281     | 202.8% | 5007         |  |
| pess_events: sweeps every block, triggers, freeze, loads | warm  | bench-bench-XIP.log        | 10797      | 108.0% | 14268     | 142.7% | 15954       | 159.5% | 17811     | 178.1% | 4000         |  |
| pess_events: sweeps every block, triggers, freeze, loads | cold  | bench-bench-XIP.log        | 12399      | 124.0% | 15911     | 159.1% | 17770       | 177.7% | 19374     | 193.7% | 4000         |  |
| corpus:tail_post_fb                                      | warm  | bench-bench-XIP.log        | 3649       | 36.5%  | 5980      | 59.8%  | 6235        | 62.4%  | 6556      | 65.6%  | 421          |  |
| corpus:tail_post_fb                                      | cold  | bench-bench-XIP.log        | 5196       | 52.0%  | 7809      | 78.1%  | 8056        | 80.6%  | 8324      | 83.2%  | 421          |  |
| corpus:pitch_reverse_spray                               | warm  | bench-bench-XIP.log        | 5844       | 58.4%  | 8530      | 85.3%  | 8867        | 88.7%  | 9160      | 91.6%  | 5429         |  |
| corpus:pitch_reverse_spray                               | cold  | bench-bench-XIP.log        | 7286       | 72.9%  | 10277     | 102.8% | 10632       | 106.3% | 10885     | 108.8% | 5429         |  |
| corpus:max_delay_spray_rev_up24                          | warm  | bench-bench-XIP.log        | 2614       | 26.1%  | 4512      | 45.1%  | 4972        | 49.7%  | 5178      | 51.8%  | 5525         |  |
| corpus:max_delay_spray_rev_up24                          | cold  | bench-bench-XIP.log        | 3881       | 38.8%  | 5813      | 58.1%  | 6685        | 66.8%  | 6890      | 68.9%  | 5525         |  |
| corpus:dense_1ms                                         | warm  | bench-bench-XIP.log        | 8640       | 86.4%  | 11562     | 115.6% | 12031       | 120.3% | 12608     | 126.1% | 5007         |  |
| corpus:dense_1ms                                         | cold  | bench-bench-XIP.log        | 10473      | 104.7% | 13387     | 133.9% | 13917       | 139.2% | 14493     | 144.9% | 5007         |  |
| default                                                  | warm  | bench-bench-ITCM-hooks.log | 1902       | 19.0%  | 3664      | 36.6%  | 4095        | 40.9%  | 4131      | 41.3%  | 9834         |  |
| default                                                  | cold  | bench-bench-ITCM-hooks.log | 1947       | 19.5%  | 3706      | 37.1%  | 4138        | 41.4%  | 4173      | 41.7%  | 9834         |  |
| nominal                                                  | warm  | bench-bench-ITCM-hooks.log | 7776       | 77.8%  | 9791      | 97.9%  | 9887        | 98.9%  | 9964      | 99.6%  | 4159         |  |
| nominal                                                  | cold  | bench-bench-ITCM-hooks.log | 7910       | 79.1%  | 9853      | 98.5%  | 9951        | 99.5%  | 10009     | 100.1% | 8661         |  |
| pess_render                                              | warm  | bench-bench-ITCM-hooks.log | 9063       | 90.6%  | 12709     | 127.1% | 13245       | 132.5% | 13529     | 135.3% | 1482         |  |
| pess_render                                              | cold  | bench-bench-ITCM-hooks.log | 9085       | 90.8%  | 12731     | 127.3% | 13292       | 132.9% | 13562     | 135.6% | 1482         |  |
| pess_births                                              | warm  | bench-bench-ITCM-hooks.log | 11960      | 119.6% | 15307     | 153.1% | 16112       | 161.1% | 16842     | 168.4% | 5007         |  |
| pess_births                                              | cold  | bench-bench-ITCM-hooks.log | 12045      | 120.4% | 15385     | 153.9% | 16179       | 161.8% | 16904     | 169.0% | 5007         |  |
| pess_events: sweeps every block, triggers, freeze, loads | warm  | bench-bench-ITCM-hooks.log | 8925       | 89.3%  | 11807     | 118.1% | 13050       | 130.5% | 14630     | 146.3% | 8010         |  |
| pess_events: sweeps every block, triggers, freeze, loads | cold  | bench-bench-ITCM-hooks.log | 8985       | 89.9%  | 11862     | 118.6% | 13097       | 131.0% | 14723     | 147.2% | 8010         |  |
| corpus:tail_post_fb                                      | warm  | bench-bench-ITCM-hooks.log | 3167       | 31.7%  | 4949      | 49.5%  | 5381        | 53.8%  | 5428      | 54.3%  | 421          |  |
| corpus:tail_post_fb                                      | cold  | bench-bench-ITCM-hooks.log | 3184       | 31.8%  | 4949      | 49.5%  | 5374        | 53.7%  | 5432      | 54.3%  | 3967         |  |
| corpus:pitch_reverse_spray                               | warm  | bench-bench-ITCM-hooks.log | 5483       | 54.8%  | 8012      | 80.1%  | 8224        | 82.2%  | 8437      | 84.4%  | 5429         |  |
| corpus:pitch_reverse_spray                               | cold  | bench-bench-ITCM-hooks.log | 5524       | 55.2%  | 8037      | 80.4%  | 8254        | 82.5%  | 8468      | 84.7%  | 5429         |  |
| corpus:max_delay_spray_rev_up24                          | warm  | bench-bench-ITCM-hooks.log | 2501       | 25.0%  | 4305      | 43.0%  | 4766        | 47.7%  | 4833      | 48.3%  | 4261         |  |
| corpus:max_delay_spray_rev_up24                          | cold  | bench-bench-ITCM-hooks.log | 2520       | 25.2%  | 4320      | 43.2%  | 4778        | 47.8%  | 4829      | 48.3%  | 4213         |  |
| corpus:dense_1ms                                         | warm  | bench-bench-ITCM-hooks.log | 7997       | 80.0%  | 10807     | 108.1% | 11333       | 113.3% | 11865     | 118.7% | 5007         |  |
| corpus:dense_1ms                                         | cold  | bench-bench-ITCM-hooks.log | 8117       | 81.2%  | 10890     | 108.9% | 11408       | 114.1% | 11974     | 119.7% | 5007         |  |

bench-bench-ITCM.log: warm and cold renders of each configuration produced identical output.

bench-bench-XIP.log: warm and cold renders of each configuration produced identical output.

bench-bench-ITCM-hooks.log: warm and cold renders of each configuration produced identical output.

## Explicit FMA (profile §7.3, §8.3 Q2)

From bench-bench-ITCM.log, contraction off (every build: the profile's flags forbid contraction): the worst warm-cache pessimistic block is 168.5% (pess_births, p99.9 160.9%), the worst with cold caches 169.0% (pess_births). The profile estimated 77-78 % for the pessimistic row.
**Verdict (§7.3 rule):** contraction-off THREATENS the budget at 64 voices (a block over its deadline): explicit FMA in the inner loops is a candidate; build and measure it before adopting.
This pass measures the contraction-off cost only: no build with contraction on or with explicit FMA exists to compare against, and the births suite bounds the per-birth cost from above without deciding polynomial kernels against Init-built tables (§3.9).

## Code placement: QSPI execute-in-place against ITCM (profile §7.1)

| suite  | config                                                   | cache | XIP / ITCM mean | p99.9 | max   |
|--------|----------------------------------------------------------|-------|-----------------|-------|-------|
| blocks | default                                                  | warm  | 1.076           | 1.099 | 1.148 |
| blocks | default                                                  | cold  | 1.697           | 1.489 | 1.534 |
| blocks | nominal                                                  | warm  | 1.137           | 1.130 | 1.135 |
| blocks | nominal                                                  | cold  | 1.355           | 1.304 | 1.303 |
| blocks | pess_render                                              | warm  | 1.131           | 1.116 | 1.105 |
| blocks | pess_render                                              | cold  | 1.329           | 1.252 | 1.254 |
| blocks | pess_births                                              | warm  | 1.109           | 1.095 | 1.090 |
| blocks | pess_births                                              | cold  | 1.259           | 1.216 | 1.200 |
| blocks | pess_events: sweeps every block, triggers, freeze, loads | warm  | 1.210           | 1.231 | 1.217 |
| blocks | pess_events: sweeps every block, triggers, freeze, loads | cold  | 1.379           | 1.352 | 1.310 |
| blocks | corpus:tail_post_fb                                      | warm  | 1.153           | 1.157 | 1.204 |
| blocks | corpus:tail_post_fb                                      | cold  | 1.632           | 1.499 | 1.532 |
| blocks | corpus:pitch_reverse_spray                               | warm  | 1.066           | 1.078 | 1.082 |
| blocks | corpus:pitch_reverse_spray                               | cold  | 1.319           | 1.289 | 1.284 |
| blocks | corpus:max_delay_spray_rev_up24                          | warm  | 1.045           | 1.043 | 1.072 |
| blocks | corpus:max_delay_spray_rev_up24                          | cold  | 1.540           | 1.398 | 1.428 |
| blocks | corpus:dense_1ms                                         | warm  | 1.080           | 1.062 | 1.063 |
| blocks | corpus:dense_1ms                                         | cold  | 1.290           | 1.219 | 1.210 |
| stage  | stages:all stages                                        | warm  | 1.131           | 1.113 | 1.113 |
| stage  | stages:-modulation                                       | warm  | 1.124           | 1.106 | 1.106 |
| stage  | stages:-post delay                                       | warm  | 1.130           | 1.113 | 1.108 |
| stage  | stages:-reverb                                           | warm  | 1.130           | 1.112 | 1.111 |
| stage  | stages:-filter                                           | warm  | 1.110           | 1.096 | 1.094 |
| stage  | stages:-feedback                                         | warm  | 1.129           | 1.114 | 1.110 |
| stage  | stages:-onset grains and marks                           | warm  | 1.124           | 1.114 | 1.111 |
| stage  | stages:-pitch (r = 1)                                    | warm  | 1.147           | 1.133 | 1.123 |
| stage  | stages:-grains (1 voice)                                 | warm  | 1.101           | 1.185 | 1.208 |
| births | births:48 voices, 1 ms                                   | warm  | 1.053           | 1.055 | 1.061 |
| births | births:48 voices, 20 ms                                  | warm  | 1.086           | 1.084 | 1.087 |
| births | births:16 voices, 1 ms                                   | warm  | 1.125           | 1.119 | 1.116 |
| births | births:16 voices, 20 ms                                  | warm  | 1.127           | 1.121 | 1.129 |

Both builds place the firmware's memcpy, memmove and memset with the engine's code (ITCM in bench-bench-ITCM.log, QSPI in bench-bench-XIP.log), so the ratio covers them too; libgcc's helpers likewise.

## Per-stage cost by difference (pessimistic configuration, warm cache)

| log                        | configuration                  | mean c/smp | p99.9 c/smp | max c/smp | stage cost (mean) c/smp |
|----------------------------|--------------------------------|------------|-------------|-----------|-------------------------|
| bench-bench-ITCM.log       | stages:all stages              | 9062       | 13266       | 13566     |                         |
| bench-bench-ITCM.log       | stages:-modulation             | 8870       | 13065       | 13376     | 192                     |
| bench-bench-ITCM.log       | stages:-post delay             | 8743       | 12928       | 13245     | 319                     |
| bench-bench-ITCM.log       | stages:-reverb                 | 8504       | 12703       | 12999     | 559                     |
| bench-bench-ITCM.log       | stages:-filter                 | 8863       | 13041       | 13341     | 200                     |
| bench-bench-ITCM.log       | stages:-feedback               | 8995       | 13223       | 13464     | 68                      |
| bench-bench-ITCM.log       | stages:-onset grains and marks | 9918       | 13419       | 13842     | -855                    |
| bench-bench-ITCM.log       | stages:-pitch (r = 1)          | 8530       | 11654       | 12076     | 532                     |
| bench-bench-ITCM.log       | stages:-grains (1 voice)       | 2513       | 4657        | 4730      | 6549                    |
| bench-bench-XIP.log        | stages:all stages              | 10248      | 14771       | 15094     |                         |
| bench-bench-XIP.log        | stages:-modulation             | 9971       | 14452       | 14799     | 278                     |
| bench-bench-XIP.log        | stages:-post delay             | 9876       | 14394       | 14669     | 372                     |
| bench-bench-XIP.log        | stages:-reverb                 | 9607       | 14131       | 14447     | 641                     |
| bench-bench-XIP.log        | stages:-filter                 | 9834       | 14292       | 14600     | 414                     |
| bench-bench-XIP.log        | stages:-feedback               | 10159      | 14728       | 14939     | 89                      |
| bench-bench-XIP.log        | stages:-onset grains and marks | 11145      | 14954       | 15373     | -897                    |
| bench-bench-XIP.log        | stages:-pitch (r = 1)          | 9782       | 13207       | 13562     | 466                     |
| bench-bench-XIP.log        | stages:-grains (1 voice)       | 2767       | 5521        | 5714      | 7481                    |
| bench-bench-ITCM-hooks.log | stages:all stages              | 9063       | 13292       | 13581     |                         |
| bench-bench-ITCM-hooks.log | stages:-modulation             | 8872       | 13077       | 13357     | 192                     |
| bench-bench-ITCM-hooks.log | stages:-post delay             | 8745       | 12903       | 13244     | 318                     |
| bench-bench-ITCM-hooks.log | stages:-reverb                 | 8506       | 12683       | 12974     | 557                     |
| bench-bench-ITCM-hooks.log | stages:-filter                 | 8864       | 13073       | 13368     | 199                     |
| bench-bench-ITCM-hooks.log | stages:-feedback               | 8996       | 13204       | 13467     | 67                      |
| bench-bench-ITCM-hooks.log | stages:-onset grains and marks | 9920       | 13426       | 13847     | -857                    |
| bench-bench-ITCM-hooks.log | stages:-pitch (r = 1)          | 8532       | 11656       | 12022     | 531                     |
| bench-bench-ITCM-hooks.log | stages:-grains (1 voice)       | 2514       | 4661        | 4734      | 6550                    |

A stage's cost is the drop in the mean when it alone is switched off (exact bypass for the post stages). The onset detector has no bypass and is inside every figure. Maxima come from different blocks in each render, so no per-stage maximum is derived from them.

## Per-birth cost at the maximum birth rate, an upper bound (grain-engine.md §8 ScheduleGrain row)

| log                        | voices | births/block (1 ms) | births/block (20 ms) | mean c/smp 1 ms | mean c/smp 20 ms | cycles per birth | birth cost c/smp at 1 ms |
|----------------------------|--------|---------------------|----------------------|-----------------|------------------|------------------|--------------------------|
| bench-bench-ITCM.log       | 48     | 48.0                | 2.4                  | 12331           | 6439             | <= 6203          | <= 6203                  |
| bench-bench-ITCM.log       | 16     | 16.0                | 0.8                  | 4215            | 2997             | <= 3846          | <= 1282                  |
| bench-bench-XIP.log        | 48     | 48.0                | 2.4                  | 12988           | 6991             | <= 6313          | <= 6313                  |
| bench-bench-XIP.log        | 16     | 16.0                | 0.8                  | 4741            | 3377             | <= 4308          | <= 1436                  |
| bench-bench-ITCM-hooks.log | 48     | 48.0                | 2.4                  | 12338           | 6440             | <= 6209          | <= 6209                  |
| bench-bench-ITCM-hooks.log | 16     | 16.0                | 0.8                  | 4217            | 2998             | <= 3850          | <= 1283                  |

The 1 ms grains also read the ring at a new spray position every birth, so the difference includes that locality cost: an upper bound on ScheduleGrain, not a kernels-against-tables measurement.

## Silent tails and the denormal decision (profile §4.2 test b, §8.3 Q1)

| tail                                  | flush | log                        | blocks | silent from block | whole-run max c/smp | silent-tail max c/smp | silent-tail max | silent-tail p99.9 c/smp | silent-tail blocks with IDC or UFC | sha256           |
|---------------------------------------|-------|----------------------------|--------|-------------------|---------------------|-----------------------|-----------------|-------------------------|------------------------------------|------------------|
| golden:strums_tail_123s/tail_post_fb  | FZ=0  | bench-bench-ITCM.log       | 123000 | 3000              | 5447                | 5439                  | 54.4%           | 5338                    | -                                  | 2227acb0df4ff2b8 |
| noise-tail:pessimistic, feedback 0.95 | FZ=0  | bench-bench-ITCM.log       | 122000 | 2000              | 14315               | 14315                 | 143.1%          | 13322                   | -                                  | a11b5941da389289 |
| golden:strums_tail_123s/tail_post_fb  | FZ=0  | bench-bench-XIP.log        | 123000 | 3000              | 6540                | 6540                  | 65.4%           | 6197                    | -                                  | 2227acb0df4ff2b8 |
| noise-tail:pessimistic, feedback 0.95 | FZ=0  | bench-bench-XIP.log        | 122000 | 2000              | 15841               | 15841                 | 158.4%          | 14794                   | -                                  | a11b5941da389289 |
| golden:strums_tail_123s/tail_post_fb  | FZ=0  | bench-bench-ITCM-hooks.log | 123000 | 3000              | 5448                | 5438                  | 54.4%           | 5338                    | 0 / 120000                         | 2227acb0df4ff2b8 |
| golden:strums_tail_123s/tail_post_fb  | FZ=1  | bench-bench-ITCM-hooks.log | 123000 | 3000              | 5436                | 5426                  | 54.3%           | 5338                    | 0 / 120000                         | 2227acb0df4ff2b8 |
| noise-tail:pessimistic, feedback 0.95 | FZ=0  | bench-bench-ITCM-hooks.log | 122000 | 2000              | 14251               | 14251                 | 142.5%          | 13331                   | 0 / 120000                         | a11b5941da389289 |
| noise-tail:pessimistic, feedback 0.95 | FZ=1  | bench-bench-ITCM-hooks.log | 122000 | 2000              | 14269               | 14269                 | 142.7%          | 13327                   | 0 / 120000                         | a11b5941da389289 |

- bench-bench-ITCM.log, golden:strums_tail_123s/tail_post_fb at FZ=0: MATCHES golden.json
- bench-bench-XIP.log, golden:strums_tail_123s/tail_post_fb at FZ=0: MATCHES golden.json
- bench-bench-ITCM-hooks.log, golden:strums_tail_123s/tail_post_fb at FZ=0: MATCHES golden.json
- bench-bench-ITCM-hooks.log, golden:strums_tail_123s/tail_post_fb at FZ=1: MATCHES golden.json
- bench-bench-ITCM-hooks.log, golden:strums_tail_123s/tail_post_fb: silent-tail worst block FZ=0 / FZ=1 = 1.002 (rule: <= 1.2; whole run 1.002, context only); silent-tail blocks raising IDC or UFC at FZ = 1: 0.000 % (rule: <= 0.5 %; at FZ = 0 0.000 %, which Armv7-M under-reports: IDC needs FZ = 1 and UFC an inexact tiny result); the FZ = 1 and FZ = 0 renders equal golden.json -> **keep gradual underflow**
- bench-bench-ITCM-hooks.log, noise-tail:pessimistic, feedback 0.95: silent-tail worst block FZ=0 / FZ=1 = 0.999 (rule: <= 1.2; whole run 0.999, context only); silent-tail blocks raising IDC or UFC at FZ = 1: 0.000 % (rule: <= 0.5 %; at FZ = 0 0.000 %, which Armv7-M under-reports: IDC needs FZ = 1 and UFC an inexact tiny result); the FZ = 1 render equals the FZ = 0 render -> **keep gradual underflow**

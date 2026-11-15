---
id: S01
title: "FIC4 and the user crypto processor"
track: security
level: 3
summary: What FIC4 is, how the on-chip crypto processor can be owned by the processors or by the fabric, and why this project documents it instead of driving it.
repro: none
status: validated
hardware: [fic4, crypto-processor]
related: [S02, S03, F03]
---

# FIC4 and the user crypto processor

After this page you can say what FIC4 connects, which of the four ownership modes of the crypto processor needs it, what any design that uses the processor must contain, and which parts of its programming interface are public and which are not. It is a reading page: nothing here was run on the board, and the reason is part of what it teaches.

Every statement comes from Microchip's online documentation, from the Hart Software Services source, or from files in this project. Where the sources say nothing, the page says so.

## Before you start

- **Level 3.** You should know what the four fabric interface controllers are for (FIC0 and FIC3 are used in this project; see the system page).
- **Tools:** none. Read-only.

## The idea

The PolarFire SoC has, next to its processors and its FPGA, a hard cryptography block, the **user crypto processor**. Microchip describes it as an Athena TeraFire EXP-F5200B and says it is available in the "S" devices of the family. It offers, according to the same page:

| Group | What is listed |
|---|---|
| Symmetric | AES with 128, 192 and 256-bit keys in ECB, CBC, CFB, OFB, CTR and GCM modes, AES key wrap, AES-CMAC |
| Hashing and MAC | SHA-1, SHA-224, SHA-256, SHA-384, SHA-512 and two truncated SHA-512 variants; HMAC with each |
| Public key | elliptic curves from NIST P-192 to P-521 and three Brainpool curves (point operations, ECDSA, key generation, ECDH); RSA at 1024, 2048 and 3072 bits; DSA |
| Random numbers | SP800-90A CTR_DRBG-256 and an SP800-90B (draft) generator |
| Other | 256-bit key-tree derivation, side-channel-resistant implementation, zeroisation on request |

It runs at up to 200 MHz on the PolarFire SoC.

One engine, but two places it can be reached from, and that is where FIC4 comes in.

### Four ownership modes

| Mode | Who owns the engine | How it is reached | What the vendor says is needed |
|---|---|---|---|
| **MSS** | the processors | inside the MSS, through the MSS switch and two bridges; "totally disconnected from the fabric interface" | the vendor's library (CAL) running on a hart; control inputs (GO, PURGE) come from MSS system-register bits |
| **Fabric** | the fabric | through **FIC4**: "accessible from Fabric through AHB master and slave interfaces using FIC4" | "A soft processor is needed in the FPGA fabric to access the Cryptoprocessor using CAL driver"; the engine must be clocked from the fabric |
| **Shared-MSS** | the MSS at start; the fabric may request it | both | cooperation between the two designs |
| **Shared-Fabric** | the fabric at start; the MSS may request it | both | the same |

So **FIC4 is the fabric's door to this one engine**. Its two links are the AHB-Lite slave through which the fabric controls the engine and moves data, and an AHB-Lite master through which the engine's own DMA reads and writes fabric-side memory. The vendor gives 200 MHz as the engine's maximum clock, 70 MHz without its embedded DLL, and says the DLL must be enabled at 125 MHz or above.

### Ownership handover

In the shared modes, ownership does not change on its own. Six signals cross between the MSS and the fabric:

| Signal | Direction | Meaning |
|---|---|---|
| `CRYPTO_REQUEST_F2M` | fabric to MSS | the fabric is requesting or using the engine |
| `CRYPTO_RELEASE_F2M` | fabric to MSS | the fabric has released it |
| `CRYPTO_MSS_REQUEST_M2F` | MSS to fabric | the MSS is requesting or using it |
| `CRYPTO_MSS_RELEASE_M2F` | MSS to fabric | the MSS has released it |
| `CRYPTO_OWNER_M2F` | MSS to fabric | the fabric owns it and the fabric interface is enabled |
| `CRYPTO_MSS_OWNER_M2F` | MSS to fabric | the MSS owns it and the fabric interface is disabled |

The vendor's sequence, with the fabric as the current owner: the MSS asks; the fabric design **purges** the engine and asserts the release signal; a small state machine inside the MSS resets the engine and switches its clock to the MSS; the engine is released from reset and belongs to the MSS. The vendor recommends that the current owner purge before releasing, "to prevent sensitive data being accidentally released to the other system". The signals are asynchronous to the fabric clock, so the fabric design must synchronise them, and the MSS is not told when the fabric requests the engine: the vendor suggests wiring the request to a general-purpose fabric-to-MSS interrupt (the same kind of line as in module F03).

Other ports in the vendor's list: `GO` (start, for use without a host processor), `PURGE`, `STALL` (adds a clock of delay to vary the engine's external signature), `ALARM` (uncorrectable memory error), `BUSERROR`, `BUSY`, `COMPLETE`, `DLL_LOCK`, a security-mesh error and its clear input, and a streaming interface (below).

### A streaming interface

In the MSS and shared modes, the engine can also be fed and drained by fabric logic through plain data ports, clocked by the engine's clock (which must then come from the fabric). Transfers are commanded by the instructions "DXI" (transfer a block in) and "DXO" (transfer a block out), sent through the AHB slave. The vendor's page lists the signals (`CRYPTO_XWDATA_F2M`, `CRYPTO_XENABLE_F2M`, `CRYPTO_XINACCEPT_M2F` and so on) and does not describe their sequencing or width.

## On this board

- **The engine is configured as unused.** The Discovery Kit reference design's MSS configuration lists `CRYPTO UNUSED`. Nothing in the fabric connects to FIC4, and no software in this project touches the engine.
- **The device may have it.** The JTAG device report during this project read `MPFS095TS`; the vendor says the engine is in the "S" devices. That suggests it is present. It was not checked on the board.
- **The processors can see its plumbing.** The MSS register header in the boot monitor's tree has a block for the engine's security mesh and flags for the FIC4 DLL locking and unlocking, which software could read.
- **The boot monitor can use the engine.** Its code-signing check has two back-ends, chosen when it is built: an open-source software implementation (libecc, the default) and one that uses the user crypto processor through the vendor library (see S02). Which one the boot monitor in this chip's boot flash was built with is not known. No other user of the engine was found in the sources read for this project.

## What is not public, and what that decided

The vendor's feature page says: "Microchip provides an Athena TeraFire Cryptographic Applications Library (CAL) to access the User Cryptoprocessor functions", describes it as a C library that the application must call, and gives an e-mail address for requesting it. The pages read for this project give no register-level description of the engine's programming interface.

That settled the question this page started from. A fabric block could be built to wire FIC4, but nothing public says what to put on it: the vendor's own statement is that fabric mode needs a soft processor running CAL. Writing a driver from guesses about an undocumented register interface would break the one rule of this project: no invented addresses or commands. So:

| Possible build | Needs | Done here |
|---|---|---|
| Fabric mode over FIC4 | a soft processor in the fabric, CAL for it, the MSS configuration changed to a fabric or shared mode, a fabric rebuild | no |
| MSS mode from a hart | CAL (or the library inside the boot monitor's repository, whose terms for reuse are not stated), and enabling the engine | no |
| Ownership handshake only | a fabric block driving the request and release signals, an MSS configuration in a shared mode | no |

A note on the library in the boot monitor's open-source tree: it ships a prebuilt library and the matching headers for the crypto-processor back-end. The licence on the repository is MIT, but whether that covers redistributing the prebuilt library is not stated there, so this project does not copy or republish it.

## Safety

Changing the MSS configuration to another crypto mode changes what the boot monitor is built for and what the fabric netlist expects. Whether the boot monitor already in the chip's boot flash would cope was not established, and this project never reprograms that flash. So no part of the table above was attempted, and nothing on this page changes a board.

## Trade-offs and going further

- **Fabric mode costs a processor.** The engine's value there is hashing or encrypting at line rate next to fabric logic. The price is a soft core, a licence for CAL, and a more complex boot flow.
- **MSS mode costs a library.** It is the easy way to get SHA-384 or ECDSA on a hart, and the one the boot monitor takes.
- **Next step, if you want one:** request CAL from Microchip, then read how the boot monitor initialises the engine, and try a SHA-384 of a known string from the bare-metal supervisor core. That would not involve any key or security setting, but it is a use of a hardware block that this project has not validated, so it belongs on a branch.

## Sources

- Microchip, User Cryptoprocessor: [features](https://onlinedocs.microchip.com/oxy/GUID-DF33D7C4-5604-4C6B-92AF-8C864323C016-en-US-14/GUID-BCAF885A-EF59-4FBD-9587-6F9E1D47921D.html), [port list](https://onlinedocs.microchip.com/oxy/GUID-DF33D7C4-5604-4C6B-92AF-8C864323C016-en-US-14/GUID-195FC57B-4C3E-4E96-9837-43CE6BD4BD10.html), [MSS mode](https://onlinedocs.microchip.com/oxy/GUID-DF33D7C4-5604-4C6B-92AF-8C864323C016-en-US-14/GUID-3521506C-5DF5-4248-A76C-0658627F1854.html), [fabric mode](https://onlinedocs.microchip.com/oxy/GUID-DF33D7C4-5604-4C6B-92AF-8C864323C016-en-US-14/GUID-F93F5BF2-7498-4510-9CFB-ADDCBC62BFA9.html), [shared modes](https://onlinedocs.microchip.com/oxy/GUID-DF33D7C4-5604-4C6B-92AF-8C864323C016-en-US-14/GUID-0AEC38EA-34BF-4546-84E2-8052509AF7F9.html), [streaming interface](https://onlinedocs.microchip.com/oxy/GUID-DF33D7C4-5604-4C6B-92AF-8C864323C016-en-US-14/GUID-AD7D8707-A8F7-4A58-8F80-760F6198C51A.html).
- Hart Software Services source, `services/boot/hss_boot_secure.c` and `services/crypto/`, release v2026.04.1.
- The Discovery Kit reference design's MSS configuration, v2026.04.

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **How to run** | No board or tools needed |
| **In the system** | [FIC4 (AHB-Lite, crypto)](../system/README.md#fic4) · [User crypto processor](../system/README.md#crypto-processor) |
| **Related** | [S02 Secure boot on PolarFire SoC: the chain of trust, and what we left alone](S02-secure-boot.md)<br>[S03 A signed boot chain you can run](S03-signed-boot-chain.md)<br>[F03 Interrupts from fabric to hart: F2M lines, the PLIC and contexts](F03-fabric-interrupts.md) |
| **Status** | validated |
<!-- nav:end -->

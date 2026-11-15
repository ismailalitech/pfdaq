---
id: S03
title: "A signed boot chain you can run"
track: security
level: 2
summary: Sign a real HSS boot payload offline, verify it twice, and watch a toy chain of trust refuse six kinds of tampering. Nothing is flashed.
needs: [S02]
repro: sim
status: validated
hardware: [hss]
sources:
  - experiments/secure-boot-chain/chain_demo.py:check_image,boot,sign_image
  - experiments/secure-boot-chain/verify_boot_image.py:verify
  - experiments/secure-boot-chain/sign_and_verify.sh
evidence: [secure-boot-chain-demo, secure-boot-sign-verify]
related: [S02, S01]
---

# A signed boot chain you can run

After this lab you have signed a boot payload with the same tool and format the PolarFire SoC boot monitor uses, checked the signature with two independent programs, and seen a small chain of trust stop every attack you throw at it.

## Before you start

- **Level 2.** Module S02 explains what is being checked and why.
- **Tools:** Python 3 with the `cryptography` package, OpenSSL. For part 2 also the HSS payload generator (the Yocto build contains one) and the bare-metal payload that `firmware/build_payload.sh` builds.
- **Board:** none. Nothing in this lab is written to a board or an SD card.

## The idea

A signature is made over the whole image by the owner's private key. Anyone holding the public key can check it; nobody without the private key can forge it. In both parts of this lab the checker holds only a public key.

## Part 1: a toy chain of trust

```text
ROM  --verifies-->  stage 1  --verifies-->  stage 2  --verifies-->  application
```

The ROM holds only the root public key and the lowest version it will accept. Each image carries the public key for the next link inside the part its own signature covers. Algorithms: SHA-384 and ECDSA on curve P-384, the same as the boot monitor.

### Run it

```sh
python3 experiments/secure-boot-chain/chain_demo.py
```

### What you should see

Six scenarios. The first boots; the other five stop at the first link that fails:

```text
== 1. Everything is genuine
   ROM: stage 1 (version 2) is genuine, starting it
   stage 1: stage 2 (version 1) is genuine, starting it
   stage 2: application (version 1) is genuine, starting it
   application running: 'hello from the application'
   -> BOOTED

== 5. An old, genuinely signed stage 1 (rollback)
   HALT  stage 1: version 1 is older than the minimum 2 (rollback)
   -> stopped, nothing further was run
...
6 of 6 scenarios behaved as a chain of trust should.
```

Scenario 5 is the interesting one: the old image's signature is perfectly valid. Only the version check stops it.

## Part 2: the real format, offline

### Run it

```sh
firmware/build_payload.sh                      # builds the payload and its YAML under build/payload-bm
experiments/secure-boot-chain/sign_and_verify.sh
```

The script makes throwaway keys, builds the payload unsigned and signed (`hss-payload-generator -c … -p …`), then verifies four images with the vendor tool (`-d … -u …`) and with `verify_boot_image.py`.

### What you should see

```text
   signed, right key                  vendor tool: passed | independent verifier: VALID
   signed, one bit flipped            vendor tool: failed | independent verifier: NOT VALID
   never signed                       vendor tool: failed | independent verifier: NOT VALID
   signed, someone else's key         vendor tool: failed | independent verifier: NOT VALID
```

Two things to notice. The vendor tool wants the public key as DER, not PEM; with PEM it stops on an assertion. And an unsigned image fails not because the check sees "no signature" but because a field of zeros is not a valid signature: there is no special case to get wrong.

### How the independent verifier finds the signature

The signature field's position depends on constants chosen when the boot monitor is built, so the verifier does not hard-code it. It looks for the 8-byte image length, which must equal the file size, directly in front of a 144-byte field, zeroes that field, and checks that SHA-384 of the result equals the digest stored in the field before it trusts anything. In the image used here the field starts at byte 1488 of a 842,208-byte file.

## Evidence

Both outputs above are recorded (host-side runs; no board, no fabric). Keys are random per run, so only the messages are reproducible.

## When it goes wrong

| Symptom | Cause |
|---|---|
| The vendor tool aborts with an assertion on the public key | the key is PEM; it wants DER (`openssl ec -pubout -outform DER`) |
| `sign_and_verify.sh` cannot find the generator | set `HSS_PAYLOAD_GENERATOR` |
| The verifier says "no image-length field matching the file size" | the file is not an HSS payload, or has been truncated or padded |

## Trade-offs and going further

- **This is a model, not the chip.** It shows that the format and the algorithm are understood. It says nothing about how the boot monitor inside a given chip treats a signed payload.
- **Exercise:** add a second signer to the toy chain, so that stage 2 may be signed by either of two keys. What does the ROM now have to trust?
- **Exercise:** let the ROM's minimum version be stored in the image's own header. Which of the six scenarios now pass that should not?
- **Exercise:** change the signature algorithm to Ed25519 in `chain_demo.py`. What changes in the sizes?

<!-- nav:begin (generated by tools/gen_docs.py from the front matter; do not edit) -->
---

| | |
|---|---|
| **Read first** | [S02 Secure boot on PolarFire SoC: the chain of trust, and what we left alone](S02-secure-boot.md) |
| **How to run** | Simulation only: Icarus Verilog and Python, no board |
| **In the system** | [Hart Software Services](../system/README.md#hss) |
| **Source** | [`experiments/secure-boot-chain/chain_demo.py`](../../experiments/secure-boot-chain/chain_demo.py): `check_image`, `boot`, `sign_image`<br>[`experiments/secure-boot-chain/verify_boot_image.py`](../../experiments/secure-boot-chain/verify_boot_image.py): `verify`<br>[`experiments/secure-boot-chain/sign_and_verify.sh`](../../experiments/secure-boot-chain/sign_and_verify.sh) |
| **Evidence** | [Toy chain of trust, six scenarios](../../evidence/host/secure-boot/chain-demo.txt) (host, 2026-11-15)<br>[Signing an HSS payload offline and verifying it with two independent implementations](../../evidence/host/secure-boot/sign-and-verify.txt) (host, 2026-11-15) |
| **Related** | [S02 Secure boot on PolarFire SoC: the chain of trust, and what we left alone](S02-secure-boot.md)<br>[S01 FIC4 and the user crypto processor](S01-fic4-crypto-processor.md) |
| **Status** | validated |
<!-- nav:end -->

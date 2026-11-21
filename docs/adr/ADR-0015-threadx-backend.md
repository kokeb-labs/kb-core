# ADR-0015: Eclipse ThreadX is a supported kb::rtos backend

Status: Accepted (2026-11-14). Amends `docs/design/plan.md` M6 (the ThreadX backend lands before `port-stm32`).

## Context

kb-core's plan has carried ThreadX as a "later" backend since revision 1, and `KB_RTOS=THREADX` has been refused
with a "not yet" message. Before writing the backend we checked whether ThreadX is still worth supporting in late
2026, against FreeRTOS (and its certified sibling SAFERTOS) and Zephyr. The evidence, as found on 2026-11-14 and
checked again in review (rows marked *not re-verified* could not be loaded the second time):

| Question | Finding | Source |
| --- | --- | --- |
| Ownership and licence | Eclipse Foundation project since the hand-over from Microsoft (announced November 2023); MIT, no royalties | [threadx.io FAQ](https://threadx.io/faq/), [GitHub repository](https://github.com/eclipse-threadx/threadx) (licence field `MIT`) |
| Release cadence | Seven tagged releases in the last 13 months (publish dates): `v6.4.3.202503_rel` (2025-10-02), `v6.4.5.202504_rel` (2026-01-09), `v6.5.0.202601_rel` (2026-03-06), `v6.5.1.202602_rel` and `...a` (2026-06-08), `v6.5.2.202603_rel` (2026-10-02, the fixes of nine advisories), `v6.5.2.202603b_rel` (2026-11-01, latest: eight security fixes, seven in ThreadX Modules and one in the TrustZone-M entry functions) | [releases](https://github.com/eclipse-threadx/threadx/releases), [security advisories](https://github.com/eclipse-threadx/threadx/security/advisories) |
| Community | About 3.6k stars, 944 forks, 50 contributors (57 counting anonymous ones), last push 2026-11-09; a small core team that asks members to fund more staff | [repository](https://github.com/eclipse-threadx/threadx), [project lead, threadx-dev, 2026-01-09](https://www.eclipse.org/lists/threadx-dev/msg00080.html) |
| Safety certificates | SGS-TUV Saar certified specific releases to IEC 61508-3:2010 (route 3S, SIL 4), IEC 62304:2015 (class C), ISO 26262-8:2018 (ASIL D) and EN 50128:2011. The certified kernel is **ThreadX 6.1.1** (SMP 6.1.3); the certificates were being transferred from Microsoft to the Eclipse Foundation, and 6.4.x was not certified | [threadx.io FAQ](https://threadx.io/faq/), [threadx-dev, 2024-04-04](https://www.eclipse.org/lists/threadx-dev/msg00019.html) |
| Access to them | Subscribers of the **ThreadX Alliance** can buy a licence to the safety artefacts package (safety manuals and the documents of each certified release); the code stays MIT. The sources do not say who holds the certificates now; in 2024 their transfer from Microsoft to the Eclipse Foundation was still pending | [threadx.io FAQ](https://threadx.io/faq/), [threadx-dev, 2024-04-04](https://www.eclipse.org/lists/threadx-dev/msg00019.html) |
| Certification road map | 2026 goal: certify **v6.4.1** (IEC 61508, ISO 26262, ...) and finish the SESIP assessment Microsoft started; in April 2026 the project lead said 6.4.1 is "still on track" for this year, with no date | [threadx-dev, 2026-01-09](https://www.eclipse.org/lists/threadx-dev/msg00080.html), [threadx, 2026-04-15](https://www.eclipse.org/lists/threadx/msg00150.html) |
| STM32 | ST ships ThreadX in the STM32Cube firmware packages and the X-CUBE-AZRTOS expansion packages (security bulletin SB0067, April 2026; *not re-verified*, the PDF did not load in review), and in the STM32CubeMX middleware for newer families (H5, U5); a community answer says STM32CubeMX2 does not offer it yet | [ST SB0067](https://www.st.com/resource/en/security_bulletin/sb0067-applicability-of-threadx-middleware-vulnerabilities-on-stm32cube-packages-stmicroelectronics.pdf), [ST community, H5](https://community.st.com/stm32cubemx-mcus-29/stm32cubemx-6-18-0-missing-x-cube-azrtos-h5-166580), [ST community, CubeMX2](https://community.st.com/stm32cubemx2-mcus-151/threadx-support-for-stm32cubemx2-167329) |
| NXP, Renesas | NXP's MCUXpresso IDE lists ThreadX beside FreeRTOS and Zephyr (the NXP community article on the SDK is *not re-verified*: it refused automated access); Renesas ships ThreadX in the RA FSP and the RX FSP | [NXP MCUXpresso IDE](https://nxp.com/design/:MCUXpresso-IDE), [NXP community](https://community.nxp.com/t5/MCUXpresso-SDK-Knowledge-Base/Using-Azure-RTOS-ThreadX-with-MCUXpresso-SDK/ta-p/1184585), [Renesas FSP](https://www.renesas.com/en/software-tool/flexible-software-package-fsp), [Renesas RX FSP](https://www.renesas.com/en/software-tool/rx-flexible-software-package-fsp) |
| FreeRTOS and SAFERTOS | No certificate for the FreeRTOS kernel itself was found (*not sourced*: the FreeRTOS page gave no content in review); SAFERTOS (a separate, commercial code base on the FreeRTOS functional model) is pre-certified by TUV SUD to IEC 61508 SIL 3 and ISO 26262 ASIL D | [FreeRTOS: SAFERTOS](https://FreeRTOS.org/FreeRTOS-Plus/Safety_Critical_Certified/SafeRTOS.html), [WHIS](https://www.highintegritysystems.com/?p=296) |
| Zephyr | Working toward IEC 61508 SIL 3 (route 3S); no certificate yet in 2026; ISO 26262 to follow | [Zephyr safety overview](https://docs.zephyrproject.org/latest/safety/safety_overview.html), [Embedded World 2026 talk](https://www.inovex.de/en/?p=66655) |

What this means for kb-core:

- ThreadX is the only RTOS in the field whose **source** is MIT and whose specific releases hold IEC 61508 SIL 4,
  IEC 62304 class C and ISO 26262 ASIL D certificates. SAFERTOS is certified but commercial and closed; Zephyr is
  not certified yet and is a larger, Devicetree-driven system that does not fit a thin seam.
- It is shipped by the vendors of the MCU families kb-core targets next (STM32 first, then NXP and Renesas), so a
  board on those families can use the kernel the vendor tooling already configures.
- The risks are real: a small team, the certified release (6.1.1) is old and the artefacts cost money, the
  6.4.1 certificate is promised but not delivered, and ST's newest tooling (CubeMX2) leaves ThreadX out.
- The cost to kb-core is small: one backend folder of boundary files behind the existing seam
  (`kb/rtos/backend.hpp`), the same conformance tests, and one more SOUP entry.

## Decision

1. **Support Eclipse ThreadX** as a `kb::rtos` backend, `KB_RTOS=THREADX`, beside `NONE`, `FREERTOS` and the
   host test backend. It implements the same seam with the same contract: static allocation only (`tx_*_create`
   on control blocks and stacks that kb objects own), at-least delays, bounded waits, priority-inheriting
   mutexes, the same ISR rules, and it fails closed when not configured.
2. ThreadX is **SOUP**: a git submodule of the official `eclipse-threadx/threadx` repository at a release tag
   (`third_party/threadx`, recorded in `docs/assurance/soup.md` and the SBOM inputs). No ThreadX source is copied
   into the tree, edited, or MISRA-scanned; its call sites are boundary files.
3. kb-core's own tests run the ThreadX backend on the kernel itself: the conformance suite on ThreadX's Linux
   port (host), and cross-compiles of the backend for Cortex-M0+, M4F and M7F.
4. kb-core pins the **latest release** for development and CI. A product that claims the ThreadX certificate
   must build against a certified release and the Alliance's artefacts; kb-core uses only the long-stable 6.x
   API (no 6.5-only call), so the backend builds against 6.1.x too: on 2026-11-14 the backend and the kernel at
   `v6.1.1_rel` were cross-compiled for Cortex-M0+ and M4F (`THREADX_DIR=... PORTS=... tools/preflight/threadx_cross.sh`);
   CI builds only the pinned release. That pin belongs to the product's lock, not to kb-core.
5. This decision is reviewed when the 6.4.1 certificate is issued or when the project's release cadence stops
   (no release in 12 months).

## Consequences

- The boundary file keeps some bookkeeping that needs the kernel's own types and is a deliberate exception to the
  "no logic in boundary files" rule of the design (section 20.3): the critical-section nesting count, the timer
  slot table (a ThreadX expiry gets one `ULONG`, too narrow for a pointer on a 64-bit host) and the stack
  fill-pattern scan. The rules that need no kernel type (delay-until, the non-recursive retake, the 64-bit tick
  extension) are platform-free in `kb/rtos/backend_rules.hpp` and tested on the host. All of it is under the
  mutation and coverage gates through the fake kernel.
- The configurator, `kb resolve` and CMake stop refusing ThreadX; STM32 is still a planned family until
  `port-stm32` lands, so the RTOS is available but not yet on a released family.
- One more SOUP item to watch for anomalies (the ThreadX security advisories on GitHub).
- The seam gains what the design asked for and the ThreadX backend needs anyway (a recursive mutex, a thread's
  stack free space); the other backends implement it too, so the API stays the same on every RTOS.

## Alternatives considered

| Option | Why not |
| --- | --- |
| Drop ThreadX, FreeRTOS only | Gives up the only MIT-sourced kernel with SIL 4 / ASIL D / class C certificates, and the kernel ST, NXP and Renesas tooling configures |
| SAFERTOS backend instead | Commercial and closed; cannot be a pinned, buildable SOUP in CI. Its API follows FreeRTOS, so the FreeRTOS backend is the starting point if a product needs it |
| Zephyr backend | No certificate yet; its kernel objects are configured through Kconfig and Devicetree, a different integration model; revisit when certified |
| CMSIS-RTOS2 as the seam | Another layer between kb-core and the kernel, with its own allocation rules; the kb seam already is that layer |
| Copy the ThreadX sources into the tree | Breaks the SOUP rule (never edited, pinned, traceable to the official repository) |

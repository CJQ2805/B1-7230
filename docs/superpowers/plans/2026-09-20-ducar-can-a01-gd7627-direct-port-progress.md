# SDD ledger — plan: E:\DUCAR\DUCAR CODE\new platform\DUCAR_N32G45x_Project\docs\superpowers\plans\2026-09-20-ducar-can-a01-gd7627-direct-port.md

Execution root: `E:\DUCAR\DUCAR CODE\new platform\DUCAR_N32G45x_Project`

Ruling: the default E-drive project is not a Git repository, so an isolated Git worktree, commits, `task-start`, and `task-done` scripts cannot be used. Implementation will remain in the user-designated E-drive root; this costs automatic per-task rollback, mitigated by narrow `apply_patch` edits and build evidence recorded here.

Ruling: no host C compiler/runtime is installed. Each interface will first be introduced through a Keil-compilable target contract, and behavior will be verified with the existing hardware CAN path after build; this costs automated host-level edge-case execution, mitigated by explicit frame-level CAN acceptance cases.

Pre-flight: Task 1 produces `app_can_init`, `CAN_ReceiveTask`, `CAN_TransmitTask`, `d_can_is_tx_idle`, and Protocol/IAP entry contracts; Task 2 consumes the Protocol contract. Compatible — `t_can_packet` remains the sole raw frame type.

Pre-flight: Task 2 produces packet encoders and protocol state; Task 3 consumes its five periodic functions and data-renew entry. Compatible — a single internal 5ms counter remains owned by `ducar_can_protocol.c`.

Pre-flight: Task 4 consumes Task 1 frame classification and Task 2 ID helpers; Task 5 consumes Task 1 driver idle state and IAP classification. Compatible — normal PF processing and E1/DF00 IAP processing remain disjoint.

Pre-flight: Task 5 produces `0xBBBBBBBB` APP-to-BOOT handoff; Task 6 consumes the existing BOOT wait-package state. Compatible — BOOT remains the only owner of APP2 receive, CRC, and APP2-to-APP1 move.

Pre-flight: Task 7 consumes all new CAN files and Keil group changes. Compatible — stale `app_can_xingheng.h` is only removed after an exact source/project reference scan.

Implementation carried forward: APP/BOOT CAN code and project entries from Tasks 1–6 are present; hardware acceptance steps remain open.

Ruling: the A0.1 workbook is the authority for F217 half-charge count; its default is `0x0000`, so the plan/spec were corrected and the existing encoder's `0x0000` retained. Cost if wrong: F217's final word would disagree with the protocol definition.

Final-review finding fixed: APP CAN IAP could remain forever in `REPLY_SEND` if `d_can_send()` never accepted the ACK. Added a bounded 100ms queue retry; E1/02 then resets into the already-marked BOOT wait state, while non-reset replies are released. The source-contract test failed before the change and passed after it.

Final-review finding fixed: normal CAN control ACK could remain pending forever if the mailbox never accepted it. Added a bounded 100ms queue retry; timeout drops the unsent ACK and its associated action, so reset/power-off cannot occur without an accepted ACK. The source-contract test failed before the change and passed after it.

Hungarian-name cleanup: renamed BOOT CAN local temporaries (`data_pages`, `frame`, `address`, `page_offset`, `length`, `offset`, `remain`, `copy_length`, `now`, `tx_idle`, `fields`) to type- and purpose-bearing names. Static audit confirms none of those generic local names remain.

Legacy cleanup: removed `app_can_xingheng.h` after an APP/BOOT/Keil/test-tree reference scan found no consumer. No project entry required removal.

Verification 2026-09-21 15:05 CST: APP clean rebuild via Keil `-cr`, Code=74012, RO-data=2676, RW-data=1136, ZI-data=32064, 0 errors/0 warnings; HEX=216833 bytes, BIN=77072 bytes. BOOT clean rebuild, Code=23482, RO-data=742, RW-data=4, ZI-data=9764, 0 errors/0 warnings; HEX=68210 bytes, BIN=24232 bytes. Both final logs show the relevant CAN sources compiled.

Verification: `can_iap_send_timeout_contract_test.ps1` passes both bounded-ACK source contracts; BOOT generic-local-name audit passes; forbidden `_can_stage`/extra transmit wrapper/legacy-header scan passes. These are source-contract checks, not target runtime simulations.

Final review: self-review (no subagent tool). Compared the G45x entry/scheduling structure against GD7627's receive/send flow and explicit 5ms counter with offset `if / else if` slots. The architecture preserves those shapes; no numeric 95% source-similarity score was computed.

Open acceptance: physical CAN analyzer/hardware verification remains required for DETD 5s window, ordinary request/TP traffic, E1 APP→BOOT handoff, first-page metadata validation, and complete image update. Overall protocol acceptance is not marked complete until that is run.

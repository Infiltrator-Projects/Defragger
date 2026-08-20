<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Safety audit status

Status: **write operations quarantined**  
Effective date: 2026-08-21  
Applies to: unreleased development version 1.8.0-138

Analyse, Map, identification and other read-only operations remain available. Defragment, Growth Defrag and Recover are disabled by default in the GUI operation planner, the operation-engine boundary and every native write-capable worker.

The quarantine exists because the repository's previous release process did not provide reliable exact-head validation or independent review for raw filesystem changes. It is a containment control, not a statement that every current engine is defective.

## Disposable-media development override

The override is intended only for developers testing disposable filesystem images or sacrificial media whose complete loss is acceptable:

```bash
export LINUX_DEFRAGGER_ENABLE_UNAUDITED_WRITES=I_ACCEPT_UNAUDITED_RAW_WRITES
```

Setting the variable acknowledges only that the target is disposable. It does not certify an engine or make important media safe. Remove the variable to restore the quarantine.

## Release status

The release workflow is statically disabled while this audit is open. Version 1.8.0-138 must not be tagged or published by bypassing that workflow.

## Exit criteria

The quarantine and release freeze may be removed only by a reviewed change that records:

1. independent review of each write-capable filesystem engine;
2. exact-head CI success for the audit-exit commit;
3. disposable-image mutation, remount/reopen and payload verification;
4. interrupted-operation and recovery verification;
5. protected-main settings requiring the permanent quality gate; and
6. an explicit release decision for version 1.8.0-138.

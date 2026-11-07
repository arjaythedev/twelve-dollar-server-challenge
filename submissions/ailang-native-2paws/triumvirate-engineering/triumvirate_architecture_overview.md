# The Engineering Triumvirate Pattern

## 1. Executive Summary & Philosophy

Modern autonomous agent workflows often fail due to conflated responsibilities. When a single agent is tasked with implementing a feature, optimizing performance, and keeping code minimal, it invariably compromises:
- It rationalizes its own bloat.
- It writes benchmarks that validate its specific implementation rather than the domain requirements.
- It treats its initial assumptions as immutable constraints.

The **Triumvirate Pattern** introduces institutional tension by dividing software engineering into three adversarial yet mutually reinforcing roles:
1. **The Implementer (`feature-implementer`)**: Translates explicit specifications into working, compilable software. Focused entirely on correctness and contract fulfillment.
2. **The Verifier (`performance-compliance`)**: An unyielding gatekeeper enforcing resource budgets, hardware limits, throughput/latency metrics, and deterministic compliance.
3. **The Reducer (`code-reducer`)**: A ruthless editor that strips dependencies, reduces branches, collapses abstractions, and challenges the premise of every line written.

```
       [ Specification / Requirements ]
                       │
                       ▼
       ┌───────────────────────────────┐
       │   Skill 1: Implementer        │ ◄─────────────────────────┐
       │   (Correctness & Contract)    │                           │
       └───────────────┬───────────────┘                           │
                       │ Working Code & Functional Tests           │ Reject (Regression /
                       ▼                                           │ Broken Contract)
       ┌───────────────────────────────┐                           │
       │   Skill 2: Verifier           │                           │
       │   (Budgets, SLAs, Invariants) │ ── Reject (Budget Fail) ──┤
       └───────────────┬───────────────┘                           │
                       │ Pass (Meets SLAs)                         │
                       ▼                                           │
       ┌───────────────────────────────┐                           │
       │   Skill 3: Reducer            │                           │
       │   (Prune, Flatten, Decouple)  │                           │
       └───────────────┬───────────────┘                           │
                       │ Simplified Candidate Diff                 │
                       └───────────────────────────────────────────┘
```

---

## 2. The Core Mechanics

### Why Separate the Implementer from the Reducer?
Programmers (and LLMs) suffer from the *sunk cost heuristic*. Once an agent builds an elaborate object hierarchy or pulls in a third-party utility crate to solve an edge case, it will defend that decision. The Reducer has no emotional attachment to the implementation: its single metric of success is complexity reduction without functional regression.

### Why Separate the Verifier from the Implementer?
Implementers optimize for passing tests. Verifiers optimize for exposing limits. The Verifier acts as an automated QA and site reliability engineer, running soak tests, memory profilers, fuzzers, and tail-latency analysis.

### Convergence Loop
1. **Pass 1**: Implementer delivers `v1.0-raw`.
2. **Gate 1**: Verifier establishes baseline metrics and confirms invariant compliance. If failed, sent back to Implementer with failure traces.
3. **Pass 2**: Reducer takes `v1.0-raw` + Verifier baselines, strips superfluous layers, and emits `v1.1-pruned`.
4. **Gate 2**: Verifier validates `v1.1-pruned`.
   - If performance improves or holds parity while complexity dropped $\rightarrow$ **Accepted**.
   - If performance regressed beyond threshold $\rightarrow$ **Rejected with delta diagnostics**.
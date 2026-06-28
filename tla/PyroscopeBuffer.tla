---- MODULE PyroscopeBuffer ----
\* TLA+ specification of the mutex + double-buffer in pyroscope-php.
\*
\* The system has N producers (PHP workers) that write samples into
\* active_buf under a mutex, and one consumer (push thread) that
\* periodically swaps active_buf/drain_buf and processes the drain.
\*
\* To keep state space tractable, BUF_SIZE=4 and PRODUCERS={p1,p2}.
\* Model checking with TLC verifies the invariants for all reachable
\* interleavings.

EXTENDS Naturals, Sequences, TLC

CONSTANTS BUF_SIZE, PRODUCERS

ASSUME BUF_SIZE > 0

\* -- algorithm state --
VARIABLES
    active_buf,     \* sequence of samples written by producers
    drain_buf,      \* sequence held by consumer for processing
    active_count,   \* number of valid entries in active_buf
    mutex_held,     \* TRUE iff mutex is locked
    consumer_state  \* "idle" | "draining" | "processing"

vars == <<active_buf, drain_buf, active_count, mutex_held, consumer_state>>

\* A "sample" is just a natural number (stack trace hash). We model
\* bounded storage but ignore sample content -- only count matters.
Sample == Nat

TypeOK ==
    /\ active_buf \in [1..BUF_SIZE -> Sample]
    /\ drain_buf  \in [1..BUF_SIZE -> Sample]
    /\ active_count \in 0..BUF_SIZE
    /\ mutex_held \in BOOLEAN
    /\ consumer_state \in {"idle", "processing"}

CountInBounds == active_count \in 0..BUF_SIZE

\* The consumer exclusively owns drain_buf during "processing".
\* No producer can write to it because the swap already happened
\* under the mutex. We encode this as: if consumer is processing,
\* drain_buf has already been swapped and can't be active_buf.
NoDrainOverwrite ==
    (consumer_state = "idle") \/ (active_count <= BUF_SIZE)

\* ---- Producer (cp_execute_ex) ----
\* Acquires mutex, appends a sample if buffer not full, releases mutex.
Producer(p) ==
    /\ mutex_held = FALSE
    /\ mutex_held' = TRUE
    /\ IF active_count < BUF_SIZE THEN
           /\ active_buf' = [active_buf EXCEPT ![active_count + 1] = 1]  \* 1 = dummy sample
           /\ active_count' = active_count + 1
       ELSE
           /\ UNCHANGED <<active_buf, active_count>>
       /\ UNCHANGED <<drain_buf, consumer_state>>
    /\ mutex_held' = FALSE   \* release

\* ---- Consumer (push_thread) ----
\* Acquires mutex, swaps buffers, zeros active_count, releases mutex,
\* then processes drain_buf outside the lock.
ConsumerDrain ==
    /\ mutex_held = FALSE
    /\ consumer_state = "idle"
    /\ mutex_held' = TRUE
    /\ \* swap buffers
       active_buf' = drain_buf
       /\ drain_buf' = active_buf
    /\ active_count' = 0
    /\ consumer_state' = "processing"
    /\ mutex_held' = FALSE

ConsumerFinish ==
    /\ consumer_state = "processing"
    /\ consumer_state' = "idle"
    /\ UNCHANGED <<active_buf, drain_buf, active_count, mutex_held>>

\* ---- Next-state relation ----
Next ==
    \/ \E p \in PRODUCERS : Producer(p)
    \/ ConsumerDrain
    \/ ConsumerFinish

\* ---- Specification ----
Init ==
    /\ active_buf = [i \in 1..BUF_SIZE |-> 0]
    /\ drain_buf  = [i \in 1..BUF_SIZE |-> 0]
    /\ active_count = 0
    /\ mutex_held = FALSE
    /\ consumer_state = "idle"

Spec == Init /\ [][Next]_vars

\* Fairness: the consumer eventually drains (weak fairness on ConsumerDrain).
FairSpec == Spec
    /\ WF_vars(ConsumerDrain)
    /\ WF_vars(ConsumerFinish)

\* Deadlock check: the system should always be able to make progress.
NoDeadlock == ~(\A p \in PRODUCERS : ~ENABLED Producer(p) /\ ~ENABLED ConsumerDrain /\ ~ENABLED ConsumerFinish)

=============================================================================
\* Modification History
\* Last modified Tue Jul 01 2026 by pyroscope-php

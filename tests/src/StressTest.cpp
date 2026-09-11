// @@@LICENSE
//
//      Copyright (c) 2009-2013 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// LICENSE@@@

/**
 * ****************************************************************************
 * @file StressTest.cpp
 *
 * @brief  Deterministic stress / invariant-checking harness for
 *         PmStateMachineEngine.
 *
 * Drives a 25-state hierarchy (maximum supported nesting depth) with a
 * pseudo-random event storm.  State handlers randomly handle events,
 * propagate them to ancestors, or request transitions to arbitrary
 * states (including self-transitions, ancestor/descendant local
 * transitions, and BEGIN-scope initial transitions to proper
 * descendants).  After every dispatch the harness verifies the
 * engine's core invariants:
 *
 *   * ENTER/EXIT strict pairing: EXIT is only ever delivered to the
 *     deepest active state; ENTER only to a child of the deepest
 *     active state (or a top-level state when none is active).
 *   * The set of active states always equals the parent chain of the
 *     engine's reported current state (FsmDbgPeekCurrentState /
 *     FsmDbgPeekParentState).
 *   * BEGIN is delivered exactly once, to the target of the
 *     just-completed transition, while that target is the deepest
 *     active state.
 *   * After a dispatch that requested transition(s), the current
 *     state is the last requested target; after a dispatch with no
 *     transition, the current state is unchanged.
 *
 * The RNG is a fixed-seed LCG, so failures are reproducible.
 *
 * Usage: PmStateMachineEngineStress [iterations] [seed]
 * ****************************************************************************
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include <PmStateMachineEngine/PalmFsm.h>
#include <PmStateMachineEngine/PalmFsmDbg.h>


enum { kNumStates = 25 };

/// Parent of each state (-1 = top-level).  States 0..9 form a chain of
/// the maximum supported nesting depth (kFsmMaxStateNestingDepth); the
/// rest branch off at assorted depths.
static const int kParentOf[kNumStates] = {
    /* 0..9: full-depth chain  */ -1, 0, 1, 2, 3, 4, 5, 6, 7, 8,
    /* 10..14: branch under 2  */  2, 10, 11, 10, 2,
    /* 15..18: branch under 5  */  5, 15, 15, 16,
    /* 19..21: second top tree */ -1, 19, 19,
    /* 22..24: branch under 20 */ 20, 22, 20
};

static FsmMachine   g_fsm;
static FsmState     g_states[kNumStates];
static char         g_stateNames[kNumStates][8];

/// ---- deterministic PRNG -------------------------------------------------
static unsigned int g_seed = 0xC0FFEE42u;

static unsigned int
NextRand(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}

/// ---- shadow bookkeeping -------------------------------------------------
static int  g_activeStack[kFsmMaxStateNestingDepth + 1];
static int  g_activeDepth = 0;

static int  g_lastTranTarget = -1;      ///< last requested transition target
static int  g_tranRequested = 0;        ///< a transition was requested this dispatch
static int  g_beginDeliveries = 0;      ///< BEGIN events seen this dispatch

static unsigned long g_statDispatches = 0;
static unsigned long g_statTransitions = 0;
static unsigned long g_statEnters = 0;
static unsigned long g_statExits = 0;
static unsigned long g_statLogLines = 0;
static unsigned long g_statRestarts = 0;

static unsigned long g_iteration = 0;

static void
Fail(const char* fmt, ...)
{
    va_list args;
    fprintf(stderr, "\nSTRESS FAILURE (iteration %lu, seed state 0x%08X): ",
            g_iteration, g_seed);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    abort();
}

static int
StateIndex(const FsmState* pState)
{
    int idx = (int)(pState - g_states);
    if (idx < 0 || idx >= kNumStates) {
        Fail("handler got state pointer outside g_states");
    }
    return idx;
}

static int
IsActive(int idx)
{
    int i;
    for (i = 0; i < g_activeDepth; ++i) {
        if (g_activeStack[i] == idx) {
            return 1;
        }
    }
    return 0;
}

/// Pick a proper descendant of `idx`, or -1 if it has none
static int
RandomProperDescendant(int idx)
{
    int candidates[kNumStates];
    int numCandidates = 0;
    int i;

    for (i = 0; i < kNumStates; ++i) {
        int walk = kParentOf[i];
        while (walk != -1) {
            if (walk == idx) {
                candidates[numCandidates++] = i;
                break;
            }
            walk = kParentOf[walk];
        }
    }
    return numCandidates ? candidates[NextRand() % numCandidates] : -1;
}

/// ---- log callback (exercises the callback logging path) -----------------
static void
StressLogCb(FsmMachine* pFsm, void* cookie, enum FsmDbgLogLevel level,
            const char* pFmt, ...)
{
    /// Format into a real buffer so the engine's format strings and
    /// arguments are actually consumed/validated.
    char buf[256];
    va_list args;

    (void)pFsm;
    (void)cookie;
    (void)level;

    va_start(args, pFmt);
    vsnprintf(buf, sizeof(buf), pFmt, args);
    va_end(args);

    ++g_statLogLines;
}

/// ---- the (shared) state handler ------------------------------------------
enum { kSigStress = kFsmEventFirstUserEvent, kSigNeverHandled };

static int
StressStateHandler(FsmState* pState, FsmMachine* pFsm, const FsmEvent* pEvt)
{
    const int idx = StateIndex(pState);

    switch (pEvt->evtId) {
    case kFsmEventEnterScope: {
        /// ENTER must go to a child of the deepest active state
        /// (or a top-level state if nothing is active)
        int expectedParent = g_activeDepth
                             ? g_activeStack[g_activeDepth - 1] : -1;
        if (IsActive(idx)) {
            Fail("ENTER for already-active state %d", idx);
        }
        if (kParentOf[idx] != expectedParent) {
            Fail("ENTER for state %d (parent %d) but deepest active is %d",
                 idx, kParentOf[idx], expectedParent);
        }
        g_activeStack[g_activeDepth++] = idx;
        ++g_statEnters;
        return 1;
    }

    case kFsmEventExitScope: {
        /// EXIT must only ever target the deepest active state
        if (!g_activeDepth || g_activeStack[g_activeDepth - 1] != idx) {
            Fail("EXIT for state %d which is not deepest active", idx);
        }
        --g_activeDepth;
        ++g_statExits;
        return 1;
    }

    case kFsmEventBegin: {
        /// BEGIN goes to the transition target, exactly once, while
        /// that target is the deepest active state
        ++g_beginDeliveries;
        if (!g_activeDepth || g_activeStack[g_activeDepth - 1] != idx) {
            Fail("BEGIN for state %d which is not deepest active", idx);
        }
        if (g_lastTranTarget != idx) {
            Fail("BEGIN for state %d but last transition target was %d",
                 idx, g_lastTranTarget);
        }
        /// Occasionally take an initial transition to a proper descendant
        if ((NextRand() % 100) < 25) {
            int dest = RandomProperDescendant(idx);
            if (dest != -1) {
                g_lastTranTarget = dest;
                g_tranRequested = 1;
                ++g_statTransitions;
                FsmBeginTransition(pFsm, &g_states[dest]);
                return 1;
            }
        }
        return 1;
    }

    case kSigStress: {
        unsigned int r = NextRand() % 100;

        /// The engine must only deliver user events to active states
        if (!IsActive(idx)) {
            Fail("user event delivered to inactive state %d", idx);
        }

        if (r < 30) {
            /// Request a transition to a random state (any target is
            /// legal from user-event scope, per the API contract)
            int dest = (int)(NextRand() % kNumStates);
            g_lastTranTarget = dest;
            g_tranRequested = 1;
            ++g_statTransitions;
            FsmBeginTransition(pFsm, &g_states[dest]);
            return 1;   ///< MUST return handled after FsmBeginTransition
        }
        else if (r < 60) {
            return 1;   ///< handled, no transition
        }
        return 0;       ///< propagate to parent
    }

    case kSigNeverHandled:
        return 0;       ///< always propagates all the way to the root

    default:
        Fail("unexpected event id %d in state %d", pEvt->evtId, idx);
    }

    return 0;
}

/// ---- harness -------------------------------------------------------------
static void
BuildMachine(void)
{
    int i;

    FsmInitMachine(&g_fsm, "Stress");
    FsmDbgEnableLogging(&g_fsm, kFsmDbgLogOptEvents, StressLogCb, NULL);
    /// Info threshold: exercises the per-delivery logging/snprintf code
    /// while suppressing (and thereby also exercising) debug-level calls
    FsmDbgSetLogLevelThreshold(&g_fsm, kFsmDbgLogLevelInfo);

    for (i = 0; i < kNumStates; ++i) {
        snprintf(g_stateNames[i], sizeof(g_stateNames[i]), "s%d", i);
        FsmInitState(&g_states[i], StressStateHandler, g_stateNames[i]);
    }
    for (i = 0; i < kNumStates; ++i) {
        FsmInsertState(&g_fsm, &g_states[i],
                       kParentOf[i] == -1 ? NULL : &g_states[kParentOf[i]]);
    }

    g_activeDepth = 0;
}

static void
VerifyActiveChain(void)
{
    /// The engine's current-state parent chain must exactly equal the
    /// shadow active stack
    const FsmState* pWalk = FsmDbgPeekCurrentState(&g_fsm);
    int depth = g_activeDepth;

    if (!pWalk) {
        Fail("current state is NULL after dispatch settled");
    }

    while (pWalk) {
        int idx = StateIndex(pWalk);
        if (depth <= 0) {
            Fail("engine chain deeper than shadow stack at state %d", idx);
        }
        --depth;
        if (g_activeStack[depth] != idx) {
            Fail("chain mismatch at depth %d: engine %d, shadow %d",
                 depth, idx, g_activeStack[depth]);
        }
        if (strcmp(FsmDbgPeekStateName(pWalk), g_stateNames[idx]) != 0) {
            Fail("state name mismatch for state %d", idx);
        }
        pWalk = FsmDbgPeekParentState(&g_fsm, pWalk);
    }
    if (depth != 0) {
        Fail("shadow stack deeper than engine chain (%d left)", depth);
    }
}

static void
StartMachine(int initialIdx)
{
    g_lastTranTarget = initialIdx;
    g_tranRequested = 1;
    g_beginDeliveries = 0;
    FsmStart(&g_fsm, &g_states[initialIdx]);
    if (!g_beginDeliveries) {
        Fail("no BEGIN delivered during FsmStart");
    }
    VerifyActiveChain();
}

int
main(int argc, char* argv[])
{
    unsigned long iterations = 200000;
    unsigned long i;

    if (argc > 1) {
        iterations = strtoul(argv[1], NULL, 0);
    }
    if (argc > 2) {
        g_seed = (unsigned int)strtoul(argv[2], NULL, 0);
    }

    printf("PmStateMachineEngine stress: %lu iterations, seed 0x%08X\n",
           iterations, g_seed);

    BuildMachine();
    StartMachine(0);

    for (i = 1; i <= iterations; ++i) {
        FsmEvent evt;
        const FsmState* pBefore = FsmDbgPeekCurrentState(&g_fsm);
        int handled;

        g_iteration = i;

        /// Periodically tear the whole machine down and rebuild it,
        /// stressing init/insert/start as well
        if ((i % 10007) == 0) {
            BuildMachine();
            StartMachine((int)(NextRand() % kNumStates));
            ++g_statRestarts;
            continue;
        }

        evt.evtId = ((NextRand() % 8) == 0) ? kSigNeverHandled : kSigStress;

        g_tranRequested = 0;
        g_beginDeliveries = 0;

        handled = FsmDispatchEvent(&g_fsm, &evt);
        ++g_statDispatches;

        VerifyActiveChain();

        if (g_tranRequested) {
            /// Settled current state must be the last requested target
            int curIdx = StateIndex(FsmDbgPeekCurrentState(&g_fsm));
            if (curIdx != g_lastTranTarget) {
                Fail("settled on state %d, expected transition target %d",
                     curIdx, g_lastTranTarget);
            }
            if (!handled) {
                Fail("transition requested but dispatch reported unhandled");
            }
        }
        else {
            /// No transition: current state must be unchanged and no
            /// BEGIN may have been delivered
            if (FsmDbgPeekCurrentState(&g_fsm) != pBefore) {
                Fail("current state changed without a transition request");
            }
            if (g_beginDeliveries) {
                Fail("BEGIN delivered without a transition request");
            }
        }

        if (evt.evtId == kSigNeverHandled && handled) {
            Fail("kSigNeverHandled was reported as handled");
        }
    }

    printf("PASS: %lu dispatches, %lu transitions, %lu enters, %lu exits,\n"
           "      %lu restarts, %lu log lines; final state: %s\n",
           g_statDispatches, g_statTransitions, g_statEnters, g_statExits,
           g_statRestarts, g_statLogLines,
           FsmDbgPeekStateName(FsmDbgPeekCurrentState(&g_fsm)));
    return 0;
}

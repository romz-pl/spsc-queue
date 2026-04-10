# Here's a breakdown of the 11 test groups and the invariants each one targets:


## Structural / compile-time

+ `StaticCapacityRequirement` — documents that `N < 2` is a hard compile error.

## Empty-queue invariants

+ `PopOnEmptyReturnsFalse` — `pop` must return `false` and must not touch the output value.
+ `InitialStateIsEmpty` — freshly constructed queue reports empty immediately.

## Basic correctness

+ `SinglePushPopRoundTrip` — one item in, same item out.
+ `FIFOOrdering` — five items come out in exactly the order they were inserted.

## Capacity / full-queue

+ `CapacityIsNMinusOne` — usable `slots = N − 1` (one slot is sacrificed to distinguish full from empty).
+ `PushOnFullReturnsFalse` — smallest legal queue (`N=2`, capacity 1) hits this immediately.
+ `PushFailsWhenFullThenSucceedsAfterPop` — after draining one slot, the next push succeeds.

## Lifecycle & wrap-around

+ `DrainAndRefill` — queue can be emptied and reused correctly.
+ `IndexWrapAround` — three full fill-drain cycles with N=4 exercises every modulo path.

## Type generality

+ `WorksWithStrings` / `WorksWithStructs` — confirms the template works beyond `int`.

## Blocking wrappers (single-threaded, precondition already met)

+ `PopBlockingReturnsImmediatelyWhenItemPresent`
+ `PushBlockingSucceedsImmediatelyWhenNotFull`

## Edge-case sequences

+ `InterleavedPushPop` — alternating pushes and pops in varied order.
+ `LargeCapacityFillAndDrain` — `N = 1025` to ensure no off-by-one at scale.
+ `PopDoesNotClobberValueOnFailure` — sentinel value is untouched after a failed pop.



# Unsigned integer reciprocal-multiply experiment — unapplied

Prototype lives in ignored build/experiments/integer-mul/. Production src/bin and
shared build objects are unchanged. Patch: integer-mul.patch. Default remains
--integer=div; --integer=mul selects the proposed alternative. No measured gain
or default change is claimed.

## Mechanism

append_uint currently performs an unsigned 64-bit DIV 10 for each decimal digit.
The alternative uses q=floor(n*M/2^67), M=0xCCCCCCCCCCCCCCCD, then r=n-q*10.
M=(2^67+2)/10. Thus n*M/2^67=n/10+n/(5*2^67); the added term is less than 1/40
for any unsigned 64-bit n, and n/10 has fractional part at most 0.9, proving q exact.
MUL yields the 128-bit product; shifting its high 64 bits by 3 obtains this quotient.
q*10 never exceeds n, so computing the remainder does not overflow.

The existing reverse-digit buffer and RDI end-pointer return are retained.
A previously unused 8-byte slot at rsp+32 keeps n while MUL overwrites RAX/RDX.
Digits occupy at most 20 bytes below that slot. The helper remains a leaf with its
original 40-byte frame, and R8/R9 are deliberately untouched. Both paths clobber
only the original volatile footprint RAX/RCX/RDX/RSI/RDI and flags. Both return
RAX=0 and RCX=0. RDX's exact return value differs, but no existing append_uint
caller carries a live RDX value across the call. Call sites in health, posts,
likes, header lengths and append_post were inspected; needed persistent values
are in memory or callee-saved registers. The signed-ID caller already emits a
minus sign and negates into unsigned magnitude, including INT64_MIN.

## Exact prototype

Binary SHA256: 9ed933a3628af438efff026befa8e3a72be7243a3bd96c46aa12d2fa3ac7ed7b.
Scratch server.asm: 9391a1d0102bf321a3beef4e2fd3758fe9d58dea800970b416c51c5d55a8ad89.
Scratch experiments.inc: 8a02a85be2873ce1161e1b910c53630aa56a9e7417974602d7ff6f2a26b51f60.
Other seven sources match baseline. Reused measured SQLite object 44e0a3b...,
without recompiling SQLite. NASM assembly and GCC -no-pie -flto link only.
Full identity: scratch identity.json.

## Passed checks

Exact extracted routine, both div and mul:
- 10,000 independently generated random 64-bit values per mode, compared to Python str.
- 64 boundary values covering zero, powers of 10 plus/minus 1, 2^63 neighbors, UINT64_MAX.
- Exact returned end pointer, prefix/suffix canaries, output start/end guard pages.
- Callee-saved registers, caller stack and DF through independent ABI wrapper.
- Explicit preservation of R8/R9, avoiding accidental new implicit caller breakage.

Existing integration passed both modes on both native io_uring and epoll with a
tiny canonical-schema fixture: auth, Unicode/FK rollback, duplicate likes,
fragmented/chunked/continue, framing and acknowledged-write SIGKILL recovery.
Signed-boundary HTTP feed differential also passed both modes: INT64_MIN,-1,0,1
and INT64_MAX, tied timestamps, correct ID ordering and exact independent SQLite
reference objects. Raw scratch signed-wire-results.json, integer-test-results.json
and four mode/backend integration logs.

## Remaining gate

No throughput or capacity run was performed. Full official 42 tests should be repeated on a combined candidate before adoption. The opt-in branch adds dispatch overhead; any gain must be measured
with the same binary and controlled whole-request workload. A faster micro
operation does not prove higher user capacity. Baseline historical measurements
remain tied to their original binary.

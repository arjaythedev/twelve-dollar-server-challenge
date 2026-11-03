import Spec

/-! Executable sanity check for `Db.checkInv` (not part of the `Spec` library). -/

def users : List User := [⟨1, "ana"⟩, ⟨2, "bo"⟩]
def p1 : Post := ⟨1, 1, "a", "2026-01-01T00:00:00.000Z"⟩
def p2 : Post := ⟨2, 2, "b", "2026-01-01T00:00:01.000Z"⟩
def p3 : Post := ⟨3, 1, "c", "2026-01-01T00:00:01.000Z"⟩

def good    : Db := ⟨[p1, p2, p3], users, [⟨1, 3⟩]⟩
def idGap   : Db := ⟨[p1, p2, { p3 with id := 4 }], users, []⟩
def timeDec : Db := ⟨[p1, p2, { p3 with createdAt := "2025-12-31T23:59:59.000Z" }], users, []⟩
def noAuth  : Db := ⟨[p1, p2, { p3 with userId := 9 }], users, []⟩

#eval (good.checkInv, idGap.checkInv, timeDec.checkInv, noAuth.checkInv)
#eval good.feed == good.feedPlan
#eval good.feed.map (·.id)
#eval (good.getPost 3 == good.postPlan 3, good.getPost 0 == good.postPlan 0, good.getPost 4 == good.postPlan 4)
#eval (good.insertOk ⟨4, 2, "d", "2026-01-01T00:00:01.000Z"⟩, good.insertOk ⟨4, 2, "d", "2025-01-01T00:00:00.000Z"⟩)

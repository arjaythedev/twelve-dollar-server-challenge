//go:build linux && (amd64 || arm64)

package netring

import "golang.org/x/sys/unix"

// 16 MiB per ring: the direct backend uses two rings, so its fixed send
// mapping is 32 MiB on the 2 GiB VM. The 4 KiB class has 2,048 slots per
// ring; the remaining 8 MiB covers larger replies. Overflow uses mmap.
const sendArenaBytes = 16 << 20

var sendArenaCounts = [5]int{
	sendArenaBytes / 2 / 4096,
	sendArenaBytes / 4 / 8192,
	sendArenaBytes / 8 / 16384,
	sendArenaBytes / 16 / 32768,
	sendArenaBytes / 16 / 65536,
}

type sendSlot struct {
	buf   []byte
	class int // -1 means a dedicated mmap
	index int
}

type arenaClass struct {
	start, size int
	free        []int
	inUse       []bool
}

type sendArena struct {
	mapping []byte
	classes [5]arenaClass
}

func newSendArena(counts [5]int) (*sendArena, error) {
	total := 0
	for i, count := range counts {
		if count < 0 {
			panic("negative send arena class count")
		}
		total += count * (4096 << i)
	}
	if total == 0 {
		panic("empty send arena")
	}
	mapping, err := anon(total)
	if err != nil {
		return nil, err
	}
	a := &sendArena{mapping: mapping}
	offset := 0
	for i, count := range counts {
		c := &a.classes[i]
		c.start, c.size = offset, 4096<<i
		c.free = make([]int, count)
		c.inUse = make([]bool, count)
		for j := range c.free {
			c.free[j] = count - j - 1
		}
		offset += count * c.size
	}
	return a, nil
}

func (a *sendArena) acquire(n int) (sendSlot, bool) {
	for i := range a.classes {
		c := &a.classes[i]
		if n > c.size {
			continue
		}
		if len(c.free) == 0 {
			return sendSlot{}, false
		}
		last := len(c.free) - 1
		index := c.free[last]
		c.free = c.free[:last]
		if c.inUse[index] {
			panic("send arena slot already owned")
		}
		c.inUse[index] = true
		start := c.start + index*c.size
		return sendSlot{buf: a.mapping[start : start+c.size], class: i, index: index}, true
	}
	return sendSlot{}, false
}

func (a *sendArena) release(s sendSlot) {
	if s.class < 0 || s.class >= len(a.classes) {
		panic("invalid send arena class")
	}
	c := &a.classes[s.class]
	if s.index < 0 || s.index >= len(c.inUse) || !c.inUse[s.index] {
		panic("send arena double return")
	}
	c.inUse[s.index] = false
	c.free = append(c.free, s.index)
}

func (a *sendArena) close() error {
	if a == nil || a.mapping == nil {
		return nil
	}
	err := unix.Munmap(a.mapping)
	a.mapping = nil
	return err
}

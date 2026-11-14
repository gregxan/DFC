"""Randomised model of the TX path in src/link.c (U-13 lazy completion
interrupt). It mirrors link_write(), link_tx_free(), tx_reap(), tx_start()
and tx_dma_isr() step for step, and interleaves DMA progress, UART draining
and the ISR between every step of link_write().

Checks, for every seed:
  - bytes leave in order, with none lost or duplicated;
  - the queue drains with no further API calls (no stall);
  - tx_start() is never entered with a completion still latched (the C code
    does not clear the latch there, so a stale one would be a bug);
  - the completion interrupt only fires when data is waiting behind the
    finished transfer (U-13: enabled exactly when needed).

    py tools/models/tx_model.py
"""
import random

SIZE = 64   # TX ring (bytes)
FIFO = 32   # UART TX FIFO depth


def run(seed):
    R = random.Random(seed)
    st = dict(head=0, tail=0, infl=0, busy=False, inte=False, raw=False, dma_rem=0, dma_pos=0)
    ring = [None] * SIZE
    fifo, wire, sent = [], [], []
    nxt = [0]

    def dma_step():                     # the DMA moves one byte into the FIFO
        if st['dma_rem'] > 0 and len(fifo) < FIFO:
            fifo.append(ring[st['dma_pos'] % SIZE])
            st['dma_pos'] += 1
            st['dma_rem'] -= 1
            if st['dma_rem'] == 0:
                st['raw'] = True        # completion latched, whatever INTE is

    def uart_step():                    # the UART sends one byte
        if fifo:
            wire.append(fifo.pop(0))

    def tx_start():                     # C: tx_start()
        st['inte'] = False
        n = st['head'] - st['tail']
        if n == 0:
            st['busy'] = False
            return
        assert not st['raw'], (seed, 'tx_start with a completion still latched')
        st['infl'] = n
        st['busy'] = True
        st['dma_pos'] = st['tail']
        st['dma_rem'] = n

    def reap():                         # C: tx_reap()
        if st['busy'] and st['dma_rem'] == 0:
            st['raw'] = False           # ints0 write
            st['tail'] += st['infl']
            st['busy'] = False
            tx_start()

    def isr():                          # C: tx_dma_isr()
        if st['inte'] and st['raw']:
            st['raw'] = False
            if st['busy']:
                st['tail'] += st['infl']
                st['busy'] = False
            assert st['head'] != st['tail'], (seed, 'completion interrupt with nothing queued')
            tx_start()

    def background(in_cs=False):
        for _ in range(R.randint(0, 3)):
            r = R.random()
            if r < 0.4:
                dma_step()
            elif r < 0.7:
                uart_step()
            elif not in_cs:
                isr()

    def tx_free():                      # C: link_tx_free()
        reap()
        return SIZE - (st['head'] - st['tail'])

    def write(msg):                     # C: link_write()
        reap()                          # critical section 1
        background()
        used = st['head'] - st['tail']
        if len(msg) > SIZE - used:
            return False                # U-11: rejected in full
        background()
        for b in msg:                   # copy into the ring, publish head
            ring[st['head'] % SIZE] = b
            st['head'] += 1
        background()
        reap()                          # critical section 2
        if not st['busy']:
            tx_start()
        elif st['tail'] + st['infl'] != st['head']:
            st['inte'] = True           # running transfer doesn't cover it all
        background()
        return True

    for _ in range(400):
        r = R.random()
        if r < 0.5:
            n = R.choice([1, 1, 1, 2, 5, 20, 40, 70])
            msg = [nxt[0] + i for i in range(n)]
            if write(msg):
                nxt[0] += n
                sent.extend(msg)
        elif r < 0.6:
            tx_free()
        background()

    for _ in range(5000):               # drain: no API calls at all
        background()

    assert wire == sent, (seed, len(wire), len(sent))
    return len(sent)


if __name__ == '__main__':
    total = sum(run(s) for s in range(3000))
    print('ok, seeds=3000, bytes checked:', total)

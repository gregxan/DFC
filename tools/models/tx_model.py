"""Randomised model of the TX path in src/link.c (U-13 lazy completion
interrupt; Rev 2.12). Interleaves DMA progress, UART draining and
the ISR between every step of link_write(), and checks byte order, no loss,
and that the queue drains with no further API calls.

    py tools/models/tx_model.py
"""
# Randomised model of link.c TX (A + B). Checks order, no loss, no stall.
import random
SIZE=64; FIFO=32
def run(seed):
    R=random.Random(seed)
    st=dict(head=0,tail=0,infl=0,busy=False,inte=False,raw=False,dma_rem=0,dma_pos=0)
    ring=[None]*SIZE; fifo=[]; wire=[]; sent=[]; nxt=[0]
    def isr_ready(): return st['inte'] and st['raw']
    def dma_step():
        if st['dma_rem']>0 and len(fifo)<FIFO:
            fifo.append(ring[st['dma_pos']%SIZE]); st['dma_pos']+=1; st['dma_rem']-=1
            if st['dma_rem']==0: st['raw']=True
    def uart_step():
        if fifo: wire.append(fifo.pop(0))
    def tx_start():
        st['inte']=False
        n=st['head']-st['tail']
        if n==0: st['busy']=False; return
        st['infl']=n; st['busy']=True; st['raw']=False
        st['dma_pos']=st['tail']; st['dma_rem']=n
    def reap():
        if st['busy'] and st['dma_rem']==0:
            st['raw']=False
            st['tail']+=st['infl']; st['busy']=False
            tx_start()
    def isr():
        if isr_ready():
            st['raw']=False
            if st['busy']: st['tail']+=st['infl']; st['busy']=False
            tx_start()
    def background(cs=False):
        for _ in range(R.randint(0,3)):
            r=R.random()
            if r<0.4: dma_step()
            elif r<0.7: uart_step()
            elif not cs: isr()
    def write(msg):
        # step 1: CS reap
        reap(); background()
        used=st['head']-st['tail']
        if len(msg)>SIZE-used: return False
        n=list(msg)
        background()
        for b in n: ring[st['head']%SIZE]=b; st['head']+=1
        background()
        # CS
        reap()
        if not st['busy']: tx_start()
        else: st['inte']=True
        background()
        return True
    for step in range(400):
        if R.random()<0.5:
            L=R.choice([1,1,1,2,5,20,40,70])
            msg=[nxt[0]+i for i in range(L)]
            if write(msg): nxt[0]+=L; sent.extend(msg)
        background()
    # drain: no more writes; background must finish everything w/o main help except link_tx_free reaps
    for _ in range(5000):
        background()
    assert wire==sent, (seed, len(wire), len(sent), wire[:10], sent[:10])
    return len(sent)
tot=0
for s in range(3000): tot+=run(s)
print("ok, seeds=3000, bytes checked:", tot)

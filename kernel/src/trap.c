#include "asm/x86.h"
#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "traps.h"
#include "spinlock.h"
#include "vm.h"
#include "lab2_ag.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[]; // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

void tvinit(void)
{
  int i;

  for (i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE << 3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE << 3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void idtinit(void)
{
  lidt(idt, sizeof(idt));
}

// PAGEBREAK: 41
void trap(struct trapframe *tf)
{
  if (tf->trapno == T_SYSCALL)
  {
    if (myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if (myproc()->killed)
      exit();
    return;
  }

  switch (tf->trapno)
  {
  case T_IRQ0 + IRQ_TIMER:
    if (cpuid() == 0)
    {
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE + 1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;
  case T_PGFLT:
    /*
    cprintf("Page Fault\n");
    cprintf("Error code: %d\n", tf->err);
    cprintf("Process: ID: %d, Name: %s\n", myproc()->pid, myproc()->name);
    */
    lab2_report_pagefault(tf);

    uint va = PGROUNDDOWN(rcr2()); // Read virtual address that was being accessed.
    pde_t *pgdir_p = myproc()->pgdir;
    pte_t *pte_p = walkpgdir(pgdir_p, (void *)va, 0);
    if (pte_p == 0 || !(*pte_p & PTE_P) || !(*pte_p & PTE_U) || *pte_p & PTE_W) {
      myproc()->killed = 1;
      break;
    }
    uint pa = PTE_ADDR(*pte_p);
    uint flags = PTE_FLAGS(*pte_p);
    /*
    cprintf("PTE: %d\n", *pte_p);
    cprintf("Page Fault 2\n");
    cprintf("PA: %d\n", pa);
    cprintf("VA: %d\n", va);
    */

    //  Read error code to determine if it is a copy on write page fault/zero write page fault(Caused by a page protection write access violation)
    if ((tf->err & 1) && (tf->err & 3))
    {
      // Kill process if trying to access non user space.
      if ((flags & PTE_U) == 0)
      {
        myproc()->killed = 1;
        break;
      }
      // Decide if it's COW or Zero

      if ((flags & PTE_ZERO) != 0)
      {
        char *mem = kalloc();
        if (mem == 0)
        {
          myproc()->killed = 1;
          break;
        }
        // cprintf("Mem: %p\n", mem);

        lab2_pgzero((void *)mem, PGROUNDDOWN(va));
        *pte_p = (((((V2P(mem) / 4096) << 12) | flags) | PTE_W) & ~PTE_COW) & ~PTE_ZERO;
        // cprintf("New PTE: %d\n", *pte_p);

        invlpg((void *)va);
        // cprintf("End flag ZERO\n");

        break;
      }

      else if ((flags & PTE_COW) != 0)
      {
        if (get_ref_count(pa) > 0)
        { // Make a copy of the physical page accessed, decrement its reference counter, and set the virtual address to point to the new physical address

          // cprintf("Ref count: %d\n", get_ref_count(pa));

          char *mem = kalloc();
          if (mem == 0)
          {
            myproc()->killed = 1;
            break;
          }
          lab2_pgcopy(mem, P2V(pa), PGROUNDDOWN(va));
          *pte_p = (((((V2P(mem) / 4096) << 12) | flags) | PTE_W) & ~PTE_COW) & ~PTE_ZERO;
          // cprintf("New PA: %d\n", V2P(mem));

          invlpg((void *)va);

          // mappages(pgdir_p, (void *)va, PGSIZE, V2P(mem), flags | SET_WRITEABLE);
          dec_ref_count(pa);
        }
        else
        {
          // Set the page to be writeable
          *pte_p = (*pte_p | PTE_W) & ~PTE_COW;
          invlpg((void *)va);
        }
        break;
      }

      // cprintf("Page Fault Flags not Set\n");
    }

  // PAGEBREAK: 13
  default:
    if (myproc() == 0 || (tf->cs & 3) == 0)
    {
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  // NOTE(lab2): Disabled preemptive yield for testing.
  /*
  if(myproc() && myproc()->state == RUNNING &&
     tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();
  */

  // Check if the process has been killed since we yielded
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();
}

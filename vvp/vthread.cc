/*
 * Copyright (c) 2001 Stephen Williams (steve@icarus.com)
 *
 *    This source code is free software; you can redistribute it
 *    and/or modify it in source code form under the terms of the GNU
 *    General Public License as published by the Free Software
 *    Foundation; either version 2 of the License, or (at your option)
 *    any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */
#if !defined(WINNT)
#ident "$Id: vthread.cc,v 1.17 2001/04/01 04:34:28 steve Exp $"
#endif

# include  "vthread.h"
# include  "codes.h"
# include  "schedule.h"
# include  "functor.h"
# include  "vpi_priv.h"
# include  <malloc.h>
# include  <string.h>
# include  <assert.h>

struct vthread_s {
	/* This is the program counter. */
      unsigned long pc;
      unsigned char *bits;
      unsigned short nbits;
	/* This points to the top (youngest) child of the thread. */
      struct vthread_s*child;
	/* This is set if the parent is waiting for me to end. */
      struct vthread_s*reaper;
	/* This is used for keeping wait queues. */
      struct vthread_s*next;
};

static void thr_check_addr(struct vthread_s*thr, unsigned addr)
{
      if (addr < thr->nbits)
	    return;
      assert(addr < 0x10000);
      while (thr->nbits <= addr) {
	    thr->bits = (unsigned char*)realloc(thr->bits, thr->nbits/4 + 16);
	    memset(thr->bits + thr->nbits/4, 0xaa, 16);
	    thr->nbits += 16*4;
      }
}

static inline unsigned thr_get_bit(struct vthread_s*thr, unsigned addr)
{
      assert(addr < thr->nbits);
      unsigned idx = addr % 4;
      addr /= 4;
      return (thr->bits[addr] >> (idx*2)) & 3;
}

static inline void thr_put_bit(struct vthread_s*thr,
			       unsigned addr, unsigned val)
{
      thr_check_addr(thr, addr);
      unsigned idx = addr % 4;
      addr /= 4;
      unsigned mask = 3 << (idx*2);

      thr->bits[addr] = (thr->bits[addr] & ~mask) | (val << (idx*2));
}

/*
 * Create a new thread with the given start address.
 */
vthread_t v_newthread(unsigned long pc)
{
      vthread_t thr = new struct vthread_s;
      thr->pc = pc;
      thr->bits = (unsigned char*)malloc(16);
      thr->nbits = 16*4;
      thr->child = 0;
      thr->reaper = 0;
      thr->next = 0;

      thr_put_bit(thr, 0, 0);
      thr_put_bit(thr, 1, 1);
      thr_put_bit(thr, 2, 2);
      thr_put_bit(thr, 3, 3);
      return thr;
}

static void vthread_reap(vthread_t thr)
{
      assert(thr->next == 0);
      assert(thr->child == 0);
      free(thr);
}


/*
 * This function runs a thread by fetching an instruction,
 * incrementing the PC, and executing the instruction.
 */
void vthread_run(vthread_t thr)
{
      for (;;) {
	    vvp_code_t cp = codespace_index(thr->pc);
	    thr->pc += 1;

	    assert(cp->opcode);

	      /* Run the opcode implementation. If the execution of
		 the opcode returns false, then the thread is meant to
		 be paused, so break out of the loop. */
	    bool rc = (cp->opcode)(thr, cp);
	    if (rc == false)
		  return;
      }
}

void vthread_schedule_list(vthread_t thr)
{
      while (thr) {
	    vthread_t tmp = thr;
	    thr = thr->next;
	    schedule_vthread(tmp, 0);
      }
}

bool of_ADD(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx1 >= 4);
      assert(cp->number <= 8*sizeof(unsigned long));

      unsigned idx1 = cp->bit_idx1;
      unsigned idx2 = cp->bit_idx2;
      unsigned long lv = 0, rv = 0;

      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {
	    unsigned lb = thr_get_bit(thr, idx1);
	    unsigned rb = thr_get_bit(thr, idx2);

	    if ((lb | rb) & 2)
		  goto x_out;

	    lv |= lb << idx;
	    rv |= rb << idx;

	    idx1 += 1;
	    if (idx2 >= 4)
		  idx2 += 1;
      }

      lv += rv;

      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {
	    thr_put_bit(thr, cp->bit_idx1+idx, (lv&1) ? 1 : 0);
	    lv >>= 1;
      }

      return true;

 x_out:
      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1)
	    thr_put_bit(thr, cp->bit_idx1+idx, 2);

      return true;
}

bool of_ASSIGN(vthread_t thr, vvp_code_t cp)
{
      unsigned char bit_val = thr_get_bit(thr, cp->bit_idx2);
      schedule_assign(cp->iptr, bit_val, cp->bit_idx1);
      return true;
}

bool of_CMPU(vthread_t thr, vvp_code_t cp)
{
      unsigned eq = 1;
      unsigned eeq = 1;
      unsigned lt = 2;

      unsigned idx1 = cp->bit_idx1;
      unsigned idx2 = cp->bit_idx2;

      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {
	    unsigned lv = thr_get_bit(thr, idx1);
	    unsigned rv = thr_get_bit(thr, idx2);

	    if (lv != rv)
		  eeq = 0;
	    if (eq != 2) {
		  if ((lv == 0) && (rv != 0))
			eq = 0;
		  if ((lv == 1) && (rv != 1))
			eq = 0;
		  if ((lv | rv) >= 2)
			eq = 2;
	    }

	    if (idx1 >= 4) idx1 += 1;
	    if (idx2 >= 4) idx2 += 1;
      }

      thr_put_bit(thr, 4, eq);
      thr_put_bit(thr, 5, lt);
      thr_put_bit(thr, 6, eeq);

      return true;
}

bool of_CMPX(vthread_t thr, vvp_code_t cp)
{
      unsigned eq = 1;

      unsigned idx1 = cp->bit_idx1;
      unsigned idx2 = cp->bit_idx2;

      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {
	    unsigned lv = thr_get_bit(thr, idx1);
	    unsigned rv = thr_get_bit(thr, idx2);

	    if ((lv < 2) && (rv < 2) && (lv != rv)) {
		  eq = 0;
		  break;
	    }

	    if (idx1 >= 4) idx1 += 1;
	    if (idx2 >= 4) idx2 += 1;
      }

      thr_put_bit(thr, 4, eq);

      return true;
}

bool of_CMPZ(vthread_t thr, vvp_code_t cp)
{
      unsigned eq = 1;

      unsigned idx1 = cp->bit_idx1;
      unsigned idx2 = cp->bit_idx2;

      for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1) {
	    unsigned lv = thr_get_bit(thr, idx1);
	    unsigned rv = thr_get_bit(thr, idx2);

	    if ((lv < 3) && (rv < 3) && (lv != rv)) {
		  eq = 0;
		  break;
	    }

	    if (idx1 >= 4) idx1 += 1;
	    if (idx2 >= 4) idx2 += 1;
      }

      thr_put_bit(thr, 4, eq);

      return true;
}

bool of_DELAY(vthread_t thr, vvp_code_t cp)
{
	//printf("thread %p: %%delay %lu\n", thr, cp->number);
      schedule_vthread(thr, cp->number);
      return false;
}

bool of_END(vthread_t thr, vvp_code_t cp)
{
      if (thr->reaper)
	    schedule_vthread(thr->reaper, 0);
      thr->reaper = thr;
      return false;
}

bool of_FORK(vthread_t thr, vvp_code_t cp)
{
      vthread_t child = v_newthread(cp->cptr);
      child->child = thr->child;
      thr->child = child;
      schedule_vthread(child, 0);
      return true;
}

bool of_INV(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx1 >= 4);
      for (unsigned idx = 0 ;  idx < cp->bit_idx2 ;  idx += 1) {
	    unsigned val = thr_get_bit(thr, cp->bit_idx1+idx);
	    switch (val) {
		case 0:
		  val = 1;
		  break;
		case 1:
		  val = 0;
		  break;
		default:
		  val = 2;
		  break;
	    }
	    thr_put_bit(thr, cp->bit_idx1+idx, val);
      }
      return true;
}

bool of_JMP(vthread_t thr, vvp_code_t cp)
{
      thr->pc = cp->cptr;
      return true;
}

bool of_JMP0(vthread_t thr, vvp_code_t cp)
{
      if (thr_get_bit(thr, cp->bit_idx1) == 0)
	    thr->pc = cp->cptr;
      return true;
}

bool of_JMP0XZ(vthread_t thr, vvp_code_t cp)
{
      if (thr_get_bit(thr, cp->bit_idx1) != 1)
	    thr->pc = cp->cptr;
      return true;
}

bool of_JMP1(vthread_t thr, vvp_code_t cp)
{
      if (thr_get_bit(thr, cp->bit_idx1) == 1)
	    thr->pc = cp->cptr;
      return true;
}

bool of_JOIN(vthread_t thr, vvp_code_t cp)
{
      assert(thr->child);
      if (thr->child->reaper == thr->child) {
	    vthread_reap(thr->child);
	    return true;
      }

      assert(thr->child->reaper == 0);
      thr->child->reaper = thr;
      return false;
}

bool of_LOAD(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx1 >= 4);
      thr_put_bit(thr, cp->bit_idx1, functor_get(cp->iptr));
      return true;
}

bool of_MOV(vthread_t thr, vvp_code_t cp)
{
      assert(cp->bit_idx1 >= 4);

      if (cp->bit_idx2 >= 4) {
	    for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1)
		  thr_put_bit(thr,
			      cp->bit_idx1+idx,
			      thr_get_bit(thr, cp->bit_idx2+idx));

      } else {
	    for (unsigned idx = 0 ;  idx < cp->number ;  idx += 1)
		  thr_put_bit(thr,
			      cp->bit_idx1+idx,
			      thr_get_bit(thr, cp->bit_idx2));
      }

      return true;
}

bool of_NOOP(vthread_t thr, vvp_code_t cp)
{
      return true;
}

bool of_SET(vthread_t thr, vvp_code_t cp)
{
      unsigned char bit_val = thr_get_bit(thr, cp->bit_idx1);
      functor_set(cp->iptr, bit_val);

      return true;
}

bool of_VPI_CALL(vthread_t thr, vvp_code_t cp)
{
	// printf("thread %p: %%vpi_call\n", thr);
      vpip_execute_vpi_call(cp->handle);
      return schedule_finished()? false : true;
}

/*
 * Implement the wait by locating the functor for the event, and
 * adding this thread to the threads list for the event.
 */
bool of_WAIT(vthread_t thr, vvp_code_t cp)
{
      functor_t fp = functor_index(cp->iptr);
      assert((fp->mode == 1) || (fp->mode == 2));
      vvp_event_t ep = fp->event;
      thr->next = ep->threads;
      ep->threads = thr;

      return false;
}

/*
 * $Log: vthread.cc,v $
 * Revision 1.17  2001/04/01 04:34:28  steve
 *  Implement %cmp/x and %cmp/z instructions.
 *
 * Revision 1.16  2001/03/31 17:36:02  steve
 *  Add the jmp/1 instruction.
 *
 * Revision 1.15  2001/03/31 01:59:59  steve
 *  Add the ADD instrunction.
 *
 * Revision 1.14  2001/03/30 04:55:22  steve
 *  Add fork and join instructions.
 *
 * Revision 1.13  2001/03/29 03:46:36  steve
 *  Support named events as mode 2 functors.
 *
 * Revision 1.12  2001/03/26 04:00:39  steve
 *  Add the .event statement and the %wait instruction.
 *
 * Revision 1.11  2001/03/25 03:54:26  steve
 *  Add JMP0XZ and postpone net inputs when needed.
 *
 * Revision 1.10  2001/03/23 04:56:03  steve
 *  eq is x if either value of cmp/u has x or z.
 *
 * Revision 1.9  2001/03/23 01:53:46  steve
 *  Support set of functors from thread bits.
 *
 * Revision 1.8  2001/03/23 01:11:06  steve
 *  Handle vectors pulled out of a constant bit.
 *
 * Revision 1.7  2001/03/22 05:08:00  steve
 *  implement %load, %inv, %jum/0 and %cmp/u
 *
 * Revision 1.6  2001/03/20 06:16:24  steve
 *  Add support for variable vectors.
 *
 * Revision 1.5  2001/03/19 01:55:38  steve
 *  Add support for the vpiReset sim control.
 *
 * Revision 1.4  2001/03/16 01:44:34  steve
 *  Add structures for VPI support, and all the %vpi_call
 *  instruction. Get linking of VPI modules to work.
 *
 * Revision 1.3  2001/03/11 23:06:49  steve
 *  Compact the vvp_code_s structure.
 *
 * Revision 1.2  2001/03/11 22:42:11  steve
 *  Functor values and propagation.
 *
 * Revision 1.1  2001/03/11 00:29:39  steve
 *  Add the vvp engine to cvs.
 *
 */


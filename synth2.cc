/*
 * Copyright (c) 2002-2006 Stephen Williams (steve@icarus.com)
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
#ifdef HAVE_CVS_IDENT
#ident "$Id: synth2.cc,v 1.39.2.38 2006/07/02 00:50:15 steve Exp $"
#endif

# include "config.h"

# include  "functor.h"
# include  "netlist.h"
# include  "compiler.h"
# include  <assert.h>

static int debug_synth2=0;

#ifdef __FUNCTION__

#define DEBUG_SYNTH2_ENTRY(class) if (debug_synth2) { cerr << "Enter " << class << "::" \
       	<< __FUNCTION__ << endl; dump(cerr, 4); }
#define DEBUG_SYNTH2_EXIT(class,val) if (debug_synth2) { cerr << "Exit " << class << "::" \
       	<< __FUNCTION__ << ", result " << val << endl; }

#else
#define DEBUG_SYNTH2_ENTRY(class)
#define DEBUG_SYNTH2_EXIT(class,val)
#endif

bool NetProc::synth_async_noaccum(Design*des, NetScope*scope, bool sync_flag,
				  struct sync_accounting_cell*nex_ff,
				  NetNet*nex_map, NetNet*nex_out)
{
	/* Make an unconnected stub for the accum_in net. */
      const perm_string tmp = perm_string::literal("tmp");
      NetNet*stub = new NetNet(scope, tmp, NetNet::WIRE, nex_out->pin_count());

      bool flag = synth_async(des, scope, sync_flag, nex_ff,
			      nex_map, nex_out, stub);

      delete stub;
      return flag;
}

bool NetProc::synth_async(Design*des, NetScope*scope, bool sync_flag,
			  struct sync_accounting_cell*nex_ff,
			  NetNet*nex_map, NetNet*nex_out,
			  NetNet*accum_in)
{
      return false;
}

bool NetProc::synth_sync(Design*des, NetScope*scope,
			 struct sync_accounting_cell*nex_ff,
			 NetNet*nex_map, NetNet*nex_out,
			 const svector<NetEvProbe*>&events)
{
      return synth_async_noaccum(des, scope, true, nex_ff, nex_map, nex_out);
}

static unsigned find_nexus_in_set(const NetNet*nset, const Nexus*nex)
{
      unsigned idx = 0;
      for (idx = 0 ;  idx < nset->pin_count() ;  idx += 1)
	    if (nset->pin(idx).nexus() == nex)
		  return idx;

      return idx;
}

/*
 * Async synthesis of assignments is done by synthesizing the rvalue
 * expression, then connecting the l-value directly to the output of
 * the r-value.
 *
 * The nex_map is the O-set for the statement, and lists the positions
 * of the outputs as the caller wants results linked up. The nex_out,
 * however, is the set of nexa that are to actually get linked to the
 * r-value.
 */

bool NetAssignBase::synth_async(Design*des, NetScope*scope, bool sync_flag,
				struct sync_accounting_cell*nex_ff,
				NetNet*nex_map, NetNet*nex_out,
				NetNet*accum_in)
{
      NetNet*rsig = rval_->synthesize(des);
      if (rsig == 0) {
	    cerr << get_line() << ": error: Cannot synthesize r-value "
		 << "expression of assignment." << endl;
	    des->errors += 1;
	    return false;
      }

      assert(rsig);

      unsigned roff = 0;

      for (NetAssign_*cur = lval_ ;  cur ;  cur = cur->more) {

	    NetMemory*lmem = cur->mem();
	    if (lmem && !sync_flag) {
		  cerr << get_line() << ": error: Cannot synthesize memory "
		       << "assignment in asynchronous logic." << endl;
		  des->errors += 1;
		  return false;
	    }

	      /* Is this an assignment to a memory? If so, then
		 explode the memory to an array of reg bits. The
		 context that is calling this will attach a decoder
		 between the ff and the r-val. In fact, the memory at
		 this point has already been scanned and exploded, so
		 the explode_to_reg method below will return a
		 pre-existing vector.

		 Note that this is only workable if we are in the
		 asynchronous path of a synchronous thread. The
		 sync_flag must be true in this case. */
	    if (lmem) {
		  assert(sync_flag);
		  synth_async_mem_sync_(des, scope, cur, rsig, roff,
					nex_map, nex_out);
		  continue;
	    }


	    NetNet*lsig = cur->sig();
	    if (!lsig) {
		  cerr << get_line() << ": error: NetAssignBase::synth_async "
			"on unsupported lval ";
		  dump_lval(cerr);
		  cerr << endl;
		  des->errors += 1;
		  return false;
	    }

	    if (cur->bmux() && !sync_flag) {
		  cerr << get_line() << ": error: Assign to bit select "
		       << "Not possible in asynchronous logic." << endl;
		  des->errors += 1;
		  return false;
	    }

	      /* Handle the special case that this is a decoded
		 enable. generate a demux for the device, with the
		 WriteData connected to the r-value and the Data
		 vector connected to the feedback. */
	    if (cur->bmux() != 0) {
		  assert(sync_flag);

		  NetNet*adr = cur->bmux()->synthesize(des);

		    /* Create a NetEemux wide enough to connect to all
		       the bits of the lvalue signal (generally more
		       then the bits of lwidth). */
		  NetDemux*dq = new NetDemux(scope, scope->local_symbol(),
					     lsig->pin_count(),
					     adr->pin_count(),
					     lsig->pin_count());
		  des->add_node(dq);
		  dq->set_line(*this);

		    /* The bmux expression connects to the address of
		       the Demux device. */
		  for (unsigned idx = 0; idx < adr->pin_count() ;  idx += 1)
			connect(dq->pin_Address(idx), adr->pin(idx));

		  assert(cur->lwidth() == 1);

		    /* Cycle the associated FF Data and Q through the
		       demux to make synchronous "latches" that the
		       Demux modifies. */
		  assert(nex_ff[0].ff->width() >= lsig->pin_count());
		  for (unsigned idx = 0; idx < lsig->pin_count(); idx += 1) {
			unsigned off = cur->get_loff()+idx;
			connect(nex_ff[0].ff->pin_Q(off), dq->pin_Data(idx));
		  }

		  for (unsigned idx = 0; idx < lsig->pin_count(); idx += 1) {
			unsigned off = cur->get_loff()+idx;
			unsigned ptr = find_nexus_in_set(nex_map, lsig->pin(off).nexus());
			assert(ptr < nex_out->pin_count());
			connect(nex_out->pin(ptr), dq->pin_Q(idx));
		  }

		    /* The r-value (1 bit) connects to the WriteData
		       input of the demux. */
		  connect(dq->pin_WriteData(0), rsig->pin(roff));

		  roff += cur->lwidth();
		  cur->turn_sig_to_wire_on_release();
		  continue;
	    }

	      /* By this point ant bmux() has been dealt with. Panic
		 if that is not so. */
	    assert(! cur->bmux());

	      /* Bind the outputs that we do make to the nex_out. Use the
		 nex_map to map the l-value bit position to the nex_out bit
		 position. */
	    for (unsigned idx = 0 ;  idx < cur->lwidth() ;  idx += 1) {
		  unsigned off = cur->get_loff()+idx;
		  unsigned ptr = find_nexus_in_set(nex_map, lsig->pin(off).nexus());

		  assert(ptr <= nex_map->pin_count());
		  connect(nex_out->pin(ptr), rsig->pin(roff+idx));
	    }

	    roff += cur->lwidth();

	      /* This lval_ represents a reg that is a WIRE in the
		 synthesized results. This function signals the destructor
		 to change the REG that this l-value refers to into a
		 WIRE. It is done then, at the last minute, so that pending
		 synthesis can continue to work with it as a WIRE. */
	    cur->turn_sig_to_wire_on_release();
      }

      return true;
}

/*
 * Handle the special case of assignment to memory. Explode the memory
 * to an array of reg bits. The context that is calling this will
 * attach a decoder between the ff and the r-val. In fact, the memory
 * at this point has already been scanned and exploded, so the
 * explode_to_reg method below will return a pre-existing vector.
 */
bool NetAssignBase::synth_async_mem_sync_(Design*des, NetScope*scope,
					  NetAssign_*cur,
					  NetNet*rsig, unsigned&roff,
					  NetNet*nex_map, NetNet*nex_out)
{
      NetMemory*lmem = cur->mem();
      assert(lmem);

      NetNet*msig = lmem->explode_to_reg();
      cur->incr_mem_lref();

	/* Handle the special case that the mux expression is
	   constant. In this case, just hook up the pertinent bits. */
      if (NetEConst*ae = dynamic_cast<NetEConst*>(cur->bmux())) {
	    long adr_s = ae->value().as_long();
	    if (adr_s >= (long)lmem->count()) {
		  cerr << get_line() << ": error: "
		       << "Address " << adr_s
		       << " is outside range of memory."
		       << " Skipping assignment." << endl;
		  des->errors += 1;
		  return false;
	    }

	    unsigned adr = lmem->index_to_address(adr_s) * lmem->width();
	    for (unsigned idx = 0 ;  idx < cur->lwidth() ;  idx += 1) {
		  unsigned off = adr+idx;
		  unsigned ptr = find_nexus_in_set(nex_map, msig->pin(off).nexus());
		  assert(ptr <= nex_map->pin_count());
		  connect(nex_out->pin(ptr), rsig->pin(roff+idx));
	    }

	    cur->turn_sig_to_wire_on_release();
	    return true;
      }

      assert(cur->bmux() != 0);

      NetNet*adr = cur->bmux()->synthesize(des);

      NetDemux*dq = new NetDemux(scope, scope->local_symbol(),
				 msig->pin_count(),
				 adr->pin_count(),
				 msig->pin_count() / cur->lwidth());
      des->add_node(dq);
      dq->set_line(*this);

      for (unsigned idx = 0; idx < adr->pin_count() ;  idx += 1)
	    connect(dq->pin_Address(idx), adr->pin(idx));

      for (unsigned idx = 0; idx < msig->pin_count(); idx += 1) {
	    unsigned off = idx;
	    unsigned ptr = find_nexus_in_set(nex_map, msig->pin(off).nexus());
	    assert(ptr <= nex_map->pin_count());
	    connect(nex_out->pin(ptr), dq->pin_Q(idx));
      }

      for (unsigned idx = 0 ;  idx < msig->pin_count(); idx += 1)
	    connect(dq->pin_Data(idx), nex_map->pin(roff+idx));

      for (unsigned idx = 0 ;  idx < cur->lwidth() ;  idx += 1)
	    connect(dq->pin_WriteData(idx), rsig->pin(roff+idx));

      roff += cur->lwidth();
      cur->turn_sig_to_wire_on_release();

      return true;
}

/*
 * Sequential blocks are translated to asynchronous logic by
 * translating each statement of the block, in order, into gates. The
 * nex_out for the block is the union of the nex_out for all the
 * substatements.
 */
bool NetBlock::synth_async(Design*des, NetScope*scope, bool sync_flag,
			   struct sync_accounting_cell*nex_ff,
			   NetNet*nex_map, NetNet*nex_out, NetNet*accum_in)
{
      if (last_ == 0) {
	    return true;
      }

      if (debug_synth) {
	    cerr << get_line() << ": debug: "
		 << (sync_flag?"sync":"async")
		 << " synthesis of statement block." << endl;
      }

      const perm_string tmp1 = perm_string::literal("tmp1");
      const perm_string tmp2 = perm_string::literal("tmp2");
      const perm_string tmp3 = perm_string::literal("tmp3");

      NetNet*accum_out = new NetNet(scope, tmp3, NetNet::WIRE,
				    nex_out->pin_count());
      accum_out->local_flag(true);

      bool flag = true;
      NetProc*cur = last_;
      do {
	    NetNet*new_accum;

	    cur = cur->next_;

	      /* Create a temporary nex_map for the substatement. */
	    NexusSet tmp_set;
	    cur->nex_output(tmp_set);
	    NetNet*tmp_map = new NetNet(scope, tmp1, NetNet::WIRE,
					tmp_set.count());
	    for (unsigned idx = 0 ;  idx < tmp_map->pin_count() ;  idx += 1)
		  connect(tmp_set[idx], tmp_map->pin(idx));

	      /* Create also a temporary net_out to collect the
		 output. */
	    NetNet*tmp_out = new NetNet(scope, tmp2, NetNet::WIRE,
					tmp_set.count());
	    tmp_out->set_line(*this);

	      /* Make a temporary set of currently accumulated outputs
		 that we can pass to the synth_async of the
		 sub-statement. Some sub-statements will use this to
		 handle default cases specially. We will delete this
		 temporary map as soon as the synth_async is done. */
	    new_accum = new NetNet(scope, tmp3, NetNet::WIRE, tmp_set.count());
	    for (unsigned idx = 0 ;  idx < tmp_set.count() ;  idx += 1) {
		  unsigned ptr = find_nexus_in_set(nex_map, tmp_set[idx]);
		  if (accum_out->pin(ptr).is_linked())
			connect(new_accum->pin(idx), accum_out->pin(ptr));
	    }

	    bool ok_flag = cur->synth_async(des, scope, sync_flag, nex_ff,
					    tmp_map, tmp_out, new_accum);
	    flag = flag && ok_flag;

	    delete new_accum;

	      /* NOTE: tmp_set is not valid after this point, since
		 the cur->synth_async method may change nexa that it
		 refers to. */

	    if (ok_flag == false)
		  continue;

	      /* Now start building a new set of accumulated outputs
		 that we will pass to the next statement of the block,
		 or that will be the output of the block. */
	    new_accum = new NetNet(scope, tmp3, NetNet::WIRE,
				   nex_out->pin_count());
	    new_accum->set_line(*this);

	      /* Use the nex_map to link up the output from the
		 substatement to the output of the block as a whole. */
	    for (unsigned idx = 0 ;  idx < tmp_out->pin_count() ; idx += 1) {
		  unsigned ptr = find_nexus_in_set(nex_map, tmp_map->pin(idx).nexus());
		  if (ptr >= nex_map->pin_count()) {
			cerr << cur->get_line() << ": internal error: "
			     << "Nexus isn't in nex_map?! idx=" << idx
			     << " map width = " << nex_map->pin_count()
			     << " tmp_map count = " << tmp_map->pin_count()
			     << endl;
		  }
		  assert(ptr < new_accum->pin_count());
		  connect(new_accum->pin(ptr), tmp_out->pin(idx));
	    }

	    delete tmp_map;
	    delete tmp_out;

	      /* Anything that is not redriven by the current
		 statement inherits the value that was driven from any
		 previous statements. */
	    for (unsigned idx = 0;  idx < new_accum->pin_count();  idx += 1) {
		  if (new_accum->pin(idx).is_linked())
			continue;

		  connect(new_accum->pin(idx), accum_out->pin(idx));
	    }
	    delete accum_out;
	    accum_out = new_accum;

      } while (cur != last_);

	/* Now bind the accumulated output valies to the nex_out
	   passed in. Note that each previous step has already did the
	   pin mapping, so just connect. */
      for (unsigned idx = 0 ;  idx < accum_out->pin_count() ;  idx += 1) {
	    connect(nex_out->pin(idx), accum_out->pin(idx));
      }

      delete accum_out;

      return flag;
}

bool NetCase::synth_async(Design*des, NetScope*scope, bool sync_flag,
			  struct sync_accounting_cell*nex_ff,
			  NetNet*nex_map, NetNet*nex_out, NetNet*accum)
{
      unsigned cur;

      NetNet*esig = expr_->synthesize(des);

	/* Scan the select vector looking for constant bits. The
	   constant bits will be elided from the select input connect,
	   but we still need to keep track of them. */
      unsigned sel_pins = 0;
      unsigned long sel_mask = 0;
      unsigned long sel_ref = 0;
      for (unsigned idx = 0 ;  idx < esig->pin_count() ;  idx += 1) {

	    if (esig->pin(idx).nexus()->drivers_constant()) {
		  verinum::V bit = esig->pin(idx).nexus()->driven_value();
		  if (bit == verinum::V1)
			sel_ref |= 1 << idx;

	    } else {
		  sel_pins += 1;
		  sel_mask |= 1 << idx;
	    }
      }

	/* At this point, sel_ref represents the constant part of the
	   select input. That is, (select&sel_mask) == sel_ref for all
	   guard values that are reachable. We can use this to skip
	   unreachable guards. */


	/* Build a map of guard values to mux select values. This
	   helps account for constant select bits that are being
	   elided. The guard2sel mapping will only be valid for
	   reachable guards. */
      map<unsigned long,unsigned long>guard2sel;
      cur = 0;
      for (unsigned idx = 0 ;  idx < (1U<<esig->pin_count()) ;  idx += 1) {
	    if ((idx & ~sel_mask) == sel_ref) {
		  guard2sel[idx] = cur;
		  cur += 1;
	    }
      }
      assert(cur == (1U << sel_pins));

      unsigned nondefault_items = 0;
      for (unsigned item = 0 ;  item < nitems_ ;  item += 1) {
	    if (items_[item].guard != 0)
		  nondefault_items += 1;
      }

	/* Handle the special case that this can be done it a smaller
	   1-hot MUX. */
      if (nondefault_items < sel_pins)
	    return synth_async_1hot_(des, scope, sync_flag, nex_ff,
				    nex_map, nex_out, accum,
				    esig, nondefault_items);

      NetMux*mux = new NetMux(scope, scope->local_symbol(),
			      nex_out->pin_count(),
			      1U << sel_pins, sel_pins);
      mux->set_line(*this);

	/* Connect the non-constant select bits to the select input of
	   the mux device. */
      cur = 0;
      for (unsigned idx = 0 ;  idx < esig->pin_count() ;  idx += 1) {
	      /* skip bits that are known to be constant. */
	    if ((sel_mask & (1U << idx)) == 0)
		  continue;

	    connect(mux->pin_Sel(cur), esig->pin(idx));
	    cur += 1;
      }
      assert(cur == sel_pins);

	/* Hook up the output of the mux to the mapped output pins. */
      for (unsigned idx = 0 ;  idx < mux->width() ;  idx += 1)
	    connect(nex_out->pin(idx), mux->pin_Result(idx));

      NetProc**statement_map = new NetProc*[1 << sel_pins];
      for (unsigned item = 0 ;  item < (1U<<sel_pins) ;  item += 1)
	    statement_map[item] = 0;

	/* Assign the input statements to MUX inputs. This involves
	   calculating the guard value, passing that through the
	   guard2sel map, then saving the statement in the
	   statement_map. If I find a default case, then save that for
	   use later. */
      NetProc*default_statement = 0;
      for (unsigned item = 0 ;  item < nitems_ ;  item += 1) {
	      /* Skip the default case this pass. */
	    if (items_[item].guard == 0) {
		  default_statement = items_[item].statement;
		  continue;
	    }

	    NetEConst*ge = dynamic_cast<NetEConst*>(items_[item].guard);
	    assert(ge);
	    verinum gval = ge->value();

	    list<verinum>gstack;
	    gstack.push_front(gval);

	      /* A guard may have X/Z values, if this is a casex
		 statement. In this case, replace a number with an x/z
		 values with two numbers, one with a 0 substituted,
		 another with a 1 substituted. Only process as a guard
		 numbers that are well defined. The gstack allows us
		 to build a list of numbers that match the pattern. */
	    while (! gstack.empty()) {
		  verinum tmp = gstack.front();
		  gstack.pop_front();

		  if (tmp.is_defined()
		      || type() == NetCase::EQ) {

			  /* Skip guards that are unreachable. */
			if ((sel_ref&~sel_mask) != (tmp.as_ulong()&~sel_mask))
			      continue;

			unsigned sel_idx = guard2sel[tmp.as_ulong()];
			assert(items_[item].statement);
			statement_map[sel_idx] = items_[item].statement;

		  } else if (type() == NetCase::EQX) {
			  /* Process casex patterns. */
			verinum tmp0 = tmp;
			verinum tmp1 = tmp;
			unsigned idx = 0;
			while (idx < tmp.len()) {
			      verinum::V tv = tmp.get(idx);
			      if (tv == verinum::Vx)
				    break;
			      if (tv == verinum::Vz)
				    break;
			      idx += 1;
			}
			assert(idx < tmp.len());
			tmp0.set(idx, verinum::V0);
			tmp1.set(idx, verinum::V1);
			gstack.push_front(tmp1);
			gstack.push_front(tmp0);
		  } else {
			assert(type() == NetCase::EQZ);
			assert(0);
		  }
	    }
      }

	/* Set up a default default_sig that uses the accumulated
	   input pins. This binding is suppressed by an actual default
	   statement if one exists. */
      NetNet*default_sig = 0;
      if (default_statement == 0) {
	    default_sig = accum;
	    for (unsigned idx = 0 ;  idx < accum->pin_count() ;  idx += 1) {
		  if (! accum->pin(idx).is_linked()) {
			default_sig = 0;
			break;
		  }
	    }
      }

      bool return_flag = true;

	/* Now that statements match with mux inputs, synthesize the
	   sub-statements. If I get to an input that has no statement,
	   then use the default statement there. */
      for (unsigned item = 0 ;  item < (1U<<sel_pins) ;  item += 1) {

	      /* Detect the case that this is a default input, and I
		 have a precalculated default_sig. */
	    if ((statement_map[item] == 0) && (default_sig != 0)) {
		for (unsigned idx = 0 ;  idx < mux->width() ;  idx += 1)
		      connect(mux->pin_Data(idx, item), default_sig->pin(idx));
		continue;
	    }

	    NetNet*sig = new NetNet(scope, scope->local_symbol(),
				    NetNet::WIRE, nex_map->pin_count());
	    sig->local_flag(true);

	      /* If this statement is missing, arrange for the default
		 statement to be processed here. Also, make the sig be
		 the default sig so that the next time we run into a
		 reference to the default, we just hook up to the
		 default again. */
	    if (statement_map[item] == 0) {
		  statement_map[item] = default_statement;
		  default_statement = 0;
		  default_sig = sig;
	    }

	    if (statement_map[item] == 0 && !sync_flag) {
		  /* Missing case and no default; this could still be
		   * synthesizable with synchronous logic, but not here. */
		  cerr << get_line()
		       << ": error: Case item " << item << " is missing"
		       << " in combinational process." << endl;
		  cerr << get_line()
		       << ":      : Do you need a default case?" << endl;
		  des->errors += 1;
		  return_flag = false;
		  continue;
	    }

	    if (statement_map[item] == 0) {

		    /* If this is an unspecified case, then get the
		       input from the synchronous output. Note that we
		       know by design that there is no relevent
		       default or accum input to use here, as those
		       cases are handled above. */

		  for (unsigned idx=0; idx < mux->width(); idx += 1)
			connect(mux->pin_Data(idx,item), nex_map->pin(idx));

	    } else {
		    /* Synthesize this specified case. The synth_async will
		       connect all the output bits it knows how to the
		       sig net. */
		  statement_map[item]->synth_async(des, scope, sync_flag,
						   nex_ff,
						   nex_map, sig, accum);

		  for (unsigned idx = 0 ;  idx < mux->width() ;  idx += 1) {
			if (sig->pin(idx).is_linked())
			      connect(mux->pin_Data(idx,item), sig->pin(idx));
			else if (accum->pin(idx).is_linked())
			      connect(mux->pin_Data(idx,item), accum->pin(idx));
			else if (sync_flag)
			      connect(mux->pin_Data(idx,item), nex_map->pin(idx));
			else {
				/* No likely input for this bit. So
				   leave it. The connectivity test
				   below will determine if this is an
				   error or not. */

			}
		  }
	    }
      }

	/* Input connectivity check. */

      for (unsigned wdx = 0 ;  wdx < mux->width() ;  wdx += 1) {
	    unsigned linked_count = 0;
	    unsigned last_linked = 0;
	    for (unsigned item = 0 ; item < (1U<<sel_pins) ;  item += 1) {
		  if (mux->pin_Data(wdx,item).is_linked()) {
			linked_count += 1;
			last_linked = item;
		  }
	    }

	    if (linked_count == (1U<<sel_pins))
		  continue;

	      /* If we find a single input is connected to the mux,
		 then we can guess that this is probably a case of an
		 internal value that is not really an output. In that
		 case, repeat the connection to all the inputs so that
		 it consistently follows the expression that feeds it,
		 no matter what the select.

		 NOTE: Perhaps it would be better th reduce the mux
		 width by one and connect this through? */
	    if (linked_count==1) {
		  for (unsigned item = 0; item < (1U<<sel_pins);  item += 1) {
			if (item == last_linked)
			      continue;

			connect(mux->pin_Data(wdx,item),
				mux->pin_Data(wdx,last_linked));
		  }
		  continue;
	    }

	      /* Strange connection pattern. Error message. */
	    if (return_flag != false) {
		  cerr << get_line()
		       << ": error: case " << last_linked << " statement"
		       << " does not assign expected outputs." << endl;
		  des->errors += 1;
		  return_flag = false;
	    }
      }

      delete[]statement_map;
      des->add_node(mux);

      return return_flag;
}

bool NetCase::synth_async_1hot_(Design*des, NetScope*scope, bool sync_flag,
				struct sync_accounting_cell*nex_ff,
				NetNet*nex_map, NetNet*nex_out, NetNet*accum,
				NetNet*esig, unsigned hot_items)
{
      unsigned sel_pins = hot_items;

      NetMux*mux = new NetMux(scope, scope->local_symbol(),
			      nex_out->pin_count(),
			      1U << sel_pins, sel_pins);
      mux->set_line(*this);

	/* Hook up the output of the mux to the mapped output pins. */
      for (unsigned idx = 0 ;  idx < mux->width() ;  idx += 1)
	    connect(nex_out->pin(idx), mux->pin_Result(idx));

      NetNet*tmps = new NetNet(scope, scope->local_symbol(),
			       NetNet::WIRE, sel_pins);
      for (unsigned idx = 0 ;  idx < sel_pins ;  idx += 1)
	    connect(tmps->pin(idx), mux->pin_Sel(idx));

      NetProc*default_statement = 0;
      unsigned use_item = 0;
      for (unsigned item = 0 ;  item < nitems_ ;  item += 1) {
	    if (items_[item].guard == 0) {
		  default_statement = items_[item].statement;
		  continue;
	    }

	    NetNet*tmp1 = items_[item].guard->synthesize(des);
	    assert(tmp1);

	    NetLogic*reduc = new NetLogic(scope, scope->local_symbol(),
					  1 + esig->pin_count(),
					  NetLogic::AND);
	    des->add_node(reduc);

	    tmps = new NetNet(scope, scope->local_symbol(),
			      NetNet::WIRE, esig->pin_count());
	    for (unsigned idx = 0 ;  idx < tmps->pin_count() ;  idx += 1)
		  connect(tmps->pin(idx), reduc->pin(idx+1));

	    assert(tmp1->pin_count() == esig->pin_count());
	    for (unsigned idx = 0 ;  idx < tmp1->pin_count() ; idx += 1) {
		  NetCaseCmp*cmp = new NetCaseCmp(scope,scope->local_symbol());
		  des->add_node(cmp);
		  connect(cmp->pin(0), reduc->pin(1+idx));
		  connect(cmp->pin(1), esig->pin(idx));
		  connect(cmp->pin(2), tmp1->pin(idx));
	    }

	    connect(mux->pin_Sel(use_item), reduc->pin(0));

	    NetNet*item_sig = new NetNet(scope, scope->local_symbol(),
					 NetNet::WIRE, nex_map->pin_count());
	    assert(items_[item].statement);
	    items_[item].statement->synth_async(des, scope, sync_flag, nex_ff,
						nex_map, item_sig, accum);
	    for (unsigned idx = 0 ;  idx < item_sig->pin_count() ;  idx += 1)
		  connect(mux->pin_Data(idx, 1<<use_item), item_sig->pin(idx));

	    use_item += 1;
      }

      assert(use_item == hot_items);

	/* Set up a default default_sig that uses the accumulated
	   input pins. This binding is suppressed by an actual default
	   statement if one exists. */
      NetNet*default_sig = 0;
      if (default_statement) {
	    default_sig = new NetNet(scope, scope->local_symbol(),
				     NetNet::WIRE, nex_map->pin_count());
	    default_statement->synth_async(des, scope, sync_flag, nex_ff,
					   nex_map, default_sig, accum);

      }

      if (default_sig == 0 && default_statement == 0) {
	    default_sig = accum;
	    for (unsigned idx = 0 ;  idx < accum->pin_count() ;  idx += 1) {
		  if (! accum->pin(idx).is_linked()) {
			default_sig = 0;
			break;
		  }
	    }
      }

	/* No explicit sig, so if this is a synchronous process,
	   connect the input to the output. */
      if (default_sig == 0 && sync_flag) {
	    default_sig = new NetNet(scope, scope->local_symbol(),
				     NetNet::WIRE, nex_map->pin_count());
	    for (unsigned idx = 0; idx < default_sig->pin_count() ;  idx += 1)
		  connect(default_sig->pin(idx), nex_map->pin(idx));
      }


      for (unsigned item = 0 ;  item < (1UL<<sel_pins) ;  item += 1) {
	    unsigned count_bits = 0;
	    for (unsigned idx = 0 ;  idx < sel_pins ;  idx += 1) {
		  if (item & (1<<idx))
			count_bits += 1;
	    }

	    if (count_bits == 1)
		  continue;

	    for (unsigned idx = 0 ;  idx < mux->width() ;  idx += 1)
		  connect(mux->pin_Data(idx,item), default_sig->pin(idx));
      }

      des->add_node(mux);
      return true;
}


/*
 * Handle synthesis for an asynchronous condition statement. If we get
 * here, we know that the CE of a DFF has already been filled, so the
 * condition expression goes to the select of an asynchronous mux.
 */
bool NetCondit::synth_async(Design*des, NetScope*scope, bool sync_flag,
			    struct sync_accounting_cell*nex_ff,
			    NetNet*nex_map, NetNet*nex_out,
			    NetNet*accum)
{
      DEBUG_SYNTH2_ENTRY("NetCondit")
      NetNet*ssig = expr_->synthesize(des);
      assert(ssig);

	/* Use the accumulated input net as a default input for
	   covering a missing clause, except that if I find portions
	   are unconnected, then give up on that idea. */
      NetNet*default_sig = accum;
      for (unsigned idx = 0 ;  idx < default_sig->pin_count() ;  idx += 1) {
	    if (! default_sig->pin(idx).is_linked()) {
		  default_sig = 0;
		  break;
	    }
      }

	// At least one of the clauses must have contents. */
      assert(if_ != 0 || else_ != 0);

	/* If there is no default_sig, and if this is a fully
	   asynchronous process (nex_map is not a synchronous output)
	   then both clases *must* be present. 

	   If either clause is missing, and the output is synchronous,
	   then the code below can take as the input the output from
	   the DFF without worry for asynchronous cycles. */
      if (default_sig == 0 && ! sync_flag) {
	    if (if_ == 0) {
		  cerr << get_line() << ": error: Asynchronous if statement"
		       << " is missing the if clause." << endl;
		  des->errors += 1;
		  return false;
	    }

	    if (else_ == 0) {
		  cerr << get_line() << ": error: Asynchronous if statement"
		       << " is missing the else clause." << endl;
		  des->errors += 1;
		  return false;
	    }
      }

      NetNet*asig = new NetNet(scope, scope->local_symbol(),
			       NetNet::WIRE, nex_map->pin_count());
      asig->local_flag(true);

      if (if_ == 0) {
	      /* If the if clause is missing, then take the clause to
		 be an assignment from the defaults input. If there is
		 no defaults input, then take the input to be from the
		 output. */
	    if (default_sig) {
		  for (unsigned idx = 0 ;  idx < asig->pin_count() ;  idx += 1)
			connect(asig->pin(idx), default_sig->pin(idx));
	    } else {
		  for (unsigned idx = 0 ;  idx < asig->pin_count() ;  idx += 1)
			connect(asig->pin(idx), nex_map->pin(idx));
	    }

      } else {
	    bool flag = if_->synth_async(des, scope, sync_flag, nex_ff,
					 nex_map, asig, accum);
	    if (!flag) {
		  delete asig;
		  cerr << get_line() << ": error: Asynchronous if statement"
		       << " true clause failed to synthesize." << endl;
		  des->errors += 1;
		  return false;
	    }
      }

      NetNet*bsig = new NetNet(scope, scope->local_symbol(),
			       NetNet::WIRE, nex_map->pin_count());
      bsig->local_flag(true);

      if (else_ == 0) {
	    if (default_sig) {
		  for (unsigned idx = 0 ;  idx < asig->pin_count() ;  idx += 1)
			connect(bsig->pin(idx), default_sig->pin(idx));
	    } else {
		  for (unsigned idx = 0 ;  idx < asig->pin_count() ;  idx += 1)
			connect(bsig->pin(idx), nex_map->pin(idx));
	    }
      } else {
	    bool flag = else_->synth_async(des, scope, sync_flag, nex_ff,
					   nex_map, bsig, accum);
	    if (!flag) {
		  delete asig;
		  delete bsig;
		  cerr << get_line() << ": error: Asynchronous if statement"
		       << " else clause failed to synthesize." << endl;
		  des->errors += 1;
		  return false;
	    }
      }

      unsigned mux_width = 0;

	/* Figure out how many mux bits we are going to need. */
      for (unsigned idx = 0 ;  idx < nex_out->pin_count();  idx += 1) {
	    int flag = 0;
	    if (asig->pin(idx).is_linked())
		  flag |= 0100;
	    if (bsig->pin(idx).is_linked())
		  flag |= 0010;
	    if (accum->pin(idx).is_linked())
		  flag |= 0001;
	    switch (flag) {
		case 0111:
		case 0110:
		case 0101:
		  mux_width += 1;
		  break;
		case 0100:
		  if (sync_flag)
			mux_width += 1;
		  break;
		case 0011:
		  mux_width += 1;
		  break;
		case 0010:
		  if (sync_flag)
			mux_width += 1;
		  break;
		case 0001:
		  mux_width += 1;
		  break;
		case 0000:
		  break;
	    }

      }

	/* Create a mux and hook it up. */
      NetMux*mux = new NetMux(scope, scope->local_symbol(),
			      mux_width, 2, 1);
      mux->set_line(*this);

      connect(mux->pin_Sel(0), ssig->pin(0));

	/* Connected the clauses to the data inputs of the
	   condition. If there are bits unassigned by the case, then
	   bind them from the accum bits instead. If the bit is not
	   represented in the accum list, but this is a synchronous
	   output, then get the bit from the nex_map, which is the
	   output held in the DFF. */
      mux_width = 0;
      for (unsigned idx = 0 ;  idx < nex_out->pin_count() ;  idx += 1) {
	    int flag = 0;
	    if (asig->pin(idx).is_linked())
		  flag |= 0100;
	    if (bsig->pin(idx).is_linked())
		  flag |= 0010;
	    if (accum->pin(idx).is_linked())
		  flag |= 0001;
	    switch (flag) {
		case 0111:
		case 0110:
		  connect(mux->pin_Data(mux_width, 1), asig->pin(idx));
		  connect(mux->pin_Data(mux_width, 0), bsig->pin(idx));
		  connect(nex_out->pin(idx), mux->pin_Result(mux_width));
		  mux_width += 1;
		  break;
		case 0101:
		  connect(mux->pin_Data(mux_width, 1), asig->pin(idx));
		  connect(mux->pin_Data(mux_width, 0), accum->pin(idx));
		  connect(nex_out->pin(idx), mux->pin_Result(mux_width));
		  mux_width += 1;
		  break;
		case 0100:
		  if (sync_flag) {
			connect(mux->pin_Data(mux_width, 1), asig->pin(idx));
			connect(mux->pin_Data(mux_width, 0),nex_map->pin(idx));
			connect(nex_out->pin(idx), mux->pin_Result(mux_width));
			mux_width += 1;
		  } else {
#if 0
			cerr << get_line()
			     << ": error: Condition false clause "
			     << "does not assign expected outputs." << endl;
			des->errors += 1;
			return_flag = false;
#else
			  /* This should check that bsig is latched by
			     the condition select or is used
			     internally by the false clause. but since
			     there is no latch support, assume it is
			     used internally. */
			connect(nex_out->pin(idx), asig->pin(idx));
#endif
		  }
		  break;
		case 0011:
		  connect(mux->pin_Data(mux_width, 1), accum->pin(idx));
		  connect(mux->pin_Data(mux_width, 0), bsig->pin(idx));
		  connect(nex_out->pin(idx), mux->pin_Result(mux_width));
		  mux_width += 1;
		  break;
		case 0010:
		  if (sync_flag) {
			connect(mux->pin_Data(mux_width, 1),nex_map->pin(idx));
			connect(mux->pin_Data(mux_width, 0), bsig->pin(idx));
			connect(nex_out->pin(idx), mux->pin_Result(mux_width));
			mux_width += 1;
		  } else {
#if 0
			cerr << get_line()
			     << ": error: Condition true clause "
			     << "does not assign expected outputs." << endl;
			des->errors += 1;
			return_flag = false;
#else
			  /* This should check that bsig is latched by
			     the condition select or is used
			     internally by the false clause. but since
			     there is no latch support, assume it is
			     used internally. */
			connect(nex_out->pin(idx), bsig->pin(idx));
#endif
		  }
		  break;
		case 0001:
		  connect(mux->pin_Data(mux_width, 1), accum->pin(idx));
		  connect(mux->pin_Data(mux_width, 0), accum->pin(idx));
		  connect(nex_out->pin(idx), mux->pin_Result(mux_width));
		  mux_width += 1;
		  break;
		case 0000:
		  if (sync_flag) {
			connect(nex_out->pin(idx), nex_map->pin(idx));
		  } else {
			cerr << get_line() << ": internal error: "
			     << "Unexplained mode?" << endl;
			cerr << get_line() << ": XXXX: "
			     << "nex_out->pin_count() = "
			     << nex_out->pin_count() << endl;
			assert(0);
		  }
		  break;
		default:
		  assert(0);
		  break;
	    }
      }

      assert(mux_width == mux->width());


      des->add_node(mux);

      return true;
}

bool NetEvWait::synth_async(Design*des, NetScope*scope, bool sync_flag,
			    sync_accounting_cell*nex_ff,
			    NetNet*nex_map, NetNet*nex_out, NetNet*accum_in)
{
      bool flag = statement_->synth_async(des, scope, sync_flag, nex_ff,
					  nex_map, nex_out, accum_in);
      return flag;
}

bool NetProcTop::synth_async(Design*des)
{
      NexusSet nex_set;
      statement_->nex_output(nex_set);

      const perm_string tmp1 = perm_string::literal("tmp");
      NetNet*nex_out = new NetNet(scope(), tmp1, NetNet::WIRE,
				  nex_set.count());
      for (unsigned idx = 0 ;  idx < nex_out->pin_count() ;  idx += 1)
	    connect(nex_set[idx], nex_out->pin(idx));

      bool flag = statement_->synth_async_noaccum(des, scope(), false, 0,
						  nex_out, nex_out);

      delete nex_out;
      return flag;
}

static bool merge_ff_slices(NetFF*ff1, unsigned idx1,
			    NetFF*ff2, unsigned idx2)
{

	/* If the Aset inputs are connected, and not to each other
	   (possible since pre-existing Asets are carried forwards)
	   then there is a conflict. */
      if (ff1->pin_Aset().is_linked()
	  && ff2->pin_Aset().is_linked()
	  && ! ff1->pin_Aset().is_linked(ff2->pin_Aset())) {
	    cerr << ff2->get_line() << ": error: "
		 << "DFF Aset conflicts with "
		 << ff1->get_line() << "." << endl;
	    return false;
      }

      if (ff1->pin_Aclr().is_linked()
	  && ff2->pin_Aclr().is_linked()
	  && ! ff1->pin_Aclr().is_linked(ff2->pin_Aclr())) {
	    cerr << ff2->get_line() << ": error: "
		 << "DFF Aclr conflicts with "
		 << ff1->get_line() << "." << endl;
	    return false;
      }

#if XXXX
      if (ff2->pin_Data(idx2).is_linked())
	    connect(ff1->pin_Data(idx1), ff2->pin_Data(idx2));
      if (ff2->pin_Aset().is_linked())
	    connect(ff1->pin_Aset(), ff2->pin_Aset());
      if (ff2->pin_Aclr().is_linked())
	    connect(ff1->pin_Aclr(), ff2->pin_Aclr());
      if (ff2->pin_Sclr().is_linked())
	    connect(ff1->pin_Sclr(),  ff2->pin_Sclr());
      if (ff2->pin_Sset().is_linked())
	    connect(ff1->pin_Sset(),  ff2->pin_Sset());
      if (ff2->pin_Clock().is_linked())
	    connect(ff1->pin_Clock(), ff2->pin_Clock());
#endif
      if (ff2->pin_Enable().is_linked())
	    connect(ff1->pin_Enable(),ff2->pin_Enable());

      return true;
}

bool NetAssignBase::synth_sync(Design*des, NetScope*scope,
			       struct sync_accounting_cell*nex_ff,
			       NetNet*nex_map, NetNet*nex_out,
			       const svector<NetEvProbe*>&events)
{
      unsigned count_lval = 0;
      NetAssign_*demux = 0;

      for (NetAssign_*cur = lval_ ;  cur ;  cur = cur->more) {
	    if (cur->bmux())
		  demux = cur;
	    if (cur->mem())
		  demux = cur;

	    count_lval += 1;
      }

      if (demux != 0 && count_lval != 1) {
	    cerr << get_line() << ": error: Cannot synthesize assignmnents"
		 << "that mix memory and vector assignments." << endl;
	    return false;
      }

	/* There is no memory address, so resort to async
	   assignments. */
      if (demux == 0) {
	      /* Synthesize the input to the DFF. */
	    return synth_async_noaccum(des, scope, true, nex_ff,
				       nex_map, nex_out);
      }

      assert(demux->bmux() != 0);

	/* Obviously, we need the r-value synthesized to connect it up. */
      NetNet*rsig = rval_->synthesize(des);
      assert(rsig->pin_count() == lval_->lwidth());

	/* Detect and handle the special case that the l-value is an
	   assign to a constant bit. We don't need a demux in that
	   case. */
      if (demux->mem() && dynamic_cast<NetEConst*>(demux->bmux())) {
	    NetMemory*lmem = demux->mem();
	    NetNet*msig = lmem->explode_to_reg();
	    demux->incr_mem_lref();

	    NetEConst*ae = dynamic_cast<NetEConst*>(demux->bmux());
	    long adr = ae->value().as_long();
	    adr = lmem->index_to_address(adr) * lmem->width();

	    for (unsigned idx = 0 ;  idx < demux->lwidth() ;  idx += 1) {
		  unsigned off = adr+idx;
		  unsigned ptr = find_nexus_in_set(nex_map, msig->pin(off).nexus());
		  assert(ptr <= nex_map->pin_count());
		  connect(nex_out->pin(ptr), rsig->pin(idx));
	    }
	    lval_->turn_sig_to_wire_on_release();
	    return true;
      }

	/* We also need the address (now known to be non-constant)
	   synthesized and connected to a decoder. */
      NetNet*adr = demux->bmux()->synthesize(des);
      NetDecode*dq = new NetDecode(scope, scope->local_symbol(),
				   nex_ff[0].ff, adr->pin_count(),
				   lval_->lwidth());
      des->add_node(dq);
      dq->set_line(*this);

      for (unsigned idx = 0 ;  idx < adr->pin_count() ;  idx += 1)
	    connect(dq->pin_Address(idx), adr->pin(idx));

      for (unsigned idx = 0 ;  idx < nex_ff[0].ff->width() ;  idx += 1)
	    connect(nex_ff[0].ff->pin_Data(idx), rsig->pin(idx%lval_->lwidth()));

      if (lval_->mem()) {
	    NetNet*exp = lval_->mem()->reg_from_explode();
	    assert(exp);
	    lval_->incr_mem_lref();
      }
      lval_->turn_sig_to_wire_on_release();
      return true;
}

/*
 * This method is called when a block is encountered near the surface
 * of a synchronous always statement. For example, this code will be
 * invoked for input like this:
 *
 *     always @(posedge clk...) begin
 *           <statement1>
 *           <statement2>
 *           ...
 *     end
 *
 * This needs to be split into a DFF bank for each statement, because
 * the statements may each infer different reset and enable signals.
 */
bool NetBlock::synth_sync(Design*des, NetScope*scope,
			  struct sync_accounting_cell*nex_ff,
			  NetNet*nex_map, NetNet*nex_out,
			  const svector<NetEvProbe*>&events_in)
{
	/* Do nothing for empty blocks. */
      if (last_ == 0)
	    return true;

	/* Assert that this region still represents a single DFF. */
      for (unsigned idx = 1 ;  idx < nex_out->pin_count() ; idx += 1) {
	    assert(nex_ff[0].ff == nex_ff[idx].ff);
      }

      NetFF*ff = nex_ff[0].ff;

      assert(ff->width() == nex_out->pin_count());
      unsigned block_width = nex_out->pin_count();

      bool flag = true;

      const perm_string tmp1 = perm_string::literal("tmp1");
      const perm_string tmp2 = perm_string::literal("tmp2");

      NetProc*cur = last_;
      do {
	    cur = cur->next_;

	      /* Create a temporary nex_map for the substatement. */
	    NexusSet tmp_set;
	    cur->nex_output(tmp_set);
	    NetNet*tmp_map = new NetNet(scope, tmp1, NetNet::WIRE,
					tmp_set.count());
	    for (unsigned idx = 0 ;  idx < tmp_map->pin_count() ;  idx += 1)
		  connect(tmp_set[idx], tmp_map->pin(idx));

	      /* NOTE: After this point, tmp_set should not be used as
		 the various functions I call do a lot of connecting,
		 and the nexa in the tmp_set may get realloced. Use
		 the tmp_map instead. */

	      /* Create also a temporary net_out to collect the
		 output. The tmp1 and tmp2 map and out sets together
		 are used to collect the outputs from the substatement
		 for the inputs of the FF bank. */
	    NetNet*tmp_out = new NetNet(scope, tmp2, NetNet::WIRE,
					tmp_map->pin_count());

	    verinum tmp_aset = ff->aset_value();
	    verinum tmp_sset = ff->sset_value();

	      /* Create a new DFF to handle this part of the begin-end
		 block. Connect this NetFF to the associated pins of
		 the existing wide NetFF device. While I'm at it, also
		 copy the aset_value bits for the new ff device. */
	    NetFF*ff2 = new NetFF(scope, scope->local_symbol(),
				  tmp_out->pin_count());
	    ff2->set_line(*cur);
	    des->add_node(ff2);

	    struct sync_accounting_cell*tmp_ff
		  = new struct sync_accounting_cell[ff2->width()];

	    verinum aset_value2 (verinum::V1, ff2->width());
	    verinum sset_value2 (verinum::V1, ff2->width());
	    for (unsigned idx = 0 ;  idx < ff2->width() ;  idx += 1) {
		  unsigned ptr = find_nexus_in_set(nex_map,
						   tmp_map->pin(idx).nexus());

		    /* Copy the asynch set bit to the new device. */
		  if (ptr < tmp_aset.len())
			aset_value2.set(idx, tmp_aset[ptr]);

		    /* Copy the synch set bit to the new device. */
		  if (ptr < tmp_sset.len())
			sset_value2.set(idx, tmp_sset[ptr]);

		    // Connect the tmp_out vector, what the
		    // sub-synthesis is linking, to the inputs of this
		    // new FF.
		  connect(tmp_out->pin(idx), ff2->pin_Data(idx));
		  tmp_ff[idx].ff = ff2;
		  tmp_ff[idx].pin = idx;
		  tmp_ff[idx].proc = cur;
	    }

	      /* PUll the non-sliced inputs (clock, set, reset, etc)
		 forward to the new FF we are building. */
	    if (ff->pin_Aclr().is_linked())
		  connect(ff->pin_Aclr(),  ff2->pin_Aclr());
	    if (ff->pin_Aset().is_linked())
		  connect(ff->pin_Aset(),  ff2->pin_Aset());
	    if (ff->pin_Sclr().is_linked())
		  connect(ff->pin_Sclr(),  ff2->pin_Sclr());
	    if (ff->pin_Sset().is_linked())
		  connect(ff->pin_Sset(),  ff2->pin_Sset());
	    if (ff->pin_Clock().is_linked())
		  connect(ff->pin_Clock(), ff2->pin_Clock());
	    if (ff->pin_Enable().is_linked())
		  connect(ff->pin_Enable(),ff2->pin_Enable());

	      /* Remember to store the aset value into the new FF. If
		 this leads to an Aset value of 0 (and Aclr is not
		 otherwise used) then move the Aset input to Aclr. */
	    if (tmp_aset.len() == ff->width()) {

		  if (aset_value2.is_zero()
		      && ff2->pin_Aset().is_linked()
		      && !ff2->pin_Aclr().is_linked()) {

			ff2->pin_Aset().unlink();
			connect(ff2->pin_Aclr(), ff->pin_Aset());

		  } else {
			ff2->aset_value(aset_value2);
		  }
	    }

	    if (tmp_sset.len() == ff->width()) {

		  if (sset_value2.is_zero()
		      && ff2->pin_Sset().is_linked()
		      && !ff2->pin_Sclr().is_linked()) {

			ff2->pin_Sset().unlink();
			connect(ff2->pin_Sclr(), ff->pin_Sset());

		  } else {
			ff2->sset_value(sset_value2);
		  }
	    }

	      /* Now go on with the synchronous synthesis for this
		 statement of the block. The tmp_map is the output
		 nexa that we expect, and the tmp_out is where we want
		 those outputs connected. */
	    bool ok_flag = cur->synth_sync(des, scope,
					   tmp_ff, tmp_map, tmp_out,
					   events_in);
	    ff2 = 0; // NOTE: The synth_sync may delete ff2. 
	    flag = flag && ok_flag;

	    if (ok_flag == false)
		  continue;

	      /* Use the nex_map to link up the output from the
		 substatement to the output of the block as a
		 whole. It is occasionally possible to have outputs
		 beyond the input set, for example when the l-value of
		 an assignment is smaller then the r-value. */
	    for (unsigned idx = 0 ;  idx < tmp_out->pin_count() ; idx += 1) {
		  ff2 = tmp_ff[idx].ff;
		  unsigned ff2_pin = tmp_ff[idx].pin;
		  unsigned ptr = find_nexus_in_set(nex_map,
					   tmp_map->pin(idx).nexus());

		  if (ptr >= nex_out->pin_count())
			continue;

		    // This is the current DFF for this bit slice...
		  NetFF*ff1 = nex_ff[ptr].ff;
		  unsigned ff1_pin = nex_ff[ptr].pin;

		    // Connect the new NetFF to the baseline
		    // NetFF Q and D pins.
		  connect(ff1->pin_Data(ff1_pin), ff2->pin_Data(ff2_pin));
		  connect(ff1->pin_Q(ff1_pin), ff2->pin_Q(ff2_pin));
		    // Merge the new ff2 with the current ff1. This
		    // brings all the non-sliced bits forward from
		    // ff1, as well as any other merging needed.
		  merge_ff_slices(ff2, ff2_pin, ff1, ff1_pin);

		    // Save the bit slice map as the new referece.
		  nex_ff[ptr] = tmp_ff[idx];

		    // If the (old) current DFF is not the same as the
		    // baseline DFF, then it is possible that the
		    // slice update rendered ff1 obsolete. If so, it
		    // will no longer appear in the nex_ff map, so
		    // remove it.
		  if (ff1 != ff) {
			bool found_flag = false;
			for (unsigned scan=0
				   ; scan < ff->width() && !found_flag
				   ; scan += 1) {
			      if (nex_ff[scan].ff == ff1)
				    found_flag = true;
			}
			if (! found_flag) {
			      delete ff1;
			}
		  }
	    }

	    delete tmp_map;
	    delete tmp_out;
	    delete tmp_ff;

      } while (cur != last_);


      if (flag == false)
	    return false;

	/* Done. The large NetFF is no longer needed, as it has been
	   taken up by the smaller NetFF devices. */
      delete ff;
      ff = 0;

	/* Run through the pin accounting one more time to make sure
	   the data inputs are all connected. */
      for (unsigned idx = 0 ;  idx < block_width ;  idx += 1) {
	    if (nex_ff[idx].proc == 0)
		  continue;

	    NetFF*ff2 = nex_ff[idx].ff;
	    unsigned pin = nex_ff[idx].pin;
	      /* Skip this output if it is not handled in this block. */
	    if (ff2 == 0)
		  continue;

	    if (pin >= ff2->width()) {
		  cerr << ff2->get_line() << ": internal error: "
		       << "pin " << pin << " out of range of "
		       << ff2->width() << " bit DFF." << endl;
		  flag = false;
		  des->errors += 1;

	      /* If this block mentioned it, then the data must have
		 been set here. */
	    } else if (!ff2->pin_Data(pin).is_linked()) {
		  cerr << ff2->get_line() << ": error: "
		       << "DFF introduced here is missing Data "
		       << pin << " input." << endl;
		  flag = false;
		  des->errors += 1;
	    }
      }

      return flag;
}

static bool test_ff_set_clr(struct sync_accounting_cell*nex_ff, unsigned bits)
{
      for (unsigned idx = 0 ;  idx < bits ;  idx += 1) {
	    NetFF*ff = nex_ff[idx].ff;
	    if (ff->pin_Sset().is_linked())
		  return true;
	    if (ff->pin_Sclr().is_linked())
		  return true;
      }

      return false;
}

int NetCondit::connect_set_clr_range_(struct sync_accounting_cell*nex_ff,
				      unsigned bits, NetNet*rst,
				      const verinum&val)
{
	/* Oops, this is not a constant, presumably because the case
	   is not fully connected. In this case, we need to fall back
	   on more general synthesis. */
      if (! val.is_defined()) {
	    if (debug_synth)
		  cerr << get_line() << ": debug: "
		       << "Give up on set/clr synthesis, since "
		       << "r-value = " << val << endl;
	    return -1;
      }

      assert(val.is_defined());

      unsigned base = 0;
      unsigned wid = nex_ff[0].ff->width();
      while (base < bits) {
	    NetFF*ff = nex_ff[base].ff;
	    assert(base+wid <= bits);

	    verinum tmp(0UL, wid);
	    for (unsigned idx = 0 ;  idx < wid ;  idx += 1) {
		  assert(nex_ff[base+idx].ff == ff);
		  tmp.set(idx, val.get(base+idx));
	    }

	    if (tmp.is_zero()) {
		  connect(ff->pin_Sclr(), rst->pin(0));

	    } else {
		  connect(ff->pin_Sset(), rst->pin(0));
		  ff->sset_value(tmp);
	    }
	    if (debug_synth)
		  cerr << get_line() << ": debug: "
		       << "Create a synchronous set for "
		       << ff->width() << " bit ff." << endl;

	    base += wid;
      }

      return 0;
}

int NetCondit::connect_enable_range_(Design*des, NetScope*scope,
				     struct sync_accounting_cell*nex_ff,
				     unsigned bits, NetNet*ce)
{
      unsigned base = 0;
      unsigned wid = nex_ff[0].ff->width();
      while (base < bits) {
	    NetFF*ff = nex_ff[base].ff;
	    assert(base+wid <= bits);

	    for (unsigned idx = 0 ;  idx < wid ;  idx += 1) {
		  assert(nex_ff[base+idx].ff == ff);
	    }

	      /* Watch out for the special case that there is already
		 a CE connected to this FF. This can be caused by code
		 like this: 

		 if (a) if (b) <statement>;

		 In this case, we are working on the inner IF, so we
		 AND the a and b expressions to make a new CE. */

	    if (ff->pin_Enable().is_linked()) {
		  NetLogic*ce_and = new NetLogic(scope,
						 scope->local_symbol(), 3,
						 NetLogic::AND);
		  des->add_node(ce_and);
		  connect(ff->pin_Enable(), ce_and->pin(1));
		  connect(ce->pin(0), ce_and->pin(2));

		  ff->pin_Enable().unlink();
		  connect(ff->pin_Enable(), ce_and->pin(0));

		  NetNet*tmp = new NetNet(scope, scope->local_symbol(),
					  NetNet::IMPLICIT, 1);
		  tmp->local_flag(true);
		  connect(ff->pin_Enable(), tmp->pin(0));

	    } else {

		  connect(ff->pin_Enable(), ce->pin(0));
	    }

	    base += wid;
      }

      return 0;
}

/*
 * This method handles the case where I find a conditional near the
 * surface of a synchronous thread. This conditional can be a CE or an
 * asynchronous set/reset, depending on whether the pin of the
 * expression is connected to an event, or not.
 */
bool NetCondit::synth_sync(Design*des, NetScope*scope,
			   struct sync_accounting_cell*nex_ff,
			   NetNet*nex_map, NetNet*nex_out,
			   const svector<NetEvProbe*>&events_in)
{
	/* First try to turn the condition expression into an
	   asynchronous set/reset. If the condition expression has
	   inputs that are included in the sensitivity list, then it
	   is likely intended as an asynchronous input. */

      NexusSet*expr_input = expr_->nex_input();
      assert(expr_input);
      for (unsigned idx = 0 ;  idx < events_in.count() ;  idx += 1) {


	    NetEvProbe*ev = events_in[idx];
	    NexusSet pin_set;
	    pin_set.add(ev->pin(0).nexus());

	    if (! expr_input->contains(pin_set))
		  continue;

	      /* If we are taking this to be an asynchronous
		 set/clear, then *all* the condition expression inputs
		 must be asynchronous. Check that here. */
	    if (! pin_set.contains(*expr_input)) {

		  NexusSet tmp_set;
		  tmp_set.add(ev->pin(0).nexus());
		  for (unsigned tmp = idx+1; tmp<events_in.count(); tmp += 1) {
			NetEvProbe*ev_tmp = events_in[tmp];
			tmp_set.add(ev_tmp->pin(0).nexus());
		  }

		  if (! tmp_set.contains(*expr_input)) {
			cerr << get_line() << ": error: Condition expression "
			     << "mixes synchronous and asynchronous "
			     << "inputs." << endl;
			des->errors += 1;
		  }
	    }

	      /* Ah, this edge is in the sensitivity list for the
		 expression, so we have an asynchronous
		 input. Synthesize the set/reset input expression. */

	    NetNet*rst = expr_->synthesize(des);
	    assert(rst->pin_count() == 1);

	      /* WARNING: This case relies on the ff vector being a
		 single NetFF. */
	    for (unsigned bit = 1 ;  bit < nex_out->pin_count() ; bit += 1) {
		  assert(nex_ff[0].ff == nex_ff[bit].ff);
	    }
	    NetFF*ff = nex_ff[0].ff;

	      /* XXXX I really should find a way to check that the
		 edge used on the reset input is correct. This would
		 involve interpreting the exression that is fed by the
		 reset expression. */
	      //assert(ev->edge() == NetEvProbe::POSEDGE);

	      /* Synthesize the true clause to figure out what
		 kind of set/reset we have. */
	    NetNet*asig = new NetNet(scope, scope->local_symbol(),
				     NetNet::WIRE, nex_map->pin_count());
	    asig->local_flag(true);

	    assert(if_ != 0);
	    bool flag = if_->synth_async_noaccum(des, scope, true, nex_ff,
						 nex_map, asig);

	    assert(asig->pin_count() == nex_map->pin_count());

	      /* Collect the set/reset value into a verinum. If
		 this turns out to be entirely 0 values, then
		 use the Aclr input. Otherwise, use the Aset
		 input and save the set value. */
	    verinum tmp (verinum::V0, ff->width());
	    unsigned count_z = 0;
	    unsigned count_x = 0;
	    for (unsigned bit = 0 ;  bit < ff->width() ;  bit += 1) {

		  assert(asig->pin(bit).nexus()->drivers_constant());
		  verinum::V val = asig->pin(bit).nexus()->driven_value();
		  tmp.set(bit, val);

		  switch (val) {
		      case verinum::V0:
		      case verinum::V1:
			break;
		      case verinum::Vz:
			count_z += 1;
			break;
		      case verinum::Vx:
			count_x += 1;
			break;
		  }
	    }

	    if (count_x > 0) {
		  cerr << get_line() << ": internal error: "
		       << "True clause returns constant 'bx values"
		       << " which are not plausible for set/reset." << endl;
		  cerr << get_line() << ":               : "
		       << "XXXX val = " << tmp << endl;
		  return false;
	    }

	    if (count_z > 0) {
#if 0
		  cerr << get_line() << ": internal error: "
		       << "True clause returns constant 'bz values"
		       << " which are not plausible for set/reset." << endl;
		  cerr << get_line() << ":               : "
		       << "XXXX val = " << tmp << endl;
		  return false;
#endif
		  assert(count_z < ff->width());
		    /* Some bits are not reset by this input. To make
		       this work, split the current ff into a pair of
		       FF, one connected to the reset, and the other
		       not. */
		  NetFF*ff1 = new NetFF(scope, ff->name(),
					ff->width() - count_z);
		  NetFF*ffz = new NetFF(scope, scope->local_symbol(),
					count_z);
		  des->add_node(ff1);
		  des->add_node(ffz);
		  connect(ff->pin_Clock(), ff1->pin_Clock());
		  connect(ff->pin_Clock(), ffz->pin_Clock());
		  verinum tmp1(0UL, ff1->width());
		  unsigned bit1 = 0;
		  unsigned bitz = 0;

		  for (unsigned bit = 0 ;  bit < ff->width() ;  bit += 1) {

			if (tmp.get(bit) == verinum::Vz) {
			      connect(ffz->pin_Q(bitz), ff->pin_Q(bit));
			      connect(ffz->pin_Data(bitz), ff->pin_Data(bit));
			      nex_ff[bit].ff = ffz;
			      nex_ff[bit].pin = bitz;
			      bitz += 1;

			} else {
			      connect(ff1->pin_Q(bit1), ff->pin_Q(bit));
			      connect(ff1->pin_Data(bit1), ff->pin_Data(bit));
			      nex_ff[bit].ff = ff1;
			      nex_ff[bit].pin = bit1;
			      tmp1.set(bit1, tmp.get(bit));
			      bit1 += 1;
			}
		  }

		  delete ff;
		  ff = ff1;
		  tmp = tmp1;
	    }

	    assert(tmp.is_defined());
	    if (tmp.is_zero()) {
		  connect(ff->pin_Aclr(), rst->pin(0));

	    } else {
		  connect(ff->pin_Aset(), rst->pin(0));
		  ff->aset_value(tmp);
	    }

	    delete asig;
	    delete expr_input;

	    if (else_ == 0) {

		    /* The lack of an else_ clause here means that
		       there is no data input to the DFF yet
		       defined. This is bad, but the data input may be
		       given later in an enclosing block, so don't
		       report an error here quite yet. */
		  return true;
	    }

	    assert(else_ != 0);

	      /* Create a new NetEvProbe list that does not include
		 the current probe that we've absorbed into this
		 input. */
	    assert(events_in.count() >= 1);
	    svector<NetEvProbe*> events_tmp (events_in.count() - 1);
	    unsigned count_events = 0;
	    for (unsigned tmp = 0 ;  tmp < events_in.count() ;  tmp += 1) {
		  if (tmp == idx) continue;
		  events_tmp[count_events++] = events_in[tmp];
	    }

	    flag = flag && else_->synth_sync(des, scope,
					     nex_ff, nex_map,
					     nex_out, events_tmp);

	    return flag;
      }

      delete expr_input;

	/* Detect the case that this is a *synchronous* set/reset. It
	   is not asyncronous because we know the condition is not
	   included in the sensitivity list, but if the if_ case is
	   constant (has no inputs) then we can model this as a
	   synchronous set/reset.

	   This is only synchronous set/reset if there is a true and a
	   false clause, and no inputs. The "no inputs" requirement is
	   met if the assignments are of all constant values.

	   Also, we will not allow both Sset and Sclr to be used on a
	   single LPM_FF (due to unclear priority issues) so don't try
	   if either are already connected.

	   XXXX This should be disabled if there is a memory involved
	   in any sub-statements? */
      assert(if_ != 0);
      NexusSet*a_set = if_->nex_input();

      if ((a_set->count() == 0)
	  && if_ && else_
	  && !test_ff_set_clr(nex_ff, nex_map->pin_count())) {

	    NetNet*rst = expr_->synthesize(des);
	    assert(rst->pin_count() == 1);

	      /* Synthesize the true clause to figure out what
		 kind of set/reset we have. */
	    NetNet*asig = new NetNet(scope, scope->local_symbol(),
				     NetNet::WIRE, nex_map->pin_count());
	    asig->local_flag(true);
	    bool flag = if_->synth_async_noaccum(des, scope, true, nex_ff,
						 nex_map, asig);

	    if (!flag) {
		  /* This path leads nowhere */
		  delete asig;
	    } else do {
		  assert(asig->pin_count() == nex_map->pin_count());

		  unsigned nbits = nex_map->pin_count();

		    /* Collect the set/reset value into a verinum. If
		       this turns out to be entirely 0 values, then
		       use the Sclr input. Otherwise, use the Aset
		       input and save the set value. */
		  verinum tmp (verinum::V0, nbits);
		  for (unsigned bit = 0; bit< nbits; bit += 1) {

			assert(asig->pin(bit).nexus()->drivers_constant());
			tmp.set(bit, asig->pin(bit).nexus()->driven_value());
		  }

		  if (connect_set_clr_range_(nex_ff, nbits, rst, tmp) < 0) {
			delete asig;
			break;
		  }


		  delete a_set;

		  assert(else_ != 0);
		  flag = else_->synth_sync(des, scope,
					   nex_ff, nex_map, nex_out,
					   svector<NetEvProbe*>(0))
			&& flag;
		  return flag;
	    } while (0);
      }

      delete a_set;

	/* Failed to find an asynchronous set/reset, so any events
	   input are probably in error, or simply not in use. */


	/* If this is an if/then/else, then it is likely a
	   combinational if, and I should synthesize it that way. */
      if (if_ && else_) {
	    bool flag =synth_async_noaccum(des, scope, true, nex_ff,
					   nex_map, nex_out);
	    return flag;
      }

      assert(if_);
      assert(!else_);

	/* Synthesize the enable expression. */
      NetNet*ce = expr_->synthesize(des);
      assert(ce->pin_count() == 1);

	/* What's left, is a synchronous CE statement like this:

	     if (expr_) <true statement>;

	   The expr_ expression has already been synthesized to the ce
	   net, so we connect it here to the FF. What's left is to
	   synthesize the substatement as a combinational
	   statement. */

      unsigned nbits = nex_map->pin_count();
      connect_enable_range_(des, scope, nex_ff, nbits, ce);

      bool flag = if_->synth_sync(des, scope,
				  nex_ff, nex_map, nex_out,
				  events_in);

      return flag;
}

bool NetEvWait::synth_sync(Design*des, NetScope*scope,
			   struct sync_accounting_cell*nex_ff,
			   NetNet*nex_map, NetNet*nex_out,
			   const svector<NetEvProbe*>&events_in)
{
      DEBUG_SYNTH2_ENTRY("NetEvWait")
      if (events_in.count() > 0) {
	    cerr << get_line() << ": error: Events are unaccounted"
		 << " for in process synthesis. (evw)" << endl;
	    des->errors += 1;
      }

      assert(events_in.count() == 0);

	/* This can't be other then one unless there are named events,
	   which I cannot synthesize. */
      assert(nevents_ == 1);
      NetEvent*ev = events_[0];

      assert(ev->nprobe() >= 1);
      svector<NetEvProbe*>events (ev->nprobe() - 1);

	/* Get the input set from the substatement. This will be used
	   to figure out which of the probes in the clock. */
      NexusSet*statement_input = statement_ -> nex_input();

	/* Search for a clock input. The clock input is the edge event
	   that is not also an input to the substatement. */
      NetEvProbe*pclk = 0;
      unsigned event_idx = 0;
      for (unsigned idx = 0 ;  idx < ev->nprobe() ;  idx += 1) {
	    NetEvProbe*tmp = ev->probe(idx);
	    assert(tmp->pin_count() == 1);

	    NexusSet tmp_nex;
	    tmp_nex .add( tmp->pin(0).nexus() );

	    if (! statement_input ->contains(tmp_nex)) {
		  if (pclk != 0) {
			cerr << get_line() << ": error: Too many "
			     << "clocks for synchronous logic." << endl;
			cerr << get_line() << ":      : Perhaps an"
			     << " asynchronous set/reset is misused?" << endl;
			des->errors += 1;
		  }
		  pclk = tmp;

	    } else {
		  events[event_idx++] = tmp;
	    }
      }

      if (pclk == 0) {
	    cerr << get_line() << ": error: None of the edges"
		 << " are valid clock inputs." << endl;
	    cerr << get_line() << ":      : Perhaps the clock"
		 << " is read by a statement or expression?" << endl;
	    des->errors += 1;
	    DEBUG_SYNTH2_EXIT("NetEvWait",false)
	    return false;
      }

      unsigned base = 0;
      while (base < nex_map->pin_count()) {
	    unsigned wid = nex_ff[base].ff->width();
	    assert((base + wid) <= nex_map->pin_count());

	    NetFF*ff = nex_ff[base].ff;
	    connect(ff->pin_Clock(), pclk->pin(0));
	    if (pclk->edge() == NetEvProbe::NEGEDGE)
		  ff->attribute(perm_string::literal("ivl:clock_polarity"),
				verinum("INVERT"));

	    base += wid;
      }
      assert(base == nex_map->pin_count());

	/* Synthesize the input to the DFF. */
      bool flag = statement_->synth_sync(des, scope, nex_ff,
					 nex_map, nex_out, events);

      DEBUG_SYNTH2_EXIT("NetEvWait",flag)
      return flag;
}

bool NetWhile::synth_async(Design*des, NetScope*scope, bool sync_flag,
			   struct sync_accounting_cell*nex_ff,
			   NetNet*nex_map, NetNet*nex_out, NetNet*accum_in)
{
      cerr << get_line()
	   << ": error: Cannot synthesize for or while loops."
	   << endl;
      des->errors += 1;
      return false;
}

bool NetProcTop::synth_sync(Design*des)
{
      NexusSet nex_set;
      statement_->nex_output(nex_set);

      NetFF*ff = new NetFF(scope(), scope()->local_symbol(),
			   nex_set.count());
      des->add_node(ff);
      ff->attribute(perm_string::literal("LPM_FFType"), verinum("DFF"));
      unsigned process_pin_count = ff->width();

      struct sync_accounting_cell*nex_ff
	    = new struct sync_accounting_cell[ff->pin_count()];
      for (unsigned idx = 0 ;  idx < process_pin_count ;  idx += 1) {
	    nex_ff[idx].ff = ff;
	    nex_ff[idx].pin = idx;
	    nex_ff[idx].proc = statement_;
      }

	/* The D inputs to the DFF device will receive the output from
	   the statements of the process. */
      NetNet*nex_d = new NetNet(scope(), scope()->local_symbol(),
				NetNet::WIRE, nex_set.count());
      nex_d->local_flag(true);
      for (unsigned idx = 0 ;  idx < nex_set.count() ;  idx += 1) {
	    connect(nex_d->pin(idx), ff->pin_Data(idx));
      }

	/* The Q outputs of the DFF will connect to the actual outputs
	   of the process. Thus, the DFF will be between the outputs
	   of the process and the outputs of the substatement. */
      const perm_string tmpq = perm_string::literal("tmpq");
      NetNet*nex_q = new NetNet(scope(), tmpq, NetNet::WIRE,
				nex_set.count());
      for (unsigned idx = 0 ;  idx < nex_set.count() ;  idx += 1) {
	    connect(nex_set[idx], nex_q->pin(idx));
	    connect(nex_q->pin(idx), ff->pin_Q(idx));
      }

	/* Synthesize the input to the DFF. */
      bool flag = statement_->synth_sync(des, scope(),
					 nex_ff,
					 nex_q, nex_d,
					 svector<NetEvProbe*>());


      delete nex_q;
      delete[]nex_ff;

      return flag;
}

class synth2_f  : public functor_t {

    public:
      void process(class Design*, class NetProcTop*);

    private:
};


/*
 * Look at a process. If it is asynchronous, then synthesize it as an
 * asynchronous process and delete the process itself for its gates.
 */
void synth2_f::process(class Design*des, class NetProcTop*top)
{
      if (top->attribute(perm_string::literal("ivl_synthesis_off")).as_ulong() != 0)
	    return;

	/* If the scope that contains this process as a cell attribute
	   attached to it, then skip synthesis. */
      if (top->scope()->attribute(perm_string::literal("ivl_synthesis_cell")).len() > 0)
	    return;

      if (top->is_synchronous()) do {
	    bool flag = top->synth_sync(des);
	    if (! flag) {
		  cerr << top->get_line() << ": error: "
		       << "Unable to synthesize synchronous process." << endl;
		  des->errors += 1;
		  return;
	    }
	    des->delete_process(top);
	    return;
      } while (0);

      if (! top->is_asynchronous()) {
	    bool synth_error_flag = false;
	    if (top->attribute(perm_string::literal("ivl_combinational")).as_ulong() != 0) {
		  cerr << top->get_line() << ": error: "
		       << "Process is marked combinational,"
		       << " but isn't really." << endl;
		  des->errors += 1;
		  synth_error_flag = true;
	    }

	    if (top->attribute(perm_string::literal("ivl_synthesis_on")).as_ulong() != 0) {
		  cerr << top->get_line() << ": error: "
		       << "Process is marked for synthesis,"
		       << " but I can't do it." << endl;
		  des->errors += 1;
		  synth_error_flag = true;
	    }

	    if (! synth_error_flag) {
		  cerr << top->get_line() << ": warning: "
		       << "Process not synthesized." << endl;
	    }
	    return;
      }

      if (! top->synth_async(des)) {
	    cerr << top->get_line() << ": error: "
		 << "Asynchronous process cannot be synthesized." << endl;
	    des->errors += 1;
	    return;
      }

      des->delete_process(top);
}

void synth2(Design*des)
{
      debug_synth2 = atoi(des->get_flag("ivl-synth2-debug"));
      synth2_f synth_obj;
      des->functor(&synth_obj);
}

/*
 * $Log: synth2.cc,v $
 * Revision 1.39.2.38  2006/07/02 00:50:15  steve
 *  Properly synthesize casex statements.
 *
 * Revision 1.39.2.37  2006/06/26 00:05:46  steve
 *  Handle case where case output appears to be internal.
 *
 * Revision 1.39.2.36  2006/06/23 03:49:47  steve
 *  synthesis of NetCondit handles partial resets.
 *
 * Revision 1.39.2.35  2006/06/07 03:17:39  steve
 *  Fix partial use of NetMux in sync condit statements.
 *
 * Revision 1.39.2.34  2006/06/02 23:42:48  steve
 *  Compilation warnings.
 *
 * Revision 1.39.2.33  2006/06/01 03:01:48  steve
 *  Handle condit clauses with unassigned outputs.
 *
 * Revision 1.39.2.32  2006/05/20 16:06:48  steve
 *  Replace assertions with error messages.
 *
 * Revision 1.39.2.31  2006/05/18 01:47:12  steve
 *  Fix synthesis of l-value bit select in block.
 *
 * Revision 1.39.2.30  2006/05/05 01:56:36  steve
 *  Handle memory assignments out of range during synthesis
 *
 * Revision 1.39.2.29  2006/04/16 19:26:39  steve
 *  Fix handling of exploded memories with partial or missing resets.
 *
 * Revision 1.39.2.28  2006/04/10 03:43:40  steve
 *  Exploded memories accessed by constant indices.
 *
 * Revision 1.39.2.27  2006/04/01 01:37:58  steve
 *  Punt on set/reset if some sources are unconnected.
 *
 * Revision 1.39.2.26  2006/03/26 23:09:24  steve
 *  Handle asynchronous demux/bit replacements.
 *
 * Revision 1.39.2.25  2006/03/18 18:43:22  steve
 *  Better error messages when synthesis fails.
 *
 * Revision 1.39.2.24  2006/03/16 05:39:43  steve
 *  Fix a spelling error in an error message.
 *
 * Revision 1.39.2.23  2006/03/12 07:34:18  steve
 *  Fix the memsynth1 case.
 *
 * Revision 1.39.2.22  2006/02/25 05:03:29  steve
 *  Add support for negedge FFs by using attributes.
 *
 * Revision 1.39.2.21  2006/02/19 00:11:33  steve
 *  Handle synthesis of FF vectors with l-value decoder.
 *
 * Revision 1.39.2.20  2006/01/27 01:58:53  steve
 *  Document how the default statement handling works.
 *
 * Revision 1.39.2.19  2006/01/22 00:13:59  steve
 *  Fix pin_Sel overrun.
 *
 * Revision 1.39.2.18  2006/01/21 21:42:32  steve
 *  When mux has wide select but sparse choices, use 1hot translation.
 *
 * Revision 1.39.2.17  2006/01/18 06:16:11  steve
 *  Restrict DFF to only one of Sset and Sclr.
 *
 * Revision 1.39.2.16  2006/01/18 01:23:25  steve
 *  Rework l-value handling to allow for more l-value type flexibility.
 *
 * Revision 1.39.2.15  2006/01/01 02:25:07  steve
 *  Case statement handles partial outputs.
 *
 * Revision 1.39.2.14  2006/01/01 01:30:39  steve
 *  Allow for implicit case default in synchronous processes.
 *
 * Revision 1.39.2.13  2005/12/31 04:28:15  steve
 *  Fix crashes caused bu synthesis of sqrt32.v.
 *
 * Revision 1.39.2.12  2005/12/19 01:13:47  steve
 *  Handle DFF definitions spread out within a block.
 *
 * Revision 1.39.2.11  2005/12/15 02:38:51  steve
 *  Fix missing outputs from synchronous conditionals to get out from in.
 *
 * Revision 1.39.2.10  2005/12/14 01:53:39  steve
 *  Handle both asynchronous set and reset.
 *
 * Revision 1.39.2.9  2005/12/14 00:54:30  steve
 *  Account for sync vs async muxes.
 *
 * Revision 1.39.2.8  2005/12/10 04:26:32  steve
 *  Handle concatenations in l-values.
 *
 * Revision 1.39.2.7  2005/12/10 03:30:51  steve
 *  Fix crash on block with assignments that assign lval to self.
 *
 * Revision 1.39.2.6  2005/12/07 02:14:37  steve
 *  Error messages for missing else clauses.
 *
 * Revision 1.39.2.5  2005/11/16 00:38:26  steve
 *  Handle partial sets of conditional clauses.
 *
 * Revision 1.39.2.4  2005/11/13 22:28:48  steve
 *  Allow for block output to be set throughout the statements.
 *
 * Revision 1.39.2.3  2005/09/11 02:56:38  steve
 *  Attach line numbers to NetMux devices.
 *
 * Revision 1.39.2.2  2005/08/22 01:00:42  steve
 *  Add support for implicit defaults in case and conditions.
 *
 * Revision 1.39.2.1  2005/08/21 22:49:54  steve
 *  Handle statements in blocks overriding previous statement outputs.
 */


// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */


#include "ReplicatedPG.h"
#include "OSD.h"

#include "common/Logger.h"

#include "messages/MOSDOp.h"
#include "messages/MOSDOpReply.h"

#include "config.h"

#undef dout
#define  dout(l)    if (l<=g_conf.debug || l<=g_conf.debug_osd) cout << g_clock.now() << " osd" << osd->get_nodeid() << " " << (osd->osdmap ? osd->osdmap->get_epoch():0) << " " << *this << " "

#include <errno.h>
#include <sys/stat.h>



// =======================
// pg changes

bool ReplicatedPG::same_for_read_since(epoch_t e)
{
  return (e >= info.history.same_acker_since);
}

bool ReplicatedPG::same_for_modify_since(epoch_t e)
{
  return (e >= info.history.same_primary_since);
}

bool ReplicatedPG::same_for_rep_modify_since(epoch_t e)
{
  // check osd map: same set, or primary+acker?

  if (g_conf.osd_rep == OSD_REP_CHAIN) {
    return e >= info.history.same_since;   // whole pg set same
  } else {
    // primary, splay
    return (e >= info.history.same_primary_since &&
	    e >= info.history.same_acker_since);    
  }
}

// ====================
// missing objects

bool ReplicatedPG::is_missing_object(object_t oid)
{
  return missing.missing.count(oid);
}
 

void ReplicatedPG::wait_for_missing_object(object_t oid, MOSDOp *op)
{
  assert(is_missing_object(oid));

  // we don't have it (yet).
  eversion_t v = missing.missing[oid];
  if (objects_pulling.count(oid)) {
    dout(7) << "missing "
	    << oid 
	    << " v " << v
	    << ", already pulling"
	    << endl;
  } else {
    dout(7) << "missing " 
	    << oid 
	    << " v " << v
	    << ", pulling"
	    << endl;
    pull(oid);
  }
  waiting_for_missing_object[oid].push_back(op);
}




// ========================================================================
// READS

int ReplicatedPG::op_read(MOSDOp *op)
{
  object_t oid = op->get_oid();

  dout(10) << "op_read " << oid 
           << " " << op->get_offset() << "~" << op->get_length() 
    //<< " in " << *pg 
           << endl;

  long r = 0;
  bufferlist bl;

  if (oid.rev && !pick_object_rev(oid)) {
    // we have no revision for this request.
    r = -EEXIST;
  } else {
    // read into a buffer
    r = osd->store->read(oid, 
			 op->get_offset(), op->get_length(),
			 bl);
  }
  
  // set up reply
  MOSDOpReply *reply = new MOSDOpReply(op, 0, osd->osdmap->get_epoch(), true); 
  if (r >= 0) {
    reply->set_result(0);
    reply->set_data(bl);
    reply->set_length(r);
  } else {
    reply->set_result(r);   // error
    reply->set_length(0);
  }
  
  dout(10) << " read got " << r << " / " << op->get_length() << " bytes from obj " << oid << endl;
  
  // send it
  osd->messenger->send_message(reply, op->get_client_inst());
  
  delete op;

  return r;
}


void ReplicatedPG::op_stat(MOSDOp *op)
{
  object_t oid = op->get_oid();

  struct stat st;
  memset(&st, sizeof(st), 0);
  int r = 0;

  if (oid.rev && !pick_object_rev(oid)) {
    // we have no revision for this request.
    r = -EEXIST;
  } else {
    r = osd->store->stat(oid, &st);
  }
  
  dout(3) << "op_stat on " << oid 
          << " r = " << r
          << " size = " << st.st_size
    //<< " in " << *pg
          << endl;
  
  MOSDOpReply *reply = new MOSDOpReply(op, r, osd->osdmap->get_epoch(), true);
  reply->set_object_size(st.st_size);
  osd->messenger->send_message(reply, op->get_client_inst());
  
  delete op;
}




// ========================================================================
// MODIFY

void ReplicatedPG::prepare_log_transaction(ObjectStore::Transaction& t, 
					   MOSDOp *op, eversion_t& version, 
					   objectrev_t crev, objectrev_t rev,
					   eversion_t trim_to)
{
  const object_t oid = op->get_oid();

  // clone entry?
  if (crev && rev && rev > crev) {
    eversion_t cv = version;
    cv.version--;
    Log::Entry cloneentry(PG::Log::Entry::CLONE, oid, cv, op->get_reqid());
    log.add(cloneentry);

    dout(10) << "prepare_log_transaction " << op->get_op()
	     << " " << cloneentry
	     << endl;
  }

  // actual op
  int opcode = PG::Log::Entry::MODIFY;
  if (op->get_op() == OSD_OP_DELETE) opcode = PG::Log::Entry::DELETE;
  PG::Log::Entry logentry(opcode, oid, version, op->get_reqid());

  dout(10) << "prepare_log_transaction " << op->get_op()
           << " " << logentry
           << endl;

  // append to log
  assert(version > log.top);
  log.add(logentry);
  assert(log.top == version);
  dout(10) << "prepare_log_transaction appended" << endl;

  // write to pg log on disk
  append_log(t, logentry, trim_to);
}


/** prepare_op_transaction
 * apply an op to the store wrapped in a transaction.
 */
void ReplicatedPG::prepare_op_transaction(ObjectStore::Transaction& t, 
					  MOSDOp *op, eversion_t& version, 
					  objectrev_t crev, objectrev_t rev)
{
  const object_t oid = op->get_oid();
  const pg_t pgid = op->get_pg();

  bool did_clone = false;

  dout(10) << "prepare_op_transaction " << MOSDOp::get_opname( op->get_op() )
           << " " << oid 
           << " v " << version
	   << " crev " << crev
	   << " rev " << rev
           << endl;
  
  // WRNOOP does nothing.
  if (op->get_op() == OSD_OP_WRNOOP) 
    return;

  // raise last_complete?
  if (info.last_complete == info.last_update)
    info.last_complete = version;
  
  // raise last_update.
  assert(version > info.last_update);
  info.last_update = version;
  
  // write pg info
  t.collection_setattr(pgid, "info", &info, sizeof(info));

  // clone?
  if (crev && rev && rev > crev) {
    object_t noid = oid;
    noid.rev = rev;
    dout(10) << "prepare_op_transaction cloning " << oid << " crev " << crev << " to " << noid << endl;
    t.clone(oid, noid);
    did_clone = true;
  }  

  // apply the op
  switch (op->get_op()) {
  case OSD_OP_WRLOCK:
    { // lock object
      //r = store->setattr(oid, "wrlock", &op->get_asker(), sizeof(msg_addr_t), oncommit);
      t.setattr(oid, "wrlock", &op->get_client(), sizeof(entity_name_t));
    }
    break;  
    
  case OSD_OP_WRUNLOCK:
    { // unlock objects
      //r = store->rmattr(oid, "wrlock", oncommit);
      t.rmattr(oid, "wrlock");
      
      // unblock all operations that were waiting for this object to become unlocked
      if (osd->waiting_for_wr_unlock.count(oid)) {
        osd->take_waiters(osd->waiting_for_wr_unlock[oid]);
        osd->waiting_for_wr_unlock.erase(oid);
      }
    }
    break;
    
  case OSD_OP_WRITE:
    { // write
      assert(op->get_data().length() == op->get_length());
      bufferlist bl;
      bl.claim( op->get_data() );  // give buffers to store; we keep *op in memory for a long time!
      
      //if (oid < 100000000000000ULL)  // hack hack-- don't write client data
      t.write( oid, op->get_offset(), op->get_length(), bl );
    }
    break;
    
  case OSD_OP_ZERO:
    {
      assert(0);  // are you sure this is what you want?
      // zero, remove, or truncate?
      struct stat st;
      int r = osd->store->stat(oid, &st);
      if (r >= 0) {
	if (op->get_offset() + op->get_length() >= st.st_size) {
	  if (op->get_offset()) 
	    t.truncate(oid, op->get_length() + op->get_offset());
	  else
	    t.remove(oid);
	} else {
	  // zero.  the dumb way.  FIXME.
	  bufferptr bp(op->get_length());
	  bp.zero();
	  bufferlist bl;
	  bl.push_back(bp);
	  t.write(oid, op->get_offset(), op->get_length(), bl);
	}
      } else {
	// noop?
	dout(10) << "apply_transaction zero on " << oid << ", but dne?  stat returns " << r << endl;
      }
    }
    break;

  case OSD_OP_TRUNCATE:
    { // truncate
      //r = store->truncate(oid, op->get_offset());
      t.truncate(oid, op->get_length() );
    }
    break;
    
  case OSD_OP_DELETE:
    { // delete
      //r = store->remove(oid);
      t.remove(oid);
    }
    break;
    
  default:
    assert(0);
  }
  
  // object collection, version
  if (op->get_op() == OSD_OP_DELETE) {
    // remove object from c
    t.collection_remove(pgid, oid);
  } else {
    // add object to c
    t.collection_add(pgid, oid);
    
    // object version
    t.setattr(oid, "version", &version, sizeof(version));

    // set object crev
    if (crev == 0 ||   // new object
	did_clone)     // we cloned
      t.setattr(oid, "crev", &rev, sizeof(rev));
  }
}



// ========================================================================
// rep op gather

class C_OSD_WriteCommit : public Context {
public:
  OSD *osd;
  pg_t pgid;
  tid_t rep_tid;
  eversion_t pg_last_complete;
  C_OSD_WriteCommit(OSD *o, pg_t p, tid_t rt, eversion_t lc) : osd(o), pgid(p), rep_tid(rt), pg_last_complete(lc) {}
  void finish(int r) {
    ReplicatedPG *pg = (ReplicatedPG*)(osd->_lock_pg(pgid));
    if (pg) {
      pg->op_modify_commit(rep_tid, pg_last_complete);
      osd->_unlock_pg(pg->info.pgid);
    }
  }
};


void ReplicatedPG::get_rep_gather(RepGather *repop)
{
  //repop->lock.Lock();
  dout(10) << "get_repop " << *repop << endl;
}

void ReplicatedPG::apply_repop(RepGather *repop)
{
  dout(10) << "apply_repop  applying update on " << *repop << endl;
  assert(!repop->applied);

  Context *oncommit = new C_OSD_WriteCommit(osd, info.pgid, repop->rep_tid, repop->pg_local_last_complete);
  unsigned r = osd->store->apply_transaction(repop->t, oncommit);
  if (r)
    dout(-10) << "apply_repop  apply transaction return " << r << " on " << *repop << endl;
  
  // discard my reference to the buffer
  repop->op->get_data().clear();
  
  repop->applied = true;
}

void ReplicatedPG::put_rep_gather(RepGather *repop)
{
  dout(10) << "put_repop " << *repop << endl;
  
  // commit?
  if (repop->can_send_commit() &&
      repop->op->wants_commit()) {
    // send commit.
    MOSDOpReply *reply = new MOSDOpReply(repop->op, 0, osd->osdmap->get_epoch(), true);
    dout(10) << "put_repop  sending commit on " << *repop << " " << reply << endl;
    osd->messenger->send_message(reply, repop->op->get_client_inst());
    repop->sent_commit = true;
  }

  // ack?
  else if (repop->can_send_ack() &&
           repop->op->wants_ack()) {
    // apply
    apply_repop(repop);

    // send ack
    MOSDOpReply *reply = new MOSDOpReply(repop->op, 0, osd->osdmap->get_epoch(), false);
    dout(10) << "put_repop  sending ack on " << *repop << " " << reply << endl;
    osd->messenger->send_message(reply, repop->op->get_client_inst());
    repop->sent_ack = true;

    utime_t now = g_clock.now();
    now -= repop->start;
    osd->logger->finc("rlsum", now);
    osd->logger->inc("rlnum", 1);
  }

  // done.
  if (repop->can_delete()) {
    // adjust peers_complete_thru
    if (!repop->pg_complete_thru.empty()) {
      eversion_t min = info.last_complete;  // hrm....
      for (unsigned i=0; i<acting.size(); i++) {
        if (repop->pg_complete_thru[acting[i]] < min)      // note: if we haven't heard, it'll be zero, which is what we want.
          min = repop->pg_complete_thru[acting[i]];
      }
      
      if (min > peers_complete_thru) {
        dout(10) << "put_repop  peers_complete_thru " 
				 << peers_complete_thru << " -> " << min
				 << endl;
        peers_complete_thru = min;
      }
    }

    dout(10) << "put_repop  deleting " << *repop << endl;
    //repop->lock.Unlock();  
	
    assert(rep_gather.count(repop->rep_tid));
    rep_gather.erase(repop->rep_tid);
	
    delete repop->op;
    delete repop;

  } else {
    //repop->lock.Unlock();
  }
}


void ReplicatedPG::issue_repop(MOSDOp *op, int dest)
{
  object_t oid = op->get_oid();
  
  dout(7) << " issue_repop rep_tid " << op->get_rep_tid()
          << " o " << oid
          << " to osd" << dest
          << endl;
  
  // forward the write/update/whatever
  MOSDOp *wr = new MOSDOp(op->get_client_inst(), op->get_client_inc(), op->get_reqid().tid,
                          oid,
                          info.pgid,
                          osd->osdmap->get_epoch(),
                          op->get_op());
  wr->get_data() = op->get_data();   // _copy_ bufferlist
  wr->set_length(op->get_length());
  wr->set_offset(op->get_offset());
  wr->set_version(op->get_version());
  
  wr->set_rep_tid(op->get_rep_tid());
  wr->set_pg_trim_to(peers_complete_thru);
  
  osd->messenger->send_message(wr, osd->osdmap->get_inst(dest));
}

ReplicatedPG::RepGather *ReplicatedPG::new_rep_gather(MOSDOp *op)
{
  dout(10) << "new_rep_gather rep_tid " << op->get_rep_tid() << " on " << *op << endl;

  RepGather *repop = new RepGather(op, op->get_rep_tid(), 
                                               op->get_version(), 
                                               info.last_complete);
  
  // osds. commits all come to me.
  for (unsigned i=0; i<acting.size(); i++) {
    int osd = acting[i];
    repop->osds.insert(osd);
    repop->waitfor_commit.insert(osd);
  }

  // acks vary:
  if (g_conf.osd_rep == OSD_REP_CHAIN) {
    // chain rep. 
    // there's my local ack...
    repop->osds.insert(whoami);
    repop->waitfor_ack.insert(whoami);
    repop->waitfor_commit.insert(whoami);

    // also, the previous guy will ack to me
    int myrank = osdmap->calc_pg_rank(whoami, acting);
    if (myrank > 0) {
      int osd = acting[ myrank-1 ];
      repop->osds.insert(osd);
      repop->waitfor_ack.insert(osd);
      repop->waitfor_commit.insert(osd);
    }
  } else {
    // primary, splay.  all osds ack to me.
    for (unsigned i=0; i<acting.size(); i++) {
      int osd = acting[i];
      repop->waitfor_ack.insert(osd);
    }
  }

  repop->start = g_clock.now();

  rep_gather[ repop->rep_tid ] = repop;

  // anyone waiting?  (acks that got here before the op did)
  if (waiting_for_repop.count(repop->rep_tid)) {
    osd->take_waiters(waiting_for_repop[repop->rep_tid]);
    waiting_for_repop.erase(repop->rep_tid);
  }

  return repop;
}
 

void ReplicatedPG::repop_ack(RepGather *repop,
                    int result, bool commit,
                    int fromosd, eversion_t pg_complete_thru)
{
  MOSDOp *op = repop->op;

  dout(7) << "repop_ack rep_tid " << repop->rep_tid << " op " << *op
          << " result " << result << " commit " << commit << " from osd" << fromosd
          << endl;

  get_rep_gather(repop);
  {
    if (commit) {
      // commit
      assert(repop->waitfor_commit.count(fromosd));      
      repop->waitfor_commit.erase(fromosd);
      repop->waitfor_ack.erase(fromosd);
      repop->pg_complete_thru[fromosd] = pg_complete_thru;
    } else {
      // ack
      repop->waitfor_ack.erase(fromosd);
    }
  }
  put_rep_gather(repop);
}























/** op_modify_commit
 * transaction commit on the acker.
 */
void ReplicatedPG::op_modify_commit(tid_t rep_tid, eversion_t pg_complete_thru)
{
  if (rep_gather.count(rep_tid)) {
    RepGather *repop = rep_gather[rep_tid];
    
    dout(10) << "op_modify_commit " << *repop->op << endl;
    get_rep_gather(repop);
    {
      assert(repop->waitfor_commit.count(osd->get_nodeid()));
      repop->waitfor_commit.erase(osd->get_nodeid());
      repop->pg_complete_thru[osd->get_nodeid()] = pg_complete_thru;
    }
    put_rep_gather(repop);
    dout(10) << "op_modify_commit done on " << repop << endl;
  } else {
    dout(10) << "op_modify_commit rep_tid " << rep_tid << " dne" << endl;
  }
}



void ReplicatedPG::assign_version(MOSDOp *op)
{
  // check crev
  objectrev_t crev = 0;
  osd->store->getattr(oid, "crev", (char*)&crev, sizeof(crev));

  // assign version
  eversion_t clone_version;
  eversion_t nv = log.top;
  if (op->get_op() != OSD_OP_WRNOOP) {
    nv.epoch = osd->osdmap->get_epoch();
    nv.version++;
    assert(nv > info.last_update);
    assert(nv > log.top);

    // will clone?
    if (crev && op->get_rev() && op->get_rev() > crev) {
      clone_version = nv;
      nv.version++;
    }

    if (op->get_version().version) {
      // replay!
      if (nv.version < op->get_version().version) {
        nv.version = op->get_version().version; 

	// clone?
	if (crev && op->get_rev() && op->get_rev() > crev) {
	  // backstep clone
	  clone_version = nv;
	  clone_version.version--;
	}
      }
    }
  }

  // set version in op, for benefit of client and our eventual reply
  op->set_version(nv);
}



void ReplicatedPG::op_modify(MOSDOp *op)
{
  object_t oid = op->get_oid();
  const char *opname = MOSDOp::get_opname(op->get_op());

  // dup op?
  if (is_dup(op->get_reqid())) {
    dout(-3) << "op_modify " << opname << " dup op " << op->get_reqid()
             << ", doing WRNOOP" << endl;
    op->set_op(OSD_OP_WRNOOP);
    opname = MOSDOp::get_opname(op->get_op());
  }

  // assign the op a version
  assign_version(op);

  // are any peers missing this?
  for (unsigned i=1; i<pg->acting.size(); i++) {
    int peer = pg->acting[i];
    if (pg->peer_missing.count(peer) &&
        pg->peer_missing[peer].is_missing(oid)) {
      // push it before this update. 
      // FIXME, this is probably extra much work (eg if we're about to overwrite)
      pg->peer_missing[peer].got(oid);
      push(pg, oid, peer);
    }
  }

  dout(10) << "op_modify " << opname 
           << " " << oid 
           << " v " << nv 
		   << " crev " << crev
		   << " rev " << op->get_rev()
           << " " << op->get_offset() << "~" << op->get_length()
           << endl;  

  // issue replica writes
  RepGather *repop = 0;
  bool alone = (pg->acting.size() == 1);
  tid_t rep_tid = ++last_tid;
  op->set_rep_tid(rep_tid);

  if (g_conf.osd_rep == OSD_REP_CHAIN && !alone) {
    // chain rep.  send to #2 only.
    int next = pg->acting[1];
    if (pg->acting.size() > 2)
      next = pg->acting[2];
    issue_repop(pg, op, next);
  } 
  else if (g_conf.osd_rep == OSD_REP_SPLAY && !alone) {
    // splay rep.  send to rest.
    for (unsigned i=1; i<pg->acting.size(); ++i)
    //for (unsigned i=pg->acting.size()-1; i>=1; --i)
      issue_repop(pg, op, pg->acting[i]);
  } else {
    // primary rep, or alone.
    repop = new_rep_gather(pg, op);

    // send to rest.
    if (!alone)
      for (unsigned i=1; i<pg->acting.size(); i++)
        issue_repop(pg, op, pg->acting[i]);
  }

  if (repop) {    
    // we are acker.
    if (op->get_op() != OSD_OP_WRNOOP) {
      // log and update later.
      prepare_log_transaction(repop->t, op, nv, crev, op->get_rev(), pg, pg->peers_complete_thru);
      prepare_op_transaction(repop->t, op, nv, crev, op->get_rev(), pg);
    }

    // (logical) local ack.
    // (if alone, this will apply the update.)
    get_rep_gather(repop);
    {
      assert(repop->waitfor_ack.count(whoami));
      repop->waitfor_ack.erase(whoami);
    }
    put_rep_gather(pg, repop);

  } else {
    // chain or splay.  apply.
    ObjectStore::Transaction t;
    prepare_log_transaction(t, op, nv, crev, op->get_rev(), pg, pg->peers_complete_thru);
    prepare_op_transaction(t, op, nv, crev, op->get_rev(), pg);

    C_OSD_RepModifyCommit *oncommit = new C_OSD_RepModifyCommit(this, op, pg->get_acker(), 
                                                                pg->info.last_complete);
    unsigned r = store->apply_transaction(t, oncommit);
    if (r != 0 &&   // no errors
        r != 2) {   // or error on collection_add
      cerr << "error applying transaction: r = " << r << endl;
      assert(r == 0);
    }

    oncommit->ack();
  }



}



// replicated 

// commit (to disk) callback
class C_OSD_RepModifyCommit : public Context {
public:
  OSD *osd;
  MOSDOp *op;
  int destosd;

  eversion_t pg_last_complete;

  Mutex lock;
  Cond cond;
  bool acked;
  bool waiting;

  C_OSD_RepModifyCommit(OSD *o, MOSDOp *oo, int dosd, eversion_t lc) : 
    osd(o), op(oo), destosd(dosd), pg_last_complete(lc),
    acked(false), waiting(false) { }
  void finish(int r) {
    lock.Lock();
    assert(!waiting);
    while (!acked) {
      waiting = true;
      cond.Wait(lock);
    }
    assert(acked);
    lock.Unlock();
    osd->op_rep_modify_commit(op, destosd, pg_last_complete);
  }
  void ack() {
    lock.Lock();
    assert(!acked);
    acked = true;
    if (waiting) cond.Signal();

    // discard my reference to buffer
    op->get_data().clear();

    lock.Unlock();
  }
};




void ReplicatedPG::op_rep_modify(MOSDOp *op)
{
  object_t oid = op->get_oid();
  eversion_t nv = op->get_version();

  const char *opname = MOSDOp::get_opname(op->get_op());

  // check crev
  objectrev_t crev = 0;
  store->getattr(oid, "crev", (char*)&crev, sizeof(crev));

  dout(10) << "op_rep_modify " << opname 
           << " " << oid 
           << " v " << nv 
           << " " << op->get_offset() << "~" << op->get_length()
           << " in " << *pg
           << endl;  
  
  // we better not be missing this.
  assert(!pg->missing.is_missing(oid));

  // prepare our transaction
  ObjectStore::Transaction t;

  // am i acker?
  RepGather *repop = 0;
  int ackerosd = pg->acting[0];

  if ((g_conf.osd_rep == OSD_REP_CHAIN || g_conf.osd_rep == OSD_REP_SPLAY)) {
    ackerosd = pg->get_acker();
  
    if (pg->is_acker()) {
      // i am tail acker.
      if (pg->rep_gather.count(op->get_rep_tid())) {
        repop = pg->rep_gather[ op->get_rep_tid() ];
      } else {
        repop = new_rep_gather(pg, op);
      }
      
      // infer ack from source
      int fromosd = op->get_source().num();
      get_rep_gather(repop);
      {
        //assert(repop->waitfor_ack.count(fromosd));   // no, we may come thru here twice.
        repop->waitfor_ack.erase(fromosd);
      }
      put_rep_gather(pg, repop);

      // prepare dest socket
      //messenger->prepare_send_message(op->get_client());
    }

    // chain?  forward?
    if (g_conf.osd_rep == OSD_REP_CHAIN && !pg->is_acker()) {
      // chain rep, not at the tail yet.
      int myrank = osdmap->calc_pg_rank(whoami, pg->acting);
      int next = myrank+1;
      if (next == (int)pg->acting.size())
	next = 1;
      issue_repop(pg, op, pg->acting[next]);	
    }
  }

  // do op?
  C_OSD_RepModifyCommit *oncommit = 0;

  logger->inc("r_wr");
  logger->inc("r_wrb", op->get_length());
  
  if (repop) {
    // acker.  we'll apply later.
    if (op->get_op() != OSD_OP_WRNOOP) {
      prepare_log_transaction(repop->t, op, nv, crev, op->get_rev(), pg, op->get_pg_trim_to());
      prepare_op_transaction(repop->t, op, nv, crev, op->get_rev(), pg);
    }
  } else {
    // middle|replica.
    if (op->get_op() != OSD_OP_WRNOOP) {
      prepare_log_transaction(t, op, nv, crev, op->get_rev(), pg, op->get_pg_trim_to());
      prepare_op_transaction(t, op, nv, crev, op->get_rev(), pg);
    }

    oncommit = new C_OSD_RepModifyCommit(this, op, ackerosd, pg->info.last_complete);

    // apply log update. and possibly update itself.
    unsigned tr = store->apply_transaction(t, oncommit);
    if (tr != 0 &&   // no errors
        tr != 2) {   // or error on collection_add
      cerr << "error applying transaction: r = " << tr << endl;
      assert(tr == 0);
    }
  }
  
  // ack?
  if (repop) {
    // (logical) local ack.  this may induce the actual update.
    get_rep_gather(repop);
    {
      assert(repop->waitfor_ack.count(whoami));
      repop->waitfor_ack.erase(whoami);
    }
    put_rep_gather(pg, repop);
  } 
  else {
    // send ack to acker?
    if (g_conf.osd_rep != OSD_REP_CHAIN) {
      MOSDOpReply *ack = new MOSDOpReply(op, 0, osdmap->get_epoch(), false);
      messenger->send_message(ack, osdmap->get_inst(ackerosd));
    }

    // ack myself.
    assert(oncommit);
    oncommit->ack(); 
  }

}


void ReplicatedPG::op_rep_modify_commit(MOSDOp *op, int ackerosd, eversion_t last_complete)
{
  // send commit.
  dout(10) << "rep_modify_commit on op " << *op
           << ", sending commit to osd" << ackerosd
           << endl;
  MOSDOpReply *commit = new MOSDOpReply(op, 0, osdmap->get_epoch(), true);
  commit->set_pg_complete_thru(last_complete);
  messenger->send_message(commit, osdmap->get_inst(ackerosd));
  delete op;
}










// ===========================================================

/** pull - request object from a peer
 */
void ReplicatedPG::pull(object_t oid)
{
  assert(missing.loc.count(oid));
  eversion_t v = missing.missing[oid];
  int osd = missing.loc[oid];
  
  dout(7) << "pull " << oid
          << " v " << v 
          << " from osd" << osd
          << endl;

  // send op
  tid_t tid = ++last_tid;
  MOSDOp *op = new MOSDOp(osd->messenger->get_myinst(), 0, tid,
                          oid, info.pgid,
                          osd->osdmap->get_epoch(),
                          OSD_OP_PULL);
  op->set_version(v);
  osd->messenger->send_message(op, osdmap->get_inst(osd));
  
  // take note
  assert(objects_pulling.count(oid) == 0);
  num_pulling++;
  objects_pulling[oid] = v;
}


/** push - send object to a peer
 */
void ReplicatedPG::push(object_t oid, int dest)
{
  // read data+attrs
  bufferlist bl;
  eversion_t v;
  int vlen = sizeof(v);
  map<string,bufferptr> attrset;
  
  ObjectStore::Transaction t;
  t.read(oid, 0, 0, &bl);
  t.getattr(oid, "version", &v, &vlen);
  t.getattrs(oid, attrset);
  unsigned tr = osd->store->apply_transaction(t);
  
  assert(tr == 0);  // !!!

  // ok
  dout(7) << "push " << oid << " v " << v 
          << " size " << bl.length()
          << " to osd" << dest
          << endl;

  osd->logger->inc("r_push");
  osd->logger->inc("r_pushb", bl.length());
  
  // send
  MOSDOp *op = new MOSDOp(osd->messenger->get_myinst(), 0, ++last_tid,
                          oid, info.pgid, osdmap->get_epoch(), 
                          OSD_OP_PUSH); 
  op->set_offset(0);
  op->set_length(bl.length());
  op->set_data(bl);   // note: claims bl, set length above here!
  op->set_version(v);
  op->set_attrset(attrset);
  
  osd->messenger->send_message(op, osd->osdmap->get_inst(dest));
}



/** op_pull
 * process request to pull an entire object.
 * NOTE: called from opqueue.
 */
void ReplicatedPG::op_pull(MOSDOp *op)
{
  const object_t oid = op->get_oid();
  const eversion_t v = op->get_version();
  int from = op->get_source().num();

  dout(7) << "op_pull " << oid << " v " << op->get_version()
          << " from " << op->get_source()
          << endl;

  // is a replica asking?  are they missing it?
  if (is_primary()) {
    // primary
    assert(peer_missing.count(from));  // we had better know this, from the peering process.

    if (!peer_missing[from].is_missing(oid)) {
      dout(7) << "op_pull replica isn't actually missing it, we must have already pushed to them" << endl;
      delete op;
      return;
    }

    // do we have it yet?
    if (is_missing_object(oid)) {
      wait_for_missing_object(oid, op);
      return;
    }
  } else {
    // non-primary
    if (missing.is_missing(oid)) {
      dout(7) << "op_pull not primary, and missing " << oid << ", ignoring" << endl;
      delete op;
      return;
    }
  }
    
  // push it back!
  push(oid, op->get_source().num());
}


/** op_push
 * NOTE: called from opqueue.
 */
void ReplicatedPG::op_push(MOSDOp *op)
{
  object_t oid = op->get_oid();
  eversion_t v = op->get_version();

  if (!is_missing_object(oid)) {
    dout(7) << "op_push not missing " << oid << endl;
    return;
  }
  
  dout(7) << "op_push " 
          << oid 
          << " v " << v 
          << " size " << op->get_length() << " " << op->get_data().length()
          << endl;

  assert(op->get_data().length() == op->get_length());
  
  // write object and add it to the PG
  ObjectStore::Transaction t;
  t.remove(oid);  // in case old version exists
  t.write(oid, 0, op->get_length(), op->get_data());
  t.setattrs(oid, op->get_attrset());
  t.collection_add(info.pgid, oid);

  // close out pull op?
  num_pulling--;
  if (objects_pulling.count(oid))
    objects_pulling.erase(oid);
  missing.got(oid, v);


  // raise last_complete?
  assert(log.complete_to != log.log.end());
  while (log.complete_to != log.log.end()) {
    if (missing.missing.count(log.complete_to->oid)) break;
    if (info.last_complete < log.complete_to->version)
      info.last_complete = log.complete_to->version;
    log.complete_to++;
  }
  dout(10) << "last_complete now " << info.last_complete << endl;
  
  
  // apply to disk!
  t.collection_setattr(info.pgid, "info", &info, sizeof(info));
  unsigned r = osd->store->apply_transaction(t);
  assert(r == 0);



  // am i primary?  are others missing this too?
  if (is_primary()) {
    for (unsigned i=1; i<acting.size(); i++) {
      int peer = acting[i];
      assert(peer_missing.count(peer));
      if (peer_missing[peer].is_missing(oid)) {
        // ok, push it, and they (will) have it now.
        peer_missing[peer].got(oid, v);
        push(oid, peer);
      }
    }
  }

  // continue recovery
  do_recovery();
  
  // kick waiters
  if (waiting_for_missing_object.count(oid)) {
    osd->take_waiters(waiting_for_missing_object[oid]);
    waiting_for_missing_object.erase(oid);
  }

  delete op;
}




void ReplicatedPG::op_reply(MOSDOpReply *r)
{
  // must be replication.
  tid_t rep_tid = r->get_rep_tid();
  
  if (rep_gather.count(rep_tid)) {
    // oh, good.
    int fromosd = r->get_source().num();
    repop_ack(rep_gather[rep_tid], 
	      r->get_result(), r->get_commit(), 
	      fromosd, 
	      r->get_pg_complete_thru());
    delete m;
  } else {
    // early ack.
    waiting_for_repop[rep_tid].push_back(r);
  }
}

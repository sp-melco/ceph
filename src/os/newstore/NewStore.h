// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_OSD_NEWSTORE_H
#define CEPH_OSD_NEWSTORE_H

#include "include/assert.h"
#include "include/unordered_map.h"
#include "include/memory.h"
#include "common/Finisher.h"
#include "common/RWLock.h"
#include "common/shared_cache.hpp"
#include "os/ObjectStore.h"
#include "os/KeyValueDB.h"

#include "newstore_types.h"

class NewStore : public ObjectStore {
  // -----------------------------------------------------
  // types
public:

  struct FragmentHandle {
    int fd;
    FragmentHandle() : fd(-1) {}
    FragmentHandle(int f) : fd(f) {}
    ~FragmentHandle() {
      if (fd >= 0)
	::close(fd);
    }
    int fsync() {
      return ::fsync(fd);
    }
    int fdatasync() {
      return ::fdatasync(fd);
    }
  };
  typedef ceph::shared_ptr<FragmentHandle> FragmentHandleRef;

  class TransContext;

  /// an in-memory object
  struct Onode {
    ghobject_t oid;
    string key;     ///< key under PREFIX_OBJ where we are stored
    onode_t onode;  ///< metadata stored as value in kv store
    bool dirty;     // ???
    bool exists;

    //list<TransContextRef> uncommitted_txns; ///< pending txns
    list<TransContext*> unapplied_txns;   ///< committed but unapplied txns
    Cond wal_cond;   ///< wait here for unapplied txns

    Onode(const ghobject_t& o, const string& k);
  };
  typedef ceph::shared_ptr<Onode> OnodeRef;

  struct Collection {
    NewStore *store;
    coll_t cid;
    RWLock lock;

    // cache onodes on a per-collection basis to avoid lock
    // contention.
    SharedLRU<ghobject_t,Onode> onode_map;

    OnodeRef get_onode(const ghobject_t& oid, bool create);

    Collection(NewStore *ns, coll_t c)
      : store(ns),
	cid(c),
	lock("NewStore::Collection::lock")
    {}
  };
  typedef ceph::shared_ptr<Collection> CollectionRef;

  class OmapIteratorImpl : public ObjectMap::ObjectMapIteratorImpl {
    CollectionRef c;
    OnodeRef o;
    //map<string,bufferlist>::iterator it;
  public:
    OmapIteratorImpl(CollectionRef c, OnodeRef o)
      : c(c), o(o) /*, it(o->omap.begin())*/ {}

    int seek_to_first() {
      RWLock::RLocker l(c->lock);
      //it = o->omap.begin();
      return 0;
    }
    int upper_bound(const string &after) {
      RWLock::RLocker l(c->lock);
      //it = o->omap.upper_bound(after);
      return 0;
    }
    int lower_bound(const string &to) {
      RWLock::RLocker l(c->lock);
      //it = o->omap.lower_bound(to);
      return 0;
    }
    bool valid() {
      RWLock::RLocker l(c->lock);
      return false; //return it != o->omap.end();
    }
    int next() {
      RWLock::RLocker l(c->lock);
      //++it;
      return 0;
    }
    string key() {
      RWLock::RLocker l(c->lock);
      return string(); //return it->first;
    }
    bufferlist value() {
      RWLock::RLocker l(c->lock);
      return bufferlist(); //return it->second;
    }
    int status() {
      return 0;
    }
  };

  struct TransContext {
    list<int> fds;             ///< these fds need to be synced
    list<OnodeRef> onodes;     ///< these onodes need to be updated/written
    KeyValueDB::Transaction t; ///< then we will commit this
    list<fid_t> remove;        ///< later, these fids need to be removed
    list<Context*> oncommit;   ///< signal on commit

    wal_transaction_t *wal_txn; ///< wal transaction (if any)

    TransContext()
      : wal_txn(NULL) {}
    ~TransContext() {
      delete wal_txn;
    }

    void sync_fd(int f) {
      fds.push_back(f);
    }
    void write_onode(OnodeRef &o) {
      onodes.push_back(o);
    }
    void remove_fid(fid_t f) {
      remove.push_back(f);
    }

    void start_wal_apply() {
      for (list<OnodeRef>::iterator p = onodes.begin(); p != onodes.end(); ++p) {
	(*p)->unapplied_txns.push_back(this);
      }
    }
    void finish_wal_apply() {
      for (list<OnodeRef>::iterator p = onodes.begin(); p != onodes.end(); ++p) {
	assert((*p)->unapplied_txns.front() == this);
	(*p)->unapplied_txns.pop_front();
	if ((*p)->unapplied_txns.empty())
	  (*p)->wal_cond.Signal();
      }
    }
  };
  typedef ceph::shared_ptr<TransContext> TransContextRef;


  class OpSequencer : public Sequencer_impl {
    Mutex qlock;
    Cond qcond;
    list<TransContextRef> q;  ///< transactions
  public:
    Sequencer *parent;

    OpSequencer()
      : qlock("NewStore::OpSequencer::qlock", false, false),
	parent(NULL) {
    }
    ~OpSequencer() {
      assert(q.empty());
    }

    void flush() {
      Mutex::Locker l(qlock);
      while (!q.empty())
	qcond.Wait(qlock);
    }

    bool flush_commit(Context *c) {
      Mutex::Locker l(qlock);
      if (q.empty()) {
	delete c;
	return true;
      }
      q.back()->oncommit.push_back(c); /// XXX onreadable?
      return false;
    }
  };


  // --------------------------------------------------------
  // members
private:
  KeyValueDB *db;
  uuid_d fsid;
  int path_fd;  ///< open handle to $path
  int fsid_fd;  ///< open handle (locked) to $path/fsid
  int frag_fd;  ///< open handle to $path/fragments
  int fset_fd;  ///< open handle to $path/fragments/$cur_fid.fset
  bool mounted;

  RWLock coll_lock;    ///< rwlock to protect coll_map
  ceph::unordered_map<coll_t, CollectionRef> coll_map;

  Mutex fid_lock;
  fid_t cur_fid;

  Mutex wal_lock;
  atomic64_t wal_seq;

  Finisher finisher;
  Logger *logger;

  Sequencer default_osr;


  // --------------------------------------------------------
  // private methods

  void _init_logger();
  void _shutdown_logger();

  int _open_path();
  void _close_path();
  int _open_fsid(bool create);
  int _lock_fsid();
  int _read_fsid(uuid_d *f);
  int _write_fsid();
  void _close_fsid();
  int _open_frag();
  int _create_frag();
  void _close_frag();
  int _open_db();
  void _close_db();

  CollectionRef _get_collection(coll_t cid);

  int _open_next_fid(fid_t *fid);
  int _open_fid(fid_t fid);
  int _remove_fid(fid_t fid);
  TransContext *_create_txc(OpSequencer *osr);
  int _finalize_txc(OpSequencer *osr, TransContextRef& txc);
  wal_op_t *_get_wal_op(TransContextRef& txc);
  int _apply_wal_transaction(TransContextRef& txc);
  friend class C_ApplyWAL;

public:
  NewStore(CephContext *cct, const string& path);
  ~NewStore();

  bool need_journal() { return true; };
  int peek_journal_fsid(uuid_d *fsid);

  bool test_mount_in_use();

  int mount();
  int umount();

  unsigned get_max_object_name_length() {
    return 4096;
  }
  unsigned get_max_attr_name_length() {
    return 256;  // arbitrary; there is no real limit internally
  }

  int mkfs();
  int mkjournal() {
    return 0;
  }

private:
  bool sharded;
public:
  void set_allow_sharded_objects() {
    sharded = true;
  }
  bool get_allow_sharded_objects() {
    return sharded;
  }

  int statfs(struct statfs *buf);

  bool exists(coll_t cid, const ghobject_t& oid);
  int stat(
    coll_t cid,
    const ghobject_t& oid,
    struct stat *st,
    bool allow_eio = false); // struct stat?
  int read(
    coll_t cid,
    const ghobject_t& oid,
    uint64_t offset,
    size_t len,
    bufferlist& bl,
    uint32_t op_flags = 0,
    bool allow_eio = false);
  int fiemap(coll_t cid, const ghobject_t& oid, uint64_t offset, size_t len, bufferlist& bl);
  int getattr(coll_t cid, const ghobject_t& oid, const char *name, bufferptr& value);
  int getattrs(coll_t cid, const ghobject_t& oid, map<string,bufferptr>& aset);

  int list_collections(vector<coll_t>& ls);
  bool collection_exists(coll_t c);
  bool collection_empty(coll_t c);
  int collection_list(coll_t cid, vector<ghobject_t>& o);
  int collection_list_partial(coll_t cid, ghobject_t start,
			      int min, int max, snapid_t snap,
			      vector<ghobject_t> *ls, ghobject_t *next);
  int collection_list_range(coll_t cid, ghobject_t start, ghobject_t end,
			    snapid_t seq, vector<ghobject_t> *ls);

  int omap_get(
    coll_t cid,                ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    bufferlist *header,      ///< [out] omap header
    map<string, bufferlist> *out /// < [out] Key to value map
    );

  /// Get omap header
  int omap_get_header(
    coll_t cid,                ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    bufferlist *header,      ///< [out] omap header
    bool allow_eio = false ///< [in] don't assert on eio
    );

  /// Get keys defined on oid
  int omap_get_keys(
    coll_t cid,              ///< [in] Collection containing oid
    const ghobject_t &oid, ///< [in] Object containing omap
    set<string> *keys      ///< [out] Keys defined on oid
    );

  /// Get key values
  int omap_get_values(
    coll_t cid,                    ///< [in] Collection containing oid
    const ghobject_t &oid,       ///< [in] Object containing omap
    const set<string> &keys,     ///< [in] Keys to get
    map<string, bufferlist> *out ///< [out] Returned keys and values
    );

  /// Filters keys into out which are defined on oid
  int omap_check_keys(
    coll_t cid,                ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    const set<string> &keys, ///< [in] Keys to check
    set<string> *out         ///< [out] Subset of keys defined on oid
    );

  ObjectMap::ObjectMapIterator get_omap_iterator(
    coll_t cid,              ///< [in] collection
    const ghobject_t &oid  ///< [in] object
    );

  void set_fsid(uuid_d u) {
    fsid = u;
  }
  uuid_d get_fsid() {
    return fsid;
  }

  objectstore_perf_stat_t get_cur_stats() {
    return objectstore_perf_stat_t();
  }

  int queue_transactions(
    Sequencer *osr,
    list<Transaction*>& tls,
    TrackedOpRef op = TrackedOpRef(),
    ThreadPool::TPHandle *handle = NULL);

private:
  // --------------------------------------------------------
  // write ops

  int _do_transaction(Transaction *t,
		      TransContextRef& txc,
		      ThreadPool::TPHandle *handle);

  int _write(TransContextRef& txc,
	     CollectionRef& c,
	     const ghobject_t& oid,
	     uint64_t offset, size_t len,
	     const bufferlist& bl,
	     uint32_t fadvise_flags);
  int _touch(TransContextRef& txc,
	     CollectionRef& c,
	     const ghobject_t& oid);
  int _zero(TransContextRef& txc,
	    CollectionRef& c,
	    const ghobject_t& oid,
	    uint64_t offset, size_t len);
  int _truncate(TransContextRef& txc,
		CollectionRef& c,
		const ghobject_t& oid,
		uint64_t offset);
  int _remove(TransContextRef& txc,
	      CollectionRef& c,
	      const ghobject_t& oid);
  int _setattr(TransContextRef& txc,
	       CollectionRef& c,
	       const ghobject_t& oid,
	       const string& name,
	       bufferptr& val);
  int _setattrs(TransContextRef& txc,
		CollectionRef& c,
		const ghobject_t& oid,
		const map<string,bufferptr>& aset);
  int _rmattr(TransContextRef& txc,
	      CollectionRef& c,
	      const ghobject_t& oid,
	      const string& name);
  int _rmattrs(TransContextRef& txc,
	       CollectionRef& c,
	       const ghobject_t& oid);
  int _clone(TransContextRef& txc,
	     CollectionRef& c,
	     const ghobject_t& old_oid,
	     const ghobject_t& new_oid);
  int _create_collection(TransContextRef& txc, coll_t cid, CollectionRef *c);
  int _remove_collection(TransContextRef& txc, coll_t cid, CollectionRef *c);


};

inline ostream& operator<<(ostream& out, const NewStore::OpSequencer& s) {
  return out << *s.parent;
}

#endif

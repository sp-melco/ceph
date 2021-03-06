// -*- mode:c++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * ceph - scalable distributed file system
 *
 * copyright (c) 2014 john spray <john.spray@inktank.com>
 *
 * this is free software; you can redistribute it and/or
 * modify it under the terms of the gnu lesser general public
 * license version 2.1, as published by the free software
 * foundation.  see file copying.
 */

#ifndef JOURNAL_SCANNER_H
#define JOURNAL_SCANNER_H

// For Journaler::Header, can't forward-declare nested classes
#include <osdc/Journaler.h>

namespace librados {
  class IoCtx;
}

#include "JournalFilter.h"

/**
 * A simple sequential reader for metadata journals.  Unlike
 * the MDS Journaler class, this is written to detect, record,
 * and read past corruptions and missing objects.  It is also
 * less efficient but more plainly written.
 */
class JournalScanner
{
  private:
  librados::IoCtx &io;

  // Input constraints
  int const rank;
  JournalFilter const filter;

  void gap_advance();

  public:
  JournalScanner(
      librados::IoCtx &io_,
      int rank_,
      JournalFilter const &filter_) :
    io(io_),
    rank(rank_),
    filter(filter_),
    pointer_present(false),
    pointer_valid(false),
    header_present(false),
    header_valid(false),
    header(NULL) {};

  JournalScanner(
      librados::IoCtx &io_,
      int rank_) :
    io(io_),
    rank(rank_),
    pointer_present(false),
    pointer_valid(false),
    header_present(false),
    header_valid(false),
    header(NULL) {};

  ~JournalScanner();

  int scan(bool const full=true);
  int scan_pointer();
  int scan_header();
  int scan_events();
  void report(std::ostream &out) const;

  std::string obj_name(uint64_t offset) const;
  std::string obj_name(inodeno_t ino, uint64_t offset) const;

  // The results of the scan
  inodeno_t ino;  // Corresponds to JournalPointer.front
  class EventRecord {
    public:
    EventRecord() : log_event(NULL), raw_size(0) {}
    EventRecord(LogEvent *le, uint32_t rs) : log_event(le), raw_size(rs) {}
    LogEvent *log_event;
    uint32_t raw_size;  //< Size from start offset including all encoding overhead
  };
  typedef std::map<uint64_t, EventRecord> EventMap;
  typedef std::pair<uint64_t, uint64_t> Range;
  bool pointer_present;
  bool pointer_valid;
  bool header_present;
  bool header_valid;
  Journaler::Header *header;

  bool is_healthy() const;
  bool is_readable() const;
  std::vector<std::string> objects_valid;
  std::vector<uint64_t> objects_missing;
  std::vector<Range> ranges_invalid;
  std::vector<uint64_t> events_valid;
  EventMap events;

  private:
  // Forbid copy construction because I have ptr members
  JournalScanner(const JournalScanner &rhs);
};

#endif // JOURNAL_SCANNER_H


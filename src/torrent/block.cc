// libTorrent - BitTorrent library
// Copyright (C) 2005-2006, Jari Sundell
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
// In addition, as a special exception, the copyright holders give
// permission to link the code of portions of this program with the
// OpenSSL library under certain conditions as described in each
// individual source file, and distribute linked combinations
// including the two.
//
// You must obey the GNU General Public License in all respects for
// all of the code used other than OpenSSL.  If you modify file(s)
// with this exception, you may extend this exception to your version
// of the file(s), but you are not obligated to do so.  If you do not
// wish to do so, delete this exception statement from your version.
// If you delete this exception statement from all source files in the
// program, then also delete it here.
//
// Contact:  Jari Sundell <jaris@ifi.uio.no>
//
//           Skomakerveien 33
//           3185 Skoppum, NORWAY

#include "config.h"

#include <algorithm>
#include <functional>
#include <rak/functional.h>

#include "block.h"
#include "block_list.h"
#include "block_transfer.h"
#include "exceptions.h"

namespace torrent {

Block::~Block() {
  std::for_each(m_failedList.begin(), m_failedList.end(), rak::on(rak::mem_ptr_ref(&failed_list_type::value_type::first), rak::call_delete<char>()));
}

inline void
Block::invalidate_transfer(BlockTransfer* transfer) {
  if (transfer == m_leader)
    throw internal_error("Block::invalidate_transfer(...) transfer == m_leader.");

  transfer->set_block(NULL);

  // FIXME: Various other accounting like position and counters.
  if (transfer->is_erased()) {
    delete transfer;

  } else {
    m_notStalled -= transfer->stall() == 0;

    transfer->set_state(BlockTransfer::STATE_ERASED);
  }
}

void
Block::clear() {
  m_leader = NULL;

  std::for_each(m_queued.begin(), m_queued.end(), std::bind1st(std::mem_fun(&Block::invalidate_transfer), this));
  m_queued.clear();

  std::for_each(m_transfers.begin(), m_transfers.end(), std::bind1st(std::mem_fun(&Block::invalidate_transfer), this));
  m_transfers.clear();

  if (m_notStalled != 0)
    throw internal_error("Block::clear() m_stalled != 0.");
}

BlockTransfer*
Block::insert(PeerInfo* peerInfo) {
  if (find_queued(peerInfo) || find_transfer(peerInfo))
    throw internal_error("Block::insert(...) find_queued(peerInfo) || find_transfer(peerInfo).");

  m_notStalled++;

  transfer_list_type::iterator itr = m_queued.insert(m_queued.end(), new BlockTransfer());

  (*itr)->set_peer_info(peerInfo);
  (*itr)->set_block(this);
  (*itr)->set_piece(m_piece);
  (*itr)->set_state(BlockTransfer::STATE_QUEUED);
  (*itr)->set_position(0);
  (*itr)->set_stall(0);

  return (*itr);
}
  
void
Block::erase(BlockTransfer* transfer) {
  if (transfer->is_erased())
    throw internal_error("Block::erase(...) transfer already erased.");

  m_notStalled -= transfer->stall() == 0;

  if (transfer->is_queued()) {
    transfer_list_type::iterator itr = std::find(m_queued.begin(), m_queued.end(), transfer);

    if (itr == m_queued.end())
      throw internal_error("Block::erase(...) Could not find transfer.");

    m_queued.erase(itr);

  } else {
    transfer_list_type::iterator itr = std::find(m_transfers.begin(), m_transfers.end(), transfer);

    if (itr == m_transfers.end())
      throw internal_error("Block::erase(...) Could not find transfer.");

    // Need to do something different here for now, i think.
    m_transfers.erase(itr);

    if (transfer == m_leader) {

      // When the leader is erased then any non-leading transfer must
      // be promoted. These non-leading transfers are guaranteed to
      // have the same data up to their position. PeerConnectionBase
      // assumes that a Block with non-leaders have a leader.

      transfer_list_type::iterator newLeader = std::max_element(std::find_if(m_transfers.begin(), m_transfers.end(), std::not1(std::mem_fun(&BlockTransfer::is_finished))),
                                                           m_transfers.end(),
                                                           rak::less2(std::mem_fun(&BlockTransfer::position), std::mem_fun(&BlockTransfer::position)));
      if (newLeader != m_transfers.end()) {
        m_leader = *newLeader;
        m_leader->set_state(BlockTransfer::STATE_LEADER);
      } else {
        m_leader = NULL;
      }
    }
  }

  delete transfer;
}

bool
Block::transfering(BlockTransfer* transfer) {
  if (!transfer->is_valid())
    throw internal_error("Block::transfering(...) transfer->block() == NULL.");

  transfer_list_type::iterator itr = std::find(m_queued.begin(), m_queued.end(), transfer);

  if (itr == m_queued.end())
    throw internal_error("Block::transfering(...) not queued.");

  m_queued.erase(itr);
  m_transfers.insert(m_transfers.end(), transfer);

  // If this block already has an active transfer, make this transfer
  // skip the piece. If this transfer gets ahead of the currently
  // transfering, it will (a) take over as the leader if the data is
  // the same or (b) erase itself from this block if the data does not
  // match.
  if (m_leader != NULL) {
    transfer->set_state(BlockTransfer::STATE_NOT_LEADER);
    return false;

  } else {
    m_leader = transfer;

    transfer->set_state(BlockTransfer::STATE_LEADER);
    return true;
  }
}

bool
Block::completed(BlockTransfer* transfer) {
  if (!transfer->is_valid())
    throw internal_error("Block::completed(...) transfer->block() == NULL.");

  if (transfer->is_erased())
    throw internal_error("Block::completed(...) transfer is erased.");

  if (!transfer->is_leader())
    throw internal_error("Block::completed(...) transfer is not the leader.");

  if (transfer->is_queued())
    throw internal_error("Block::completed(...) transfer is queued.");

  // How does this check work now?
//   if (transfer->block()->is_finished())
//     throw internal_error("Block::completed(...) transfer is already marked as finished.");

  // Special case where another ignored transfer finished before the
  // leader?
  //
  // Perhaps do magic to the transfer, erase it or something.
  if (!is_finished())
    throw internal_error("Block::completed(...) !is_finished().");

  if (transfer != m_leader)
    throw internal_error("Block::completed(...) transfer != m_leader.");

  m_parent->inc_finished();

  m_notStalled -= transfer->stall() == 0;
  transfer->set_stall(~uint32_t());

  // Currently just throw out the queued transfers. In case the hash
  // check fails, we might consider telling pcb during the call to
  // Block::transfering(...). But that would propably not be correct
  // as we want to trigger cancel messages from here, as hash fail is
  // a rare occurrence.
  std::for_each(m_queued.begin(), m_queued.end(), std::bind1st(std::mem_fun(&Block::invalidate_transfer), this));
  m_queued.clear();

  // We need to invalidate those unfinished and keep the one that
  // finished for later reference.

  transfer_list_type::iterator split = std::stable_partition(m_transfers.begin(), m_transfers.end(), std::mem_fun(&BlockTransfer::is_finished));

  std::for_each(split, m_transfers.end(), std::bind1st(std::mem_fun(&Block::invalidate_transfer), this));
  m_transfers.erase(split, m_transfers.end());
  
  if (m_transfers.empty() || m_transfers.back() != transfer)
    throw internal_error("Block::completed(...) m_transfers.empty() || m_transfers.back() != transfer.");

  return m_parent->is_all_finished();
}

// Mark a non-leading transfer as having received dissimilar data to
// the leader. It is then marked as erased so that we know its data
// was not used, yet keep it in m_transfers so as not to cause a
// re-download.
void
Block::transfer_dissimilar(BlockTransfer* transfer) {
  if (!transfer->is_not_leader())
    throw internal_error("Block::transfer_dissimilar(...) !transfer->is_not_leader().");

  m_notStalled -= transfer->stall() == 0;
  
  transfer->set_state(BlockTransfer::STATE_ERASED);
  transfer->set_position(0);
  transfer->set_block(NULL);
}

void
Block::stalled_transfer(BlockTransfer* transfer) {
  if (transfer->stall() == 0) {
    if (m_notStalled == 0)
      throw internal_error("Block::stalled(...) m_notStalled == 0.");

    m_notStalled--;

    // Do magic here.
  }

  transfer->set_stall(transfer->stall() + 1);
}

void
Block::change_leader(BlockTransfer* transfer) {
  if (m_leader == transfer)
    throw internal_error("Block::change_leader(...) m_leader == transfer.");

  if (m_leader != NULL)
    m_leader->set_state(BlockTransfer::STATE_NOT_LEADER);

  m_leader = transfer;
  m_leader->set_state(BlockTransfer::STATE_LEADER);
}

void
Block::failed_leader() {
  if (m_leader == NULL)
    throw internal_error("Block::change_leader(...) m_leader == transfer.");
  
  m_leader->set_state(BlockTransfer::STATE_LEADER);
  m_leader = NULL;
}

BlockTransfer*
Block::find_queued(const PeerInfo* p) {
  transfer_list_type::iterator itr = std::find_if(m_queued.begin(), m_queued.end(), rak::equal(p, std::mem_fun(&BlockTransfer::peer_info)));

  if (itr == m_queued.end())
    return NULL;
  else
    return *itr;
}

const BlockTransfer*
Block::find_queued(const PeerInfo* p) const {
  transfer_list_type::const_iterator itr = std::find_if(m_queued.begin(), m_queued.end(), rak::equal(p, std::mem_fun(&BlockTransfer::peer_info)));

  if (itr == m_queued.end())
    return NULL;
  else
    return *itr;
}

BlockTransfer*
Block::find_transfer(const PeerInfo* p) {
  transfer_list_type::iterator itr = std::find_if(m_transfers.begin(), m_transfers.end(), rak::equal(p, std::mem_fun(&BlockTransfer::peer_info)));

  if (itr == m_transfers.end())
    return NULL;
  else
    return *itr;
}

const BlockTransfer*
Block::find_transfer(const PeerInfo* p) const {
  transfer_list_type::const_iterator itr = std::find_if(m_transfers.begin(), m_transfers.end(), rak::equal(p, std::mem_fun(&BlockTransfer::peer_info)));

  if (itr == m_transfers.end())
    return NULL;
  else
    return *itr;
}

}
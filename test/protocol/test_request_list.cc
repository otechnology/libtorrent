#include "config.h"

#include "test_request_list.h"

#include "torrent/exceptions.h"
#include "torrent/peer/peer_info.h"
#include "download/delegator.h"
#include "protocol/peer_chunks.h"
#include "protocol/request_list.h"
#include "rak/socket_address.h"

CPPUNIT_TEST_SUITE_REGISTRATION(TestRequestList);

static uint32_t
chunk_index_size(uint32_t index) {
  return 1 << 10;
}

static void
transfer_list_void() {
  // std::cout << "list_void" << std::endl;
}

static void
transfer_list_completed(torrent::TransferList* transfer_list, uint32_t index) {
  torrent::TransferList::iterator itr = transfer_list->find(index);

  // std::cout << "list_completed:" << index << " found: " << (itr != transfer_list->end()) << std::endl;

  CPPUNIT_ASSERT(itr != transfer_list->end());

  transfer_list->erase(itr);
}

// Move to support header:
#define SETUP_DELEGATOR(fpc_prefix)                                     \
  torrent::Delegator* delegator = new torrent::Delegator;               \
  delegator->slot_chunk_find() = std::tr1::bind(&fpc_prefix ## _find_peer_chunk, std::tr1::placeholders::_1, std::tr1::placeholders::_2); \
  delegator->slot_chunk_size() = std::tr1::bind(&chunk_index_size, std::tr1::placeholders::_1); \
  delegator->transfer_list()->slot_canceled()  = std::tr1::bind(&transfer_list_void); \
  delegator->transfer_list()->slot_queued()    = std::tr1::bind(&transfer_list_void); \
  delegator->transfer_list()->slot_completed() = std::tr1::bind(&transfer_list_completed, delegator->transfer_list(), std::tr1::placeholders::_1); \
  delegator->transfer_list()->slot_corrupt()   = std::tr1::bind(&transfer_list_void);

// Set bitfield size...
#define SETUP_PEER_CHUNKS()                                             \
  rak::socket_address peer_info_address;                                \
  torrent::PeerInfo* peer_info = new torrent::PeerInfo(peer_info_address.c_sockaddr()); \
  torrent::PeerChunks* peer_chunks =  new torrent::PeerChunks;          \
  peer_chunks->set_peer_info(peer_info);

#define CLEANUP_PEER_CHUNKS()                   \
  delete peer_chunks;                           \
  delete peer_info;

#define SETUP_REQUEST_LIST()                                            \
  torrent::RequestList* request_list = new torrent::RequestList;        \
  request_list->set_delegator(delegator);                               \
  request_list->set_peer_chunks(peer_chunks);

#define SETUP_ALL(fpc_prefix)                   \
  SETUP_DELEGATOR(basic);                       \
  SETUP_PEER_CHUNKS();                          \
  SETUP_REQUEST_LIST();
  
#define CLEANUP_ALL(fpc_prefix)                 \
  CLEANUP_PEER_CHUNKS();                        \
  delete delegator;                             \
  delete request_list;

#define CLEAR_TRANSFERS(fpc_prefix)           \
  delegator->transfer_list()->clear();

//
//
//

#define VERIFY_PIECE_IS_LEADER(piece)                                   \
  CPPUNIT_ASSERT(request_list->transfer() != NULL);                      \
  CPPUNIT_ASSERT(request_list->transfer()->is_leader());                 \
  CPPUNIT_ASSERT(request_list->transfer()->peer_info() != NULL);         \
  CPPUNIT_ASSERT(request_list->transfer()->peer_info() == peer_info);

#define VERIFY_TRANSFER_COUNTER(transfer, count)                        \
  CPPUNIT_ASSERT(transfer != NULL);                                     \
  CPPUNIT_ASSERT(transfer->peer_info() != NULL);                        \
  CPPUNIT_ASSERT(transfer->peer_info()->transfer_counter() == count);


//
// Basic tests:
//

static uint32_t
basic_find_peer_chunk(torrent::PeerChunks* peerChunk, bool highPriority) {
  return 0;
}

void
TestRequestList::test_basic() {
  SETUP_ALL(basic);

  CPPUNIT_ASSERT(!request_list->is_downloading());
  CPPUNIT_ASSERT(!request_list->is_interested_in_active());

  CPPUNIT_ASSERT(!request_list->has_index(0));
  CPPUNIT_ASSERT(!request_list->has_index(1));
  CPPUNIT_ASSERT(!request_list->has_index(1024));

  CPPUNIT_ASSERT(request_list->queued_empty());
  CPPUNIT_ASSERT(request_list->queued_size() == 0);
  CPPUNIT_ASSERT(request_list->canceled_empty());
  CPPUNIT_ASSERT(request_list->canceled_size() == 0);

  CPPUNIT_ASSERT(request_list->calculate_pipe_size(1024 * 0) == 2);
  CPPUNIT_ASSERT(request_list->calculate_pipe_size(1024 * 10) == 12);

  CPPUNIT_ASSERT(request_list->transfer() == NULL);

  CLEANUP_ALL();
}

void
TestRequestList::test_single_request() {
  SETUP_ALL(basic);

  const torrent::Piece* piece = request_list->delegate();
  // std::cout << piece->index() << ' ' << piece->offset() << ' ' << piece->length() << std::endl;
  // std::cout << peer_info->transfer_counter() << std::endl;

  CPPUNIT_ASSERT(request_list->downloading(*piece));

  VERIFY_PIECE_IS_LEADER(*piece);
  VERIFY_TRANSFER_COUNTER(request_list->transfer(), 1);

  request_list->transfer()->adjust_position(piece->length());

  CPPUNIT_ASSERT(request_list->transfer()->is_finished());

  request_list->finished();

  CPPUNIT_ASSERT(peer_info->transfer_counter() == 0);

  CLEANUP_ALL();
}

void
TestRequestList::test_single_canceled() {
  SETUP_ALL(basic);

  const torrent::Piece* piece = request_list->delegate();
  // std::cout << piece->index() << ' ' << piece->offset() << ' ' << piece->length() << std::endl;
  // std::cout << peer_info->transfer_counter() << std::endl;

  CPPUNIT_ASSERT(request_list->downloading(*piece));

  VERIFY_PIECE_IS_LEADER(*piece); // REMOVE
  VERIFY_TRANSFER_COUNTER(request_list->transfer(), 1); // REMOVE

  request_list->transfer()->adjust_position(piece->length() / 2);

  CPPUNIT_ASSERT(!request_list->transfer()->is_finished());

  request_list->skipped();

  // The transfer remains in Block until it gets the block has a
  // transfer trigger finished.

  // TODO: We need to have a way of clearing disowned transfers, then
  // make transfer list delete Block's(?) with those before dtor...

  CPPUNIT_ASSERT(peer_info->transfer_counter() == 0);

  CLEAR_TRANSFERS();
  CLEANUP_ALL();
}

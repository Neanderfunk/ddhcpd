#include "block.h"

#include <errno.h>
#include <math.h>

#include "dhcp.h"
#include "dhcp_options.h"
#include "logger.h"
#include "statistics.h"
#include "tools.h"

// TODO define sane value
#define UPDATE_CLAIM_MAX_BLOCKS 32

int block_alloc(ddhcp_block* block) {
  DEBUG("block_alloc(block)\n");

  if (!block) {
    WARNING("block_alloc(...): No block given to initialize\n");
    return 1;
  }

  // Do not allocate memory and initialise, when the block is already allocated
  if (block->addresses) {
    return 0;
  }

  block->addresses = calloc(block->subnet_len, sizeof(struct dhcp_lease));

  if (!block->addresses) {
    WARNING("block_alloc(...): Failed to allocate memory for lease management on block %i\n", block->index);
    return 1;
  }

  for (unsigned int index = 0; index < block->subnet_len; index++) {
    block->addresses[index].state = FREE;
    block->addresses[index].lease_end = 0;
  }

  return 0;
}

ATTR_NONNULL(2) int block_own(ddhcp_block* block, ddhcp_config* config) {
  if (!block) {
    WARNING("block_own(...): No block given to own\n");
    return 1;
  }

  if (block_alloc(block)) {
    WARNING("block_own(...): Failed to initialize block %i for owning\n", block->index);
    return 1;
  }

  block->state = DDHCP_OURS;
  block->first_claimed = time(NULL);
  NODE_ID_CP(&block->node_id, &config->node_id);
  return 0;
}

ATTR_NONNULL_ALL void block_free(ddhcp_block* block) {
  DEBUG("block_free(%i)\n", block->index);

  if (block->state != DDHCP_BLOCKED) {
    NODE_ID_CLEAR(&block->node_id);
    block->state = DDHCP_FREE;
  }

  if (block->addresses) {
    DEBUG("block_free(%i): Freeing DHCP leases\n", block->index);
    free(block->addresses);
    block->addresses = NULL;
    // Reset needless timeout
    block->needless_since = 0;
  }
}

ATTR_NONNULL_ALL ddhcp_block* block_find_free(ddhcp_config* config) {
  DEBUG("block_find_free(config)\n");
  ddhcp_block* block = config->blocks;

  ddhcp_block_list free_blocks;
  INIT_LIST_HEAD(&free_blocks);
  uint32_t num_free_blocks = 0;

  for (uint32_t i = 0; i < config->number_of_blocks; i++) {
    if (block->state == DDHCP_FREE) {
      list_add_tail((&block->tmp_list), &free_blocks);
      num_free_blocks++;
    }

    block++;
  }

  DEBUG("block_find_free(...): found %i free blocks\n", num_free_blocks);

  ddhcp_block* random_free = NULL;
  uint32_t r = ~0u;

  if (num_free_blocks > 0) {
    r = (uint32_t)rand() % num_free_blocks;
  } else {
    DEBUG("block_find_free(...): no free block found\n");
    return NULL;
  }

  struct list_head* pos, *q;

  list_for_each_safe(pos, q, &free_blocks) {
    block = list_entry(pos, ddhcp_block, tmp_list);
    list_del(pos);

    if (r == 0) {
      random_free = block;
      break;
    }

    r--;
  }

#if LOG_LEVEL_LIMIT >= LOG_WARNING
  if (random_free) {
    DEBUG("block_find_free(...): found block %i\n", random_free->index);
  } else {
    WARNING("block_find_free(...): no free block found\n");
  }
#endif

  return random_free;
}

ATTR_NONNULL_ALL int block_claim(int32_t num_blocks, ddhcp_config* config) {
  DEBUG("block_claim(count:%i, config)\n", num_blocks);

  // Handle blocks already in claiming process
  struct list_head* pos, *q;
  time_t now = time(NULL);

  list_for_each_safe(pos, q, &config->claiming_blocks) {
    ddhcp_block* block = list_entry(pos, ddhcp_block, claim_list);

    if (block->claiming_counts == 3) {
      if ( block_own(block, config) > 0) {
        ERROR("block_claim(...): Claiming block (%i) failed, resetting claim counter.", block->index);
        block->claiming_counts = 0;
      } else {
        //Reduce number of blocks we need to claim
        num_blocks--;
        INFO("block_claim(...): block %i claimed after 3 claims.\n", block->index);
      }

      list_del(pos);
      config->claiming_blocks_amount--;
    } else if (block->state != DDHCP_CLAIMING) {
      DEBUG("block_claim(...): block %i is no longer marked for claiming\n", block->index);
      list_del(pos);
      config->claiming_blocks_amount--;
    }
  }

  // Do we still need more, then lets find some.
  if (num_blocks > config->claiming_blocks_amount) {
    // find num_blocks - config->claiming_blocks_amount free blocks
    uint32_t needed_blocks = (uint32_t) num_blocks - config->claiming_blocks_amount;

    for (uint32_t i = 0; i < needed_blocks; i++) {
      ddhcp_block* block = block_find_free(config);

      if (block) {
        block->state = DDHCP_CLAIMING;
        block->claiming_counts = 0;
        block->timeout = now + config->tentative_timeout;
        list_add_tail(&(block->claim_list), &config->claiming_blocks);
        config->claiming_blocks_amount++;
      } else {
        // We are short on free blocks in the network.
        WARNING("block_claim(...): Network has no free blocks left!\n");
        // TODO In a future version we could start to forward DHCP requests
        //      to other servers.
      }
    }
  }

  // TODO Sort blocks in claiming process by number of claims already processed.

  // TODO If we have more blocks in claiming process than we need, drop the tail
  //      of blocks for which we had less claim announcements.

  if (config->claiming_blocks_amount < 1) {
    DEBUG("block_claim(...): No blocks need claiming.\n");
    return 0;
  }

  // Send claim message for all blocks in claiming process.
  struct ddhcp_mcast_packet* packet = new_ddhcp_packet(DDHCP_MSG_INQUIRE, config);

  if (!packet) {
    WARNING("block_claim(...): Failed to allocate ddhcpd mcast packet.\n");
    return -ENOMEM;
  }

  packet->count = config->claiming_blocks_amount;

  packet->payload = calloc(config->claiming_blocks_amount, sizeof(struct ddhcp_payload));

  if (!packet->payload) {
    free(packet);
    WARNING("block_claim(...): Failed to allocate ddhcpd mcast packet payload.\n");
    return -ENOMEM;
  }

  int index = 0;
  ddhcp_block* block;

  list_for_each_entry(block, &config->claiming_blocks, claim_list) {
    packet->payload[index].block_index = block->index;
    packet->payload[index].timeout = 0;
    packet->payload[index].reserved = 0;
    index++;
  }

  statistics_record(config, STAT_MCAST_SEND_PKG, 1);
  statistics_record(config, STAT_MCAST_SEND_INQUIRE, 1);
  ssize_t bytes_send = send_packet_mcast(packet, DDHCP_SKT_MCAST(config));
  statistics_record(config, STAT_MCAST_SEND_BYTE, (long int) bytes_send);

  if (bytes_send > 0) {
    list_for_each_entry(block, &config->claiming_blocks, claim_list) {
      block->claiming_counts++;
    }
  } else {
    DEBUG("block_claim(...): Send failed, no updates made.\n");
  }

  free(packet->payload);
  free(packet);
  return 0;
}

ATTR_NONNULL_ALL uint32_t block_num_free_leases(ddhcp_config* config) {
  DEBUG("block_num_free_leases(config)\n");

  ddhcp_block* block = config->blocks;
  uint32_t free_leases = 0;
#if LOG_LEVEL_LIMIT >= LOG_DEBUG
  uint32_t num_blocks = 0;
#endif

  for (uint32_t i = 0; i < config->number_of_blocks; i++) {
    if (block->state == DDHCP_OURS) {
      free_leases += dhcp_num_free(block);
#if LOG_LEVEL_LIMIT >= LOG_DEBUG
      num_blocks++;
#endif
    }

    block++;
  }

  DEBUG("block_num_free_leases(...): Found %lu free DHCP leases in OUR (%lu) blocks\n", free_leases, num_blocks);
  return free_leases;
}

ATTR_NONNULL_ALL uint32_t block_num_owned(ddhcp_config* config) {
  uint32_t owned_blocks = 0;
  ddhcp_block* block = config->blocks;
  for (uint32_t i = 0; i < config->number_of_blocks; i++) {
    if (block->state == DDHCP_OURS) {
      owned_blocks++;
    }
  }
  DEBUG("block_num_owned(...): We own %lu blocks\n", owned_blocks);
  return owned_blocks;
}

ATTR_NONNULL_ALL ddhcp_block* block_find_free_leases(ddhcp_config* config) {
  DEBUG("block_find_free_leases(config)\n");

  ddhcp_block* block = config->blocks;
  ddhcp_block* selected = NULL;

  for (uint32_t i = 0; i < config->number_of_blocks; i++) {
    if (block->state == DDHCP_OURS) {
      if (dhcp_has_free(block)) {
        if (selected) {
          // If observed block is claimed earlier, select that block.
          if (selected->first_claimed > block->first_claimed) {
            selected = block;
          }
        } else {
          selected = block;
        }
      }
    }

    block++;
  }

#if LOG_LEVEL_LIMIT >= LOG_DEBUG

  if (selected) {
    DEBUG("block_find_free_leases(...): Block %i selected\n", selected->index);
  } else {
    DEBUG("block_find_free_leases(...): No block found!\n");
  }

#endif

  return selected;
}

ATTR_NONNULL_ALL void block_drop_unused(ddhcp_config* config) {
  DEBUG("block_drop_unsued(config)\n");
  ddhcp_block* block = config->blocks;
  ddhcp_block* freeable_block = NULL;

  for (uint32_t i = 0; i < config->number_of_blocks; i++) {
    if (block->state == DDHCP_OURS) {
      if (dhcp_num_free(block) == config->block_size) {
        DEBUG("block_drop unused(...): block %i is unused.\n", block->index);

        if (freeable_block) {
          // If observed block is younger than current selected, select block.
          if (freeable_block->first_claimed < block->first_claimed) {
            freeable_block = block;
          }
        } else {
          freeable_block = block;
        }
      }
    }

    block++;
  }

  if (freeable_block) {
    time_t now = time(NULL);
    if ( freeable_block->needless_since == 0 ) {
      DEBUG("block_drop_unused(...): mark block %i to be needless\n",freeable_block->index);
      freeable_block->needless_since = now;
      // Set needless marks flag
      config->needless_marks = 1;
    } else {
      if ( freeable_block->needless_since <= now - config->block_needless_timeout) {
        DEBUG("block_drop_unused(...): free block %i.\n", freeable_block->index);
        block_free(freeable_block);
        config->needless_marks = 0;
      } else {
#if LOG_LEVEL_LIMIT >= LOG_DEBUG
        time_t time_left = now - config->block_needless_timeout - freeable_block->needless_since;
        DEBUG("block_drop_unused(...): free block %i in about %li seconds\n",freeable_block->index,time_left);
#endif
      }
    }
  }
}

ATTR_NONNULL_ALL static void _block_update_claim_send(struct ddhcp_mcast_packet* packet, time_t new_block_timeout, ddhcp_config* config) {
  DEBUG("block_update_claims_send(packet:%i,%li,config)\n",packet->count,new_block_timeout);

  statistics_record(config, STAT_MCAST_SEND_PKG, 1);
  statistics_record(config, STAT_MCAST_SEND_UPDATECLAIM, 1);
  ssize_t bytes_send = send_packet_mcast(packet, DDHCP_SKT_MCAST(config));
  statistics_record(config, STAT_MCAST_SEND_BYTE, (long int) bytes_send);
  // TODO? Stat the number of blocks reclaimed.

  if (bytes_send > 0) {
    // Update the timeout value of all contained blocks
    // iff the packet has been transmitted
    for (uint8_t i = 0; i < packet->count; i++) {
      uint32_t index = packet->payload[i].block_index;
      DEBUG("block_update_claims_send(...): updated claim for block %i\n", index);
      config->blocks[index].timeout = new_block_timeout;
    }
  } else {
    DEBUG("block_update_claims_send(...): Send failed, no updates made.\n");
  }
}

ATTR_NONNULL_ALL void block_update_claims(ddhcp_config* config) {
  DEBUG("block_update_claims(config)\n");
  uint32_t our_blocks = 0;
  ddhcp_block* block = config->blocks;
  time_t now = time(NULL);
  time_t timeout_factor = now + config->block_timeout - (time_t)(config->block_timeout / config->block_refresh_factor);

  // Determine if we need to run a full update claim run
  // we run through the list until we see one block which needs update.
  // Running a full update claims (see below) is much more expensive
  for (uint32_t i = 0; i < config->number_of_blocks; i++) {
    if (block->state == DDHCP_OURS && block->timeout < timeout_factor) {
      our_blocks++;
      break;
    }

    block++;
  }

  if (our_blocks == 0) {
    DEBUG("block_update_claims(...): No blocks need claim updates.\n");
    return;
  }

  struct ddhcp_mcast_packet* packet = new_ddhcp_packet(DDHCP_MSG_UPDATECLAIM, config);

  if (!packet) {
    WARNING("block_update_claims(...): Failed to allocate ddhcpd mcast packet.\n");
    return;
  }

  // Aggressively group blocks into packets, send packet iff
  // at least one block in a packet is below the baseline.

  packet->payload = calloc(UPDATE_CLAIM_MAX_BLOCKS, sizeof(struct ddhcp_payload));

  if (!packet->payload) {
    WARNING("block_update_claims(...): Failed to allocate ddhcpd packet payload.\n");
    free(packet);
    return;
  }

  block = config->blocks;
  uint8_t send_packet = 0;
  uint8_t index = 0;
  time_t new_block_timeout = now + config->block_timeout;

  for (uint32_t i = 0; i < config->number_of_blocks; i++) {
    if (block->state == DDHCP_OURS) {

      if (block->timeout < timeout_factor) {
        DEBUG("block_update_claims(...): update claim for block %i needed\n", block->index);
        send_packet = 1;
      }

      // The former reserved byte carries the number of active leases in the
      // block (at most 128, the largest block). It decides a conflict when
      // two nodes claim the same block, see ddhcp_block_process_claims.
      uint32_t active = dhcp_num_active(block, now);

      packet->payload[index].block_index = block->index;
      packet->payload[index].timeout     = config->block_timeout;
      packet->payload[index].reserved    = (uint16_t) (active > 255 ? 255 : active);

      index++;

      if (index == UPDATE_CLAIM_MAX_BLOCKS) {
        if (send_packet) {
          packet->count = index;
          send_packet = 0;
          _block_update_claim_send(packet, new_block_timeout, config);
        }

        index = 0;
      }
    }

    block++;
  }

  if (send_packet) {
    packet->count = index;
    _block_update_claim_send(packet, new_block_timeout, config);
  }

  free(packet->payload);
  free(packet);
}

ATTR_NONNULL_ALL void block_release(ddhcp_block* block, ddhcp_node_id node_id, struct in6_addr* owner, ddhcp_config* config) {
  time_t now = time(NULL);

  block->state = DDHCP_RELEASING;
  NODE_ID_CP(&block->node_id, &node_id);
  memcpy(&block->owner_address, owner, sizeof(struct in6_addr));
  // Longest a lease of ours can still be valid
  block->timeout = now + find_in_option_store_address_lease_time(&config->options) + DHCP_LEASE_SERVER_DELTA;
  block->needless_since = 0;

  INFO("block_release(...): block %i released, %u active leases\n", block->index, dhcp_num_active(block, now));

  if (!block->addresses) {
    return;
  }

  for (uint32_t i = 0; i < block->subnet_len; i++) {
    dhcp_lease* lease = block->addresses + i;

    if (lease->state == LEASED && lease->lease_end > now) {
      lease->handover = 1;
    } else {
      // Offers and expired leases are not handed over
      lease->state = FREE;
      lease->handover = 0;
    }
  }

  block_release_handover(block, config);
}

ATTR_NONNULL_ALL void block_release_handover(ddhcp_block* block, ddhcp_config* config) {
  if (block->state != DDHCP_RELEASING || !block->addresses) {
    return;
  }

  time_t now = time(NULL);

  for (uint32_t i = 0; i < block->subnet_len; i++) {
    dhcp_lease* lease = block->addresses + i;

    if (!lease->handover || lease->state != LEASED) {
      continue;
    }

    // The same message a node sends when a client of a foreign block asks
    // it for a renewal: the owner records the lease for this client, or
    // answers with LEASENAK when it has given the address to another one.
    ddhcp_mcast_packet* packet = new_ddhcp_packet(DDHCP_MSG_RENEWLEASE, config);

    if (!packet) {
      WARNING("block_release_handover(...): Failed to allocate ddhcpd mcast packet\n");
      return;
    }

    ddhcp_renew_payload payload;
    struct in_addr address;
    addr_add(&block->subnet, &address, (int) i);
    memcpy(&payload.chaddr, &lease->chaddr, 16);
    memcpy(&payload.address, &address, sizeof(struct in_addr));
    payload.xid = 0;
    payload.lease_seconds = lease->lease_end > now ? (uint32_t) (lease->lease_end - now) : 0;
    packet->renew_payload = &payload;

    DEBUG("block_release_handover(...): hand over lease %i of block %i\n", i, block->index);
    statistics_record(config, STAT_DIRECT_SEND_PKG, 1);
    ssize_t bytes_send = send_packet_direct(packet, &block->owner_address, DDHCP_SKT_SERVER(config));
    statistics_record(config, STAT_DIRECT_SEND_BYTE, (long int) bytes_send);
    UNUSED(bytes_send);
    free(packet);
  }
}

ATTR_NONNULL_ALL void block_release_answer(uint32_t* address, uint8_t* chaddr, int ack, ddhcp_config* config) {
  struct in_addr addr;
  memcpy(&addr, address, sizeof(struct in_addr));
  uint32_t offset = ntohl(addr.s_addr) - ntohl(config->prefix.s_addr);
  uint32_t block_index = offset / config->block_size;

  if (block_index >= config->number_of_blocks) {
    return;
  }

  ddhcp_block* block = config->blocks + block_index;
  uint32_t lease_index = offset % config->block_size;

  if (block->state != DDHCP_RELEASING || !block->addresses) {
    return;
  }

  dhcp_lease* lease = block->addresses + lease_index;

  if (!lease->handover || memcmp(lease->chaddr, chaddr, 16) != 0) {
    return;
  }

  // Either way the lease is the new owner's business now. On a NAK the owner
  // has given the address to another client; ours gets a NAK on its next
  // renewal, which goes to the owner.
  INFO("block_release_answer(...): lease %i of block %i handed over (%s)\n", lease_index, block->index, ack ? "ACK" : "NAK");
  lease->handover = 0;
  lease->state = FREE;
}

ATTR_NONNULL_ALL static void block_release_finish(ddhcp_block* block, ddhcp_config* config) {
  INFO("block_release_finish(...): block %i handed over\n", block->index);

  if (block->addresses) {
    free(block->addresses);
    block->addresses = NULL;
  }

  // The winner owns it; its next claim refreshes the timeout.
  block->state = DDHCP_CLAIMED;
  block->timeout = time(NULL) + config->block_timeout;
}

ATTR_NONNULL_ALL void block_check_timeouts(ddhcp_config* config) {
  DEBUG("block_check_timeouts(config)\n");
  ddhcp_block* block = config->blocks;
  time_t now = time(NULL);

  for (uint32_t i = 0; i < config->number_of_blocks; i++) {
    if (block->state == DDHCP_RELEASING) {
      // Done when no lease of ours is active any more, at the latest when
      // the longest lease has run out (block->timeout).
      if (block->addresses) {
        dhcp_check_timeouts(block);
      }

      if (block->timeout < now || !block->addresses || dhcp_num_active(block, now) == 0) {
        block_release_finish(block, config);
      } else {
        // Unanswered handovers are sent again on every house keeping run
        block_release_handover(block, config);
      }

      block++;
      continue;
    }

    if (block->timeout < now && block->state != DDHCP_BLOCKED && block->state != DDHCP_FREE) {
      INFO("block_check_timeouts(...): Block %i FREE through timeout.\n", block->index);
      block_free(block);
    }

    if (block->state == DDHCP_OURS) {
      dhcp_check_timeouts(block);
    } else if (block->addresses) {
      int free_leases = dhcp_check_timeouts(block);

      if (free_leases == block->subnet_len) {
        block_free(block);
      }
    }

    block++;
  }
}

ATTR_NONNULL_ALL void block_show_status(int fd, ddhcp_config* config) {
  ddhcp_block* block = config->blocks;
  dprintf(fd, "block size/number\t%u/%u \n", config->block_size, config->number_of_blocks);
  dprintf(fd, "      tentative timeout\t%u\n", config->tentative_timeout);
  dprintf(fd, "      timeout\t%u\n", config->block_timeout);
  dprintf(fd, "      refresh factor\t%u\n", config->block_refresh_factor);
  dprintf(fd, "      spare leases needed\t%u\n", config->spare_leases_needed);
  dprintf(fd, "      network: %s/%i \n", inet_ntoa(config->prefix), config->prefix_len);

  char node_id[17];

  for (uint32_t j = 0; j < 8; j++) {
    sprintf(node_id + 2 * j, "%02X", config->node_id[j]);
  }

  node_id[16] = '\0';

  dprintf(fd, "node id\t%s\n", node_id);

  dprintf(fd, "ddhcp blocks\n");
  dprintf(fd, "index\tstate\towner\t\t\tclaim\tleases\ttimeout\n");

  time_t now = time(NULL);

  uint32_t num_reserved_blocks = 0;

  for (uint32_t i = 0; i < config->number_of_blocks; i++) {
    uint32_t free_leases = 0;
    uint32_t offered_leases = 0;

    if (block->addresses) {
      free_leases = dhcp_num_free(block);
      offered_leases = dhcp_num_offered(block);
    }

    for (uint32_t j = 0; j < 8; j++) {
      sprintf(node_id + 2 * j, "%02X", block->node_id[j]);
    }

    node_id[16] = '\0';

    char leases[16];

    if (block->addresses) {
      snprintf(leases, sizeof(leases), "%u/%u", offered_leases, config->block_size - free_leases - offered_leases);
    } else {
      leases[0] = '-';
      leases[1] = '\0';
    }

    double timeout = difftime(block->timeout, now);

    if (timeout > 0) {
      num_reserved_blocks++;
      dprintf(fd, "%i\t%i\t%s\t%u\t%s\t%g\n", block->index, block->state, node_id, block->claiming_counts, leases, timeout);
    }

    block++;
  }

  dprintf(fd, "\nblocks in use: %i\n", num_reserved_blocks);
}

void block_unmark_needless(ddhcp_config* config){
  DEBUG("block_unmark_needless(config)\n");
  ddhcp_block* block = config->blocks;
  for (uint32_t i = 0; i < config->number_of_blocks; i++) {
#if LOG_LEVEL_LIMIT >= LOG_DEBUG
    if (block->needless_since > 0) {
      DEBUG("block_unmark_needless(...): Unmark block %i\n",block->index);
    }
#endif
    block->needless_since = 0;
    block++;
  }
  // Remove needless marks flag
  config->needless_marks = 0;
}

ATTR_NONNULL_ALL void block_show_summary(int fd, ddhcp_config* config) {
  ddhcp_block* block = config->blocks;
  uint32_t blocks_free = 0, blocks_ours = 0, blocks_claiming = 0, blocks_foreign = 0;
  uint32_t leases_leased = 0, leases_offered = 0, leases_free = 0;

  for (uint32_t i = 0; i < config->number_of_blocks; i++, block++) {
    switch (block->state) {
    case DDHCP_FREE:
      blocks_free++;
      break;

    case DDHCP_TENTATIVE:
    case DDHCP_CLAIMING:
      blocks_claiming++;
      break;

    // BLOCKED is never set today; counted with the foreign blocks, so that
    // total = free + ours + claiming + foreign always holds.
    case DDHCP_CLAIMED:
    case DDHCP_BLOCKED:
    case DDHCP_RELEASING:
      blocks_foreign++;
      break;

    case DDHCP_OURS:
      blocks_ours++;
      break;

    default:
      break;
    }

    if (block->state != DDHCP_OURS || !block->addresses) {
      continue;
    }

    for (uint32_t j = 0; j < block->subnet_len; j++) {
      dhcp_lease* lease = block->addresses + j;

      switch (lease->state) {
      case LEASED:
        leases_leased++;
        break;

      case OFFERED:
        leases_offered++;
        break;

      case FREE:
        leases_free++;
        break;

      default:
        break;
      }
    }
  }

  dprintf(fd, "learning %u\n", config->learning_phase);
  dprintf(fd, "blocks_total %u\n", config->number_of_blocks);
  dprintf(fd, "blocks_free %u\n", blocks_free);
  dprintf(fd, "blocks_ours %u\n", blocks_ours);
  dprintf(fd, "blocks_claiming %u\n", blocks_claiming);
  dprintf(fd, "blocks_foreign %u\n", blocks_foreign);
  dprintf(fd, "leases_total %u\n", blocks_ours * config->block_size);
  dprintf(fd, "leases_leased %u\n", leases_leased);
  dprintf(fd, "leases_offered %u\n", leases_offered);
  dprintf(fd, "leases_free %u\n", leases_free);

#ifdef DDHCPD_STATISTICS
  dprintf(fd, "roaming_in %li\n", config->statistics[STAT_DIRECT_SEND_RENEWLEASE]);
  dprintf(fd, "roaming_out %li\n", config->statistics[STAT_DIRECT_RECV_RENEWLEASE]);
  dprintf(fd, "roaming_ack_sent %li\n", config->statistics[STAT_DIRECT_SEND_LEASEACK]);
  dprintf(fd, "roaming_ack_received %li\n", config->statistics[STAT_DIRECT_RECV_LEASEACK]);
  dprintf(fd, "roaming_nak_sent %li\n", config->statistics[STAT_DIRECT_SEND_LEASENAK]);
  dprintf(fd, "roaming_nak_received %li\n", config->statistics[STAT_DIRECT_RECV_LEASENAK]);
  dprintf(fd, "dhcp_discover %li\n", config->statistics[STAT_DHCP_RECV_DISCOVER]);
  dprintf(fd, "dhcp_offer %li\n", config->statistics[STAT_DHCP_SEND_OFFER]);
  dprintf(fd, "dhcp_request %li\n", config->statistics[STAT_DHCP_RECV_REQUEST]);
  dprintf(fd, "dhcp_ack %li\n", config->statistics[STAT_DHCP_SEND_ACK]);
  dprintf(fd, "dhcp_nak %li\n", config->statistics[STAT_DHCP_SEND_NAK]);
  dprintf(fd, "dhcp_release %li\n", config->statistics[STAT_DHCP_RECV_RELEASE]);
  dprintf(fd, "dhcp_inform %li\n", config->statistics[STAT_DHCP_RECV_INFORM]);
#endif

  block = config->blocks;

  for (uint32_t i = 0; i < config->number_of_blocks; i++, block++) {
    if (block->state != DDHCP_OURS || !block->addresses) {
      continue;
    }

    for (uint32_t j = 0; j < block->subnet_len; j++) {
      dhcp_lease* lease = block->addresses + j;

      if (lease->state == LEASED) {
        uint8_t* m = lease->chaddr;
        dprintf(fd, "leased %02x:%02x:%02x:%02x:%02x:%02x\n", m[0], m[1], m[2], m[3], m[4], m[5]);
      }
    }
  }
}

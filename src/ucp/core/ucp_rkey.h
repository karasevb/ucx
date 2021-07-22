/**
 * Copyright (C) Mellanox Technologies Ltd. 2020.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifndef UCP_RKEY_H_
#define UCP_RKEY_H_

#include "ucp_types.h"

#include <ucp/core/ucp_context.h>
#include <ucp/proto/proto_select.h>


/* Remote keys with that many remote MDs or less would be allocated from a
 * memory pool.
 */
#define UCP_RKEY_MPOOL_MAX_MD     2/*3*/


/**
 * UCT remote key along with component handle which should be used to release it.
 *
 */
typedef struct ucp_tl_rkey {
    uct_rkey_bundle_t             rkey;
    uct_component_h               cmpt;
} ucp_tl_rkey_t;


/**
 * Rkey flags
 */
enum {
    UCP_RKEY_DESC_FLAG_POOL       = UCS_BIT(0)  /* Descriptor was allocated from pool
                                                   and must be retuned to pool, not free */
};


/**
 * Rkey configuration key
 */
struct ucp_rkey_config_key {
    /* Which *remote* MDs have valid memory handles */
    ucp_md_map_t           md_map;

    /* Endpoint configuration index */
    ucp_worker_cfg_index_t ep_cfg_index;

    /* Remove system device id */
    ucs_sys_device_t       sys_dev;

    /* Remote memory type */
    ucs_memory_type_t      mem_type;
};


/**
 * Rkey configuration
 */
typedef struct {
    /* Configuration key */
    ucp_rkey_config_key_t    key;

    /* Put-short thresholds */
    ucp_proto_select_short_t put_short;

    /* Remote system topology distance of each lane from the remote memory
     * buffer. The number of valid entries is according to the number of lanes
     * defined by the configuration at index "key.ep_cfg_index".
     */
    ucs_sys_dev_distance_t   lanes_distance[UCP_MAX_LANES];

    /* Protocol selection data */
    ucp_proto_select_t       proto_select;
} ucp_rkey_config_t;

typedef enum {
    UCP_RMA_BASIC_PROTO = 0,
    UCP_RMA_SW_PROTO    = 1
} ucp_rma_proto_type_t;

typedef enum {
    UCP_AMO_BASIC_PROTO = 0,
    UCP_AMO_SW_PROTO    = 1
} ucp_amo_proto_type_t;

/**
 * Remote memory key structure.
 * Contains remote keys for UCT MDs.
 * md_map specifies which MDs from the current context are present in the array.
 * The array itself contains only the MDs specified in md_map, without gaps.
 */
typedef struct ucp_rkey {
    union {
        /* cached values for the most recent endpoint configuration */
        struct {
            uct_rkey_t                rma_rkey;     /* Key to use for RMAs */
            uct_rkey_t                amo_rkey;     /* Key to use for AMOs */
            uint8_t/*ucp_rma_proto_type_t*/      amo_proto;    /* Protocol for AMOs */
            uint8_t/*ucp_rma_proto_type_t*/      rma_proto;    /* Protocol for RMAs */
            ucp_worker_cfg_index_t    ep_cfg_index; /* EP configuration relevant for the cache */
            ucp_lane_index_t          rma_lane;     /* Lane to use for RMAs */
            ucp_lane_index_t          amo_lane;     /* Lane to use for AMOs */
            uint8_t                   flags;        /* Rkey flags */

            //ucp_amo_proto_t           *amo_proto;   /* Protocol for AMOs */
            //ucp_rma_proto_t           *rma_proto;   /* Protocol for RMAs */
            ssize_t                   max_put_short;/* Cached value of max_put_short */
        } cache;
        struct {
            ucp_worker_cfg_index_t        cfg_index;    /* Rkey configuration index */
            ucs_memory_type_t             mem_type;     /* Memory type of remote key memory */
            uint8_t                       gap[13]; // to align the next flags with the cache.flags
            uint8_t                       flags;        /* Rkey flags */

        };
    };

#if ENABLE_PARAMS_CHECK
    ucp_ep_h                      ep;
#endif
    ucp_md_map_t                  md_map;       /* Which *remote* MDs have valid memory handles */
    ucp_tl_rkey_t                 tl_rkey[0];   /* UCT rkey for every remote MD */
} ucp_rkey_t;


#define UCP_RKEY_RESOLVE_NOCHECK(_rkey, _ep, _op_type) \
    ({ \
        ucs_status_t _status_nc = UCS_OK; \
        if (ucs_unlikely((_ep)->cfg_index != (_rkey)->cache.ep_cfg_index)) { \
            ucp_rkey_resolve_inner(_rkey, _ep); \
        } \
        if (ucs_unlikely((_rkey)->cache._op_type##_lane == UCP_NULL_LANE)) { \
            ucs_error("remote memory is unreachable " \
                      "(remote md_map 0x%" PRIx64")", \
                      (_rkey)->md_map); \
            _status_nc = UCS_ERR_UNREACHABLE; \
        } \
        _status_nc; \
    })


#if ENABLE_PARAMS_CHECK
#define UCP_RKEY_RESOLVE(_rkey, _ep, _op_type) \
    ({ \
        ucs_status_t _status; \
        if ((_rkey)->ep != (_ep)) { \
            ucs_error("cannot use a remote key on a different endpoint than it was unpacked on"); \
            _status = UCS_ERR_INVALID_PARAM; \
        } else { \
            _status = UCP_RKEY_RESOLVE_NOCHECK(_rkey, _ep, _op_type); \
        } \
        _status; \
    })
#else
#define UCP_RKEY_RESOLVE  UCP_RKEY_RESOLVE_NOCHECK
#endif


void ucp_rkey_resolve_inner(ucp_rkey_h rkey, ucp_ep_h ep);


ucp_lane_index_t ucp_rkey_find_rma_lane(ucp_context_h context,
                                        const ucp_ep_config_t *config,
                                        ucs_memory_type_t mem_type,
                                        const ucp_lane_index_t *lanes,
                                        ucp_rkey_h rkey,
                                        ucp_lane_map_t ignore,
                                        uct_rkey_t *uct_rkey_p);


size_t ucp_rkey_packed_size(ucp_context_h context, ucp_md_map_t md_map,
                            ucs_sys_device_t sys_dev, uint64_t sys_dev_map);


void ucp_rkey_packed_copy(ucp_context_h context, ucp_md_map_t md_map,
                          ucs_memory_type_t mem_type, void *buffer,
                          const void *uct_rkeys[]);


ssize_t
ucp_rkey_pack_uct(ucp_context_h context, ucp_md_map_t md_map,
                  const uct_mem_h *memh, const ucp_memory_info_t *mem_info,
                  uint64_t sys_dev_map,
                  const ucs_sys_dev_distance_t *sys_distance, void *buffer);


ucs_status_t ucp_ep_rkey_unpack_internal(ucp_ep_h ep, const void *buffer,
                                         size_t length, ucp_rkey_h *rkey_p);


void ucp_rkey_dump_packed(const void *buffer, size_t length,
                          ucs_string_buffer_t *strb);


void ucp_rkey_config_dump_brief(const ucp_rkey_config_key_t *rkey_config_key,
                                ucs_string_buffer_t *strb);


void ucp_rkey_proto_select_dump(ucp_worker_h worker,
                                ucp_worker_cfg_index_t rkey_cfg_index,
                                ucs_string_buffer_t *strb);

#endif

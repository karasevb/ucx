/**
 * Copyright (C) Mellanox Technologies Ltd. 2001-2020.  ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "rndv.h"
#include "proto_rndv.inl"

/* TODO: Avoid dependency on tag (or other API) specifics, since this is common
 * basic rendezvous implementation.
 */
#include <ucp/tag/tag_rndv.h>
#include <ucp/tag/tag_match.inl>
#include <ucp/tag/offload.h>
#include <ucp/proto/proto_am.inl>
#include <ucs/datastruct/queue.h>


static UCS_F_ALWAYS_INLINE int
ucp_rndv_is_get_zcopy(ucp_request_t *req, ucp_context_h context)
{
    return ((context->config.ext.rndv_mode == UCP_RNDV_MODE_GET_ZCOPY) ||
            ((context->config.ext.rndv_mode == UCP_RNDV_MODE_AUTO) &&
             (!UCP_MEM_IS_GPU(req->send.mem_type) ||
              (req->send.length < context->config.ext.rndv_pipeline_send_thresh))));
}

static int ucp_rndv_is_recv_pipeline_needed(ucp_request_t *rndv_req,
                                            const ucp_rndv_rts_hdr_t *rndv_rts_hdr,
                                            const void *rkey_buf,
                                            ucs_memory_type_t mem_type,
                                            int is_get_zcopy_failed)
{
    const ucp_ep_config_t *ep_config = ucp_ep_config(rndv_req->send.ep);
    ucp_context_h context            = rndv_req->send.ep->worker->context;
    int found                        = 0;
    ucp_md_index_t md_index;
    uct_md_attr_t *md_attr;
    uint64_t mem_types;
    int i;

    for (i = 0;
         (i < UCP_MAX_LANES) &&
         (ep_config->key.rma_bw_lanes[i] != UCP_NULL_LANE); i++) {
        md_index = ep_config->md_index[ep_config->key.rma_bw_lanes[i]];
        if (context->tl_mds[md_index].attr.cap.access_mem_types
            & UCS_BIT(UCS_MEMORY_TYPE_HOST)) {
            found = 1;
            break;
        }
    }

    /* no host bw lanes for pipeline staging */
    if (!found) {
        return 0;
    }

    if (is_get_zcopy_failed) {
        return 1;
    }

    /* disqualify recv side pipeline if
     * a mem_type bw lane exist AND
     * lane can do RMA on remote mem_type
     */
    mem_types = UCS_BIT(mem_type);
    if (rndv_rts_hdr->address) {
        mem_types |= UCS_BIT(ucp_rkey_packed_mem_type(rkey_buf));
    }

    ucs_for_each_bit(md_index, ep_config->key.rma_bw_md_map) {
        md_attr = &context->tl_mds[md_index].attr;
        if (ucs_test_all_flags(md_attr->cap.reg_mem_types, mem_types)) {
            return 0;
        }
    }

    return 1;
}

static int ucp_rndv_is_put_pipeline_needed(uintptr_t remote_address,
                                           size_t length, size_t min_get_zcopy,
                                           size_t max_get_zcopy,
                                           int is_get_zcopy_failed)
{
    /* fallback to PUT pipeline if remote mem type is non-HOST memory OR
     * can't do GET ZCOPY */
    return ((remote_address == 0) || (max_get_zcopy == 0) ||
            (length < min_get_zcopy) || is_get_zcopy_failed);
}

size_t ucp_rndv_rts_pack(ucp_request_t *sreq, ucp_rndv_rts_hdr_t *rndv_rts_hdr,
                         size_t rndv_rts_hdr_size, uint16_t flags)
{
    ucp_worker_h worker = sreq->send.ep->worker;
    ssize_t packed_rkey_size;
    void *rkey_buf;

    rndv_rts_hdr->sreq.ep_id  = ucp_send_request_get_ep_remote_id(sreq);
    rndv_rts_hdr->sreq.req_id = ucp_request_get_id(sreq);
    rndv_rts_hdr->size        = sreq->send.length;
    rndv_rts_hdr->flags       = flags;

    /* Pack remote keys (which can be empty list) */
    if (UCP_DT_IS_CONTIG(sreq->send.datatype) &&
        ucp_rndv_is_get_zcopy(sreq, worker->context) &&
        (UCP_MEM_IS_HOST(sreq->send.mem_type) ||
         (sreq->send.state.dt.dt.contig.md_map != 0))) {
        /* pack rkey, ask target to do get_zcopy */
        rndv_rts_hdr->address = (uintptr_t)sreq->send.buffer;
        rkey_buf              = UCS_PTR_BYTE_OFFSET(rndv_rts_hdr,
                                                    rndv_rts_hdr_size);
        packed_rkey_size = ucp_rkey_pack_uct(worker->context,
                                             sreq->send.state.dt.dt.contig.md_map,
                                             sreq->send.state.dt.dt.contig.memh,
                                             sreq->send.mem_type, rkey_buf);
        if (packed_rkey_size < 0) {
            ucs_fatal("failed to pack rendezvous remote key: %s",
                      ucs_status_string((ucs_status_t)packed_rkey_size));
        }

        ucs_assert(packed_rkey_size <=
                   ucp_ep_config(sreq->send.ep)->rndv.rkey_size);
    } else {
        rndv_rts_hdr->address = 0;
        packed_rkey_size      = 0;
    }

    return rndv_rts_hdr_size + packed_rkey_size;
}

static size_t ucp_rndv_rtr_pack(void *dest, void *arg)
{
    ucp_request_t *rndv_req          = arg;
    ucp_rndv_rtr_hdr_t *rndv_rtr_hdr = dest;
    ucp_request_t *rreq              = rndv_req->super_req;
    ucp_ep_h ep                      = rndv_req->send.ep;
    ssize_t packed_rkey_size;

    /* Request ID of sender side (remote) */
    rndv_rtr_hdr->sreq_id = rreq->recv.remote_req_id;
    /* Request ID of receiver side (local) */
    rndv_rtr_hdr->rreq_id = ucp_request_get_id(rndv_req);

    /* Pack remote keys (which can be empty list) */
    if (UCP_DT_IS_CONTIG(rreq->recv.datatype)) {
        rndv_rtr_hdr->address = (uintptr_t)rreq->recv.buffer;
        rndv_rtr_hdr->size    = rndv_req->send.rndv_rtr.length;
        rndv_rtr_hdr->offset  = rndv_req->send.rndv_rtr.offset;

        packed_rkey_size = ucp_rkey_pack_uct(ep->worker->context,
                                             rreq->recv.state.dt.contig.md_map,
                                             rreq->recv.state.dt.contig.memh,
                                             rreq->recv.mem_type,
                                             rndv_rtr_hdr + 1);
        if (packed_rkey_size < 0) {
            return packed_rkey_size;
        }
    } else {
        rndv_rtr_hdr->address = 0;
        rndv_rtr_hdr->size    = 0;
        rndv_rtr_hdr->offset  = 0;
        packed_rkey_size      = 0;
    }

    return sizeof(*rndv_rtr_hdr) + packed_rkey_size;
}

UCS_PROFILE_FUNC(ucs_status_t, ucp_proto_progress_rndv_rtr, (self),
                 uct_pending_req_t *self)
{
    ucp_request_t *rndv_req = ucs_container_of(self, ucp_request_t, send.uct);
    size_t packed_rkey_size;
    ucs_status_t status;

    /* Send the RTR. The pack_cb will pack all the necessary fields in the RTR */
    packed_rkey_size = ucp_ep_config(rndv_req->send.ep)->rndv.rkey_size;
    status           = ucp_do_am_single(self, UCP_AM_ID_RNDV_RTR, ucp_rndv_rtr_pack,
                                        sizeof(ucp_rndv_rtr_hdr_t) + packed_rkey_size);
    if (ucs_unlikely(status != UCS_OK)) {
        if (ucs_likely(status == UCS_ERR_NO_RESOURCE)) {
            return UCS_ERR_NO_RESOURCE;
        }

        ucp_request_put(rndv_req);
    }

    /* Don't release rndv request in case of success, since it was sent to
     * a peer as a remote request ID */

    return UCS_OK;
}

ucs_status_t ucp_rndv_reg_send_buffer(ucp_request_t *sreq)
{
    ucp_ep_h ep = sreq->send.ep;
    ucp_md_map_t md_map;
    ucs_status_t status;

    if (UCP_DT_IS_CONTIG(sreq->send.datatype) &&
        ucp_rndv_is_get_zcopy(sreq, ep->worker->context)) {

        /* register a contiguous buffer for rma_get */
        md_map = ucp_ep_config(ep)->key.rma_bw_md_map;

        /* Pass UCT_MD_MEM_FLAG_HIDE_ERRORS flag, because registration may fail
         * if md does not support send memory type (e.g. CUDA memory). In this
         * case RTS will be sent with empty key, and sender will fallback to
         * PUT or pipeline protocols. */
        status = ucp_request_send_buffer_reg(sreq, md_map,
                                             UCT_MD_MEM_FLAG_HIDE_ERRORS);
        if (status != UCS_OK) {
            return status;
        }
    }

    return UCS_OK;
}

static UCS_F_ALWAYS_INLINE size_t
ucp_rndv_adjust_zcopy_length(size_t min_zcopy, size_t max_zcopy, size_t align,
                             size_t send_length, size_t offset, size_t length)
{
    size_t result_length, tail;

    /* ensure that the current length is over min_zcopy */
    result_length = ucs_max(length, min_zcopy);

    /* ensure that the current length is less than max_zcopy */
    result_length = ucs_min(result_length, max_zcopy);

    /* ensure that tail (rest of message) is over min_zcopy */
    ucs_assertv(send_length >= (offset + result_length),
                "send_length=%zu, offset=%zu, length=%zu",
                send_length, offset, result_length);
    tail = send_length - (offset + result_length);
    if (ucs_unlikely((tail != 0) && (tail < min_zcopy))) {
        /* ok, tail is less zcopy minimal & could not be processed as
         * standalone operation */
        /* check if we have room to increase current part and not
         * step over max_zcopy */
        if (result_length < (max_zcopy - tail)) {
            /* if we can increase length by min_zcopy - let's do it to
             * avoid small tail (we have limitation on minimal get zcopy) */
            result_length += tail;
        } else {
            /* reduce current length by align or min_zcopy value
             * to process it on next round */
            ucs_assert(result_length > ucs_max(min_zcopy, align));
            result_length -= ucs_max(min_zcopy, align);
        }
    }

    ucs_assertv(result_length >= min_zcopy, "length=%zu, min_zcopy=%zu",
                result_length, min_zcopy);
    ucs_assertv(((send_length - (offset + result_length)) == 0) ||
                ((send_length - (offset + result_length)) >= min_zcopy),
                "send_length=%zu, offset=%zu, length=%zu, min_zcopy=%zu",
                send_length, offset, result_length, min_zcopy);

    return result_length;
}

void ucp_rndv_req_send_ack(ucp_request_t *ack_req, ucp_request_t *req,
                           ucs_ptr_map_key_t remote_req_id, ucs_status_t status,
                           ucp_am_id_t am_id, const char *ack_str)
{
    if (am_id == UCP_AM_ID_RNDV_ATP) {
        ucs_assertv(req->send.state.dt.offset == req->send.length,
                    "req=%p offset=%zu length=%zu", req,
                    req->send.state.dt.offset, req->send.length);
    }

    ucp_trace_req(req, "%s remote_req_id 0x%"PRIxPTR, ack_str, remote_req_id);
    UCS_PROFILE_REQUEST_EVENT(req, ack_str, 0);

    ack_req->send.lane                = ucp_ep_get_am_lane(ack_req->send.ep);
    ack_req->send.uct.func            = ucp_proto_progress_am_single;
    ack_req->send.proto.am_id         = am_id;
    ack_req->send.proto.status        = status;
    ack_req->send.proto.remote_req_id = remote_req_id;
    ack_req->send.proto.comp_cb       = ucp_request_put;
    ucp_request_send_state_reset(ack_req, NULL,
                                 UCP_REQUEST_SEND_PROTO_BCOPY_AM);

    ucp_request_send(ack_req, 0);
}

static UCS_F_ALWAYS_INLINE void
ucp_rndv_recv_req_complete(ucp_request_t *req, ucs_status_t status)
{
    if (req->flags & UCP_REQUEST_FLAG_RECV_AM) {
        ucp_request_complete_am_recv(req, status);
    } else {
        ucs_assert(req->flags & UCP_REQUEST_FLAG_RECV_TAG);
        ucp_request_complete_tag_recv(req, status);
    }
}

static void ucp_rndv_zcopy_recv_req_complete(ucp_request_t *req,
                                             ucs_status_t status)
{
    ucp_request_recv_buffer_dereg(req);
    ucp_rndv_recv_req_complete(req, status);
}

static void ucp_rndv_complete_rma_put_zcopy(ucp_request_t *sreq, int is_frag_put)
{
    ucs_status_t status = sreq->send.state.uct_comp.status;
    ucp_request_t *atp_req;

    ucs_assertv(sreq->send.state.dt.offset <= sreq->send.length,
                "sreq=%p offset=%zu length=%zu", sreq,
                sreq->send.state.dt.offset, sreq->send.length);

    /* complete send request after PUT completions of all fragments */
    if (sreq->send.state.dt.offset != sreq->send.length) {
        return;
    }

    ucp_trace_req(sreq, "rndv_put completed with status %s",
                  ucs_status_string(status));
    UCS_PROFILE_REQUEST_EVENT(sreq, "complete_rndv_put", 0);

    if (is_frag_put) {
        ucp_request_id_release(sreq);
    } else {
        ucp_rkey_destroy(sreq->send.rndv.rkey);

        atp_req = ucp_request_get(sreq->send.ep->worker);
        if (ucs_unlikely(atp_req == NULL)) {
            ucs_fatal("failed to allocate request for sending ATP");
        }

        atp_req->send.ep = sreq->send.ep;
        atp_req->flags   = 0;
        ucp_rndv_req_send_ack(atp_req, sreq, sreq->send.rndv.remote_req_id,
                              status, UCP_AM_ID_RNDV_ATP, "send_atp");
    }

    ucp_request_send_buffer_dereg(sreq);
    ucs_assert(sreq->send.state.dt.dt.contig.md_map == 0);
    ucp_request_complete_send(sreq, status);
}

static void ucp_rndv_recv_data_init(ucp_request_t *rreq, size_t size)
{
    rreq->status         = UCS_OK;
    rreq->recv.remaining = size;
}

ucs_status_t ucp_rndv_send_rts(ucp_request_t *sreq, uct_pack_callback_t pack_cb,
                               size_t rts_size)
{
    size_t max_rts_size = ucp_ep_config(sreq->send.ep)->rndv.rkey_size +
                          rts_size;
    ucs_status_t status;

    status = ucp_do_am_single(&sreq->send.uct, UCP_AM_ID_RNDV_RTS, pack_cb,
                              max_rts_size);
    return ucp_rndv_rts_handle_status_from_pending(sreq, status);
}

static void ucp_rndv_req_send_rtr(ucp_request_t *rndv_req, ucp_request_t *rreq,
                                  ucs_ptr_map_key_t sender_req_id,
                                  size_t recv_length, size_t offset)
{
    ucp_trace_req(rndv_req, "send rtr remote sreq_id 0x%"PRIxPTR" rreq %p",
                  sender_req_id, rreq);

    rreq->recv.remote_req_id       = sender_req_id;
    rndv_req->super_req            = rreq;
    rndv_req->send.lane            = ucp_ep_get_am_lane(rndv_req->send.ep);
    rndv_req->send.uct.func        = ucp_proto_progress_rndv_rtr;
    rndv_req->send.rndv_rtr.length = recv_length;
    rndv_req->send.rndv_rtr.offset = offset;
    ucp_request_id_alloc(rndv_req);

    ucp_request_send(rndv_req, 0);
}

static ucp_lane_index_t ucp_rndv_zcopy_get_lane(ucp_request_t *rndv_req,
                                                uct_rkey_t *uct_rkey,
                                                unsigned proto)
{
    ucp_lane_index_t lane_idx;
    ucp_ep_config_t *ep_config;
    ucp_rkey_h rkey;
    uint8_t rkey_index;

    ucs_assert((proto == UCP_REQUEST_SEND_PROTO_RNDV_GET) ||
               (proto == UCP_REQUEST_SEND_PROTO_RNDV_PUT));

    if (ucs_unlikely(!rndv_req->send.rndv.lanes_map_all)) {
        return UCP_NULL_LANE;
    }

    lane_idx   = ucs_ffs64_safe(rndv_req->send.lanes_map_avail);
    ucs_assert(lane_idx < UCP_MAX_LANES);
    rkey       = rndv_req->send.rndv.rkey;
    rkey_index = rndv_req->send.rndv.rkey_index[lane_idx];
    *uct_rkey  = (rkey_index != UCP_NULL_RESOURCE) ?
                 rkey->tl_rkey[rkey_index].rkey.rkey : UCT_INVALID_RKEY;
    ep_config  = ucp_ep_config(rndv_req->send.ep);
    return (proto == UCP_REQUEST_SEND_PROTO_RNDV_GET) ?
           ep_config->rndv.get_zcopy.lanes[lane_idx] :
           ep_config->rndv.put_zcopy.lanes[lane_idx];
}

static void ucp_rndv_zcopy_next_lane(ucp_request_t *rndv_req)
{
    rndv_req->send.lanes_map_avail    &= rndv_req->send.lanes_map_avail - 1;
    if (!rndv_req->send.lanes_map_avail) {
        rndv_req->send.lanes_map_avail = rndv_req->send.rndv.lanes_map_all;
    }
}

static ucs_status_t
ucp_rndv_progress_rma_zcopy_common(ucp_request_t *req, ucp_lane_index_t lane,
                                   uct_rkey_t uct_rkey, unsigned proto)
{
    const size_t max_iovcnt = 1;
    ucp_ep_h ep             = req->send.ep;
    ucp_ep_config_t *config = ucp_ep_config(ep);
    uct_iov_t iov[max_iovcnt];
    size_t iovcnt;
    uct_iface_attr_t *attrs;
    ucs_status_t status;
    size_t offset, length, ucp_mtu, remaining, align, chunk;
    ucp_dt_state_t state;
    ucp_rsc_index_t rsc_index;
    size_t min_zcopy;
    size_t max_zcopy;
    double scale;
    int pending_add_res;

    ucs_assert_always(req->send.lane != UCP_NULL_LANE);
    ucs_assert_always(req->send.rndv.lanes_count > 0);

    if (req->send.mdesc == NULL) {
        status = ucp_send_request_add_reg_lane(req, lane);
        ucs_assert_always(status == UCS_OK);
    }

    rsc_index = ucp_ep_get_rsc_index(ep, lane);
    attrs     = ucp_worker_iface_get_attr(ep->worker, rsc_index);

    if (proto == UCP_REQUEST_SEND_PROTO_RNDV_GET) {
        align     = attrs->cap.get.opt_zcopy_align;
        ucp_mtu   = attrs->cap.get.align_mtu;
        min_zcopy = config->rndv.get_zcopy.min;
        max_zcopy = config->rndv.get_zcopy.max;
        scale     = config->rndv.get_zcopy.scale[lane];
    } else {
        align     = attrs->cap.put.opt_zcopy_align;
        ucp_mtu   = attrs->cap.put.align_mtu;
        min_zcopy = config->rndv.put_zcopy.min;
        max_zcopy = config->rndv.put_zcopy.max;
        scale     = config->rndv.put_zcopy.scale[lane];
    }

    offset    = req->send.state.dt.offset;
    remaining = (uintptr_t)req->send.buffer % align;

    if ((offset == 0) && (remaining > 0) && (req->send.length > ucp_mtu)) {
        length = ucp_mtu - remaining;
    } else {
        chunk  = ucs_align_up((size_t)(req->send.length /
                                       req->send.rndv.lanes_count * scale),
                              align);
        length = ucs_min(chunk, req->send.length - offset);
    }

    length = ucp_rndv_adjust_zcopy_length(min_zcopy, max_zcopy, align,
                                          req->send.length, offset, length);

    ucs_trace_data("req %p: offset %zu remain %zu RMA-%s to %p len %zu lane %d",
                   req, offset, remaining,
                   (proto == UCP_REQUEST_SEND_PROTO_RNDV_GET) ? "GET" : "PUT",
                   UCS_PTR_BYTE_OFFSET(req->send.buffer, offset), length, lane);

    state = req->send.state.dt;
    /* TODO: is this correct? memh array may skip MD's where
     * registration is not supported. for now SHM may avoid registration,
     * but it will work on single lane */
    ucp_dt_iov_copy_uct(ep->worker->context, iov, &iovcnt, max_iovcnt, &state,
                        req->send.buffer, ucp_dt_make_contig(1), length,
                        ucp_ep_md_index(ep, lane), req->send.mdesc);

    for (;;) {
        if (proto == UCP_REQUEST_SEND_PROTO_RNDV_GET) {
            status = uct_ep_get_zcopy(ep->uct_eps[lane], iov, iovcnt,
                                      req->send.rndv.remote_address + offset,
                                      uct_rkey, &req->send.state.uct_comp);
        } else {
            status = uct_ep_put_zcopy(ep->uct_eps[lane], iov, iovcnt,
                                      req->send.rndv.remote_address + offset,
                                      uct_rkey, &req->send.state.uct_comp);
        }

        ucp_request_send_state_advance(req, &state, proto, status);
        if (req->send.state.dt.offset == req->send.length) {
            if (req->send.state.uct_comp.count == 0) {
                uct_completion_update_status(&req->send.state.uct_comp, status);
                req->send.state.uct_comp.func(&req->send.state.uct_comp);
            }
            return UCS_OK;
        } else if (!UCS_STATUS_IS_ERR(status)) {
            /* return in_progress status in case if not all chunks are transmitted */
            ucp_rndv_zcopy_next_lane(req);
            return UCS_INPROGRESS;
        } else if (status == UCS_ERR_NO_RESOURCE) {
            if (lane != req->send.pending_lane) {
                /* switch to new pending lane */
                pending_add_res = ucp_request_pending_add(req, 0);
                if (!pending_add_res) {
                    /* failed to switch req to pending queue, try again */
                    continue;
                }
                return UCS_OK;
            }
            return UCS_ERR_NO_RESOURCE;
        } else {
            ucp_request_send_state_ff(req, status);
            return UCS_OK;
        }
    }
}

UCS_PROFILE_FUNC(ucs_status_t, ucp_rndv_progress_rma_get_zcopy, (self),
                 uct_pending_req_t *self)
{
    ucp_request_t *rndv_req = ucs_container_of(self, ucp_request_t, send.uct);
    uct_rkey_t uct_rkey;

    /* Figure out which lane to use for get operation */
    rndv_req->send.lane =
        ucp_rndv_zcopy_get_lane(rndv_req, &uct_rkey,
                                UCP_REQUEST_SEND_PROTO_RNDV_GET);
    if (rndv_req->send.lane != UCP_NULL_LANE) {
        return ucp_rndv_progress_rma_zcopy_common(
                rndv_req, rndv_req->send.lane, uct_rkey,
                UCP_REQUEST_SEND_PROTO_RNDV_GET);
    }

    /* If can't perform get_zcopy - switch to active-message.
     * NOTE: we do not register memory and do not send our keys. */
    ucp_trace_req(rndv_req, "remote memory unreachable, switch to rtr");
    ucp_rkey_destroy(rndv_req->send.rndv.rkey);
    ucp_rndv_recv_data_init(rndv_req->super_req, rndv_req->send.length);
    /* Update statistics counters from get_zcopy to rtr */
    UCP_WORKER_STAT_RNDV(rndv_req->send.ep->worker, GET_ZCOPY, -1);
    UCP_WORKER_STAT_RNDV(rndv_req->send.ep->worker, SEND_RTR, +1);
    ucp_rndv_req_send_rtr(rndv_req, rndv_req->super_req,
                          rndv_req->send.rndv.remote_req_id,
                          rndv_req->send.length, 0ul);
    return UCS_OK;
}

UCS_PROFILE_FUNC_VOID(ucp_rndv_get_completion, (self), uct_completion_t *self)
{
    ucp_request_t *rndv_req = ucs_container_of(self, ucp_request_t,
                                               send.state.uct_comp);
    ucp_ep_h UCS_V_UNUSED ep;
    ucp_request_t *rreq;
    ucs_status_t status;

    if (rndv_req->send.state.dt.offset != rndv_req->send.length) {
        return;
    }

    rreq   = rndv_req->super_req;
    status = rndv_req->send.state.uct_comp.status;
    ep     = rndv_req->send.ep;

    ucs_assertv(rndv_req->send.state.dt.offset == rndv_req->send.length,
                "rndv_req=%p offset=%zu length=%zu", rndv_req,
                rndv_req->send.state.dt.offset, rndv_req->send.length);

    ucp_trace_req(rndv_req, "rndv_get completed with status %s",
                  ucs_status_string(status));
    UCS_PROFILE_REQUEST_EVENT(rreq, "complete_rndv_get", 0);

    ucp_rkey_destroy(rndv_req->send.rndv.rkey);
    ucp_request_send_buffer_dereg(rndv_req);

    if (status == UCS_OK) {
        ucp_rndv_req_send_ack(rndv_req, rreq, rndv_req->send.rndv.remote_req_id,
                              UCS_OK, UCP_AM_ID_RNDV_ATS, "send_ats");
    } else {
        /* if completing RNDV with the error, just release RNDV request */
        ucp_request_put(rndv_req);
    }

    ucs_assert((rreq->recv.state.dt.contig.md_map == 0) ||
               /* Request send state fast-forward after failure detection, i.e.
                * it is called from ucp_request_send_state_ff() function.
                * md_map can be NULL, if GET Zcopy was started, but no fragments
                * were really sent yet */
               ((ep->flags & UCP_EP_FLAG_FAILED) && (status != UCS_OK)));
    ucp_rndv_recv_req_complete(rreq, status);
}

UCS_PROFILE_FUNC_VOID(ucp_rndv_put_completion, (self), uct_completion_t *self)
{
    ucp_request_t *sreq = ucs_container_of(self, ucp_request_t,
                                           send.state.uct_comp);
    ucp_rndv_complete_rma_put_zcopy(sreq, 0);
}

static void ucp_rndv_req_init_lanes(ucp_request_t *req,
                                    ucp_lane_map_t lanes_map,
                                    uint8_t lanes_count)
{
    req->send.lanes_map_avail    = lanes_map;
    req->send.rndv.lanes_map_all = lanes_map;
    req->send.rndv.lanes_count   = lanes_count;
}

static void ucp_rndv_req_init_zcopy_lane_map(ucp_request_t *rndv_req,
                                             ucs_memory_type_t mem_type,
                                             unsigned proto)
{
    ucp_ep_h ep                = rndv_req->send.ep;
    ucp_ep_config_t *ep_config = ucp_ep_config(ep);
    ucp_context_h context      = ep->worker->context;
    ucp_rkey_h rkey            = rndv_req->send.rndv.rkey;
    ucp_lane_index_t *lanes;
    ucp_lane_map_t lane_map;
    ucp_lane_index_t lane, lane_idx;
    ucp_md_index_t md_index;
    uct_md_attr_t *md_attr;
    ucp_md_index_t dst_md_index;
    ucp_rsc_index_t rsc_index;
    uct_iface_attr_t *iface_attr;
    double max_lane_bw, lane_bw;
    int i;

    ucs_assert((proto == UCP_REQUEST_SEND_PROTO_RNDV_GET) ||
               (proto == UCP_REQUEST_SEND_PROTO_RNDV_PUT));

    lanes = (proto == UCP_REQUEST_SEND_PROTO_RNDV_GET) ?
            ep_config->rndv.get_zcopy.lanes :
            ep_config->rndv.put_zcopy.lanes;

    max_lane_bw = 0;
    lane_map    = 0;
    for (i = 0; i < UCP_MAX_LANES; i++) {
        lane = lanes[i];
        if (lane == UCP_NULL_LANE) {
            break; /* no more lanes */
        }

        md_index   = ep_config->md_index[lane];
        md_attr    = &context->tl_mds[md_index].attr;
        rsc_index  = ep_config->key.lanes[lane].rsc_index;
        iface_attr = ucp_worker_iface_get_attr(ep->worker, rsc_index);
        lane_bw    = ucp_tl_iface_bandwidth(context, &iface_attr->bandwidth);

        if (ucs_unlikely((md_index != UCP_NULL_RESOURCE) &&
                         !(md_attr->cap.flags & UCT_MD_FLAG_NEED_RKEY))) {
            /* Lane does not need rkey, can use the lane with invalid rkey  */
            if (!rkey || ((md_attr->cap.access_mem_types & UCS_BIT(mem_type)) &&
                          (mem_type == rkey->mem_type))) {
                rndv_req->send.rndv.rkey_index[i] = UCP_NULL_RESOURCE;
                lane_map                         |= UCS_BIT(i);
                max_lane_bw                       = ucs_max(max_lane_bw, lane_bw);
                continue;
            }
        }

        if (ucs_unlikely((md_index != UCP_NULL_RESOURCE) &&
                         (!(md_attr->cap.reg_mem_types & UCS_BIT(mem_type))))) {
            continue;
        }

        dst_md_index = ep_config->key.lanes[lane].dst_md_index;
        if (rkey && ucs_likely(rkey->md_map & UCS_BIT(dst_md_index))) {
            /* Return first matching lane */
            rndv_req->send.rndv.rkey_index[i] = ucs_bitmap2idx(rkey->md_map,
                                                               dst_md_index);
            lane_map                         |= UCS_BIT(i);
            max_lane_bw                       = ucs_max(max_lane_bw, lane_bw);
        }
    }

    if (ucs_popcount(lane_map) > 1) {
        /* remove lanes if bandwidth is too less compare to best lane */
        ucs_for_each_bit(lane_idx, lane_map) {
            ucs_assert(lane_idx < UCP_MAX_LANES);
            lane       = lanes[lane_idx];
            rsc_index  = ep_config->key.lanes[lane].rsc_index;
            iface_attr = ucp_worker_iface_get_attr(ep->worker, rsc_index);
            lane_bw    = ucp_tl_iface_bandwidth(context, &iface_attr->bandwidth);

            if ((lane_bw / max_lane_bw) <
                (1. / context->config.ext.multi_lane_max_ratio)) {
                lane_map                                &= ~UCS_BIT(lane_idx);
                rndv_req->send.rndv.rkey_index[lane_idx] = UCP_NULL_RESOURCE;
            }
        }
    }

    ucp_rndv_req_init_lanes(rndv_req, lane_map, ucs_popcount(lane_map));
}

static void ucp_rndv_req_init(ucp_request_t *req, ucp_request_t *super_req,
                              ucp_lane_map_t lanes_map, uint8_t lanes_count,
                              ucp_rkey_h rkey, uint64_t remote_address,
                              uint8_t *rkey_index)
{
    ucp_lane_index_t i;

    req->super_req                = super_req;
    req->send.rndv.rkey           = rkey;
    req->send.rndv.remote_address = remote_address;
    req->send.pending_lane        = UCP_NULL_LANE;

    ucp_rndv_req_init_lanes(req, lanes_map, lanes_count);

    if (rkey_index != NULL) {
        memcpy(req->send.rndv.rkey_index, rkey_index,
               sizeof(*req->send.rndv.rkey_index) * UCP_MAX_LANES);
    } else {
        for (i = 0; i < UCP_MAX_LANES; i++) {
            req->send.rndv.rkey_index[i] = UCP_NULL_RESOURCE;
        }
    }
}

static void
ucp_rndv_req_init_remote_from_super_req(ucp_request_t *req,
                                        ucp_request_t *super_req,
                                        size_t remote_address_offset)
{
    req->flags   = 0;
    req->send.ep = super_req->send.ep;

    ucp_rndv_req_init(req, super_req, super_req->send.rndv.lanes_map_all,
                      super_req->send.rndv.lanes_count,
                      super_req->send.rndv.rkey,
                      super_req->send.rndv.remote_address +
                      remote_address_offset,
                      super_req->send.rndv.rkey_index);
}

static void ucp_rndv_req_init_from_super_req(ucp_request_t *req,
                                             ucp_request_t *super_req,
                                             size_t length,
                                             size_t send_buffer_offset,
                                             size_t remote_address_offset,
                                             ucs_ptr_map_key_t remote_req_id)
{
    ucs_assert(length > 0);

    req->send.length = length;
    req->send.buffer = UCS_PTR_BYTE_OFFSET(super_req->send.buffer,
                                           send_buffer_offset);

    ucp_rndv_req_init_remote_from_super_req(req, super_req,
                                            remote_address_offset);

    req->send.rndv.remote_req_id = remote_req_id;
}

static ucs_status_t ucp_rndv_req_send_rma_get(ucp_request_t *rndv_req,
                                              ucp_request_t *rreq,
                                              const ucp_rndv_rts_hdr_t *rndv_rts_hdr,
                                              const void *rkey_buf)
{
    ucp_ep_h ep = rndv_req->send.ep;
    ucs_status_t status;
    uct_rkey_t uct_rkey;

    ucp_trace_req(rndv_req, "start rma_get rreq %p", rreq);

    rndv_req->super_req                = rreq;
    rndv_req->send.uct.func            = ucp_rndv_progress_rma_get_zcopy;
    rndv_req->send.buffer              = rreq->recv.buffer;
    rndv_req->send.mem_type            = rreq->recv.mem_type;
    rndv_req->send.datatype            = ucp_dt_make_contig(1);
    rndv_req->send.length              = rndv_rts_hdr->size;
    rndv_req->send.rndv.remote_req_id  = rndv_rts_hdr->sreq.req_id;
    rndv_req->send.rndv.remote_address = rndv_rts_hdr->address;
    rndv_req->send.pending_lane        = UCP_NULL_LANE;

    status = ucp_ep_rkey_unpack(ep, rkey_buf, &rndv_req->send.rndv.rkey);
    if (status != UCS_OK) {
        ucs_fatal("failed to unpack rendezvous remote key received from %s: %s",
                  ucp_ep_peer_name(ep), ucs_status_string(status));
    }

    ucp_request_send_state_init(rndv_req, ucp_dt_make_contig(1), 0);
    ucp_request_send_state_reset(rndv_req, ucp_rndv_get_completion,
                                 UCP_REQUEST_SEND_PROTO_RNDV_GET);

    ucp_rndv_req_init_zcopy_lane_map(rndv_req, rndv_req->send.mem_type,
                                     UCP_REQUEST_SEND_PROTO_RNDV_GET);

    rndv_req->send.lane =
        ucp_rndv_zcopy_get_lane(rndv_req, &uct_rkey,
                                UCP_REQUEST_SEND_PROTO_RNDV_GET);
    if (rndv_req->send.lane == UCP_NULL_LANE) {
        return UCS_ERR_UNREACHABLE;
    }

    UCP_WORKER_STAT_RNDV(ep->worker, GET_ZCOPY, 1);
    ucp_request_send(rndv_req, 0);

    return UCS_OK;
}

UCS_PROFILE_FUNC_VOID(ucp_rndv_recv_frag_put_completion, (self),
                      uct_completion_t *self)
{
    ucp_request_t *freq     = ucs_container_of(self, ucp_request_t,
                                               send.state.uct_comp);
    /* if the super request is a receive request, it means that's used RNDV
     * scheme is PUT pipeline protocol, otherwise - GET pipeline protocol (where
     * the super request is an intermediate RNDV request) */
    int is_put_proto        = freq->super_req->flags &
                              (UCP_REQUEST_FLAG_RECV_TAG |
                               UCP_REQUEST_FLAG_RECV_AM);
    ucp_request_t *rreq     = NULL;
    ucp_request_t *rndv_req = NULL;

    /* release memory descriptor */
    ucs_mpool_put_inline((void*)freq->send.mdesc);

    /* rndv_req is NULL in case of put protocol */
    if (!is_put_proto) {
        ucs_trace_req("freq:%p: recv_frag_put done, nrdv_req:%p rreq:%p ", freq,
                      rndv_req, rreq);

        rndv_req = freq->super_req;
        rreq     = rndv_req->super_req;

        /* pipeline recv get protocol */
        rndv_req->send.state.dt.offset += freq->send.length;

        /* send ATS for fragment get rndv completion */
        if (rndv_req->send.length == rndv_req->send.state.dt.offset) {
            ucp_rkey_destroy(rndv_req->send.rndv.rkey);
            ucp_rndv_req_send_ack(rndv_req, rreq,
                                  rndv_req->send.rndv.remote_req_id,
                                  UCS_OK, UCP_AM_ID_RNDV_ATS, "send_ats");
        }
    } else {
        ucs_trace_req("freq:%p: recv_frag_put done, rreq:%p ", freq, rreq);
        rreq = freq->super_req;
    }

    ucs_assertv(rreq->recv.remaining >= freq->send.length,
                "rreq->recv.remaining %zu, freq->send.length %zu",
                rreq->recv.remaining, freq->send.length);
    rreq->recv.remaining -= freq->send.length;
    if (rreq->recv.remaining == 0) {
        ucp_rndv_recv_req_complete(rreq, UCS_OK);
    }

    ucp_request_put(freq);
}

static UCS_F_ALWAYS_INLINE void
ucp_rndv_init_mem_type_frag_req(ucp_worker_h worker, ucp_request_t *freq, int rndv_op,
                                uct_completion_callback_t comp_cb, ucp_mem_desc_t *mdesc,
                                ucs_memory_type_t mem_type, size_t length,
                                uct_pending_callback_t uct_func)
{
    ucp_ep_h mem_type_ep;
    ucp_md_index_t md_index;
    ucp_lane_index_t mem_type_rma_lane;

    ucp_request_send_state_init(freq, ucp_dt_make_contig(1), 0);
    ucp_request_send_state_reset(freq, comp_cb, rndv_op);

    freq->flags             = 0;
    freq->send.buffer       = mdesc + 1;
    freq->send.length       = length;
    freq->send.datatype     = ucp_dt_make_contig(1);
    freq->send.mem_type     = mem_type;
    freq->send.mdesc        = mdesc;
    freq->send.uct.func     = uct_func;
    freq->send.pending_lane = UCP_NULL_LANE;

    if (mem_type != UCS_MEMORY_TYPE_HOST) {
        mem_type_ep       = worker->mem_type_ep[mem_type];
        mem_type_rma_lane = ucp_ep_config(mem_type_ep)->key.rma_bw_lanes[0];
        md_index          = ucp_ep_md_index(mem_type_ep, mem_type_rma_lane);
        ucs_assert(mem_type_rma_lane != UCP_NULL_LANE);

        freq->send.lane                       = mem_type_rma_lane;
        freq->send.ep                         = mem_type_ep;
        freq->send.state.dt.dt.contig.memh[0] = ucp_memh2uct(mdesc->memh, md_index);
        freq->send.state.dt.dt.contig.md_map  = UCS_BIT(md_index);
    }
}

static void
ucp_rndv_recv_frag_put_mem_type(ucp_request_t *rreq, ucp_request_t *freq,
                                ucp_mem_desc_t *mdesc, size_t length,
                                size_t offset)
{

    ucs_assert_always(!UCP_MEM_IS_HOST(rreq->recv.mem_type));

    /* PUT on memtype endpoint to stage from
     * frag recv buffer to memtype recv buffer
     */

    ucp_rndv_init_mem_type_frag_req(rreq->recv.worker, freq,
                                    UCP_REQUEST_SEND_PROTO_RNDV_PUT,
                                    ucp_rndv_recv_frag_put_completion, mdesc,
                                    rreq->recv.mem_type, length,
                                    ucp_rndv_progress_rma_put_zcopy);

    freq->send.rndv.rkey           = NULL;
    freq->send.rndv.remote_address = (uintptr_t)rreq->recv.buffer + offset;

    ucp_rndv_req_init_zcopy_lane_map(freq, freq->send.mem_type,
                                     UCP_REQUEST_SEND_PROTO_RNDV_PUT);

    ucp_request_send(freq, 0);
}

static void
ucp_rndv_send_frag_update_get_rkey(ucp_worker_h worker, ucp_request_t *freq,
                                   ucp_mem_desc_t *mdesc,
                                   ucs_memory_type_t mem_type)
{
    ucp_rkey_h *rkey_p  = &freq->send.rndv.rkey;
    uint8_t *rkey_index = freq->send.rndv.rkey_index;
    void *rkey_buffer;
    size_t rkey_size;
    ucs_status_t status;
    ucp_ep_h mem_type_ep;
    ucp_md_index_t md_index;
    uct_md_attr_t *md_attr;
    ucp_lane_index_t mem_type_rma_lane;

    mem_type_ep       = worker->mem_type_ep[mem_type];
    mem_type_rma_lane = ucp_ep_config(mem_type_ep)->key.rma_bw_lanes[0];
    ucs_assert(mem_type_rma_lane != UCP_NULL_LANE);

    md_index = ucp_ep_md_index(mem_type_ep, mem_type_rma_lane);
    md_attr  = &mem_type_ep->worker->context->tl_mds[md_index].attr;

    if (!(md_attr->cap.flags & UCT_MD_FLAG_NEED_RKEY)) {
        return;
    }

    status = ucp_rkey_pack(mem_type_ep->worker->context, mdesc->memh,
                           &rkey_buffer, &rkey_size);
    ucs_assert_always(status == UCS_OK);

    status = ucp_ep_rkey_unpack(mem_type_ep, rkey_buffer, rkey_p);
    ucs_assert_always(status == UCS_OK);
    ucp_rkey_buffer_release(rkey_buffer);

    memset(rkey_index, 0, UCP_MAX_LANES * sizeof(uint8_t));
}

static void
ucp_rndv_send_frag_get_mem_type(ucp_request_t *sreq, size_t length,
                                uint64_t remote_address,
                                ucs_memory_type_t remote_mem_type,
                                ucp_rkey_h rkey, uint8_t *rkey_index,
                                ucp_lane_map_t lanes_map, int update_get_rkey,
                                uct_completion_callback_t comp_cb)
{
    ucp_worker_h worker = sreq->send.ep->worker;
    ucp_request_t *freq;
    ucp_mem_desc_t *mdesc;

    /* GET fragment to stage buffer */

    freq = ucp_request_get(worker);
    if (ucs_unlikely(freq == NULL)) {
        ucs_fatal("failed to allocate fragment receive request");
    }

    mdesc = ucp_worker_mpool_get(&worker->rndv_frag_mp);
    if (ucs_unlikely(mdesc == NULL)) {
        ucs_fatal("failed to allocate fragment memory desc");
    }

    freq->send.ep = sreq->send.ep;

    ucp_rndv_init_mem_type_frag_req(worker, freq, UCP_REQUEST_SEND_PROTO_RNDV_GET,
                                    comp_cb, mdesc, remote_mem_type, length,
                                    ucp_rndv_progress_rma_get_zcopy);
    ucp_rndv_req_init(freq, sreq, lanes_map, ucs_popcount(lanes_map), rkey,
                      remote_address, rkey_index);

    if (update_get_rkey) {
        ucp_rndv_send_frag_update_get_rkey(worker, freq, mdesc, remote_mem_type);
    }

    freq->status = UCS_INPROGRESS;
    ucp_request_send(freq, 0);
}

UCS_PROFILE_FUNC_VOID(ucp_rndv_recv_frag_get_completion, (self),
                      uct_completion_t *self)
{
    ucp_request_t *freq = ucs_container_of(self, ucp_request_t,
                                           send.state.uct_comp);
    ucp_request_t *rndv_req, *rreq;
    uint64_t offset;

    if (freq->send.state.dt.offset != freq->send.length) {
        return;
    }

    rndv_req = freq->super_req;
    rreq     = rndv_req->super_req;
    offset   = freq->send.rndv.remote_address -
               rndv_req->send.rndv.remote_address;

    ucs_trace_req("freq:%p: recv_frag_get done. rreq:%p length:%"PRIu64
                  " offset:%"PRIu64,
                  freq, rndv_req, freq->send.length, offset);

    /* fragment GET completed from remote to staging buffer, issue PUT from
     * staging buffer to recv buffer */
    ucp_rndv_recv_frag_put_mem_type(rreq, freq,
                                    (ucp_mem_desc_t*)freq->send.buffer - 1,
                                    freq->send.length, offset);
}

static ucs_status_t
ucp_rndv_recv_start_get_pipeline(ucp_worker_h worker, ucp_request_t *rndv_req,
                                 ucp_request_t *rreq,
                                 ucs_ptr_map_key_t remote_req_id,
                                 const void *rkey_buffer,
                                 uint64_t remote_address, size_t size,
                                 size_t base_offset)
{
    ucp_ep_h ep             = rndv_req->send.ep;
    ucp_ep_config_t *config = ucp_ep_config(ep);
    ucp_context_h context   = worker->context;
    ucs_status_t status;
    size_t max_frag_size, offset, length;
    size_t min_zcopy, max_zcopy;

    min_zcopy                          = config->rndv.get_zcopy.min;
    max_zcopy                          = config->rndv.get_zcopy.max;
    max_frag_size                      = ucs_min(context->config.ext.rndv_frag_size,
                                                 max_zcopy);
    rndv_req->super_req                = rreq;
    rndv_req->send.rndv.remote_req_id  = remote_req_id;
    rndv_req->send.rndv.remote_address = remote_address - base_offset;
    rndv_req->send.length              = size;
    rndv_req->send.state.dt.offset     = 0;
    rndv_req->send.mem_type            = rreq->recv.mem_type;
    rndv_req->send.pending_lane        = UCP_NULL_LANE;

    /* Protocol:
     * Step 1: GET remote fragment into HOST fragment buffer
     * Step 2: PUT from fragment buffer to MEM TYPE destination
     * Step 3: Send ATS for RNDV request
     */

    status = ucp_ep_rkey_unpack(rndv_req->send.ep, rkey_buffer,
                                &rndv_req->send.rndv.rkey);
    if (ucs_unlikely(status != UCS_OK)) {
        ucs_fatal("failed to unpack rendezvous remote key received from %s: %s",
                  ucp_ep_peer_name(rndv_req->send.ep), ucs_status_string(status));
    }

    ucp_rndv_req_init_zcopy_lane_map(rndv_req, rndv_req->send.mem_type,
                                     UCP_REQUEST_SEND_PROTO_RNDV_GET);

    offset = 0;
    while (offset != size) {
        length = ucp_rndv_adjust_zcopy_length(min_zcopy, max_frag_size, 0,
                                              size, offset, size - offset);

        /* GET remote fragment into HOST fragment buffer */
        ucp_rndv_send_frag_get_mem_type(rndv_req, length,
                                        remote_address + offset,
                                        UCS_MEMORY_TYPE_HOST,
                                        rndv_req->send.rndv.rkey,
                                        rndv_req->send.rndv.rkey_index,
                                        rndv_req->send.rndv.lanes_map_all, 0,
                                        ucp_rndv_recv_frag_get_completion);

        offset += length;
    }

    return UCS_OK;
}

static void ucp_rndv_send_frag_rtr(ucp_worker_h worker, ucp_request_t *rndv_req,
                                   ucp_request_t *rreq,
                                   const ucp_rndv_rts_hdr_t *rndv_rts_hdr)
{
    size_t max_frag_size = worker->context->config.ext.rndv_frag_size;
    int i, num_frags;
    size_t frag_size;
    size_t offset;
    ucp_mem_desc_t *mdesc;
    ucp_request_t *freq;
    ucp_request_t *frndv_req;
    unsigned md_index;
    unsigned memh_index;

    ucp_trace_req(rreq, "using rndv pipeline protocol rndv_req %p", rndv_req);

    offset    = 0;
    num_frags = ucs_div_round_up(rndv_rts_hdr->size, max_frag_size);

    for (i = 0; i < num_frags; i++) {
        frag_size = ucs_min(max_frag_size, (rndv_rts_hdr->size - offset));

        /* internal fragment recv request allocated on receiver side to receive
         *  put fragment from sender and to perform a put to recv buffer */
        freq = ucp_request_get(worker);
        if (freq == NULL) {
            ucs_fatal("failed to allocate fragment receive request");
        }

        /* internal rndv request to send RTR */
        frndv_req = ucp_request_get(worker);
        if (frndv_req == NULL) {
            ucs_fatal("failed to allocate fragment rendezvous reply");
        }

        /* allocate fragment recv buffer desc*/
        mdesc = ucp_worker_mpool_get(&worker->rndv_frag_mp);
        if (mdesc == NULL) {
            ucs_fatal("failed to allocate fragment memory buffer");
        }

        freq->recv.buffer                 = mdesc + 1;
        freq->recv.datatype               = ucp_dt_make_contig(1);
        freq->recv.mem_type               = UCS_MEMORY_TYPE_HOST;
        freq->recv.length                 = frag_size;
        freq->recv.state.dt.contig.md_map = 0;
        freq->recv.frag.offset            = offset;
        freq->super_req                   = rreq;
        freq->flags                       = UCP_REQUEST_FLAG_RNDV_FRAG;

        memh_index = 0;
        ucs_for_each_bit(md_index,
                         (ucp_ep_config(rndv_req->send.ep)->key.rma_bw_md_map &
                          mdesc->memh->md_map)) {
            freq->recv.state.dt.contig.memh[memh_index++] = ucp_memh2uct(mdesc->memh, md_index);
            freq->recv.state.dt.contig.md_map            |= UCS_BIT(md_index);
        }
        ucs_assert(memh_index <= UCP_MAX_OP_MDS);

        frndv_req->flags             = 0;
        frndv_req->send.ep           = rndv_req->send.ep;
        frndv_req->send.pending_lane = UCP_NULL_LANE;

        ucp_rndv_req_send_rtr(frndv_req, freq, rndv_rts_hdr->sreq.req_id,
                              freq->recv.length, offset);
        offset += frag_size;
    }

    /* release original rndv reply request */
    ucp_request_put(rndv_req);
}

static UCS_F_ALWAYS_INLINE int
ucp_rndv_is_rkey_ptr(const ucp_rndv_rts_hdr_t *rndv_rts_hdr,
                     const void *rkey_buffer, ucp_ep_h ep,
                     ucs_memory_type_t recv_mem_type, ucp_rndv_mode_t rndv_mode)
{
    const ucp_ep_config_t *ep_config = ucp_ep_config(ep);

    return /* must have remote address */
           (rndv_rts_hdr->address != 0) &&
           /* remote key must be on a memory domain for which we support rkey_ptr */
           (ucp_rkey_packed_md_map(rkey_buffer) &
            ep_config->rndv.rkey_ptr_dst_mds) &&
           /* rendezvous mode must not be forced to put/get */
           (rndv_mode == UCP_RNDV_MODE_AUTO) &&
           /* need local memory access for data unpack */
           UCP_MEM_IS_ACCESSIBLE_FROM_CPU(recv_mem_type);
}

static unsigned ucp_rndv_progress_rkey_ptr(void *arg)
{
    ucp_worker_h worker     = (ucp_worker_h)arg;
    ucp_request_t *rndv_req = ucs_queue_head_elem_non_empty(&worker->rkey_ptr_reqs,
                                                            ucp_request_t,
                                                            send.rkey_ptr.queue_elem);
    ucp_request_t *rreq     = rndv_req->super_req;
    size_t seg_size         = ucs_min(worker->context->config.ext.rkey_ptr_seg_size,
                                      rndv_req->send.length - rreq->recv.state.offset);
    ucs_status_t status;
    size_t offset, new_offset;
    int last;

    offset     = rreq->recv.state.offset;
    new_offset = offset + seg_size;
    last       = new_offset == rndv_req->send.length;
    status     = ucp_request_recv_data_unpack(rreq,
                                              UCS_PTR_BYTE_OFFSET(rndv_req->send.buffer,
                                                                  offset),
                                              seg_size, offset, last);
    if (ucs_unlikely(status != UCS_OK) || last) {
        ucs_queue_pull_non_empty(&worker->rkey_ptr_reqs);
        ucp_rndv_recv_req_complete(rreq, status);
        ucp_rkey_destroy(rndv_req->send.rkey_ptr.rkey);
        ucp_rndv_req_send_ack(rndv_req, rreq,
                              rndv_req->send.rkey_ptr.remote_req_id, status,
                              UCP_AM_ID_RNDV_ATS, "send_ats");
        if (ucs_queue_is_empty(&worker->rkey_ptr_reqs)) {
            uct_worker_progress_unregister_safe(worker->uct,
                                                &worker->rkey_ptr_cb_id);
        }
    } else {
        rreq->recv.state.offset = new_offset;
    }

    return 1;
}

static void ucp_rndv_do_rkey_ptr(ucp_request_t *rndv_req, ucp_request_t *rreq,
                                 const ucp_rndv_rts_hdr_t *rndv_rts_hdr,
                                 const void *rkey_buf)
{
    ucp_ep_h ep                      = rndv_req->send.ep;
    const ucp_ep_config_t *ep_config = ucp_ep_config(ep);
    ucp_worker_h worker              = rreq->recv.worker;
    ucp_md_index_t dst_md_index      = 0;
    ucp_lane_index_t i, lane;
    ucs_status_t status;
    unsigned rkey_index;
    void *local_ptr;
    ucp_rkey_h rkey;

    ucp_trace_req(rndv_req, "start rkey_ptr rndv rreq %p", rreq);

    status = ucp_ep_rkey_unpack(ep, rkey_buf, &rkey);
    if (status != UCS_OK) {
        ucs_fatal("failed to unpack rendezvous remote key received from %s: %s",
                  ucp_ep_peer_name(ep), ucs_status_string(status));
    }

    /* Find a lane which is capable of accessing the destination memory */
    lane = UCP_NULL_LANE;
    for (i = 0; i < ep_config->key.num_lanes; ++i) {
        dst_md_index = ep_config->key.lanes[i].dst_md_index;
        if (UCS_BIT(dst_md_index) & rkey->md_map) {
            lane = i;
            break;
        }
    }

    if (ucs_unlikely(lane == UCP_NULL_LANE)) {
        /* We should be able to find a lane, because ucp_rndv_is_rkey_ptr()
         * already checked that (rkey->md_map & ep_config->rkey_ptr_dst_mds) != 0
         */
        ucs_fatal("failed to find a lane to access remote memory domains "
                  "0x%"PRIx64, rkey->md_map);
    }

    rkey_index = ucs_bitmap2idx(rkey->md_map, dst_md_index);
    status     = uct_rkey_ptr(rkey->tl_rkey[rkey_index].cmpt,
                              &rkey->tl_rkey[rkey_index].rkey,
                              rndv_rts_hdr->address, &local_ptr);
    if (status != UCS_OK) {
        ucp_rndv_recv_req_complete(rreq, status);
        ucp_rkey_destroy(rkey);
        ucp_rndv_req_send_ack(rndv_req, rreq, rndv_rts_hdr->sreq.req_id, status,
                              UCP_AM_ID_RNDV_ATS, "send_ats");
        return;
    }

    rreq->recv.state.offset = 0;

    ucp_trace_req(rndv_req, "obtained a local pointer to remote buffer: %p",
                  local_ptr);
    rndv_req->super_req                   = rreq;
    rndv_req->send.buffer                 = local_ptr;
    rndv_req->send.length                 = rndv_rts_hdr->size;
    rndv_req->send.rkey_ptr.rkey          = rkey;
    rndv_req->send.rkey_ptr.remote_req_id = rndv_rts_hdr->sreq.req_id;

    UCP_WORKER_STAT_RNDV(ep->worker, RKEY_PTR, 1);

    ucs_queue_push(&worker->rkey_ptr_reqs, &rndv_req->send.rkey_ptr.queue_elem);
    uct_worker_progress_register_safe(worker->uct,
                                      ucp_rndv_progress_rkey_ptr,
                                      rreq->recv.worker,
                                      UCS_CALLBACKQ_FLAG_FAST,
                                      &worker->rkey_ptr_cb_id);
}

static UCS_F_ALWAYS_INLINE int
ucp_rndv_test_zcopy_scheme_support(size_t length, size_t min_zcopy,
                                   size_t max_zcopy, int split)
{
    return /* is the current message greater than the minimal GET/PUT Zcopy? */
           (length >= min_zcopy) &&
           /* is the current message less than the maximal GET/PUT Zcopy? */
           ((length <= max_zcopy) ||
            /* or can the message be split? */ split);
}

UCS_PROFILE_FUNC_VOID(ucp_rndv_receive, (worker, rreq, rndv_rts_hdr, rkey_buf),
                      ucp_worker_h worker, ucp_request_t *rreq,
                      const ucp_rndv_rts_hdr_t *rndv_rts_hdr,
                      const void *rkey_buf)
{
    ucp_rndv_mode_t rndv_mode;
    ucp_request_t *rndv_req;
    ucp_ep_h ep;
    ucp_ep_config_t *ep_config;
    ucs_status_t status;
    int is_get_zcopy_failed;
    ucp_ep_rndv_zcopy_config_t *get_zcopy;

    UCS_ASYNC_BLOCK(&worker->async);

    UCS_PROFILE_REQUEST_EVENT(rreq, "rndv_receive", 0);

    /* if receiving a message on an already closed endpoint, stop processing */
    UCP_WORKER_GET_VALID_EP_BY_ID(&ep, worker, rndv_rts_hdr->sreq.ep_id,
                                  { status = UCS_ERR_CANCELED; goto err; },
                                  "RNDV rts");

    /* the internal send request allocated on receiver side (to perform a "get"
     * operation, send "ATS" and "RTR") */
    rndv_req = ucp_request_get(worker);
    if (rndv_req == NULL) {
        ucs_error("failed to allocate rendezvous reply");
        status = UCS_ERR_NO_MEMORY;
        goto err;
    }

    rndv_req->flags      = 0;
    rndv_req->send.ep    = ep;
    rndv_req->send.mdesc = NULL;
    is_get_zcopy_failed  = 0;

    ucp_trace_req(rreq,
                  "rndv matched remote {address 0x%"PRIx64" size %zu sreq_id "
                  "0x%"PRIx64"} rndv_sreq %p", rndv_rts_hdr->address,
                  rndv_rts_hdr->size, rndv_rts_hdr->sreq.req_id, rndv_req);

    if (ucs_unlikely(rreq->recv.length < rndv_rts_hdr->size)) {
        ucp_trace_req(rndv_req,
                      "rndv truncated remote size %zu local size %zu rreq %p",
                      rndv_rts_hdr->size, rreq->recv.length, rreq);
        ucp_rndv_req_send_ack(rndv_req, rreq, rndv_rts_hdr->sreq.req_id, UCS_OK,
                              UCP_AM_ID_RNDV_ATS, "send_ats");
        ucp_request_recv_generic_dt_finish(rreq);
        ucp_rndv_zcopy_recv_req_complete(rreq, UCS_ERR_MESSAGE_TRUNCATED);
        goto out;
    }

    /* if the receive side is not connected yet then the RTS was received on a stub ep */
    ep_config = ucp_ep_config(ep);
    get_zcopy = &ep_config->rndv.get_zcopy;
    rndv_mode = worker->context->config.ext.rndv_mode;

    if (ucp_rndv_is_rkey_ptr(rndv_rts_hdr, rkey_buf, ep, rreq->recv.mem_type,
                             rndv_mode)) {
        ucp_rndv_do_rkey_ptr(rndv_req, rreq, rndv_rts_hdr, rkey_buf);
        goto out;
    }

    if (UCP_DT_IS_CONTIG(rreq->recv.datatype)) {
        if ((rndv_rts_hdr->address != 0) &&
            ucp_rndv_test_zcopy_scheme_support(rndv_rts_hdr->size,
                                               get_zcopy->min, get_zcopy->max,
                                               get_zcopy->split)) {
            /* try to fetch the data with a get_zcopy operation */
            status = ucp_rndv_req_send_rma_get(rndv_req, rreq, rndv_rts_hdr,
                                               rkey_buf);
            if (status == UCS_OK) {
                goto out;
            }

            /* fallback to non get zcopy protocol */
            ucp_rkey_destroy(rndv_req->send.rndv.rkey);
            is_get_zcopy_failed = 1;
        }

        if (rndv_mode == UCP_RNDV_MODE_AUTO) {
            /* check if we need pipelined memtype staging */
            if (UCP_MEM_IS_GPU(rreq->recv.mem_type) &&
                ucp_rndv_is_recv_pipeline_needed(rndv_req, rndv_rts_hdr,
                                                 rkey_buf, rreq->recv.mem_type,
                                                 is_get_zcopy_failed)) {
                ucp_rndv_recv_data_init(rreq, rndv_rts_hdr->size);
                if (ucp_rndv_is_put_pipeline_needed(rndv_rts_hdr->address,
                                                    rndv_rts_hdr->size,
                                                    get_zcopy->min,
                                                    get_zcopy->max,
                                                    is_get_zcopy_failed)) {
                    /* send FRAG RTR for sender to PUT the fragment. */
                    ucp_rndv_send_frag_rtr(worker, rndv_req, rreq, rndv_rts_hdr);
                } else {
                    /* sender address is present. do GET pipeline */
                    ucp_rndv_recv_start_get_pipeline(worker, rndv_req, rreq,
                                                     rndv_rts_hdr->sreq.req_id,
                                                     rkey_buf,
                                                     rndv_rts_hdr->address,
                                                     rndv_rts_hdr->size, 0);
                }
                goto out;
            }
        }

        if (!is_get_zcopy_failed) {
            /* put protocol is allowed - register receive buffer memory for rma */
            ucs_assert(rndv_rts_hdr->size <= rreq->recv.length);
            ucp_request_recv_buffer_reg(rreq, ep_config->key.rma_bw_md_map,
                                        rndv_rts_hdr->size);
        }
    }

    /* The sender didn't specify its address in the RTS, or the rndv mode was
     * configured to PUT, or GET rndv mode is unsupported - send an RTR and
     * the sender will send the data with active message or put_zcopy. */
    ucp_rndv_recv_data_init(rreq, rndv_rts_hdr->size);
    UCP_WORKER_STAT_RNDV(ep->worker, SEND_RTR, 1);
    ucp_rndv_req_send_rtr(rndv_req, rreq, rndv_rts_hdr->sreq.req_id,
                          rndv_rts_hdr->size, 0ul);

out:
    UCS_ASYNC_UNBLOCK(&worker->async);
    return;

err:
    ucp_rndv_recv_req_complete(rreq, status);
    goto out;
}

UCS_PROFILE_FUNC(ucs_status_t, ucp_rndv_rts_handler,
                 (arg, data, length, tl_flags),
                 void *arg, void *data, size_t length, unsigned tl_flags)
{
    ucp_worker_h worker         = arg;
    ucp_rndv_rts_hdr_t *rts_hdr = data;

    if (rts_hdr->flags & UCP_RNDV_RTS_FLAG_TAG) {
        return ucp_tag_rndv_process_rts(worker, rts_hdr, length, tl_flags);
    } else {
        ucs_assert(rts_hdr->flags & UCP_RNDV_RTS_FLAG_AM);
        return ucp_am_rndv_process_rts(arg, data, length, tl_flags);
    }
}

UCS_PROFILE_FUNC(ucs_status_t, ucp_rndv_ats_handler,
                 (arg, data, length, flags),
                 void *arg, void *data, size_t length, unsigned flags)
{
    ucp_worker_h worker      = arg;
    ucp_reply_hdr_t *rep_hdr = data;
    ucp_request_t *sreq;

    if (worker->context->config.ext.proto_enable) {
        return ucp_proto_rndv_ats_handler(arg, data, length, flags);
    }

    UCP_REQUEST_GET_BY_ID(&sreq, worker, rep_hdr->req_id, 1, return UCS_OK,
                          "RNDV ATS %p", rep_hdr);

    /* dereg the original send request and set it to complete */
    UCS_PROFILE_REQUEST_EVENT(sreq, "rndv_ats_recv", 0);
    if (sreq->flags & UCP_REQUEST_FLAG_OFFLOADED) {
        ucp_tag_offload_cancel_rndv(sreq);
    }

    ucp_request_complete_and_dereg_send(sreq, rep_hdr->status);
    return UCS_OK;
}

ucs_status_t ucp_rndv_rts_handle_status_from_pending(ucp_request_t *sreq,
                                                     ucs_status_t status)
{
    /* we rely on the fact that the RTS isn't being sent by an AM Bcopy multi */
    ucs_assert((status != UCP_STATUS_PENDING_SWITCH) &&
               (status != UCS_INPROGRESS));

    if (ucs_unlikely(status != UCS_OK)) {
        if (status == UCS_ERR_NO_RESOURCE) {
            return UCS_ERR_NO_RESOURCE;
        }

        ucp_request_id_release(sreq);
        ucp_request_complete_and_dereg_send(sreq, status);
    }

    return UCS_OK;
}

static size_t ucp_rndv_pack_data(void *dest, void *arg)
{
    ucp_rndv_data_hdr_t *hdr = dest;
    ucp_request_t *sreq = arg;
    size_t length, offset;

    offset       = sreq->send.state.dt.offset;
    hdr->rreq_id = sreq->send.rndv_data.remote_req_id;
    hdr->offset  = offset;
    length       = ucs_min(sreq->send.length - offset,
                           ucp_ep_get_max_bcopy(sreq->send.ep, sreq->send.lane) - sizeof(*hdr));

    return sizeof(*hdr) + ucp_dt_pack(sreq->send.ep->worker, sreq->send.datatype,
                                      sreq->send.mem_type, hdr + 1, sreq->send.buffer,
                                      &sreq->send.state.dt, length);
}

UCS_PROFILE_FUNC(ucs_status_t, ucp_rndv_progress_am_bcopy, (self),
                 uct_pending_req_t *self)
{
    ucp_request_t *sreq = ucs_container_of(self, ucp_request_t, send.uct);
    ucp_ep_t *ep        = sreq->send.ep;
    int single          = (sreq->send.length + sizeof(ucp_rndv_data_hdr_t)) <=
                          ucp_ep_config(ep)->am.max_bcopy;
    ucs_status_t status;

    if (single) {
        /* send a single bcopy message */
        status = ucp_do_am_bcopy_single(self, UCP_AM_ID_RNDV_DATA,
                                        ucp_rndv_pack_data);
        ucs_assert(status != UCS_INPROGRESS);
    } else {
        status = ucp_do_am_bcopy_multi(self, UCP_AM_ID_RNDV_DATA,
                                       UCP_AM_ID_RNDV_DATA,
                                       ucp_rndv_pack_data,
                                       ucp_rndv_pack_data, 1);

        if (status == UCS_INPROGRESS) {
            return UCS_INPROGRESS;
        } else if (ucs_unlikely(status == UCP_STATUS_PENDING_SWITCH)) {
            return UCS_OK;
        }
    }

    if (ucs_unlikely(status == UCS_ERR_NO_RESOURCE)) {
        return UCS_ERR_NO_RESOURCE;
    }

    ucp_request_complete_and_dereg_send(sreq, status);

    return UCS_OK;
}

UCS_PROFILE_FUNC(ucs_status_t, ucp_rndv_progress_rma_put_zcopy, (self),
                 uct_pending_req_t *self)
{
    ucp_request_t *sreq = ucs_container_of(self, ucp_request_t, send.uct);
    uct_rkey_t uct_rkey;

    ucs_assert_always(sreq->send.rndv.lanes_count > 0);

    /* Figure out which lane to use for put operation */
    sreq->send.lane = ucp_rndv_zcopy_get_lane(sreq, &uct_rkey,
                                              UCP_REQUEST_SEND_PROTO_RNDV_PUT);
    if (sreq->send.lane == UCP_NULL_LANE) {
        /* Unexpected behavior */
        ucs_fatal("sreq %p: unable to get PUT Zcopy lane", sreq);
    }

    return ucp_rndv_progress_rma_zcopy_common(sreq, sreq->send.lane, uct_rkey,
                                              UCP_REQUEST_SEND_PROTO_RNDV_PUT);
}

static void ucp_rndv_am_zcopy_send_req_complete(ucp_request_t *req,
                                                ucs_status_t status)
{
    ucs_assert(req->send.state.uct_comp.count == 0);
    ucp_request_send_buffer_dereg(req);
    ucp_request_complete_send(req, status);
}

static void ucp_rndv_am_zcopy_completion(uct_completion_t *self)
{
    ucp_request_t *sreq = ucs_container_of(self, ucp_request_t,
                                           send.state.uct_comp);
    ucs_status_t status = self->status;

    if (sreq->send.state.dt.offset == sreq->send.length) {
        ucp_rndv_am_zcopy_send_req_complete(sreq, status);
    } else if (status != UCS_OK) {
        ucs_fatal("error handling is unsupported with rendezvous protocol");
    }
}

static ucs_status_t ucp_rndv_progress_am_zcopy_single(uct_pending_req_t *self)
{
    ucp_request_t *sreq = ucs_container_of(self, ucp_request_t, send.uct);
    ucp_rndv_data_hdr_t hdr;

    hdr.rreq_id = sreq->send.rndv_data.remote_req_id;
    hdr.offset  = 0;
    return ucp_do_am_zcopy_single(self, UCP_AM_ID_RNDV_DATA, &hdr, sizeof(hdr),
                                  NULL, 0ul,
                                  ucp_rndv_am_zcopy_send_req_complete);
}

static ucs_status_t ucp_rndv_progress_am_zcopy_multi(uct_pending_req_t *self)
{
    ucp_request_t *sreq = ucs_container_of(self, ucp_request_t, send.uct);
    ucp_rndv_data_hdr_t hdr;

    hdr.rreq_id = sreq->send.rndv_data.remote_req_id;
    hdr.offset  = sreq->send.state.dt.offset;
    return ucp_do_am_zcopy_multi(self,
                                 UCP_AM_ID_RNDV_DATA,
                                 UCP_AM_ID_RNDV_DATA,
                                 &hdr, sizeof(hdr),
                                 &hdr, sizeof(hdr),
                                 NULL, 0ul,
                                 ucp_rndv_am_zcopy_send_req_complete, 1);
}

UCS_PROFILE_FUNC_VOID(ucp_rndv_send_frag_put_completion, (self),
                      uct_completion_t *self)
{
    ucp_request_t *freq = ucs_container_of(self, ucp_request_t,
                                           send.state.uct_comp);
    ucp_request_t *fsreq, *sreq;

    if (freq->send.state.dt.offset != freq->send.length) {
        return;
    }

    /* release memory descriptor */
    if (freq->send.mdesc != NULL) {
        ucs_mpool_put_inline((void*)freq->send.mdesc);
    }

    fsreq                        = freq->super_req;
    sreq                         = fsreq->super_req;
    fsreq->send.state.dt.offset += freq->send.length;
    ucs_assert(fsreq->send.state.dt.offset <= fsreq->send.length);

    /* send ATP for last fragment of the rndv request */
    if (fsreq->send.length == fsreq->send.state.dt.offset) {
        ucp_rkey_destroy(fsreq->send.rndv.rkey);

        sreq->send.state.dt.offset += fsreq->send.length;

        /* keep a status of a send request up to date updating it by a status from
         * a request created for tracking a UCT PUT Zcopy operation */
        uct_completion_update_status(&sreq->send.state.uct_comp, self->status);
        ucp_rndv_complete_rma_put_zcopy(sreq, 1);

        ucp_rndv_req_send_ack(fsreq, fsreq, fsreq->send.rndv.remote_req_id,
                              self->status, UCP_AM_ID_RNDV_ATP, "send_frag_atp");
    }

    /* release registered memory during doing PUT operation for a given fragment */
    ucp_request_send_buffer_dereg(freq);
    ucp_request_put(freq);
}

UCS_PROFILE_FUNC_VOID(ucp_rndv_put_pipeline_frag_get_completion, (self),
                      uct_completion_t *self)
{
    ucp_request_t *freq  = ucs_container_of(self, ucp_request_t,
                                            send.state.uct_comp);
    ucp_request_t *fsreq = freq->super_req;

    /* get rkey can be NULL if memtype ep doesn't need RKEY */
    if (freq->send.rndv.rkey != NULL) {
        ucp_rkey_destroy(freq->send.rndv.rkey);
    }

    /* get completed on memtype endpoint to stage on host. send put request to receiver*/
    ucp_request_send_state_reset(freq, ucp_rndv_send_frag_put_completion,
                                 UCP_REQUEST_SEND_PROTO_RNDV_PUT);
    ucp_rndv_req_init_remote_from_super_req(freq, fsreq,
                                            freq->send.rndv.remote_address -
                                            (uint64_t)fsreq->send.buffer);

    freq->send.uct.func                  = ucp_rndv_progress_rma_put_zcopy;
    freq->send.lane                      = fsreq->send.lane;
    freq->send.state.dt.dt.contig.md_map = 0;

    ucp_request_send(freq, 0);
}

static ucs_status_t ucp_rndv_send_start_put_pipeline(ucp_request_t *sreq,
                                                     ucp_rndv_rtr_hdr_t *rndv_rtr_hdr)
{
    ucp_ep_h ep             = sreq->send.ep;
    ucp_ep_config_t *config = ucp_ep_config(ep);
    ucp_worker_h worker     = sreq->send.ep->worker;
    ucp_context_h context   = worker->context;
    const uct_md_attr_t *md_attr;
    ucp_request_t *freq;
    ucp_request_t *fsreq;
    size_t max_frag_size, rndv_size, length;
    size_t offset, rndv_base_offset;
    size_t min_zcopy, max_zcopy;
    uct_rkey_t uct_rkey;

    ucp_trace_req(sreq, "using put rndv pipeline protocol");

    /* Protocol:
     * Step 1: GET fragment from send buffer to HOST fragment buffer
     * Step 2: PUT from fragment HOST buffer to remote HOST fragment buffer
     * Step 3: send ATP for each fragment request
     */

    min_zcopy        = config->rndv.put_zcopy.min;
    max_zcopy        = config->rndv.put_zcopy.max;
    rndv_size        = ucs_min(rndv_rtr_hdr->size, sreq->send.length);
    max_frag_size    = ucs_min(context->config.ext.rndv_frag_size, max_zcopy);
    rndv_base_offset = rndv_rtr_hdr->offset;

    /* initialize send req state on first fragment rndv request */
    if (rndv_base_offset == 0) {
        ucp_request_send_state_reset(sreq, NULL,
                                     UCP_REQUEST_SEND_PROTO_RNDV_PUT);
        ucp_rndv_req_init_zcopy_lane_map(sreq, sreq->send.rndv.rkey->mem_type,
                                         UCP_REQUEST_SEND_PROTO_RNDV_PUT);

        /* check if lane could be allocated */
        sreq->send.lane =
            ucp_rndv_zcopy_get_lane(sreq, &uct_rkey,
                                    UCP_REQUEST_SEND_PROTO_RNDV_PUT);
        if (sreq->send.lane == UCP_NULL_LANE) {
            return UCS_ERR_UNSUPPORTED;
        }

        /* check if lane supports host memory, to stage sends through host memory */
        md_attr = ucp_ep_md_attr(sreq->send.ep, sreq->send.lane);
        if (!(md_attr->cap.reg_mem_types & UCS_BIT(UCS_MEMORY_TYPE_HOST))) {
            return UCS_ERR_UNSUPPORTED;
        }

        /* check if mem type endpoint is exists */
        if (!UCP_MEM_IS_HOST(sreq->send.mem_type) &&
            (worker->mem_type_ep[sreq->send.mem_type] == NULL)) {
            return UCS_ERR_UNSUPPORTED;
        }
    }

    sreq->send.rndv.remote_address = rndv_rtr_hdr->address;

    /* internal send request allocated on sender side to handle send fragments for RTR */
    fsreq = ucp_request_get(worker);
    if (fsreq == NULL) {
        ucs_fatal("failed to allocate fragment receive request");
    }

    ucp_request_send_state_init(fsreq, ucp_dt_make_contig(1), 0);
    ucp_rndv_req_init_from_super_req(fsreq, sreq, rndv_size, rndv_base_offset,
                                     0, rndv_rtr_hdr->rreq_id);
    fsreq->send.mem_type        = sreq->send.mem_type;
    fsreq->send.state.dt.offset = 0;

    offset = 0;
    while (offset != rndv_size) {
        length = ucp_rndv_adjust_zcopy_length(min_zcopy, max_frag_size, 0,
                                              rndv_size, offset,
                                              rndv_size - offset);

        if (UCP_MEM_IS_HOST(sreq->send.mem_type)) {
            /* sbuf is in host, directly do put */
            freq = ucp_request_get(worker);
            if (ucs_unlikely(freq == NULL)) {
                ucs_error("failed to allocate fragment receive request");
                return UCS_ERR_NO_MEMORY;
            }

            ucp_request_send_state_init(freq, ucp_dt_make_contig(1), 0);
            ucp_request_send_state_reset(freq, ucp_rndv_send_frag_put_completion,
                                         UCP_REQUEST_SEND_PROTO_RNDV_PUT);

            ucp_rndv_req_init_from_super_req(freq, fsreq, length, offset,
                                             offset, UCP_REQUEST_ID_INVALID);
            freq->send.datatype     = ucp_dt_make_contig(1);
            freq->send.mem_type     = UCS_MEMORY_TYPE_HOST;
            freq->send.uct.func     = ucp_rndv_progress_rma_put_zcopy;
            freq->send.mdesc        = NULL;
            freq->send.pending_lane = UCP_NULL_LANE;

            ucp_request_send(freq, 0);
        } else {
            ucp_rndv_send_frag_get_mem_type(
                    fsreq, length,
                    (uint64_t)UCS_PTR_BYTE_OFFSET(fsreq->send.buffer, offset),
                    fsreq->send.mem_type, NULL, NULL, UCS_BIT(0), 0,
                    ucp_rndv_put_pipeline_frag_get_completion);
        }

        offset += length;
    }

    return UCS_OK;
}

UCS_PROFILE_FUNC(ucs_status_t, ucp_rndv_atp_handler,
                 (arg, data, length, flags),
                 void *arg, void *data, size_t length, unsigned flags)
{
    ucp_worker_h worker      = arg;
    ucp_reply_hdr_t *rep_hdr = data;
    ucp_request_t *rtr_sreq, *req;

    UCP_REQUEST_GET_BY_ID(&rtr_sreq, worker, rep_hdr->req_id, 1, return UCS_OK,
                          "RNDV ATP %p", rep_hdr);

    req = rtr_sreq->super_req;
    ucs_assert(req != NULL);
    ucp_request_put(rtr_sreq);

    if (req->flags & UCP_REQUEST_FLAG_RNDV_FRAG) {
        /* received ATP for frag RTR request */
        ucs_assert(req->super_req != NULL);
        UCS_PROFILE_REQUEST_EVENT(req, "rndv_frag_atp_recv", 0);
        ucp_rndv_recv_frag_put_mem_type(req->super_req, req,
                                        (ucp_mem_desc_t*)req->recv.buffer - 1,
                                        req->recv.length,
                                        req->recv.frag.offset);
    } else {
        UCS_PROFILE_REQUEST_EVENT(req, "rndv_atp_recv", 0);
        ucp_rndv_zcopy_recv_req_complete(req, UCS_OK);
    }

    return UCS_OK;
}

UCS_PROFILE_FUNC(ucs_status_t, ucp_rndv_rtr_handler,
                 (arg, data, length, flags),
                 void *arg, void *data, size_t length, unsigned flags)
{
    ucp_worker_h worker              = arg;
    ucp_context_h context            = worker->context;
    ucp_rndv_rtr_hdr_t *rndv_rtr_hdr = data;
    ucp_ep_rndv_zcopy_config_t *put_zcopy;
    ucp_request_t *sreq;
    ucp_ep_h ep;
    ucp_ep_config_t *ep_config;
    ucs_status_t status;
    int is_pipeline_rndv;
    uct_rkey_t uct_rkey;

    if (context->config.ext.proto_enable) {
        return ucp_proto_rndv_handle_rtr(arg, data, length, flags);
    }

    UCP_REQUEST_GET_BY_ID(&sreq, arg, rndv_rtr_hdr->sreq_id, 0, return UCS_OK,
                          "RNDV RTR %p", rndv_rtr_hdr);
    ep        = sreq->send.ep;
    ep_config = ucp_ep_config(ep);
    put_zcopy = &ep_config->rndv.put_zcopy;

    ucp_trace_req(sreq, "received rtr address 0x%"PRIx64" remote rreq_id"
                  "0x%"PRIx64, rndv_rtr_hdr->address, rndv_rtr_hdr->rreq_id);
    UCS_PROFILE_REQUEST_EVENT(sreq, "rndv_rtr_recv", 0);

    if (sreq->flags & UCP_REQUEST_FLAG_OFFLOADED) {
        /* Do not deregister memory here, because am zcopy rndv may
         * need it registered (if am and tag is the same lane). */
        ucp_tag_offload_cancel_rndv(sreq);
        ucs_assert(!ucp_ep_use_indirect_id(ep));
    }

    if (UCP_DT_IS_CONTIG(sreq->send.datatype) && rndv_rtr_hdr->address) {
        status = ucp_ep_rkey_unpack(ep, rndv_rtr_hdr + 1,
                                    &sreq->send.rndv.rkey);
        if (status != UCS_OK) {
            ucs_fatal("failed to unpack rendezvous remote key received from %s: %s",
                      ucp_ep_peer_name(ep), ucs_status_string(status));
        }

        is_pipeline_rndv = ((!UCP_MEM_IS_HOST(sreq->send.mem_type) ||
                             (sreq->send.length != rndv_rtr_hdr->size)) &&
                            (context->config.ext.rndv_mode != UCP_RNDV_MODE_PUT_ZCOPY));

        /*
         * Try pipeline protocol for non-host memory, if PUT_ZCOPY protocol is
         * not explicitly required. If pipeline is UNSUPPORTED, fallback to
         * PUT_ZCOPY anyway.
         */
        if (is_pipeline_rndv) {
            status = ucp_rndv_send_start_put_pipeline(sreq, rndv_rtr_hdr);
            if (status != UCS_ERR_UNSUPPORTED) {
                return status;
            }
            /* If we get here, it means that RNDV pipeline protocol is unsupported
             * and we have to use PUT_ZCOPY RNDV scheme instead */
        }

        if ((context->config.ext.rndv_mode != UCP_RNDV_MODE_GET_ZCOPY) &&
            ucp_rndv_test_zcopy_scheme_support(sreq->send.length,
                                               put_zcopy->min, put_zcopy->max,
                                               put_zcopy->split)) {
            ucp_request_send_state_reset(sreq, ucp_rndv_put_completion,
                                         UCP_REQUEST_SEND_PROTO_RNDV_PUT);
            sreq->send.uct.func            = ucp_rndv_progress_rma_put_zcopy;
            sreq->send.rndv.remote_req_id  = rndv_rtr_hdr->rreq_id;
            sreq->send.rndv.remote_address = rndv_rtr_hdr->address;
            sreq->send.mdesc               = NULL;
            sreq->send.pending_lane        = UCP_NULL_LANE;

            ucp_rndv_req_init_zcopy_lane_map(sreq, sreq->send.mem_type,
                                             UCP_REQUEST_SEND_PROTO_RNDV_PUT);

            sreq->send.lane =
                ucp_rndv_zcopy_get_lane(sreq, &uct_rkey,
                                        UCP_REQUEST_SEND_PROTO_RNDV_PUT);
            if (sreq->send.lane != UCP_NULL_LANE) {
                goto out_send;
            }
        }

        ucp_rkey_destroy(sreq->send.rndv.rkey);
    }

    ucp_trace_req(sreq, "using rdnv_data protocol");

    /* switch to AM */
    if (UCP_DT_IS_CONTIG(sreq->send.datatype) &&
        (sreq->send.length >=
         ep_config->am.mem_type_zcopy_thresh[sreq->send.mem_type]))
    {
        status = ucp_request_send_buffer_reg_lane(sreq, ucp_ep_get_am_lane(ep), 0);
        ucs_assert_always(status == UCS_OK);

        ucp_request_send_state_reset(sreq, ucp_rndv_am_zcopy_completion,
                                     UCP_REQUEST_SEND_PROTO_ZCOPY_AM);

        if ((sreq->send.length + sizeof(ucp_rndv_data_hdr_t)) <=
            ep_config->am.max_zcopy) {
            sreq->send.uct.func    = ucp_rndv_progress_am_zcopy_single;
        } else {
            sreq->send.uct.func    = ucp_rndv_progress_am_zcopy_multi;
            sreq->send.am_bw_index = 1;
        }
    } else {
        ucp_request_send_state_reset(sreq, NULL, UCP_REQUEST_SEND_PROTO_BCOPY_AM);
        sreq->send.uct.func        = ucp_rndv_progress_am_bcopy;
        sreq->send.am_bw_index     = 1;
    }

    sreq->send.rndv_data.remote_req_id = rndv_rtr_hdr->rreq_id;

out_send:
    /* if it is not a PUT pipeline protocol, delete the send request ID */
    ucp_request_id_release(sreq);
    ucp_request_send(sreq, 0);
    return UCS_OK;
}

UCS_PROFILE_FUNC(ucs_status_t, ucp_rndv_data_handler,
                 (arg, data, length, flags),
                 void *arg, void *data, size_t length, unsigned flags)
{
    ucp_worker_h worker                = arg;
    ucp_rndv_data_hdr_t *rndv_data_hdr = data;
    ucp_request_t *rreq, *rndv_req;
    size_t recv_len;
    ucs_status_t status;

    if (worker->context->config.ext.proto_enable) {
        return ucp_proto_rndv_handle_data(arg, data, length, flags);
    }

    UCP_REQUEST_GET_BY_ID(&rndv_req, worker, rndv_data_hdr->rreq_id, 0,
                          return UCS_OK, "RNDV data %p", rndv_data_hdr);

    rreq = rndv_req->super_req;
    ucs_assert(rreq != NULL);
    ucs_assert(!(rreq->flags & UCP_REQUEST_FLAG_RNDV_FRAG));
    ucs_assert(rreq->flags &
               (UCP_REQUEST_FLAG_RECV_AM | UCP_REQUEST_FLAG_RECV_TAG));

    recv_len = length - sizeof(*rndv_data_hdr);
    UCS_PROFILE_REQUEST_EVENT(rreq, "rndv_data_recv", recv_len);

    status = ucp_request_process_recv_data(rreq, rndv_data_hdr + 1, recv_len,
                                           rndv_data_hdr->offset, 1,
                                           rreq->flags &
                                                   UCP_REQUEST_FLAG_RECV_AM);
    if (status != UCS_INPROGRESS) {
        ucp_request_id_release(rndv_req);
        ucp_request_put(rndv_req);
    }

    return UCS_OK;
}

static void ucp_rndv_dump_rkey(const void *packed_rkey, char *buffer, size_t max)
{
    UCS_STRING_BUFFER_FIXED(strb, buffer, max);

    ucs_string_buffer_appendf(&strb, " rkey ");
    ucp_rkey_dump_packed(packed_rkey, &strb);
}

static void ucp_rndv_dump(ucp_worker_h worker, uct_am_trace_type_t type,
                          uint8_t id, const void *data, size_t length,
                          char *buffer, size_t max)
{

    const ucp_rndv_rts_hdr_t *rndv_rts_hdr = data;
    const ucp_rndv_rtr_hdr_t *rndv_rtr_hdr = data;
    const ucp_rndv_data_hdr_t *rndv_data   = data;
    const ucp_reply_hdr_t *rep_hdr         = data;
    UCS_STRING_BUFFER_ONSTACK(rts_info, 64);
    ucp_tag_rndv_rts_hdr_t *tag_rts;
    ucp_am_rndv_rts_hdr_t *am_rts;
    void *rkey_buf;

    switch (id) {
    case UCP_AM_ID_RNDV_RTS:
        ucs_assert(rndv_rts_hdr->sreq.ep_id != UCP_EP_ID_INVALID);

        if (rndv_rts_hdr->flags & UCP_RNDV_RTS_FLAG_AM) {
            am_rts   = ucs_derived_of(rndv_rts_hdr, ucp_am_rndv_rts_hdr_t);
            rkey_buf = am_rts + 1;
            ucs_string_buffer_appendf(&rts_info, "AM am_id %u",
                                      am_rts->am.am_id);
        } else {
            ucs_assert(rndv_rts_hdr->flags & UCP_RNDV_RTS_FLAG_TAG);

            tag_rts  = ucs_derived_of(rndv_rts_hdr, ucp_tag_rndv_rts_hdr_t);
            rkey_buf = tag_rts + 1;

            ucs_string_buffer_appendf(&rts_info, "TAG tag %"PRIx64"",
                                      tag_rts->tag.tag);
        }

        snprintf(buffer, max, "RNDV_RTS %s ep_id 0x%"PRIx64" sreq_id"
                 " 0x%"PRIx64" address 0x%"PRIx64" size %zu",
                 ucs_string_buffer_cstr(&rts_info), rndv_rts_hdr->sreq.ep_id,
                 rndv_rts_hdr->sreq.req_id, rndv_rts_hdr->address,
                 rndv_rts_hdr->size);

        if (rndv_rts_hdr->address) {
            ucp_rndv_dump_rkey(rkey_buf, buffer + strlen(buffer),
                               max - strlen(buffer));
        }
        break;
    case UCP_AM_ID_RNDV_ATS:
        snprintf(buffer, max, "RNDV_ATS sreq_id 0x%"PRIx64" status '%s'",
                 rep_hdr->req_id, ucs_status_string(rep_hdr->status));
        break;
    case UCP_AM_ID_RNDV_RTR:
        snprintf(buffer, max,
                 "RNDV_RTR sreq_id 0x%" PRIx64 " rreq_id 0x%" PRIx64
                 " address 0x%" PRIx64 " size %zu offset %zu",
                 rndv_rtr_hdr->sreq_id, rndv_rtr_hdr->rreq_id,
                 rndv_rtr_hdr->address, rndv_rtr_hdr->size,
                 rndv_rtr_hdr->offset);
        if (rndv_rtr_hdr->address != 0) {
            ucp_rndv_dump_rkey(rndv_rtr_hdr + 1, buffer + strlen(buffer),
                               max - strlen(buffer));
        }
        break;
    case UCP_AM_ID_RNDV_DATA:
        snprintf(buffer, max, "RNDV_DATA rreq_id 0x%"PRIx64" offset %zu",
                 rndv_data->rreq_id, rndv_data->offset);
        break;
    case UCP_AM_ID_RNDV_ATP:
        snprintf(buffer, max, "RNDV_ATP sreq_id 0x%"PRIx64" status '%s'",
                 rep_hdr->req_id, ucs_status_string(rep_hdr->status));
        break;
    default:
        return;
    }
}

UCP_DEFINE_AM(UCP_FEATURE_TAG | UCP_FEATURE_AM, UCP_AM_ID_RNDV_RTS,
              ucp_rndv_rts_handler, ucp_rndv_dump, 0);
UCP_DEFINE_AM(UCP_FEATURE_TAG | UCP_FEATURE_AM, UCP_AM_ID_RNDV_ATS,
              ucp_rndv_ats_handler, ucp_rndv_dump, 0);
UCP_DEFINE_AM(UCP_FEATURE_TAG | UCP_FEATURE_AM, UCP_AM_ID_RNDV_ATP,
              ucp_rndv_atp_handler, ucp_rndv_dump, 0);
UCP_DEFINE_AM(UCP_FEATURE_TAG | UCP_FEATURE_AM, UCP_AM_ID_RNDV_RTR,
              ucp_rndv_rtr_handler, ucp_rndv_dump, 0);
UCP_DEFINE_AM(UCP_FEATURE_TAG | UCP_FEATURE_AM, UCP_AM_ID_RNDV_DATA,
              ucp_rndv_data_handler, ucp_rndv_dump, 0);

UCP_DEFINE_AM_PROXY(UCP_AM_ID_RNDV_RTS);
UCP_DEFINE_AM_PROXY(UCP_AM_ID_RNDV_ATS);
UCP_DEFINE_AM_PROXY(UCP_AM_ID_RNDV_ATP);
UCP_DEFINE_AM_PROXY(UCP_AM_ID_RNDV_RTR);
UCP_DEFINE_AM_PROXY(UCP_AM_ID_RNDV_DATA);

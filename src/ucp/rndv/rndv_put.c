/**
 * Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2021. ALL RIGHTS RESERVED.
 *
 * See file LICENSE for terms.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "proto_rndv.inl"
#include "rndv_mtype.inl"

#include <ucp/core/ucp_request.inl>
#include <ucp/proto/proto_am.inl>
#include <ucp/proto/proto_multi.inl>
#include <ucp/proto/proto_single.inl>


#define UCP_PROTO_RNDV_PUT_DESC "write to remote"
#define UCP_PROTO_RNDV_RKEY_PTR_DESC "write to attached"


enum {
    /* Initial stage for put zcopy is sending the data */
    UCP_PROTO_RNDV_PUT_ZCOPY_STAGE_SEND = UCP_PROTO_STAGE_START,

    /* Initial stage for put memtype is copy the data to the fragment */
    UCP_PROTO_RNDV_PUT_MTYPE_STAGE_COPY = UCP_PROTO_STAGE_START,

    /* Flush all lanes to ensure remote delivery */
    UCP_PROTO_RNDV_PUT_STAGE_FLUSH,

    /* Send ATP without fence (could be done after a flush) */
    UCP_PROTO_RNDV_PUT_STAGE_ATP,

    /* Send ATP with fence (could be done if using send lanes for ATP) */
    UCP_PROTO_RNDV_PUT_STAGE_FENCED_ATP,

    /* Memtype only: send the fragment to the remote side */
    UCP_PROTO_RNDV_PUT_MTYPE_STAGE_SEND
};


typedef struct {
    ucp_proto_rndv_ack_priv_t ack;
    ucp_proto_single_priv_t   spriv;
    ucp_md_index_t            alloc_md_index;
} ucp_proto_rndv_rkey_ptr_mtype_priv_t;


static UCS_F_ALWAYS_INLINE ucs_status_t
ucp_proto_rndv_put_common_send(ucp_request_t *req,
                               const ucp_proto_multi_lane_priv_t *lpriv,
                               const uct_iov_t *iov, uct_completion_t *comp)
{
    uct_rkey_t tl_rkey      = ucp_rkey_get_tl_rkey(req->send.rndv.rkey,
                                                   lpriv->super.rkey_index);
    uint64_t remote_address = req->send.rndv.remote_address +
                              req->send.state.dt_iter.offset;

    return uct_ep_put_zcopy(ucp_ep_get_lane(req->send.ep, lpriv->super.lane),
                            iov, 1, remote_address, tl_rkey, comp);
}

static void
ucp_proto_rndv_put_common_flush_completion_send_atp(uct_completion_t *uct_comp)
{
    ucp_request_t *req = ucs_container_of(uct_comp, ucp_request_t,
                                          send.state.uct_comp);
    const ucp_proto_rndv_put_priv_t *rpriv = req->send.proto_config->priv;

    ucp_trace_req(req, "rndv_put_common_completion_send_atp");
    ucp_proto_completion_init(&req->send.state.uct_comp, rpriv->atp_comp_cb);
    ucp_proto_request_set_stage(req, UCP_PROTO_RNDV_PUT_STAGE_ATP);
    ucp_request_send(req);
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucp_proto_rndv_put_common_flush_send(ucp_request_t *req, ucp_lane_index_t lane)
{
    ucp_ep_h ep = req->send.ep;

    ucp_trace_req(req, "flush lane[%d] " UCT_TL_RESOURCE_DESC_FMT, lane,
                  UCT_TL_RESOURCE_DESC_ARG(ucp_ep_get_tl_rsc(ep, lane)));
    return uct_ep_flush(ucp_ep_get_lane(ep, lane), 0,
                        &req->send.state.uct_comp);
}

static ucs_status_t
ucp_proto_rndv_put_common_flush_progress(uct_pending_req_t *uct_req)
{
    ucp_request_t *req = ucs_container_of(uct_req, ucp_request_t, send.uct);

    return ucp_proto_multi_lane_map_progress(
            req, &req->send.rndv.put.flush_map,
            ucp_proto_rndv_put_common_flush_send);
}

static ucs_status_t
ucp_proto_rndv_put_common_atp_progress(uct_pending_req_t *uct_req)
{
    ucp_request_t *req = ucs_container_of(uct_req, ucp_request_t, send.uct);

    return ucp_proto_multi_lane_map_progress(req, &req->send.rndv.put.atp_map,
                                             ucp_proto_rndv_put_common_atp_send);
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucp_proto_rndv_put_common_fenced_atp_send(ucp_request_t *req,
                                          ucp_lane_index_t lane)
{
    ucs_status_t status;

    status = uct_ep_fence(ucp_ep_get_lane(req->send.ep, lane), 0);
    if (ucs_unlikely(status != UCS_OK)) {
        return status;
    }

    return ucp_proto_rndv_put_common_atp_send(req, lane);
}

static ucs_status_t
ucp_proto_rndv_put_common_fenced_atp_progress(uct_pending_req_t *uct_req)
{
    ucp_request_t *req = ucs_container_of(uct_req, ucp_request_t, send.uct);

    return ucp_proto_multi_lane_map_progress(
            req, &req->send.rndv.put.atp_map,
            ucp_proto_rndv_put_common_fenced_atp_send);
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucp_proto_rndv_put_common_data_sent(ucp_request_t *req)
{
    const ucp_proto_rndv_put_priv_t *rpriv = req->send.proto_config->priv;

    ucp_trace_req(req, "rndv_put_common_data_sent");
    ucp_proto_request_set_stage(req, rpriv->stage_after_put);
    return UCS_INPROGRESS;
}

static UCS_F_ALWAYS_INLINE void
ucp_proto_rndv_put_common_complete(ucp_request_t *req)
{
    ucp_trace_req(req, "rndv_put_common_complete");
    ucp_proto_rndv_rkey_destroy(req);
    ucp_proto_request_zcopy_complete(req, req->send.state.uct_comp.status);
}

static UCS_F_ALWAYS_INLINE void
ucp_proto_rndv_put_common_request_init(ucp_request_t *req)
{
    const ucp_proto_rndv_put_priv_t *rpriv = req->send.proto_config->priv;

    req->send.rndv.put.atp_map   = rpriv->atp_map;
    req->send.rndv.put.flush_map = rpriv->flush_map;
    ucp_proto_rndv_bulk_request_init(req, &rpriv->bulk);
}

static ucs_status_t
ucp_proto_rndv_put_common_init(const ucp_proto_init_params_t *init_params,
                               uint64_t rndv_modes, size_t max_length,
                               uct_ep_operation_t memtype_op, unsigned flags,
                               ucp_md_map_t initial_reg_md_map,
                               uct_completion_callback_t comp_cb,
                               int support_ppln)
{
    const size_t atp_size                = sizeof(ucp_rndv_ack_hdr_t);
    ucp_context_t *context               = init_params->worker->context;
    ucp_proto_rndv_put_priv_t *rpriv     = init_params->priv;
    ucp_proto_multi_init_params_t params = {
        .super.super         = *init_params,
        .super.overhead      = 0,
        .super.latency       = 0,
        .super.cfg_thresh    = ucp_proto_rndv_cfg_thresh(context, rndv_modes),
        .super.cfg_priority  = 0,
        .super.min_length    = 0,
        .super.max_length    = max_length,
        .super.min_iov       = 1,
        .super.min_frag_offs = ucs_offsetof(uct_iface_attr_t,
                                            cap.put.min_zcopy),
        .super.max_frag_offs = ucs_offsetof(uct_iface_attr_t,
                                            cap.put.max_zcopy),
        .super.max_iov_offs  = ucs_offsetof(uct_iface_attr_t, cap.put.max_iov),
        .super.send_op       = UCT_EP_OP_PUT_ZCOPY,
        .super.memtype_op    = memtype_op,
        .super.flags         = flags | UCP_PROTO_COMMON_INIT_FLAG_RECV_ZCOPY |
                               UCP_PROTO_COMMON_INIT_FLAG_REMOTE_ACCESS,
        .max_lanes           = context->config.ext.max_rndv_lanes,
        .initial_reg_md_map  = initial_reg_md_map,
        .first.tl_cap_flags  = UCT_IFACE_FLAG_PUT_ZCOPY,
        .first.lane_type     = UCP_LANE_TYPE_RMA_BW,
        .middle.tl_cap_flags = UCT_IFACE_FLAG_PUT_ZCOPY,
        .middle.lane_type    = UCP_LANE_TYPE_RMA_BW,
        .super.hdr_size      = 0,
        .opt_align_offs      = ucs_offsetof(uct_iface_attr_t,
                                            cap.put.opt_zcopy_align),
    };
    const uct_iface_attr_t *iface_attr;
    ucp_lane_index_t lane_idx, lane;
    int send_atp, use_fence;
    size_t bulk_priv_size;
    ucs_status_t status;

    if ((init_params->select_param->dt_class != UCP_DATATYPE_CONTIG) ||
        !ucp_proto_rndv_op_check(init_params, UCP_OP_ID_RNDV_SEND,
                                 support_ppln)) {
        return UCS_ERR_UNSUPPORTED;
    }

    status = ucp_proto_rndv_bulk_init(&params, &rpriv->bulk,
                                      UCP_PROTO_RNDV_PUT_DESC,
                                      UCP_PROTO_RNDV_ATP_NAME, &bulk_priv_size);
    if (status != UCS_OK) {
        return status;
    }

    *init_params->priv_size = ucs_offsetof(ucp_proto_rndv_put_priv_t, bulk) +
                              bulk_priv_size;

    /* Check if all potential lanes support sending ATP */
    rpriv     = params.super.super.priv;
    send_atp  = !ucp_proto_rndv_init_params_is_ppln_frag(init_params);
    use_fence = send_atp && !context->config.ext.rndv_put_force_flush;

    /* Check if all potential lanes support sending ATP */
    lane_idx  = 0;
    while (use_fence && (lane_idx < rpriv->bulk.mpriv.num_lanes)) {
        lane       = rpriv->bulk.mpriv.lanes[lane_idx++].super.lane;
        iface_attr = ucp_proto_common_get_iface_attr(init_params, lane);
        use_fence  = use_fence &&
                     (((iface_attr->cap.flags & UCT_IFACE_FLAG_AM_SHORT) &&
                       (iface_attr->cap.am.max_short >= atp_size)) ||
                      ((iface_attr->cap.flags & UCT_IFACE_FLAG_AM_BCOPY) &&
                       (iface_attr->cap.am.max_bcopy >= atp_size)));
    }

    /* All lanes can send ATP - invalidate am_lane, to use mpriv->lanes.
     * Otherwise, would need to flush all lanes and send ATP on
     * rpriv->super.lane when the flush is completed
     */
    if (use_fence) {
        /* Send fence followed by ATP on all lanes */
        rpriv->bulk.super.lane = UCP_NULL_LANE;
        rpriv->put_comp_cb     = comp_cb;
        rpriv->atp_comp_cb     = NULL;
        rpriv->stage_after_put = UCP_PROTO_RNDV_PUT_STAGE_FENCED_ATP;
        rpriv->flush_map       = 0;
        rpriv->atp_map         = rpriv->bulk.mpriv.lane_map;
    } else {
        /* Flush all lanes and send single ATP on control message lane */
        if (send_atp) {
            rpriv->put_comp_cb =
                    ucp_proto_rndv_put_common_flush_completion_send_atp;
            rpriv->atp_comp_cb = comp_cb;
            rpriv->atp_map     = UCS_BIT(rpriv->bulk.super.lane);
        } else {
            rpriv->put_comp_cb = comp_cb;
            rpriv->atp_comp_cb = NULL;
            rpriv->atp_map     = 0;
        }
        rpriv->stage_after_put = UCP_PROTO_RNDV_PUT_STAGE_FLUSH;
        rpriv->flush_map       = rpriv->bulk.mpriv.lane_map;
        ucs_assert(rpriv->flush_map != 0);
    }

    if (send_atp) {
        ucs_assert(rpriv->atp_map != 0);
    }
    rpriv->atp_num_lanes = ucs_popcount(rpriv->atp_map);

    return UCS_OK;
}

static const char *
ucp_proto_rndv_put_common_query(const ucp_proto_query_params_t *params,
                                ucp_proto_query_attr_t *attr)
{
    const ucp_proto_rndv_put_priv_t *rpriv     = params->priv;
    ucp_proto_query_params_t bulk_query_params = {
        .proto         = params->proto,
        .priv          = &rpriv->bulk,
        .worker        = params->worker,
        .select_param  = params->select_param,
        .ep_config_key = params->ep_config_key,
        .msg_length    = params->msg_length
    };

    ucp_proto_rndv_bulk_query(&bulk_query_params, attr);

    if (rpriv->atp_map == 0) {
        return UCP_PROTO_RNDV_PUT_DESC;
    } else if (rpriv->flush_map != 0) {
        return "flushed " UCP_PROTO_RNDV_PUT_DESC;
    } else {
        return "fenced " UCP_PROTO_RNDV_PUT_DESC;
    }
}

static UCS_F_ALWAYS_INLINE ucs_status_t ucp_proto_rndv_put_zcopy_send_func(
        ucp_request_t *req, const ucp_proto_multi_lane_priv_t *lpriv,
        ucp_datatype_iter_t *next_iter, ucp_lane_index_t *lane_shift)
{
    const ucp_proto_rndv_put_priv_t *rpriv = req->send.proto_config->priv;
    size_t max_payload;
    uct_iov_t iov;

    max_payload = ucp_proto_rndv_bulk_max_payload_align(req, &rpriv->bulk,
                                                        lpriv, lane_shift);
    ucp_datatype_iter_next_iov(&req->send.state.dt_iter, max_payload,
                               lpriv->super.md_index,
                               UCS_BIT(UCP_DATATYPE_CONTIG), next_iter, &iov,
                               1);
    return ucp_proto_rndv_put_common_send(req, lpriv, &iov,
                                          &req->send.state.uct_comp);
}

static ucs_status_t
ucp_proto_rndv_put_zcopy_send_progress(uct_pending_req_t *uct_req)
{
    ucp_request_t *req = ucs_container_of(uct_req, ucp_request_t, send.uct);
    const ucp_proto_rndv_put_priv_t *rpriv = req->send.proto_config->priv;

    return ucp_proto_multi_zcopy_progress(
            req, &rpriv->bulk.mpriv, ucp_proto_rndv_put_common_request_init,
            UCT_MD_MEM_ACCESS_LOCAL_READ, UCS_BIT(UCP_DATATYPE_CONTIG),
            ucp_proto_rndv_put_zcopy_send_func,
            ucp_proto_rndv_put_common_data_sent, rpriv->put_comp_cb);
}

static void ucp_proto_rndv_put_zcopy_completion(uct_completion_t *uct_comp)
{
    ucp_request_t *req = ucs_container_of(uct_comp, ucp_request_t,
                                          send.state.uct_comp);
    ucp_proto_rndv_put_common_complete(req);
}

static ucs_status_t
ucp_proto_rndv_put_zcopy_init(const ucp_proto_init_params_t *init_params)
{
    unsigned flags = UCP_PROTO_COMMON_INIT_FLAG_SEND_ZCOPY;

    return ucp_proto_rndv_put_common_init(init_params,
                                          UCS_BIT(UCP_RNDV_MODE_PUT_ZCOPY),
                                          SIZE_MAX, UCT_EP_OP_LAST, flags, 0,
                                          ucp_proto_rndv_put_zcopy_completion,
                                          0);
}

static void
ucp_proto_rndv_put_zcopy_query(const ucp_proto_query_params_t *params,
                               ucp_proto_query_attr_t *attr)
{
    const char *put_desc;

    put_desc = ucp_proto_rndv_put_common_query(params, attr);
    ucs_snprintf_safe(attr->desc, sizeof(attr->desc), "%s %s",
                      UCP_PROTO_ZCOPY_DESC, put_desc);
}

ucp_proto_t ucp_rndv_put_zcopy_proto = {
    .name     = "rndv/put/zcopy",
    .desc     = NULL,
    .flags    = 0,
    .init     = ucp_proto_rndv_put_zcopy_init,
    .query    = ucp_proto_rndv_put_zcopy_query,
    .progress = {
        [UCP_PROTO_RNDV_PUT_ZCOPY_STAGE_SEND] = ucp_proto_rndv_put_zcopy_send_progress,
        [UCP_PROTO_RNDV_PUT_STAGE_FLUSH]      = ucp_proto_rndv_put_common_flush_progress,
        [UCP_PROTO_RNDV_PUT_STAGE_ATP]        = ucp_proto_rndv_put_common_atp_progress,
        [UCP_PROTO_RNDV_PUT_STAGE_FENCED_ATP] = ucp_proto_rndv_put_common_fenced_atp_progress,
    },
    .abort    = (ucp_request_abort_func_t)ucs_empty_function_fatal_not_implemented_void,
    .reset    = (ucp_request_reset_func_t)ucs_empty_function_fatal_not_implemented_void
};


static void ucp_proto_rndv_put_mtype_pack_completion(uct_completion_t *uct_comp)
{
    ucp_request_t *req = ucs_container_of(uct_comp, ucp_request_t,
                                          send.state.uct_comp);
    const ucp_proto_rndv_put_priv_t *rpriv;

    ucp_trace_req(req, "mtype_pack_completion mdesc %p", req->send.rndv.mdesc);

    rpriv = req->send.proto_config->priv;
    ucp_proto_completion_init(&req->send.state.uct_comp, rpriv->put_comp_cb);
    ucp_proto_request_set_stage(req, UCP_PROTO_RNDV_PUT_MTYPE_STAGE_SEND);
    ucp_request_send(req);
}

static UCS_F_ALWAYS_INLINE ucs_status_t ucp_proto_rndv_put_mtype_send_func(
        ucp_request_t *req, const ucp_proto_multi_lane_priv_t *lpriv,
        ucp_datatype_iter_t *next_iter, ucp_lane_index_t *lane_shift)
{
    const ucp_proto_rndv_put_priv_t *rpriv = req->send.proto_config->priv;
    uct_iov_t iov;

    ucp_proto_rndv_mtype_next_iov(req, &rpriv->bulk, lpriv, next_iter, &iov);
    return ucp_proto_rndv_put_common_send(req, lpriv, &iov,
                                          &req->send.state.uct_comp);
}

static ucs_status_t
ucp_proto_rndv_put_mtype_copy_progress(uct_pending_req_t *uct_req)
{
    ucp_request_t *req = ucs_container_of(uct_req, ucp_request_t, send.uct);
    ucp_rsc_index_t memh_index = ucp_proto_rndv_mtype_get_memh_index(req);
    uct_mem_h memh;

    ucs_status_t status;

    ucs_assert(!(req->flags & UCP_REQUEST_FLAG_PROTO_INITIALIZED));

    status = ucp_proto_rndv_mtype_request_init(req);
    if (status != UCS_OK) {
        ucp_proto_request_abort(req, status);
        return UCS_OK;
    }

    ucp_proto_rndv_put_common_request_init(req);

    memh = ucp_proto_rndv_mtype_get_memh(req, memh_index);
    ucp_proto_rndv_mtype_copy(req, req->send.rndv.mdesc->ptr, memh,
                              uct_ep_get_zcopy,
                              ucp_proto_rndv_put_mtype_pack_completion,
                              "in from");

    req->flags |= UCP_REQUEST_FLAG_PROTO_INITIALIZED;
    return UCS_OK;
}

static ucs_status_t
ucp_proto_rndv_put_mtype_send_progress(uct_pending_req_t *uct_req)
{
    ucp_request_t *req = ucs_container_of(uct_req, ucp_request_t, send.uct);
    const ucp_proto_rndv_put_priv_t *rpriv;

    ucs_assert(req->flags & UCP_REQUEST_FLAG_PROTO_INITIALIZED);

    rpriv = req->send.proto_config->priv;
    return ucp_proto_multi_progress(req, &rpriv->bulk.mpriv,
                                    ucp_proto_rndv_put_mtype_send_func,
                                    ucp_proto_rndv_put_common_data_sent,
                                    UCS_BIT(UCP_DATATYPE_CONTIG));
}

static void ucp_proto_rndv_put_mtype_completion(uct_completion_t *uct_comp)
{
    ucp_request_t *req = ucs_container_of(uct_comp, ucp_request_t,
                                          send.state.uct_comp);

    ucp_trace_req(req, "rndv_put_mtype_completion");
    ucs_mpool_put(req->send.rndv.mdesc);
    ucp_proto_rndv_put_common_complete(req);
}

static void ucp_proto_rndv_put_mtype_frag_completion(uct_completion_t *uct_comp)
{
    ucp_request_t *req = ucs_container_of(uct_comp, ucp_request_t,
                                          send.state.uct_comp);

    ucp_trace_req(req, "rndv_put_mtype_frag_completion");
    ucs_mpool_put(req->send.rndv.mdesc);
    ucp_proto_rndv_ppln_send_frag_complete(req, 1);
}

static ucs_status_t
ucp_proto_rndv_put_mtype_init(const ucp_proto_init_params_t *init_params)
{
    uct_completion_callback_t comp_cb;
    ucp_md_map_t mdesc_md_map;
    ucs_status_t status;
    size_t frag_size;

    status = ucp_proto_rndv_mtype_init(init_params, &mdesc_md_map, &frag_size);
    if (status != UCS_OK) {
        return status;
    }

    if (ucp_proto_rndv_init_params_is_ppln_frag(init_params)) {
        comp_cb = ucp_proto_rndv_put_mtype_frag_completion;
    } else {
        comp_cb = ucp_proto_rndv_put_mtype_completion;
    }

    return ucp_proto_rndv_put_common_init(init_params,
                                          UCS_BIT(UCP_RNDV_MODE_PUT_PIPELINE),
                                          frag_size, UCT_EP_OP_GET_ZCOPY, 0,
                                          mdesc_md_map, comp_cb, 1);
}

static void
ucp_proto_rndv_put_mtype_query(const ucp_proto_query_params_t *params,
                               ucp_proto_query_attr_t *attr)
{
    const char *put_desc;

    put_desc = ucp_proto_rndv_put_common_query(params, attr);
    ucp_proto_rndv_mtype_query_desc(params, attr, put_desc);
}

ucp_proto_t ucp_rndv_put_mtype_proto = {
    .name     = "rndv/put/mtype",
    .desc     = NULL,
    .flags    = 0,
    .init     = ucp_proto_rndv_put_mtype_init,
    .query    = ucp_proto_rndv_put_mtype_query,
    .progress = {
        [UCP_PROTO_RNDV_PUT_MTYPE_STAGE_COPY] = ucp_proto_rndv_put_mtype_copy_progress,
        [UCP_PROTO_RNDV_PUT_MTYPE_STAGE_SEND] = ucp_proto_rndv_put_mtype_send_progress,
        [UCP_PROTO_RNDV_PUT_STAGE_FLUSH]      = ucp_proto_rndv_put_common_flush_progress,
        [UCP_PROTO_RNDV_PUT_STAGE_ATP]        = ucp_proto_rndv_put_common_atp_progress,
        [UCP_PROTO_RNDV_PUT_STAGE_FENCED_ATP] = ucp_proto_rndv_put_common_fenced_atp_progress,
    },
    .abort    = (ucp_request_abort_func_t)ucs_empty_function_fatal_not_implemented_void,
    .reset    = (ucp_request_reset_func_t)ucs_empty_function_fatal_not_implemented_void
};

static ucs_status_t ucp_proto_rndv_rkey_ptr_mtype_init_params(
        const ucp_proto_init_params_t *init_params,
        ucp_md_index_t alloc_md_index, size_t max_length)
{
    ucp_proto_rndv_rkey_ptr_mtype_priv_t *rpriv = init_params->priv;
    ucp_context_t *context                = init_params->worker->context;
    uint64_t rndv_modes                   = UCS_BIT(UCP_RNDV_MODE_PUT_PIPELINE);
    ucp_proto_single_init_params_t params = {
        .super.super         = *init_params,
        .super.overhead      = 0,
        .super.latency       = 0,
        .super.cfg_thresh    = ucp_proto_rndv_cfg_thresh(context, rndv_modes),
        .super.cfg_priority  = 0,
        .super.min_length    = 0,
        .super.max_length    = max_length,
        .super.min_iov       = 1,
        .super.min_frag_offs = ucs_offsetof(uct_iface_attr_t,
                                            cap.put.min_zcopy),
        .super.max_frag_offs = ucs_offsetof(uct_iface_attr_t,
                                            cap.put.max_zcopy),
        .super.max_iov_offs  = UCP_PROTO_COMMON_OFFSET_INVALID,
        .super.hdr_size      = 0,
        .super.send_op       = UCT_EP_OP_LAST,
        .super.memtype_op    = UCT_EP_OP_GET_ZCOPY,
        .super.flags         = UCP_PROTO_COMMON_INIT_FLAG_RKEY_PTR,
        .lane_type           = UCP_LANE_TYPE_RKEY_PTR,
        .tl_cap_flags        = 0,
    };
    ucp_proto_caps_t rkey_ptr_caps;
    ucs_status_t status;

    rpriv->alloc_md_index = alloc_md_index;

    params.super.super.caps = &rkey_ptr_caps;
    status = ucp_proto_single_init_priv(&params, &rpriv->spriv);
    if (status != UCS_OK) {
        return status;
    }

    *init_params->priv_size = sizeof(*rpriv);
    status = ucp_proto_rndv_ack_init(init_params, UCP_PROTO_RNDV_RKEY_PTR_DESC,
                                     &rkey_ptr_caps, UCS_LINEAR_FUNC_ZERO,
                                     &rpriv->ack);

    ucp_proto_select_caps_cleanup(&rkey_ptr_caps);

    return status;
}

static ucs_status_t
ucp_proto_rndv_rkey_ptr_mtype_init(const ucp_proto_init_params_t *init_params)
{
    ucp_worker_h worker           = init_params->worker;
    ucp_context_t *context        = worker->context;
    ucp_md_index_t alloc_md_index = UCP_NULL_RESOURCE;
    const uct_component_attr_t *cmpt_attr;
    const ucp_ep_config_t *ep_config;
    ucp_rsc_index_t cmpt_index;
    ucp_md_index_t md_index;
    ucs_status_t status;
    ucp_md_map_t mdesc_md_map;
    size_t frag_size;
    ucp_md_map_t md_map;

    if (!context->config.ext.rndv_shm_ppln_enable) {
        return UCS_ERR_UNSUPPORTED;
    }

    if (!ucp_proto_rndv_op_check(init_params, UCP_OP_ID_RNDV_SEND, 1) ||
        (init_params->rkey_config_key == NULL) ||
        (init_params->ep_cfg_index == UCP_WORKER_CFG_INDEX_NULL)) {
        return UCS_ERR_UNSUPPORTED;
    }

    status = ucp_proto_rndv_mtype_init(init_params, &mdesc_md_map, &frag_size);
    if (status != UCS_OK) {
        return status;
    }

    ep_config = &worker->ep_config[init_params->ep_cfg_index];
    /* Do not use xpmem, because cuda_copy registration will fail and
     * performance will not be optimal. */
    md_map    = init_params->rkey_config_key->md_map &
                ep_config->key.reachable_md_map &
                ~ep_config->rndv.rkey_ptr_dst_mds;

    ucs_for_each_bit(md_index, md_map) {
        cmpt_index = ucp_ep_config_get_dst_md_cmpt(&ep_config->key, md_index);
        cmpt_attr  = &context->tl_cmpts[cmpt_index].attr;
        if (cmpt_attr->flags & UCT_COMPONENT_FLAG_RKEY_PTR) {
            alloc_md_index = md_index;
            break;
        }
    }

    if (alloc_md_index == UCP_NULL_RESOURCE) {
        return UCS_ERR_UNSUPPORTED;
    }

    return ucp_proto_rndv_rkey_ptr_mtype_init_params(init_params,
                                                     alloc_md_index, frag_size);
}

static void ucp_proto_rndv_rkey_ptr_mtype_completion(uct_completion_t *uct_comp)
{
    ucp_request_t *req = ucs_container_of(uct_comp, ucp_request_t,
                                          send.state.uct_comp);

    ucp_trace_req(req, "ucp_proto_rndv_rkey_ptr_mtype_completion");
    ucp_proto_rndv_rkey_destroy(req);
    ucp_proto_request_zcopy_complete(req, req->send.state.uct_comp.status);
}

static void
ucp_proto_rndv_rkey_ptr_mtype_copy_completion(uct_completion_t *uct_comp)
{
    ucp_request_t *req = ucs_container_of(uct_comp, ucp_request_t,
                                          send.state.uct_comp);

    ucp_trace_req(req, "ucp_proto_rndv_rkey_ptr_mtype_copy_completion");

    ucp_proto_completion_init(&req->send.state.uct_comp,
                              ucp_proto_rndv_rkey_ptr_mtype_completion);
    ucp_proto_request_set_stage(req, UCP_PROTO_RNDV_PUT_STAGE_ATP);
    ucp_request_send(req);
}

static ucs_status_t
ucp_proto_rndv_rkey_ptr_mtype_copy_progress(uct_pending_req_t *uct_req)
{
    ucp_request_t *req    = ucs_container_of(uct_req, ucp_request_t, send.uct);
    ucp_context_h context = req->send.ep->worker->context;
    uint64_t remote_address           = req->send.rndv.remote_address;
    ucs_memory_type_t local_mem_type  = req->send.state.dt_iter.mem_info.type;
    ucs_memory_type_t remote_mem_type = req->send.rndv.rkey->mem_type;
    ucp_md_map_t md_map               = req->send.rndv.rkey->md_map;
    void *rkey_buffer                 = req->send.rndv.rkey_buffer;
    const ucp_proto_rndv_rkey_ptr_mtype_priv_t
                               *rpriv = req->send.proto_config->priv;
    ucp_lane_index_t mem_type_rma_lane;
    ucp_ep_peer_mem_data_t *ppln_data;
    ucp_ep_h mem_type_ep;
    unsigned rkey_index;
    void *local_ptr;
    ucs_status_t status;

    ucs_assert(!(req->flags & UCP_REQUEST_FLAG_PROTO_INITIALIZED));
    ucs_assert(req->send.rndv.rkey_buffer != NULL);
    ucs_assert(rpriv->alloc_md_index != UCP_NULL_RESOURCE);

    req->send.rndv.rkey_buffer = NULL;

    mem_type_ep = req->send.ep->worker->mem_type_ep[local_mem_type];
    if (mem_type_ep == NULL) {
        ucp_proto_request_abort(req, UCS_ERR_UNREACHABLE);
        return UCS_OK;
    }

    mem_type_rma_lane = ucp_ep_config(mem_type_ep)->key.rma_bw_lanes[0];
    ppln_data         = ucp_ep_peer_mem_get(context, req->send.ep,
                                            remote_address,
                                            req->send.state.dt_iter.length,
                                            rkey_buffer, rpriv->alloc_md_index);
    if (ppln_data->rkey == NULL) {
        ucp_proto_request_abort(req, UCS_ERR_UNREACHABLE);
        return UCS_OK;
    }

    rkey_index = ucs_bitmap2idx(ppln_data->rkey->md_map, rpriv->alloc_md_index);
    status     = uct_rkey_ptr(ppln_data->rkey->tl_rkey[rkey_index].cmpt,
                              &ppln_data->rkey->tl_rkey[rkey_index].rkey,
                              req->send.rndv.remote_address, &local_ptr);
    if (status != UCS_OK) {
        ppln_data->size = 0; /* Make sure hash element is updated next time */
        ucp_proto_request_abort(req, status);
        return UCS_OK;
    }

    if (ppln_data->uct_memh == NULL) {
        /* Register remote memory segment with memtype ep MD. Without
        * registration fetching data from GPU to CPU will be performance
        * inefficient. */
        md_map              = 0;
        ppln_data->md_index = ucp_ep_md_index(mem_type_ep, mem_type_rma_lane);

        status = ucp_mem_rereg_mds(context, UCS_BIT(ppln_data->md_index),
                                   local_ptr, ppln_data->size,
                                   UCT_MD_MEM_ACCESS_RMA |
                                           UCT_MD_MEM_FLAG_HIDE_ERRORS,
                                   NULL, remote_mem_type, NULL,
                                   &ppln_data->uct_memh, &md_map);

        if (status != UCS_OK) {
            ppln_data->md_index = UCP_NULL_RESOURCE;
        } else {
            ucs_assertv(md_map == UCS_BIT(ppln_data->md_index),
                        "mdmap=0x%lx, md_index=%u", md_map,
                        ppln_data->md_index);
        }
    }

    ucp_proto_rndv_mtype_copy(req, local_ptr, ppln_data->uct_memh,
                              uct_ep_get_zcopy,
                              ucp_proto_rndv_rkey_ptr_mtype_copy_completion,
                              "in from");

    req->flags |= UCP_REQUEST_FLAG_PROTO_INITIALIZED;
    return UCS_OK;
}

static ucs_status_t
ucp_proto_rndv_rkey_ptr_mtype_atp_progress(uct_pending_req_t *uct_req)
{
    ucp_request_t *req = ucs_container_of(uct_req, ucp_request_t, send.uct);
    const ucp_proto_rndv_rkey_ptr_mtype_priv_t
                *rpriv = req->send.proto_config->priv;

    ucp_proto_rndv_put_atp_pack_ctx_t pack_ctx;

    ucs_assert(rpriv->ack.lane != UCP_NULL_LANE);

    pack_ctx.req      = req;
    pack_ctx.ack_size = req->send.state.dt_iter.length;

    return ucp_proto_am_bcopy_single_progress(
        req, UCP_AM_ID_RNDV_ATP, rpriv->ack.lane,
        ucp_proto_rndv_put_common_pack_atp, &pack_ctx,
        sizeof(ucp_rndv_ack_hdr_t), ucp_request_invoke_uct_completion_success,
        0);
}

static void
ucp_proto_rndv_rkey_ptr_mtype_query(const ucp_proto_query_params_t *params,
                                    ucp_proto_query_attr_t *attr)
{
    const char *desc = UCP_PROTO_RNDV_RKEY_PTR_DESC;

    ucp_proto_rndv_bulk_query(params, attr);
    ucp_proto_rndv_mtype_query_desc(params, attr, desc);
}

ucp_proto_t ucp_rndv_rkey_ptr_mtype_proto = {
    .name     = "rndv/rkey_ptr/mtype",
    .desc     = NULL,
    .flags    = 0,
    .init     = ucp_proto_rndv_rkey_ptr_mtype_init,
    .query    = ucp_proto_rndv_rkey_ptr_mtype_query,
    .progress = {
        [UCP_PROTO_RNDV_PUT_MTYPE_STAGE_COPY] = ucp_proto_rndv_rkey_ptr_mtype_copy_progress,
        [UCP_PROTO_RNDV_PUT_STAGE_ATP]        = ucp_proto_rndv_rkey_ptr_mtype_atp_progress,
    },
    .abort    = (ucp_request_abort_func_t)ucs_empty_function_do_assert_void,
    .reset    = (ucp_request_reset_func_t)ucs_empty_function_fatal_not_implemented_void
};

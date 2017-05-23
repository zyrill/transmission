/*
 * This file Copyright (C) 2008-2014 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#include <string.h> /* strlen() */

#include <event2/buffer.h>
#include <event2/event.h>

#include "transmission.h"
#include "bandwidth.h"
#include "cache.h"
#include "inout.h" /* tr_ioFindFileLocation() */
#include "list.h"
#include "log.h"
#include "peer-mgr.h"
#include "torrent.h"
#include "trevent.h" /* tr_runInEventThread() */
#include "utils.h"
#include "web.h"
#include "webseed.h"

#if 0
#define dbgmsg(fmt, ...) fprintf(stderr, fmt "\n", __VA_ARGS__)
#elif 0
#define dbgmsg(...) tr_logAddDeepNamed("web", __VA_ARGS__)
#else
#define dbgmsg(...) ((void)0)
#endif

struct tr_webseed_task
{
    bool dead;
    struct evbuffer* content;
    struct tr_webseed* webseed;
    tr_session* session;
    tr_block_index_t block;
    tr_piece_index_t piece_index;
    uint32_t piece_offset;
    uint32_t length;
    tr_block_index_t blocks_done;
    uint32_t block_size;
    struct tr_web_task* web_task;
    long response_code;
};

struct tr_webseed
{
    tr_peer parent;
    tr_bandwidth bandwidth;
    tr_session* session;
    tr_peer_callback callback;
    void* callback_data;
    tr_list* tasks;
    struct event* timer;
    char* base_url;
    size_t base_url_len;
    int torrent_id;
    int consecutive_failures;
    int retry_tickcount;
    int retry_challenge;
    int idle_connections;
    int active_transfers;
    char** file_urls;
    uint8_t task_flag;
};

enum
{
    TR_IDLE_TIMER_MSEC = 2000,
    /* */
    FAILURE_RETRY_INTERVAL = 150,
    /* */
    MAX_CONSECUTIVE_FAILURES = 5,
    /* */
    MAX_CONNECTIONS_PER_WEBSEED = 4
};

/***
****
***/

static void publish(tr_webseed* w, tr_peer_event* e)
{
    if (w->callback != NULL)
    {
        (*w->callback)(&w->parent, e, w->callback_data);
    }
}

static void fire_client_got_rejs(tr_torrent* tor, tr_webseed* w, tr_block_index_t block, tr_block_index_t count)
{
    tr_peer_event e = TR_PEER_EVENT_INIT;
    e.eventType = TR_PEER_CLIENT_GOT_REJ;
    tr_torrentGetBlockLocation(tor, block, &e.pieceIndex, &e.offset, &e.length);

    for (tr_block_index_t i = 1; i <= count; i++)
    {
        if (i == count)
        {
            e.length = tr_torBlockCountBytes(tor, block + count - 1);
        }

        publish(w, &e);
        e.offset += e.length;
    }
}

static void fire_client_got_blocks(tr_torrent* tor, tr_webseed* w, tr_block_index_t block, tr_block_index_t count)
{
    tr_peer_event e = TR_PEER_EVENT_INIT;
    e.eventType = TR_PEER_CLIENT_GOT_BLOCK;
    tr_torrentGetBlockLocation(tor, block, &e.pieceIndex, &e.offset, &e.length);

    for (tr_block_index_t i = 1; i <= count; i++)
    {
        if (i == count)
        {
            e.length = tr_torBlockCountBytes(tor, block + count - 1);
        }

        publish(w, &e);
        e.offset += e.length;
    }
}

static void fire_client_got_piece_data(tr_webseed* w, uint32_t length)
{
    tr_peer_event e = TR_PEER_EVENT_INIT;
    e.eventType = TR_PEER_CLIENT_GOT_PIECE_DATA;
    e.length = length;
    publish(w, &e);
}

/***
****
***/

struct write_block_data
{
    tr_session* session;
    int torrent_id;
    struct tr_webseed* webseed;
    struct evbuffer* content;
    tr_piece_index_t piece_index;
    tr_block_index_t block_index;
    tr_block_index_t count;
    uint32_t block_offset;
};

static void write_block_func(void* vdata)
{
    struct write_block_data* data = vdata;
    struct tr_webseed* w = data->webseed;
    struct evbuffer* buf = data->content;
    struct tr_torrent* tor;

    tor = tr_torrentFindFromId(data->session, data->torrent_id);

    if (tor != NULL)
    {
        uint32_t const block_size = tor->blockSize;
        uint32_t len = evbuffer_get_length(buf);
        unsigned int blocks_written = 0;
        uint32_t const offset_end = data->block_offset + len;
        tr_cache* cache = data->session->cache;
        tr_piece_index_t const piece = data->piece_index;

        if (!tr_torrentPieceIsComplete(tor, piece))
        {
            while (data->count > 0)
            {
                uint32_t const bytes_this_pass = MIN(len, block_size);

                if (!tr_torrentBlockIsComplete(tor, data->block_index))
                {
                    tr_cacheWriteBlock(cache, tor, piece, offset_end - len, bytes_this_pass, buf);
                    len -= bytes_this_pass;
                    ++blocks_written;

                    dbgmsg("[write_block_func_1] ws %p piece %d buf %d this pass %d written %d count %d block_index %d url %s",
                        w, (int)piece, (int)len, (int)bytes_this_pass, blocks_written, (int)data->count, (int)data->block_index,
                        w->base_url);

                    fire_client_got_blocks(tor, w, data->block_index, 1);

                    if (data->count-- > 1)
                    {
                        ++data->block_index;
                    }
                }
                else if (data->count-- > 0)
                {
                    evbuffer_drain(buf, len);
                    len -= bytes_this_pass;

                    if (data->count > 0)
                    {
                        ++data->block_index;
                    }

                    dbgmsg("[write_block_func_2] ws %p piece %d buf %d this pass %d written %d count %d block_index %d url %s",
                        w, (int)piece, (int)len, (int)bytes_this_pass, blocks_written, (int)data->count, (int)data->block_index,
                        w->base_url);
                }
            }

            if (blocks_written > 0)
            {
                tr_bitfieldAdd(&w->parent.blame, piece);
            }
        }
    }

    evbuffer_free(buf);
    tr_free(data);
}

/***
****
***/

struct connection_succeeded_data
{
    struct tr_webseed* webseed;
    char* real_url;
    tr_piece_index_t piece_index;
    uint32_t piece_offset;
};

static void connection_succeeded(void* vdata)
{
    tr_torrent* tor;
    struct connection_succeeded_data* data = vdata;
    struct tr_webseed* w = data->webseed;

    if (++w->active_transfers >= w->retry_challenge && w->retry_challenge != 0)
    {
        /* the server seems to be accepting more connections now */
        w->consecutive_failures = w->retry_tickcount = w->retry_challenge = 0;
    }

    if (data->real_url != NULL && (tor = tr_torrentFindFromId(w->session, w->torrent_id)) != NULL)
    {
        uint64_t file_offset;
        tr_file_index_t file_index;

        tr_ioFindFileLocation(tor, data->piece_index, data->piece_offset, &file_index, &file_offset);
        tr_free(w->file_urls[file_index]);
        w->file_urls[file_index] = data->real_url;
        data->real_url = NULL;
    }

    tr_free(data->real_url);
    tr_free(data);
}

/***
****
***/

static bool piece_offset_is_valid(tr_torrent* tor, struct tr_webseed_task* t)
{
    bool is_valid;

    is_valid = evbuffer_get_length(t->content) <= t->length - (t->blocks_done * tor->blockSize);

    return is_valid;
}

static void on_content_changed(struct evbuffer* buf, struct evbuffer_cb_info const* info, void* vtask)
{
    size_t const n_added = info->n_added;
    struct tr_webseed_task* task = vtask;
    tr_session* session = task->session;

    tr_sessionLock(session);

    struct tr_webseed* w = task->webseed;
    tr_torrent* tor = tr_torrentFindFromId(w->session, w->torrent_id);

    dbgmsg("[on_content_changed_ALL] ws %p task %p piece %d strikes %d task_flag %d running %d", w, task->web_task,
        (int)task->piece_index, w->parent.strikes, w->task_flag, tr_list_size(w->tasks));

    /* stop corrupt data transfer: https://trac.transmissionbt.com/ticket/5969 */
    if (!piece_offset_is_valid(tor, task))
    {
        w->task_flag = TR_WEB_TASK_CORRUPT_DATA;
        tr_logAddTorDbg(tor, "Webseed:%s is sending corrupt data and has been disabled.", w->base_url);
    }

    if (tor != NULL && !task->dead && n_added > 0 && w->task_flag != TR_WEB_TASK_CORRUPT_DATA)
    {
        uint32_t len;

        tr_bandwidthUsed(&w->bandwidth, TR_DOWN, n_added, true, tr_time_msec());
        fire_client_got_piece_data(w, n_added);
        len = evbuffer_get_length(buf);

        if (task->response_code == 0)
        {
            tr_webGetTaskInfo(task->web_task, TR_WEB_GET_CODE, &task->response_code);

            if (task->response_code == 206)
            {
                char const* url;
                struct connection_succeeded_data* data;

                url = NULL;
                tr_webGetTaskInfo(task->web_task, TR_WEB_GET_REAL_URL, &url);

                data = tr_new(struct connection_succeeded_data, 1);
                data->webseed = w;
                data->real_url = tr_strdup(url);
                data->piece_index = task->piece_index;
                data->piece_offset = task->piece_offset + task->blocks_done * task->block_size + len - 1;

                /* processing this uses a tr_torrent pointer,
                   so push the work to the libevent thread... */
                tr_runInEventThread(w->session, connection_succeeded, data);
            }
        }

        if (task->response_code == 206 && len >= task->block_size)
        {
            /* once we've got at least one full block, save it */

            struct write_block_data* data;
            uint32_t const block_size = task->block_size;
            tr_block_index_t const completed = len / block_size;

            data = tr_new(struct write_block_data, 1);
            data->webseed = task->webseed;
            data->piece_index = task->piece_index;
            data->block_index = task->block + task->blocks_done;
            data->count = completed;
            data->block_offset = task->piece_offset + task->blocks_done * block_size;
            data->content = evbuffer_new();
            data->torrent_id = w->torrent_id;
            data->session = w->session;

            /* we don't use locking on this evbuffer so we must copy out the data
               that will be needed when writing the block in a different thread */
            evbuffer_remove_buffer(task->content, data->content, block_size * completed);

            tr_runInEventThread(w->session, write_block_func, data);
            task->blocks_done += completed;
        }

        w->task_flag = TR_WEB_TASK_SUCCESS;

        dbgmsg("[on_content_changed_206] ws %p task %p piece %d task_flag %d length %ld done %d buf %d this pass %d code %ld "
            "valid %d", w, task->web_task, (int)task->piece_index, w->task_flag, (long)task->length,
            (int)(task->blocks_done * task->block_size), (int)len, (int)((len / task->block_size) * task->block_size),
            task->response_code, (int)piece_offset_is_valid(tor, task));
    }

    if (w->task_flag == TR_WEB_TASK_CORRUPT_DATA)
    {
        dbgmsg("[on_content_changed_CD] ws %p task %p piece %d task_flag %d length %ld done %ld buf %d code %ld valid %d",
            w, task->web_task, (int)task->piece_index, w->task_flag, (long)task->length,
            (long)(task->blocks_done * tor->blockSize), (int)evbuffer_get_length(buf), task->response_code,
            (int)piece_offset_is_valid(tor, task));
    }

    tr_sessionUnlock(session);
}

static void task_request_next_chunk(struct tr_webseed_task* task);

static void on_idle(tr_webseed* w)
{
    int want;
    int running_tasks = tr_list_size(w->tasks);
    tr_torrent* tor = tr_torrentFindFromId(w->session, w->torrent_id);

    if (tor == NULL)
    {
        return;
    }

    if (w->task_flag == TR_WEB_TASK_INIT && (!tor->isRunning || tor->isStopping))
    {
        w->task_flag = 0;
    }

    if (w->task_flag == TR_WEB_TASK_INIT || w->task_flag == TR_WEB_TASK_ADDR_INVALID ||
        w->task_flag == TR_WEB_TASK_ADDR_BLOCKED || w->task_flag == TR_WEB_TASK_CORRUPT_DATA)
    {
        want = 0;
    }
    else if (w->consecutive_failures >= MAX_CONSECUTIVE_FAILURES)
    {
        want = w->idle_connections;

        if (w->retry_tickcount >= FAILURE_RETRY_INTERVAL)
        {
            /* some time has passed since our connection attempts failed. try again */
            ++want;
            /* if this challenge is fulfilled we will reset consecutive_failures */
            w->retry_challenge = running_tasks + want;
        }
    }
    else
    {
        want = MAX_CONNECTIONS_PER_WEBSEED - running_tasks;
        w->retry_challenge = running_tasks + w->idle_connections + 1;
    }

    /* check address and wait for 206 data, then go to MAX_CONNECTIONS_PER_WEBSEED */
    if (want > 1 && w->task_flag == 0)
    {
        want = 1;
    }

    if (tor->isRunning && !tor->isStopping && !tr_torrentIsSeed(tor) && want > 0)
    {
        int got = 0;
        tr_block_index_t* blocks = NULL;

        blocks = tr_new(tr_block_index_t, want * 2);
        tr_peerMgrGetNextRequests(tor, &w->parent, want, blocks, &got, true);

        w->idle_connections -= MIN(w->idle_connections, got);

        if (w->retry_tickcount >= FAILURE_RETRY_INTERVAL && got == want)
        {
            w->retry_tickcount = 0;
        }

        for (int i = 0; i < got; ++i)
        {
            tr_block_index_t const b = blocks[i * 2];
            tr_block_index_t const be = blocks[i * 2 + 1];
            struct tr_webseed_task* task;

            task = tr_new0(struct tr_webseed_task, 1);
            task->session = tor->session;
            task->webseed = w;
            task->block = b;
            task->piece_index = tr_torBlockPiece(tor, b);
            task->piece_offset = tor->blockSize * b - tor->info.pieceSize * task->piece_index;
            task->length = (be - b) * tor->blockSize + tr_torBlockCountBytes(tor, be);
            task->blocks_done = 0;
            task->response_code = 0;
            task->block_size = tor->blockSize;
            task->content = evbuffer_new();
            evbuffer_add_cb(task->content, on_content_changed, task);
            tr_list_append(&w->tasks, task);
            task_request_next_chunk(task);

            w->task_flag = TR_WEB_TASK_INIT;
        }

        tr_free(blocks);
    }
}

static void web_response_func(tr_session* session, uint8_t status_flag, bool did_connect UNUSED, bool did_timeout UNUSED,
    long response_code, void const* response UNUSED, size_t response_byte_count UNUSED, void* vtask)
{
    tr_webseed* w;
    tr_torrent* tor;
    struct tr_webseed_task* t = vtask;

    if (t->dead)
    {
        evbuffer_free(t->content);
        tr_free(t);
        return;
    }

    w = t->webseed;
    tor = tr_torrentFindFromId(session, w->torrent_id);

    if (tor != NULL)
    {
        int const success = response_code == 206;
        uint8_t const flag = status_flag;

        if (flag != TR_WEB_TASK_PAUSED && w->task_flag != TR_WEB_TASK_CORRUPT_DATA)
        {
            w->task_flag = flag;
        }

        dbgmsg("[web_response_ALL] ws %p task %p piece %d strikes %d status_flag %d task_flag %d running %d t->length %ld "
            "buf %ld bytes done %ld code %ld t->response_code %ld", w, t->web_task, (int)t->piece_index, w->parent.strikes,
            flag, w->task_flag, tr_list_size(w->tasks), (long)t->length, (long)evbuffer_get_length(t->content),
            (long)(t->blocks_done * tor->blockSize), response_code, t->response_code);

        /* active_transfers was only increased if the connection was successful */
        if (t->response_code == 206)
        {
            --w->active_transfers;
        }

        if (!success || !piece_offset_is_valid(tor, t) || flag == TR_WEB_TASK_PAUSED || w->task_flag < TR_WEB_TASK_SUCCESS)
        {
            tr_block_index_t const blocks_remain = (t->length + tor->blockSize - 1) / tor->blockSize - t->blocks_done;

            if (blocks_remain != 0)
            {
                fire_client_got_rejs(tor, w, t->block + t->blocks_done, blocks_remain);
            }

            if (t->blocks_done != 0)
            {
                ++w->idle_connections;
            }
            /* to allow for repeated button twiddling, don't increment w->consecutive_failures unless... */
            else if (tor->isRunning && !tor->isStopping)
            {
                if (++w->consecutive_failures >= MAX_CONSECUTIVE_FAILURES && w->retry_tickcount == 0)
                {
                    /* now wait a while until retrying to establish a connection */
                    ++w->retry_tickcount;
                }
            }

            tr_list_remove_data(&w->tasks, t);
            evbuffer_free(t->content);
            tr_free(t);

            dbgmsg("[web_response_FAIL] ws %p strikes %d status_flag %d task_flag %d running %d code %ld", w, w->parent.strikes,
                flag, w->task_flag, tr_list_size (w->tasks), response_code);
        }
        else
        {
            uint32_t const bytes_done = t->blocks_done * tor->blockSize;
            uint32_t const buf_len = evbuffer_get_length(t->content);

            if (bytes_done + buf_len < t->length)
            {
                /* request finished successfully but there's still data missing. that
                   means we've reached the end of a file and need to request the next one */
                t->response_code = 0;
                task_request_next_chunk(t);
            }
            else
            {
                if (buf_len != 0 && !tr_torrentPieceIsComplete(tor, t->piece_index) &&
                    !tr_torrentBlockIsComplete(tor, t->block + t->blocks_done))
                {
                    TR_ASSERT(buf_len == tor->lastBlockSize);

                    /* on_content_changed() will not write a block if it is smaller than
                       the torrent's block size, i.e. the torrent's very last block */
                    tr_cacheWriteBlock(session->cache, tor, t->piece_index, t->piece_offset + bytes_done, buf_len, t->content);

                    dbgmsg("[web_response_LAST_BLOCK_WRITE] ws %p task %p piece %d block %d size %ld", w, t->web_task,
                        (int)t->piece_index, (int)(t->block + t->blocks_done), (long)buf_len);

                    tr_bitfieldAdd(&w->parent.blame, t->piece_index);
                    fire_client_got_blocks(tor, t->webseed, t->block + t->blocks_done, 1);
                }

                ++w->idle_connections;

                tr_list_remove_data(&w->tasks, t);
                evbuffer_free(t->content);
                tr_free(t);

                on_idle(w);
            }
        }
    }
}

static struct evbuffer* make_url(tr_webseed* w, tr_file const* file)
{
    struct evbuffer* buf = evbuffer_new();

    evbuffer_add(buf, w->base_url, w->base_url_len);

    /* if url ends with a '/', add the torrent name */
    if (w->base_url[w->base_url_len - 1] == '/' && file->name != NULL)
    {
        tr_http_escape(buf, file->name, strlen(file->name), false);
    }

    return buf;
}

static void task_request_next_chunk(struct tr_webseed_task* t)
{
    tr_webseed* w = t->webseed;
    tr_torrent* tor = tr_torrentFindFromId(w->session, w->torrent_id);

    if (tor != NULL)
    {
        char range[64];
        char** urls = t->webseed->file_urls;

        tr_info const* inf = tr_torrentInfo(tor);
        uint64_t const remain = t->length - t->blocks_done * tor->blockSize - evbuffer_get_length(t->content);

        uint64_t const total_offset = tr_pieceOffset(tor, t->piece_index, t->piece_offset, t->length - remain);
        tr_piece_index_t const step_piece = total_offset / inf->pieceSize;
        uint64_t const step_piece_offset = total_offset - inf->pieceSize * step_piece;

        tr_file_index_t file_index;
        tr_file const* file;
        uint64_t file_offset;
        uint64_t this_pass;

        tr_ioFindFileLocation(tor, step_piece, step_piece_offset, &file_index, &file_offset);
        file = &inf->files[file_index];
        this_pass = MIN(remain, file->length - file_offset);

        if (!urls[file_index])
        {
            urls[file_index] = evbuffer_free_to_str(make_url(t->webseed, file), NULL);
        }

        tr_snprintf(range, sizeof(range), "%" PRIu64 "-%" PRIu64, file_offset, file_offset + this_pass - 1);

        t->web_task = tr_webRunWebseed(tor, urls[file_index], range, web_response_func, t, t->content);
    }
}

/***
****
***/

static void webseed_timer_func(evutil_socket_t foo UNUSED, short bar UNUSED, void* vw)
{
    tr_webseed* w = vw;

    if (w->retry_tickcount != 0)
    {
        ++w->retry_tickcount;
    }

    on_idle(w);

    tr_timerAddMsec(w->timer, TR_IDLE_TIMER_MSEC);
}

/***
****  tr_peer virtual functions
***/

static bool webseed_is_transferring_pieces(tr_peer const* peer, uint64_t now, tr_direction direction, unsigned int* setme_Bps)
{
    unsigned int Bps = 0;
    bool is_active = false;

    if (direction == TR_DOWN)
    {
        tr_webseed const* w = (tr_webseed const*)peer;
        is_active = w->tasks != NULL;
        Bps = tr_bandwidthGetPieceSpeed_Bps(&w->bandwidth, now, direction);
    }

    if (setme_Bps != NULL)
    {
        *setme_Bps = Bps;
    }

    return is_active;
}

static void webseed_destruct(tr_peer* peer)
{
    tr_webseed* w = (tr_webseed*)peer;

    /* flag all the pending tasks as dead */
    for (tr_list* l = w->tasks; l != NULL; l = l->next)
    {
        struct tr_webseed_task* task = l->data;
        task->dead = true;
    }

    tr_list_free(&w->tasks, NULL);

    /* if we have an array of file URLs, free it */
    if (w->file_urls != NULL)
    {
        tr_torrent* tor = tr_torrentFindFromId(w->session, w->torrent_id);
        tr_info const* inf = tr_torrentInfo(tor);

        for (tr_file_index_t i = 0; i < inf->fileCount; ++i)
        {
            tr_free(w->file_urls[i]);
        }

        tr_free(w->file_urls);
    }

    /* webseed destruct */
    event_free(w->timer);
    tr_bandwidthDestruct(&w->bandwidth);
    tr_free(w->base_url);

    /* parent class destruct */
    tr_peerDestruct(&w->parent);
}

static void webseed_add_strike(tr_peer* peer, tr_torrent* tor, tr_piece_index_t pieceIndex)
{
    tr_webseed* w = (tr_webseed*)peer;

    if (tr_bitfieldHas(&w->parent.blame, pieceIndex))
    {
        ++w->parent.strikes;

        if (w->parent.strikes == MAX_BAD_PIECES_PER_PEER)
        {
            w->task_flag = TR_WEB_TASK_CORRUPT_DATA;
            tr_logAddTorDbg(tor, "Webseed:%s has sent %d corrupt pieces and has been disabled.", w->base_url,
                MAX_BAD_PIECES_PER_PEER);
        }

        dbgmsg("[webseed_add_strike] ws %p piece %d strike_count %d task_flag %d running %d url %s", w, (int)pieceIndex,
            w->parent.strikes, w->task_flag, tr_list_size(w->tasks), w->base_url);
    }
}

static char* webseed_get_base_url(tr_peer* peer)
{
  tr_webseed* w = (tr_webseed*)peer;
  char* ret = tr_strndup(w->base_url, w->base_url_len);

  return ret;
}

static bool webseed_needs_poking(tr_peer* peer)
{
    tr_webseed* w = (tr_webseed*)peer;
    uint8_t ret = w->task_flag;

    return ret > TR_WEB_TASK_PAUSED && ret < TR_WEB_TASK_SUCCESS;
}

static struct tr_peer_virtual_funcs const my_funcs =
{
    .destruct = webseed_destruct,
    .is_transferring_pieces = webseed_is_transferring_pieces,
    .add_strike = webseed_add_strike,
    .base_url = webseed_get_base_url,
    .needs_poking = webseed_needs_poking
};

/***
****
***/

tr_webseed* tr_webseedNew(struct tr_torrent* tor, char const* url, tr_peer_callback callback, void* callback_data)
{
    tr_webseed* w = tr_new0(tr_webseed, 1);
    tr_peer* peer = &w->parent;
    tr_info const* inf = tr_torrentInfo(tor);

    /* construct parent class */
    tr_peerConstruct(peer, tor);
    peer->client = TR_KEY_webseeds;
    peer->funcs = &my_funcs;
    tr_bitfieldSetHasAll(&peer->have);
    tr_peerUpdateProgress(tor, peer);

    w->torrent_id = tr_torrentId(tor);
    w->session = tor->session;
    w->base_url_len = strlen(url);
    w->base_url = tr_strndup(url, w->base_url_len);
    w->callback = callback;
    w->callback_data = callback_data;
    w->file_urls = tr_new0(char*, inf->fileCount);
    // tr_rcConstruct(&w->download_rate);
    tr_bandwidthConstruct(&w->bandwidth, tor->session, &tor->bandwidth);
    w->timer = evtimer_new(w->session->event_base, webseed_timer_func, w);
    tr_timerAddMsec(w->timer, TR_IDLE_TIMER_MSEC);
    return w;
}

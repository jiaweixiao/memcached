/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

#include "proxy.h"

typedef struct mcp_await_s {
    int pending;
    int wait_for;
    int req_ref;
    int argtable_ref; // need to hold refs to any potential hash selectors
    int restable_ref; // table of result objects
    int coro_ref; // reference to parent coroutine
    enum mcp_await_e type;
    bool completed; // have we completed the parent coroutine or not
    mcp_request_t *rq;
    mc_resp *resp; // the top level mc_resp to fill in (as if we were an iop)
} mcp_await_t;

// TODO (v2): mcplib_await_gc()
// - needs to handle cases where an await is created, but a rare error happens
// before it completes and the coroutine is killed. must check and free its
// references.

// local restable = mcp.await(request, pools, num_wait)
// NOTE: need to hold onto the pool objects since those hold backend
// references. Here we just keep a reference to the argument table.
int mcplib_await(lua_State *L) {
    mcp_request_t *rq = luaL_checkudata(L, 1, "mcp.request");
    luaL_checktype(L, 2, LUA_TTABLE);
    int n = luaL_len(L, 2); // length of hash selector table
    int wait_for = 0; // 0 means wait for all responses
    enum mcp_await_e type = AWAIT_GOOD;

    if (n <= 0) {
        proxy_lua_error(L, "mcp.await arguments must have at least one pool");
    }
    if (lua_isnumber(L, 3)) {
        wait_for = lua_tointeger(L, 3);
        lua_pop(L, 1);
        if (wait_for > n) {
            wait_for = n;
        }
    }

    if (lua_isnumber(L, 4)) {
        type = lua_tointeger(L, 4);
        switch (type) {
            case AWAIT_GOOD:
            case AWAIT_ANY:
            case AWAIT_OK:
            case AWAIT_FIRST:
                break;
            default:
                proxy_lua_error(L, "invalid type argument tp mcp.await");
        }
    }

    // FIRST is only looking for one valid request.
    if (type == AWAIT_FIRST) {
        wait_for = 1;
    }

    // TODO (v2): quickly loop table once and ensure they're all pools?
    // TODO (v2) in case of newuserdatauv throwing an error, we need to grab
    // these references after allocating *aw else can leak memory.
    int argtable_ref = luaL_ref(L, LUA_REGISTRYINDEX); // pops the arg table
    int req_ref = luaL_ref(L, LUA_REGISTRYINDEX); // pops request object.

    // stack will be only the await object now
    // allocate before grabbing references so an error won't cause leaks.
    mcp_await_t *aw = lua_newuserdatauv(L, sizeof(mcp_await_t), 0);
    memset(aw, 0, sizeof(mcp_await_t));

    aw->wait_for = wait_for;
    aw->pending = n;
    aw->argtable_ref = argtable_ref;
    aw->rq = rq;
    aw->req_ref = req_ref;
    aw->type = type;
    P_DEBUG("%s: about to yield [HS len: %d]\n", __func__, n);

    return lua_yield(L, 1);
}

static void mcp_queue_await_io(conn *c, lua_State *Lc, mcp_request_t *rq, int await_ref, bool await_first) {
    io_queue_t *q = conn_io_queue_get(c, IO_QUEUE_PROXY);

    mcp_backend_t *be = rq->be;

    // Then we push a response object, which we'll re-use later.
    // reserve one uservalue for a lua-supplied response.
    mcp_resp_t *r = lua_newuserdatauv(Lc, sizeof(mcp_resp_t), 1);
    memset(r, 0, sizeof(mcp_resp_t));
    r->start = rq->start;

    int x;
    int end = rq->pr.reqlen-2 > RESP_CMD_MAX ? RESP_CMD_MAX : rq->pr.reqlen-2;
    for (x = 0; x < end; x++) {
        if (rq->pr.request[x] == ' ') {
            break;
        }
        r->cmd[x] = rq->pr.request[x];
    }
    r->cmd[x] = '\0';

    luaL_getmetatable(Lc, "mcp.response");
    lua_setmetatable(Lc, -2);

    io_pending_proxy_t *p = do_cache_alloc(c->thread->io_cache);
    if (p == NULL) {
        WSTAT_INCR(c, proxy_conn_oom, 1);
        proxy_lua_error(Lc, "out of memory allocating from IO cache");
        return;
    }

    // this is a re-cast structure, so assert that we never outsize it.
    assert(sizeof(io_pending_t) >= sizeof(io_pending_proxy_t));
    memset(p, 0, sizeof(io_pending_proxy_t));
    // set up back references.
    p->io_queue_type = IO_QUEUE_PROXY;
    p->thread = c->thread;
    p->c = c;
    p->resp = NULL;
    p->client_resp = r;
    p->flushed = false;
    p->ascii_multiget = rq->ascii_multiget;

    // io_p needs to hold onto its own response reference, because we may or
    // may not include it in the final await() result.
    p->mcpres_ref = luaL_ref(Lc, LUA_REGISTRYINDEX); // pops mcp.response

    // avoiding coroutine reference for sub-IO
    p->coro_ref = 0;
    p->coro = NULL;

    // await specific
    p->is_await = true;
    p->await_ref = await_ref;
    p->await_first = await_first;

    // The direct backend object. await object is holding reference
    p->backend = be;

    mcp_request_attach(Lc, rq, p);

    // link into the batch chain.
    p->next = q->stack_ctx;
    q->stack_ctx = p;
    P_DEBUG("%s: queued\n", __func__);

    return;
}

// TODO (v2): need to get this code running under pcall().
// It looks like a bulk of this code can move into mcplib_await(),
// and then here post-yield we can add the conn and coro_ref to the right
// places. Else these errors currently crash the daemon.
int mcplib_await_run(conn *c, lua_State *L, int coro_ref) {
    P_DEBUG("%s: start\n", __func__);
    mcp_await_t *aw = lua_touserdata(L, -1);
    int await_ref = luaL_ref(L, LUA_REGISTRYINDEX); // await is popped.
    assert(aw != NULL);
    lua_rawgeti(L, LUA_REGISTRYINDEX, aw->argtable_ref); // -> 1
    //dump_stack(L);
    P_DEBUG("%s: argtable len: %d\n", __func__, (int)lua_rawlen(L, -1));
    mcp_request_t *rq = aw->rq;
    aw->coro_ref = coro_ref;

    // create result table
    lua_newtable(L); // -> 2
    aw->restable_ref = luaL_ref(L, LUA_REGISTRYINDEX); // pop the result table

    // prepare the request key
    const char *key = MCP_PARSER_KEY(rq->pr);
    size_t len = rq->pr.klen;
    bool await_first = true;
    // loop arg table and run each hash selector
    lua_pushnil(L); // -> 3
    while (lua_next(L, 1) != 0) {
        P_DEBUG("%s: top of loop\n", __func__);
        // (key, -2), (val, -1)
        mcp_pool_proxy_t *pp = luaL_testudata(L, -1, "mcp.pool_proxy");
        if (pp == NULL) {
            proxy_lua_error(L, "mcp.await must be supplied with a pool");
        }
        mcp_pool_t *p = pp->main;

        // NOTE: rq->be is only held to help pass the backend into the IOP in
        // mcp_queue call. Could be a local variable and an argument too.
        rq->be = mcplib_pool_proxy_call_helper(L, p, key, len);

        mcp_queue_await_io(c, L, rq, await_ref, await_first);
        await_first = false;

        // pop value, keep key.
        lua_pop(L, 1);
    }

    lua_pop(L, 1); // remove table key.
    aw->resp = c->resp; // cuddle the current mc_resp to fill later

    // we count the await as the "response pending" since it covers a single
    // response object. the sub-IO's don't count toward the redispatch of *c
    io_queue_t *q = conn_io_queue_get(c, IO_QUEUE_PROXY);
    q->count++;

    P_DEBUG("%s\n", __func__);

    return 0;
}

// NOTE: This is unprotected lua/C code. There are no lua-style errors thrown
// purposefully as of this writing, but it's still not safe. Either the code
// can be restructured to use less lua (which I think is better long term
// anyway) or it can be pushed behind a cfunc pcall so we don't crash the
// daemon if something bad happens.
int mcplib_await_return(io_pending_proxy_t *p) {
    mcp_await_t *aw;
    lua_State *L = p->thread->L; // use the main VM coroutine for work
    bool cleanup = false;
    bool valid = false; // is response valid to add to the result table.
    bool completing = false;

    // TODO (v2): just push the await ptr into *p?
    lua_rawgeti(L, LUA_REGISTRYINDEX, p->await_ref);
    aw = lua_touserdata(L, -1);
    lua_pop(L, 1); // remove AW object from stack
    assert(aw != NULL);
    P_DEBUG("%s: start [pending: %d]\n", __func__, aw->pending);
    //dump_stack(L);

    aw->pending--;
    // Await not yet satisfied.
    // If wait_for != 0 check for response success
    // if success and wait_for is *now* 0, we complete.
    // add successful response to response table
    // Also, if no wait_for, add response to response table
    // TODO (v2): for GOOD or OK cases, it might be better to return the
    // last object as valid if there are otherwise zero valids?
    // Think we just have to count valids...
    if (!aw->completed) {
        valid = true; // always collect results unless we are completed.
        if (aw->wait_for > 0) {
            bool is_good = false;
            switch (aw->type) {
                case AWAIT_GOOD:
                    if (p->client_resp->status == MCMC_OK && p->client_resp->resp.code != MCMC_CODE_MISS) {
                        is_good = true;
                    }
                    break;
                case AWAIT_ANY:
                    is_good = true;
                    break;
                case AWAIT_OK:
                    if (p->client_resp->status == MCMC_OK) {
                        is_good = true;
                    }
                    break;
                case AWAIT_FIRST:
                    if (p->await_first) {
                        is_good = true;
                    } else {
                        // user only wants the first pool's result.
                        valid = false;
                    }
                    break;
            }

            if (is_good) {
                aw->wait_for--;
            }

            if (aw->wait_for == 0) {
                completing = true;
            }
        }
    }

    // note that post-completion, we stop gathering responses into the
    // resposne table... because it's already been returned.
    // So "valid" can only be true if also !completed
    if (aw->pending == 0) {
        if (!aw->completed) {
            // were waiting for all responses.
            completing = true;
        }
        cleanup = true;
        P_DEBUG("%s: pending == 0\n", __func__);
    }

    // a valid response to add to the result table.
    if (valid) {
        P_DEBUG("%s: valid\n", __func__);
        lua_rawgeti(L, LUA_REGISTRYINDEX, aw->restable_ref); // -> 1
        lua_rawgeti(L, LUA_REGISTRYINDEX, p->mcpres_ref); // -> 2
        // couldn't find a table.insert() equivalent; so this is
        // inserting into the length + 1 position manually.
        //dump_stack(L);
        lua_rawseti(L, 1, lua_rawlen(L, 1) + 1); // pops mcpres
        lua_pop(L, 1); // pops restable
    }

    // lose our internal mcpres reference regardless.
    luaL_unref(L, LUA_REGISTRYINDEX, p->mcpres_ref);
    // our await_ref is shared, so we don't need to release it.

    if (completing) {
        P_DEBUG("%s: completing\n", __func__);
        aw->completed = true;
        // if we haven't completed yet, the connection reference is still
        // valid. So now we pull it, reduce count, and readd if necessary.
        // here is also the point where we resume the coroutine.
        lua_rawgeti(L, LUA_REGISTRYINDEX, aw->coro_ref);
        lua_State *Lc = lua_tothread(L, -1);
        lua_rawgeti(Lc, LUA_REGISTRYINDEX, aw->restable_ref); // -> 1
        proxy_run_coroutine(Lc, aw->resp, NULL, p->c);
        luaL_unref(L, LUA_REGISTRYINDEX, aw->coro_ref);
        luaL_unref(L, LUA_REGISTRYINDEX, aw->restable_ref);

        io_queue_t *q = conn_io_queue_get(p->c, p->io_queue_type);
        q->count--;
        if (q->count == 0) {
            // call re-add directly since we're already in the worker thread.
            conn_worker_readd(p->c);
        }

    }

    if (cleanup) {
        P_DEBUG("%s: cleanup [completed: %d]\n", __func__, aw->completed);
        luaL_unref(L, LUA_REGISTRYINDEX, aw->argtable_ref);
        luaL_unref(L, LUA_REGISTRYINDEX, aw->req_ref);
        luaL_unref(L, LUA_REGISTRYINDEX, p->await_ref);
    }

    // Just remove anything we could have left on the primary VM stack
    lua_settop(L, 0);

    // always return free this sub-IO object.
    do_cache_free(p->thread->io_cache, p);

    return 0;
}


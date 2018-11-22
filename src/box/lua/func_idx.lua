-- func_idx.lua (internal file)
--

local json = require("json")

local function func_idx_space_name(space_name, idx_name)
    return '_i_' .. idx_name .. '_' .. space_name
end

local function func_idx_select_do(index, key, iterator, func, arg)
    local space = box.space[index.space_id]
    local ispace_name = func_idx_space_name(space.name, index.name)
    local ispace = box.space[ispace_name]
    local pkey_offset = index.functional.pkey_offset
    local unique_filter = {}
    for _, k in ispace:pairs(key, {iterator = iterator}) do
        local pkey = {}
        for _, p in pairs(space.index[0].parts) do
            table.insert(pkey, k[#pkey + pkey_offset + 1])
        end
        if index.is_multikey == true then
            local pkey_dump = json.encode(pkey)
            -- test tuple uniqueness for multikey index
            if unique_filter[pkey_dump] == true then
                goto continue
            end
            unique_filter[pkey_dump] = true
        end
        if func(arg, space, pkey) ~= 0 then break end
        ::continue::
    end
end

local function func_idx_select(index, key, opts)
    box.internal.check_index_arg(index, 'select')
    key = key or {}
    local iterator, offset, limit =
        box.internal.check_select_opts(opts, #key < index.functional.pkey_offset)
    local res = {}
    local curr_offset, done = 0, 0
    func_idx_select_do(index, key, iterator,
                       function(container, space, pkey)
                            if done >= limit then return 1 end
                            if curr_offset >= offset then
                                local tuple = space:get(pkey)
                                table.insert(container, tuple)
                                done = done + 1
                            end
                            curr_offset = curr_offset + 1
                            return 0
                       end, res)
    return res
end

local function func_idx_get(index, key)
    box.internal.check_index_arg(index, 'get')
    if not index.unique then
        box.error(box.error.MORE_THAN_ONE_TUPLE, "")
    end
    ret = func_idx_select(index, key, {limit = 1})[1]
    if ret ~= nil then
        return ret
    end
end

local function func_idx_bsize(index)
    box.internal.check_index_arg(index, 'bsize')
    local space = box.space[index.space_id]
    local ispace = box.space[func_idx_space_name(space.name, index.name)]
    local ispace_pk = ispace.index[0]
    return ispace_pk:bsize()
end

local function func_idx_random(index, seed)
    box.internal.check_index_arg(index, 'random')
    local space = box.space[index.space_id]
    return space.index[0]:random(seed)
end

local function func_idx_count(index, key, opts)
    box.internal.check_index_arg(index, 'count')
    key = key or {}
    local iterator, offset, limit =
        box.internal.check_select_opts(opts, #key < index.functional.pkey_offset)
    local res = {count = 0}
    func_idx_select_do(index, key, iterator,
                       function(container, space, pkey)
                            container.count = container.count + 1
                            return 0
                       end, res)
    return res.count
end

local function func_idx_rename(index, name)
    box.internal.check_index_arg(index, 'rename')
    local space = box.space[index.space_id]
    local ispace = box.space[func_idx_space_name(space.name, index.name)]
    local new_ispace_name = func_idx_space_name(space.name, name)
    ispace:rename(new_ispace_name)
    return box.schema.index.rename(space.id, index.id, name)
end

local function func_idx_min(index, key)
    box.internal.check_index_arg(index, 'min')
    return func_idx_select(index, key, {limit = 1, iterator = 'EQ'})
end

local function func_idx_max(index, key)
    box.internal.check_index_arg(index, 'max')
    return func_idx_select(index, key, {limit = 1, iterator = 'REQ'})
end

local function func_idx_drop(index, key)
    box.internal.check_index_arg(index, 'drop')
    local space = box.space[index.space_id]
    space:on_replace(nil, index.functional.trigger)
    local ispace = box.space[func_idx_space_name(space.name, index.name)]
    ispace:drop()
    return box.schema.index.drop(space.id, index.id)
end

local function func_idx_delete(index, key)
    box.internal.check_index_arg(index, 'delete')
    if not index.unique then
        box.error(box.error.MORE_THAN_ONE_TUPLE, "")
    end
    key = key or {}
    local iterator, offset, limit = box.internal.check_select_opts({}, false)
    local ret = {}
    func_idx_select_do(index, key, iterator,
                       function(container, space, pkey)
                            local tuple = space:delete(pkey)
                            if tuple ~= nil then
                                table.insert(container, tuple)
                            end
                            return 1
                       end, ret)
    if ret[1] ~= nil then
        return ret[1]
    end
end

local function func_idx_update(index, key, ops)
    box.internal.check_index_arg(index, 'update')
    if not index.unique then
        box.error(box.error.MORE_THAN_ONE_TUPLE, "")
    end
    local iterator, offset, limit = box.internal.check_select_opts({}, false)
    local ret = {}
    func_idx_select_do(index, key, iterator,
                       function(container, space, pkey)
                            local tuple = space:update(pkey, ops)
                            if tuple ~= nil then
                                table.insert(container, tuple)
                            end
                            return 1
                       end, ret)
    if ret[1] ~= nil then
        return ret[1]
    end
end

local function func_idx_pairs_next(param, state)
    local state, tuple = param.gen(param.gen_param, state)
    if state == nil then
        param.filter_set = {}
        return nil
    else
        local pkey_offset = param.index.functional.pkey_offset
        local pkey = {}
        for _, p in pairs(param.space.index[0].parts) do
            table.insert(pkey, tuple[#pkey + pkey_offset + 1])
        end
        local tuple = param.space:get(pkey)
        return state, pkey, tuple
    end
end

local function func_idx_pairs(index, key, opts)
    box.internal.check_index_arg(index, 'pairs')
    local space = box.space[index.space_id]
    local ispace_name = func_idx_space_name(space.name, index.name)
    local ispace = box.space[ispace_name]
    key = key or {}
    local iterator, offset, limit =
        box.internal.check_select_opts(opts, #key < index.functional.pkey_offset)
    local obj, param, state = ispace:pairs(key, {iterator = iterator})
    param = {filter_set = {}, gen_param = param, gen = obj.gen,
             index = index, space = space}
    obj.gen = function(param, state)
        local state, pkey, tuple = func_idx_pairs_next(param, state)
        local pkey_dump = index.is_multikey and json.encode(pkey) or nil
        while index.is_multikey == true and state ~= nil and
              param.filter_set[pkey_dump] == true do
            state, pkey, tuple = func_idx_pairs_next(param, state)
        end
        if state == nil then
            param.filter_set = {}
            return nil
        else
            if index.is_multikey then
                param.filter_set[pkey_dump] = true
            end
            return state, tuple
        end
    end
    return obj, param, state
end

local function func_idx_unique_filter(keys)
    local ret = {}
    local filter_set = {}
    for _, k in pairs(keys) do
        local dump = json.encode(k)
        if filter_set[dump] == true then goto continue end
        table.insert(ret, k)
        filter_set[dump] = true
        ::continue::
    end
    return ret
end

local function func_idx_fkeys(func, tuple, is_multikey)
    local fkeys = func(tuple) or {}
    if is_multikey then
        fkeys = func_idx_unique_filter(fkeys)
    else
        fkeys = {fkeys}
    end
    return fkeys
end

local func_idx = {}

func_idx.space_trigger_set = function (space_name, idx_name)
    local space = box.space[space_name]
    assert(space ~= nil)
    local ispace = box.space[func_idx_space_name(space_name, idx_name)]
    assert(ispace ~= nil)
    local trigger = function (old, new)
        if box.info.status ~= 'running' then
            return
        end
        local findex = space.index[idx_name]
        local func = findex.functional.func
        local pkey = {}
        local tuple = old or new
        for _, p in pairs(space.index[0].parts) do
            table.insert(pkey, tuple[p.fieldno])
        end
        if old ~= nil then
            local fkeys = func_idx_fkeys(func, old, findex.is_multikey)
            for _, key in pairs(fkeys) do
                if findex.unique == false then
                    for _, v in pairs(pkey) do
                        table.insert(key, v)
                    end
                end
                ispace:delete(key)
            end
        end
        if new ~= nil then
            local fkeys = func_idx_fkeys(func, new, findex.is_multikey)
            for _, key in pairs(fkeys) do
                for _, v in pairs(pkey) do
                    table.insert(key, v)
                end
                ispace:insert(key)
            end
        end
    end
    space:on_replace(trigger)
    return trigger
end

func_idx.ispace_create = function (space, name, func_code, func_format,
                                   is_unique, is_multikey)
    local func, err = loadstring(func_code)
    if func == nil then
        box.error(box.error.ILLEGAL_PARAMS,
                  "functional index extractor routine is invalid: "..err)
    end
    func = func()
    local iformat = {}
    local iparts = {}
    for _, f in pairs(func_format) do
        table.insert(iformat, {name = 'i' .. tostring(#iformat), type = f.type})
        table.insert(iparts, {#iparts + 1, f.type,
                              is_nullable = f.is_nullable,
                              collation = f.collation})
    end
    local pkey_offset = #func_format
    local pkey = box.internal.check_primary_index(space)
    for _, p in pairs(pkey.parts) do
        table.insert(iformat, {name = 'i' .. tostring(#iformat), type = p.type})
        if is_unique == false then
            table.insert(iparts, {#iparts + 1, p.type})
        end
    end
    local ispace = box.schema.space.create('_i_' .. name .. '_' .. space.name,
                                           {engine = space.engine})
    ispace:format(iformat)
    ispace:create_index('pk', {parts = iparts})

    for _, tuple in space:pairs() do
        local pkey = {}
        for _, p in pairs(space.index[0].parts) do
            table.insert(pkey, tuple[p.fieldno])
        end
        local fkeys = func_idx_fkeys(func, tuple, is_multikey)
        for _, key in pairs(fkeys) do
            for _, v in pairs(pkey) do
                table.insert(key, v)
            end
            ispace:insert(key)
        end
    end
end

func_idx.monkeypatch = function (index)
    local meta = getmetatable(index)
    meta.select = func_idx_select
    meta.get = func_idx_get
    meta.bsize = func_idx_bsize
    meta.random = func_idx_random
    meta.count = func_idx_count
    meta.rename = func_idx_rename
    meta.min = func_idx_min
    meta.max = func_idx_max
    meta.drop = func_idx_drop
    meta.delete = func_idx_delete
    meta.update = func_idx_update
    meta.pairs = func_idx_pairs
    meta.alter = nil
    meta.compact = nil
    meta.stat = nil
end

return func_idx

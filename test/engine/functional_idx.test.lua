msgpack = require('msgpack')
env = require('test_run')
test_run = env.new()
engine = test_run:get_cfg('engine')

format = {{'id','unsigned'}, {'name','string'},{'address', 'string'}}
s = box.schema.space.create('clients', {engine = engine, format=format})
addr_extractor = [[return function(tuple) if type(tuple.address) ~= 'string' then return nil, 'Invalid field type' end local t = tuple.address:lower():split() for k,v in pairs(t) do t[k] = {v} end return t end]]
addr_extractor_format = {{type='str', collation='unicode_ci'}}
s:create_index('address', {func_code = addr_extractor, func_format = addr_extractor_format, unique=true})
pk = s:create_index('pk')
s:insert({1, 'Vasya Pupkin', 'Russia Moscow Dolgoprudny Moscow Street 9А'})
s:create_index('address', {func_code = addr_extractor, unique=true})
s:create_index('address', {func_format = addr_extractor_format, unique=true})
s:create_index('address', {func_code = addr_extractor..'err', func_format = addr_extractor_format, unique=true})
addr_low = s:create_index('address', {func_code = addr_extractor, func_format = addr_extractor_format, unique=true, is_multikey=true})
assert(box.space.clients:on_replace()[1] == addr_low.functional.trigger)
s:insert({2, 'James Bond', 'GB London Mi6'})
s:insert({3, 'Kirill Alekseevich', 'Russia Moscow Dolgoprudny RocketBuilders 1'})
s:insert({4, 'Jack London', 'GB'})
addr_low:count()
addr_low:count({'moscow'})
addr_low:count({'gb'})
addr_extractor = [[return function(tuple) if type(tuple.address) ~= 'string' then return nil, 'Invalid field type' end return {tuple.address:upper():split()[1]} end]]
addr_high = s:create_index('address2', {func_code = addr_extractor, func_format = addr_extractor_format, unique=true, is_multikey=false})
addr_high:select()
addr_high:select({}, {limit = 1})
addr_high:select({}, {limit = 1, offset=1})
addr_high:select({'Moscow'})
for _, v in addr_high:pairs() do print(v.name) end
_ = addr_low:delete({'moscow'})
addr_high:select({'MOSCOW'})
addr_high:drop()

s:drop()

#pragma once
#include "stdafx.hpp"

#include "shared-utils.hpp"
#include "simulator/simulator.hpp"
#include "units/unit-cache.hpp"

namespace Arches {
namespace Tests {

struct Results
{
	int checks{ 0 };
	int failures{ 0 };

	void check(bool ok, const char* test, const char* what)
	{
		checks++;
		if (ok) return;
		failures++;
		printf("FAIL %s: %s\n", test, what);
	}
};

//A memory with a backing array and a fixed latency
//a load returns `latency` clocks after it is accepted, a store writes the array when it is accepted. 
//One request per port per clock.
class TestMemory : public Units::UnitMemoryBase
{
public:
	std::vector<uint8_t> data;
	uint64_t loads{ 0 };
	uint64_t stores{ 0 };

private:
	struct InFlight
	{
		cycles_t ready;
		MemoryReturn ret;
	};

	uint _latency;
	std::vector<MemoryRequest> _incoming;
	std::vector<uint8_t> _incoming_valid;
	std::vector<std::deque<InFlight>> _in_flight;

public:
	TestMemory(size_t size, uint num_ports, uint latency) : data(size, 0), _latency(latency),
		_incoming(num_ports), _incoming_valid(num_ports, 0), _in_flight(num_ports)
	{ 
	}

	void clock_rise() override
	{
		for (uint p = 0; p < _incoming.size(); ++p)
		{
			if (!_incoming_valid[p]) continue;
			const MemoryRequest& request = _incoming[p];
			_assert(request.paddr + request.size <= data.size());
			if (request.type == MemoryRequest::Type::LOAD)
			{
				_in_flight[p].push_back({ simulator->current_cycle + _latency, MemoryReturn(request, &data[request.paddr]) });
				loads++;
			}
			else if (request.type == MemoryRequest::Type::STORE)
			{
				std::memcpy(&data[request.paddr], request.data, request.size);
				stores++;
			}
			_incoming_valid[p] = 0;
		}
	}

	void clock_fall() override {}

	bool has_work() override
	{
		for (uint p = 0; p < _incoming.size(); ++p)
			if (_incoming_valid[p] || !_in_flight[p].empty()) return true;
		return false;
	}

	bool request_port_write_valid(uint port) override { return !_incoming_valid[port]; }
	void write_request(const MemoryRequest& request) override
	{
		_incoming[request.port] = request;
		_incoming_valid[request.port] = 1;
	}

	bool return_port_read_valid(uint port) override { return !_in_flight[port].empty() && _in_flight[port].front().ready <= simulator->current_cycle; }
	const MemoryReturn& peek_return(uint port) override { return _in_flight[port].front().ret; }
	const MemoryReturn read_return(uint port) override
	{
		const MemoryReturn ret = _in_flight[port].front().ret;
		_in_flight[port].pop_front();
		return ret;
	}
};

//Sends a series of requests to one port of a memory unit, one per clock, in order.
class TestClient : public Units::UnitBase
{
public:
	std::deque<MemoryRequest> script;
	std::vector<MemoryReturn> returns; //in arrival order

private:
	Units::UnitMemoryBase* _memory;
	uint _port;
	uint _outstanding{ 0 };

public:
	TestClient(Units::UnitMemoryBase* memory, uint port) : _memory(memory), _port(port) {}

	static MemoryRequest load(paddr_t paddr, uint8_t size)
	{
		MemoryRequest request;
		request.type = MemoryRequest::Type::LOAD;
		request.paddr = paddr;
		request.size = size;
		return request;
	}

	//omit_cache bit n set bypasses the level-n cache, as on MemoryRequest::Flags.
	static MemoryRequest store(paddr_t paddr, uint8_t size, const void* bytes, uint8_t omit_cache = 0)
	{
		MemoryRequest request;
		request.type = MemoryRequest::Type::STORE;
		request.paddr = paddr;
		request.size = size;
		request.flags.omit_cache = omit_cache;
		std::memcpy(request.data, bytes, size);
		return request;
	}

	static MemoryRequest fence() { return MemoryRequest(); }

	static MemoryRequest pinned(MemoryRequest store)
	{
		store.flags.no_evict = 1;
		return store;
	}

	static MemoryRequest release(paddr_t paddr)
	{
		MemoryRequest request;
		request.type = MemoryRequest::Type::RELEASE;
		request.paddr = paddr;
		return request;
	}

	void clock_rise() override
	{
		while(_memory->return_port_read_valid(_port))
		{
			returns.push_back(_memory->read_return(_port));
			_outstanding--;
		}
	}

	void clock_fall() override
	{
		if (script.empty()) return;
		if (script.front().type == MemoryRequest::Type::NA)
		{
			if (_outstanding == 0) script.pop_front();
			return;
		}
		if (!_memory->request_port_write_valid(_port)) return;
		MemoryRequest request = script.front();
		request.port = _port;
		_memory->write_request(request);
		if (request.type == MemoryRequest::Type::LOAD) _outstanding++;
		script.pop_front();
	}

	bool has_work() override { return !script.empty() || _outstanding > 0; }
};

//Byte i of a test memory's pattern.
inline uint8_t pattern(paddr_t i) { return (uint8_t)(i * 7 + 3); }

inline Units::UnitCache::Configuration small_cache(uint level, Units::UnitMemoryBase* memory)
{
	Units::UnitCache::Configuration config;
	config.level = level;
	config.miss_alloc = true;
	config.size = 4 << 10; //8 sets of 4 ways of 128 B blocks, 32 B sectors
	config.associativity = 4;
	config.num_mshr = 8;
	config.num_subentries = 4;
	config.latency = 4;
	config.mem_highers = { memory };
	return config;
}

inline void test_cache_loads(Results& r)
{
	const char* name = "cache loads";
	Simulator sim;
	TestMemory memory(1 << 16, 1, 10);
	for (size_t i = 0; i < memory.data.size(); ++i) memory.data[i] = pattern(i);
	Units::UnitCache cache(small_cache(2, &memory));
	TestClient client(&cache, 0);
	sim.register_unit(&client);
	sim.register_unit(&cache);
	sim.register_unit(&memory);

	client.script = { TestClient::load(0x104, 4), TestClient::fence(), TestClient::load(0x108, 4) };
	sim.execute();

	r.check(client.returns.size() == 2, name, "two loads return");
	bool data_ok = client.returns.size() == 2;
	for (uint k = 0; data_ok && k < 2; ++k)
		for (uint b = 0; b < 4; ++b)
			data_ok = data_ok && client.returns[k].data[b] == pattern(client.returns[k].paddr + b);
	r.check(data_ok, name, "loads return the memory's bytes");
	r.check(cache.log.misses == 1 && cache.log.hits == 1, name, "one miss, then one hit");
	r.check(memory.loads == 1, name, "one sector fetched");
}

//1 client, 1 cache, 1 memory in 1 simulator.
struct CacheRig
{
	Simulator sim;
	TestMemory memory;
	Units::UnitCache cache;
	TestClient client;

	CacheRig(uint level = 2) : memory(1 << 16, 1, 10), cache(small_cache(level, &memory)), client(&cache, 0)
	{
		for (size_t i = 0; i < memory.data.size(); ++i)
			memory.data[i] = pattern(i);
		sim.register_unit(&client);
		sim.register_unit(&cache);
		sim.register_unit(&memory);
	}

	void run(std::initializer_list<MemoryRequest> script)
	{
		client.script = script;
		sim.execute();
	}
};

template<typename F>
inline bool sector_is(const TestClient& client, uint k, F expected)
{
	if (k >= client.returns.size() || client.returns[k].size != 32) return false;
	for (uint i = 0; i < 32; ++i)
		if (client.returns[k].data[i] != expected(i)) return false;
	return true;
}

inline const uint8_t* bytes_a() { static const uint8_t b[4] = {0xa0, 0xa1, 0xa2, 0xa3}; return b; }
inline uint8_t whole(uint i) { return (uint8_t)(0xc0 + i); }

//A store to a valid sector merges into it and stays in the cache.
inline void test_store_hit(Results& r)
{
	const char* name = "store hit";
	CacheRig rig;
	rig.run({TestClient::load(0x200, 4), TestClient::fence(), TestClient::store(0x204, 4, bytes_a()), TestClient::load(0x200, 32)});
	r.check(sector_is(rig.client, 1, [](uint i) { return i >= 4 && i < 8 ? bytes_a()[i - 4] : pattern(0x200 + i); }), name, "the load sees the stored bytes");
	r.check(rig.cache.log.store_hits == 1, name, "the store hits");
	r.check(rig.memory.loads == 1 && rig.memory.stores == 0, name, "one fetch, nothing written to memory");
}

//A store covering a whole sector allocates it without fetching.
inline void test_whole_sector_store(Results& r)
{
	const char* name = "whole-sector store";
	uint8_t data[32];
	for (uint i = 0; i < 32; ++i) data[i] = whole(i);
	CacheRig rig;
	rig.run({ TestClient::store(0x400, 32, data), TestClient::fence(), TestClient::load(0x400, 32) });
	r.check(sector_is(rig.client, 0, whole), name, "the load sees the stored sector");
	r.check(rig.cache.log.store_allocates == 1 && rig.cache.log.hits == 1, name, "allocated without a fetch, then a hit");
	r.check(rig.memory.loads == 0 && rig.memory.stores == 0, name, "no memory traffic");
}

//A partial store to an absent sector fetches it and merges; a load right behind it waits for it.
inline void test_partial_store_miss(Results& r)
{
	const char* name = "partial store miss";
	CacheRig rig;
	rig.run({ TestClient::store(0x600, 4, bytes_a()), TestClient::load(0x600, 32) });
	r.check(sector_is(rig.client, 0, [](uint i) { return i < 4 ? bytes_a()[i] : pattern(0x600 + i); }), name, "the load sees the store merged into the fetched sector");
	r.check(rig.memory.loads == 1 && rig.memory.stores == 0, name, "one fetch, nothing written to memory");
}

//A load issued before a store to the same sector reads the old bytes.
inline void test_load_then_store(Results& r)
{
	const char* name = "load then store";
	CacheRig rig;
	rig.run({ TestClient::load(0x800, 32), TestClient::store(0x800, 4, bytes_a()), TestClient::fence(), TestClient::load(0x800, 32) });
	r.check(sector_is(rig.client, 0, [](uint i) { return pattern(0x800 + i); }), name, "the first load reads the old bytes");
	r.check(sector_is(rig.client, 1, [](uint i) { return i < 4 ? bytes_a()[i] : pattern(0x800 + i); }), name, "the second load reads the store");
}

//A dirty block chosen as a victim is written back before it is replaced.
inline void test_eviction_writes_back(Results& r)
{
	const char* name = "eviction write-back";
	uint8_t data[32];
	for (uint i = 0; i < 32; ++i) data[i] = whole(i);
	CacheRig rig; //8 sets of 4 ways: blocks 1 KB apart share a set
	rig.run({ TestClient::store(0x1000, 32, data), TestClient::store(0x1400, 32, data), TestClient::store(0x1800, 32, data),
		TestClient::store(0x1c00, 32, data), TestClient::store(0x2000, 32, data), TestClient::fence(), TestClient::load(0x1000, 32) });
	//The fifth store evicts the first block; reading the first block back evicts the second.
	r.check(rig.cache.log.writebacks == 2 && rig.memory.stores == 2, name, "each allocation into the full set writes back one dirty sector");
	r.check(std::memcmp(&rig.memory.data[0x1000], data, 32) == 0, name, "memory holds the evicted sector's data");
	r.check(sector_is(rig.client, 0, whole), name, "reading it back fetches the written-back data");
}

//A store that bypasses a level invalidates that level's copy, so the next load fetches it anew.
inline void test_bypassing_store_invalidates(Results& r)
{
	const char* name = "bypassing store invalidates";
	CacheRig rig(1);
	rig.run({ TestClient::load(0xa00, 32), TestClient::fence(), TestClient::store(0xa04, 4, bytes_a(), 0b0010), TestClient::load(0xa00, 32) });
	r.check(rig.cache.log.uncached_requests == 1 && rig.memory.stores == 1, name, "the store goes to memory");
	r.check(sector_is(rig.client, 1, [](uint i) { return i >= 4 && i < 8 ? bytes_a()[i - 4] : pattern(0xa00 + i); }), name, "the next load sees it");
	r.check(rig.cache.log.misses == 2, name, "the copy was invalidated, so the load missed");
}

//flush_dirty() writes every dirty sector once, then nothing is dirty.
inline void test_flush_dirty(Results& r)
{
	const char* name = "flush dirty";
	uint8_t data[32];
	for(uint i = 0; i < 32; ++i) data[i] = whole(i);
	CacheRig rig;
	rig.run({TestClient::store(0xc00, 32, data), TestClient::store(0xc44, 4, bytes_a()), TestClient::load(0xe00, 32)});
	r.check(rig.memory.stores == 0, name, "nothing written back during the run");
	auto write = [&](paddr_t paddr, const uint8_t* bytes, uint size) { std::memcpy(&rig.memory.data[paddr], bytes, size); };
	const uint64_t first = rig.cache.flush_dirty(write);
	const uint64_t second = rig.cache.flush_dirty(write);
	r.check(first == 2 && second == 0, name, "two dirty sectors written, then none");
	bool ok = std::memcmp(&rig.memory.data[0xc00], data, 32) == 0 && std::memcmp(&rig.memory.data[0xc44], bytes_a(), 4) == 0;
	for(uint i = 0; i < 32; ++i) ok = ok && (i >= 4 && i < 8 || rig.memory.data[0xc40 + i] == pattern(0xc40 + i));
	r.check(ok, name, "memory holds both sectors, the partial one merged with the fetched bytes");
}

//A no-evict block stays while five other blocks pass through its set.
inline void test_pinned_block_stays(Results& r)
{
	const char* name = "pinned block stays";
	uint8_t data[32];
	for(uint i = 0; i < 32; ++i) data[i] = whole(i);
	CacheRig rig; //blocks 1 KB apart share a set of 4 ways
	rig.run({TestClient::pinned(TestClient::store(0x1000, 32, data)), TestClient::store(0x1400, 32, data), TestClient::store(0x1800, 32, data),
		TestClient::store(0x1c00, 32, data), TestClient::store(0x2000, 32, data), TestClient::store(0x2400, 32, data), TestClient::fence(),
		TestClient::load(0x1000, 32)});
	r.check(rig.cache.log.hits == 1 && rig.cache.log.misses == 0, name, "the pinned block still hits");
	r.check(sector_is(rig.client, 0, whole), name, "with its data");
	r.check(rig.cache.log.writebacks == 2 && std::memcmp(&rig.memory.data[0x1000], data, 32) != 0, name, "the two victims were other blocks");
}

//A released block's data is dead: it is dropped, never written back, and its way is reused.
inline void test_release_discards(Results& r)
{
	const char* name = "release discards";
	uint8_t data[32];
	for(uint i = 0; i < 32; ++i) data[i] = whole(i);
	CacheRig rig;
	rig.run({TestClient::pinned(TestClient::store(0x3000, 32, data)), TestClient::release(0x3000), TestClient::store(0x3400, 32, data),
		TestClient::store(0x3800, 32, data), TestClient::store(0x3c00, 32, data), TestClient::store(0x4000, 32, data), TestClient::fence(),
		TestClient::load(0x3000, 32)});
	r.check(rig.cache.log.releases == 1, name, "one release");
	r.check(sector_is(rig.client, 0, [](uint i) { return pattern(0x3000 + i); }), name, "reading it again fetches memory's bytes");
	auto write = [&](paddr_t paddr, const uint8_t* bytes, uint size) { std::memcpy(&rig.memory.data[paddr], bytes, size); };
	rig.cache.flush_dirty(write);
	bool untouched = true;
	for(uint i = 0; i < 32; ++i) untouched = untouched && rig.memory.data[0x3000 + i] == pattern(0x3000 + i);
	r.check(untouched, name, "its data never reaches memory");
}

inline int run_unit_tests(const SimulationConfig& config)
{
	Results r;
	test_cache_loads(r);
	test_store_hit(r);
	test_whole_sector_store(r);
	test_partial_store_miss(r);
	test_load_then_store(r);
	test_eviction_writes_back(r);
	test_bypassing_store_invalidates(r);
	test_flush_dirty(r);
	test_pinned_block_stays(r);
	test_release_discards(r);
	printf("Unit tests: %d checks, %d failed\n", r.checks, r.failures);
	return r.failures;
}

}// namespace Tests
}// namespace Arches


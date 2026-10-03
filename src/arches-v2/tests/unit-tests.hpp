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

	static MemoryRequest store(paddr_t paddr, uint8_t size, const void* bytes)
	{
		MemoryRequest request;
		request.type = MemoryRequest::Type::STORE;
		request.paddr = paddr;
		request.size = size;
		std::memcpy(request.data, bytes, size);
		return request;
	}

	static MemoryRequest fence() { return MemoryRequest(); }

	void clock_rise() override
	{
		while (_memory->return_port_read_valid(_port))
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

inline int run_unit_tests(const SimulationConfig& config)
{
	Results r;
	test_cache_loads(r);
	printf("Unit tests: %d checks, %d failed\n", r.checks, r.failures);
	return r.failures;
}

}// namespace Tests
}// namespace Arches


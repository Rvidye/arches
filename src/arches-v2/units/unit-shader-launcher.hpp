#pragma once
#include "stdafx.hpp"

#include "unit-base.hpp"
#include "unit-memory-base.hpp"
#include "unit-atomic-reg-file.hpp"

namespace Arches {
namespace Units {

//Starts one shader invocation on an idle TP thread{PC + argument registers}
//Arguments are in a0-a7 and f0-f7, so the invocation is in ordinary C function call.
struct LaunchRequest
{
	uint16_t tp{ 0 };
	uint64_t pc{ 0 };
	uint32_t int_args[8]{};
	uint32_t float_args[8]{};
};

//A TP thread finished its invocation and has nothing left in flight.
struct LaunchDone
{
	uint16_t tp{ 0 };
};

//Hardware shader launcher for one TM similar to NVIDIA's per SM warp scheduler.
//Threads no lonfer fetch their own work with fchthrd.
//A compute style dispatch of dispatch_count invocations of the function at entry_pc.
class UnitShaderLauncher : public UnitBase
{
public:
	struct Configuration
	{
		uint num_tps{ 1 };
		uint threads_per_tp{ 1 };
		uint tm_index{ 0 };

		UnitAtomicRegfile* atomic_regs{ nullptr };
		uint block_size{ 32 };

		uint64_t entry_pc{ 0 }; // shader function
		uint32_t dispatch_count{ 0 };
	};

private:
	class LaunchDecascade : public Decascade<LaunchRequest>
	{
	public:
		LaunchDecascade(uint sources, uint sinks) : Decascade<LaunchRequest>(sources, sinks) {}
		uint get_sink(const LaunchRequest& request) override { return request.tp; }
	};

	Configuration _config;

	LaunchDecascade _launch_network; //launcher to TP's
	Cascade<LaunchDone> _done_network; //TP's to launcher

	std::vector<uint> _free_threads; //launch credits per TP
	uint _next_tp{ 0 };

	uint32_t _block_base{ 0 };
	uint32_t _block_offset{ 0 };
	bool _stalled_for_atomic_reg{ false };
	bool _exhausted{ true };
	bool _holding_sim{ false };

	uint64_t _launches{ 0 };

	void _release_sim()
	{
		if (!_holding_sim) return;
		_holding_sim = false;
		simulator->units_executing--;
	}

public:
	UnitShaderLauncher(const Configuration& config) : UnitBase(), _config(config),
		_launch_network(1, config.num_tps), _done_network(config.num_tps, 1), _free_threads(config.num_tps, config.threads_per_tp)
	{
	}

	void reset() override
	{
		for (uint& free : _free_threads) free = _config.threads_per_tp;
		_next_tp = 0;
		_block_base = 0;
		_block_offset = _config.block_size;
		_stalled_for_atomic_reg = false;
		_exhausted = _config.dispatch_count == 0;
		_launches = 0;
		_holding_sim = !_exhausted;
		if (_holding_sim) simulator->units_executing++;
	}

	void clock_rise() override
	{
		_done_network.clock();
		if (_done_network.is_read_valid(0))
			_free_threads[_done_network.read(0).tp]++;

		if (_stalled_for_atomic_reg && _config.atomic_regs->return_port_read_valid(_config.tm_index))
		{
			const MemoryReturn ret = _config.atomic_regs->read_return(_config.tm_index);
			_block_base = ret.data_u32;
			_block_offset = 0;
			_stalled_for_atomic_reg = false;
		}
	}

	void clock_fall() override
	{
		if (!_exhausted && !_stalled_for_atomic_reg)
		{
			if (_block_offset == _config.block_size)
			{
				if (_config.atomic_regs->request_port_write_valid(_config.tm_index))
				{
					MemoryRequest request;
					request.type = MemoryRequest::Type::AMO_ADD;
					request.size = sizeof(uint32_t);
					request.port = _config.tm_index;
					request.paddr = 0x0ull;
					request.data_u32 = _config.block_size;
					_config.atomic_regs->write_request(request);
					_stalled_for_atomic_reg = true;
				}
			}
			else if (_block_base + _block_offset >= _config.dispatch_count)
			{
				_exhausted = true;
				_release_sim();
			}
			else if(_launch_network.is_write_valid(0))
			{
				uint tp = _pick_tp();
				if (tp != ~0u)
				{
					LaunchRequest request;
					request.tp = tp;
					request.pc = _config.entry_pc;
					request.int_args[0] = _block_base + _block_offset;
					_launch_network.write(request, 0);

					_free_threads[tp]--;
					_block_offset++;
					_launches++;
				}
			}
		}
		_launch_network.clock();
	}

	bool has_work() override
	{
		return !_exhausted || _stalled_for_atomic_reg || _launch_network.has_pending() || _done_network.has_pending();
	}

	uint64_t launches() const { return _launches; }

	//TP interface, launches are read on clock rise & done notices are written on clock fall.
	bool launch_port_read_valid(uint tp) { return _launch_network.is_read_valid(tp); }
	const LaunchRequest read_launch(uint tp) { return _launch_network.read(tp); }
	bool done_port_write_valid(uint tp) { return _done_network.is_write_valid(tp); }
	void write_done(const LaunchDone& done) { _done_network.write(done, done.tp); }

private:
	uint _pick_tp()
	{
		for (uint i = 0; i < _config.num_tps; ++i)
		{
			uint tp = (_next_tp + i) % _config.num_tps;
			if (_free_threads[tp] == 0) continue;
			_next_tp = (tp + 1) % _config.num_tps;
			return tp;
		}
		return ~0u;
	}
};

}// namespace Units
}// namespace Arches

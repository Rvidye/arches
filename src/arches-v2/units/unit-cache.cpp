#include "unit-cache.hpp"

namespace Arches {namespace Units {

UnitCache::UnitCache(Configuration config) :
	UnitCacheBase(config.size, config.block_size, config.associativity, config.sector_size, config.policy),
	_request_network(config.num_ports, config.num_slices * config.num_banks, config.block_size, config.crossbar_width),
	_return_network(config.num_slices * config.num_banks, config.num_ports, config.crossbar_width),
	_mem_highers(config.mem_highers),
	_level(config.level), _block_prefetch(config.block_prefetch), _num_mshr(config.num_mshr), _num_subentries(config.num_subentries), _miss_alloc(config.miss_alloc)
{
	_slices.reserve(config.num_slices);
	for(uint i = 0; i < config.num_slices; ++i)
	{
		_slices.push_back(config);
		config.mem_higher_port += config.mem_higher_port_stride;
	}
}

UnitCache::Slice::Slice(Configuration config) :
	miss_network(config.num_banks, 1, config.num_banks, 1)
{
	mem_higher_port = config.mem_higher_port;

	banks.reserve(config.num_banks);
	for(uint i = 0; i < config.num_banks; ++i)
		banks.push_back(config);
}

UnitCache::Bank::Bank(Configuration config) :
	request_pipline(config.latency), return_pipline(1), return_queue(config.return_queue_size) {}

UnitCache::~UnitCache()
{

}

bool UnitCache::has_work()
{
	for (auto& slice : _slices)
	{
		if (!slice.mshrs.empty() || !slice.mem_higher_request_queue.empty() || slice.miss_network.has_pending())
			return true;

		for (auto& bank : slice.banks)
			if (!bank.request_pipline.empty() || !bank.return_pipline.empty() || bank.return_queue.is_read_valid())
				return true;
	}

	return _request_network.has_pending() || _return_network.has_pending();
}

void UnitCache::_allocate(paddr_t sector_addr, Slice& slice)
{
	const Victim victim = _allocate_block(sector_addr);
	if (!victim.dirty) return;
	for (uint i = 0; i < _block_size / _sector_size; ++i)
		if ((victim.dirty >> i) & 0x1)
			_write_back(victim.addr + i * _sector_size, victim.data + i * _sector_size, slice);
}

//Queued with the fills, so the next level sees it before any later fetch of the same sector.
void UnitCache::_write_back(paddr_t sector_addr, const uint8_t* data, Slice& slice)
{
	MemoryRequest request;
	request.type = MemoryRequest::Type::STORE;
	request.size = _sector_size;
	request.paddr = sector_addr;
	request.port = slice.mem_higher_port;
	std::memcpy(request.data, data, _sector_size);
	slice.mem_higher_request_queue.push(request);
	log.writebacks++;
}

//copy of the sector and into the cache
void UnitCache::_apply_subentries(MSHR& mshr, paddr_t sector_addr, Slice& slice)
{
	while (!mshr.subentries.empty())
	{
		const MemoryRequest& sub = mshr.subentries.front();
		const uint offset = _get_sector_offset(sub.paddr);
		if (sub.type == MemoryRequest::Type::STORE)
		{
			std::memcpy(mshr.sector + offset, sub.data, sub.size);
			if (!_merge_sector(sector_addr, offset, sub.data, sub.size))
			{
				_allocate(sector_addr, slice);
				_write_sector(sector_addr, mshr.sector, true);
			}
			if(sub.flags.no_evict) _pin(sector_addr);
			_count_down(slice.stores_pending, sector_addr);
			log.data_array_writes++;
		}
		else mshr.ready.push(MemoryReturn(sub, mshr.sector + offset));
		mshr.subentries.pop();
	}
}

void UnitCache::_count_down(std::unordered_map<paddr_t, uint>& counts, paddr_t sector_addr)
{
	auto it = counts.find(sector_addr);
	_assert(it != counts.end());
	if (--it->second == 0) counts.erase(it);
}

void UnitCache::_recive_return()
{
	for(uint s = 0; s < _slices.size(); ++s)
	{ 
		Slice& slice = _slices[s];
		for(UnitMemoryBase* mem_higher : _mem_highers)
		{
			if(mem_higher->return_port_read_valid(slice.mem_higher_port))
			{
				MemoryReturn ret = mem_higher->peek_return(slice.mem_higher_port);
				bool cached = !(ret.flags.omit_cache & (0x1 << _level));
				if(cached)
				{
					paddr_t sector_addr = _get_sector_addr(ret.paddr);
					MSHR& mshr = slice.mshrs[sector_addr];
					uint b = _get_bank(ret.paddr);
					Bank& bank = slice.banks[b];

					if (!mshr.filled)
					{
						if (!_miss_alloc) _allocate(sector_addr, slice);

						if (const uint8_t* cached_sector = _find_sector(sector_addr))
							std::memcpy(mshr.sector, cached_sector, _sector_size);
						else
						{
							if (!_write_sector(sector_addr, ret.data, false) && mshr.has_store)
							{
								_allocate(sector_addr, slice);
								_write_sector(sector_addr, ret.data, false);
							}
							std::memcpy(mshr.sector, ret.data, _sector_size);
						}
						mshr.filled = true;
					}
					_apply_subentries(mshr, sector_addr, slice);

					//return one load per clock
					if (bank.return_pipline.is_write_valid() && !mshr.ready.empty())
					{
						bank.return_pipline.write(mshr.ready.front());
						mshr.ready.pop();
					}

					if(mshr.ready.empty())
					{
						mem_higher->read_return(slice.mem_higher_port);
						slice.mshrs.erase(sector_addr); //free mshr
					}
				}
				else
				{
					uint b = _get_bank(ret.paddr);
					Bank& bank = slice.banks[b];
					if(bank.return_pipline.is_write_valid())
					{
						uint i = s * slice.banks.size() + b;
						ret.port = ret.dst.pop(8);
						bank.return_pipline.write(ret);
						mem_higher->read_return(slice.mem_higher_port);
					}
				}
			}
		}
	}
}

void UnitCache::_recive_request()
{
	for(uint s = 0; s < _slices.size(); ++s)
	{
		Slice& slice = _slices[s];
		for(uint b = 0; b < slice.banks.size(); ++b)
		{
			Bank& bank = slice.banks[b];
			if(!bank.request_pipline.is_read_valid()) continue;
			if(!bank.return_queue.is_write_valid() || !slice.miss_network.is_write_valid(b))
			{
				log.mshr_stalls++;
				continue;
			}

			MemoryRequest request = bank.request_pipline.peek();
			paddr_t sector_addr = _get_sector_addr(request.paddr);
			paddr_t sector_offset = _get_sector_offset(request.paddr);
			uint8_t sector_index = _get_sector_index(request.paddr);

			bool cached = !(request.flags.omit_cache & (0x1 << _level));
			if(!cached)
			{
				if (request.type == MemoryRequest::Type::STORE)
					if (const uint8_t* dirty_data = _invalidate_sector(sector_addr))
						_write_back(sector_addr, dirty_data, slice);

				//Forward request
				request.dst.push(request.port, 8);
				request.port = slice.mem_higher_port;
				slice.mem_higher_request_queue.push(request);
				log.uncached_requests++;
			}
			else if(request.type == MemoryRequest::Type::LOAD)
			{
				//a load waits behind a store to its sector still on the miss path
				uint8_t* sector_data = slice.stores_pending.count(sector_addr) ? nullptr : _read_sector(sector_addr);
				log.tag_array_access++;

				if(sector_data)
				{
					//Hit: fill request and insert into return queue
					bank.return_queue.write(MemoryReturn(request, sector_data + sector_offset));
					log.data_array_reads++;
					log.hits++;
				}
				else
				{
					//Miss: allocate a block and insert into miss queue
					if(_miss_alloc) _allocate(sector_addr, slice);
					slice.miss_network.write(request, b);
					slice.misses_pending[sector_addr]++;
				}
			}
			else if (request.type == MemoryRequest::Type::STORE)
			{
				log.stores++;
				log.tag_array_access++;
				const bool in_flight = slice.misses_pending.count(sector_addr) || slice.mshrs.count(sector_addr);
				if (!in_flight && _read_sector(sector_addr))
				{
					_merge_sector(sector_addr, sector_offset, request.data, request.size);
					if(request.flags.no_evict) _pin(sector_addr);
					log.store_hits++;
					log.data_array_writes++;
				}
				else
				{
					if (_miss_alloc) _allocate(sector_addr, slice);
					slice.miss_network.write(request, b);
					slice.misses_pending[sector_addr]++;
					slice.stores_pending[sector_addr]++;
				}
			}
			else if(request.type == MemoryRequest::Type::RELEASE)
			{
				for(uint i = 0; i < _block_size; i += _sector_size)
					_assert(!slice.stores_pending.count(_get_block_addr(request.paddr) + i));
				_release(request.paddr);
				log.releases++;
			}
			else _assert(false);

			//pop the request
			bank.request_pipline.read();
		}

		//Proccess misses
		slice.miss_network.clock();
		if(!slice.miss_network.is_read_valid(0)) continue;
		const MemoryRequest& miss = slice.miss_network.peek(0);

		//Try to fetch an mshr for the line or allocate a new mshr for the line
		bool request_sector = false;
		paddr_t sector_addr = _get_sector_addr(miss.paddr);
		if(slice.mshrs.find(sector_addr) == slice.mshrs.end())
		{
			if (miss.type == MemoryRequest::Type::STORE)
			{
				const uint offset = _get_sector_offset(miss.paddr);
				bool applied = _merge_sector(sector_addr, offset, miss.data, miss.size);
				if (!applied && offset == 0 && miss.size == _sector_size)
				{
					if (!_write_sector(sector_addr, miss.data, true))
					{
						_allocate(sector_addr, slice);
						_write_sector(sector_addr, miss.data, true);
					}
					log.store_allocates++;
					applied = true;
				}
				if (applied)
				{
					if(miss.flags.no_evict) _pin(sector_addr);
					log.data_array_writes++;
					_count_down(slice.misses_pending, sector_addr);
					_count_down(slice.stores_pending, sector_addr);
					slice.miss_network.read(0);
					continue;
				}
			}

			//Didn't find mshr. Try to allocate one
			if(slice.mshrs.size() < _num_mshr) 
				MSHR& mshr = slice.mshrs[sector_addr]; //Allocated a new MSHR
			else continue; //Out of MSHRs
			request_sector = true;
		}

		MSHR& mshr = slice.mshrs[sector_addr];
		if(mshr.subentries.size() < _num_subentries)
		{
			if (miss.type == MemoryRequest::Type::STORE) mshr.has_store = true;
			mshr.subentries.push(miss);
			_count_down(slice.misses_pending, sector_addr);
			slice.miss_network.read(0);
			if(request_sector)
			{
				MemoryRequest mshr_fill_req;
				mshr_fill_req.type = MemoryRequest::Type::LOAD;
				mshr_fill_req.paddr = sector_addr;
				mshr_fill_req.size = _sector_size;
				mshr_fill_req.port = slice.mem_higher_port;
				slice.mem_higher_request_queue.push(mshr_fill_req);
				log.misses++;
			}
			else log.half_misses++;
		}

		if(_block_prefetch)
		{
			paddr_t block_address = _get_block_addr(sector_addr);
			for(uint i = 0; i < _block_size; i += _sector_size)
			{
				sector_addr = block_address + i;
				if(slice.mshrs.find(sector_addr) == slice.mshrs.end())
				{
					slice.mshrs.insert({sector_addr, MSHR()});
					MemoryRequest mshr_fill_req;
					mshr_fill_req.type = MemoryRequest::Type::LOAD;
					mshr_fill_req.paddr = sector_addr;
					mshr_fill_req.size = _sector_size;
					mshr_fill_req.port = slice.mem_higher_port;
					slice.mem_higher_request_queue.push(mshr_fill_req);
				}
			}
		}
	}
}

void UnitCache::_send_request()
{
	for(uint s = 0; s < _slices.size(); ++s)
	{
		Slice& slice = _slices[s];
		if(slice.mem_higher_request_queue.empty()) continue;

		const MemoryRequest& request = slice.mem_higher_request_queue.front();
		UnitMemoryBase* mem_higher = _get_mem_higher(request.paddr);

		_assert(request.port == slice.mem_higher_port);
		if(!mem_higher->request_port_write_valid(request.port)) continue;

		mem_higher->write_request(request);
		slice.mem_higher_request_queue.pop();
	}
}

void UnitCache::clock_rise()
{
	_request_network.clock();

	for(uint s = 0; s < _slices.size(); ++s)
	{
		Slice& slice = _slices[s];
		for(uint b = 0; b < slice.banks.size(); ++b)
		{
			Bank& bank = slice.banks[b];
			uint port = s * slice.banks.size() + b;

			if(_request_network.is_read_valid(port) && bank.request_pipline.is_write_valid())
				bank.request_pipline.write(_request_network.read(port));
			bank.request_pipline.clock();
		}
	}

	_recive_return();
	_recive_request();
}

void UnitCache::clock_fall()
{
	_send_request();

	for(uint s = 0; s < _slices.size(); ++s)
	{
		Slice& slice = _slices[s];
		for(uint b = 0; b < slice.banks.size(); ++b)
		{
			Bank& bank = slice.banks[b];
			uint port = s * slice.banks.size() + b;

			if(bank.return_pipline.is_write_valid() && bank.return_queue.is_read_valid())
				bank.return_pipline.write(bank.return_queue.read());

			bank.return_pipline.clock();
			if(bank.return_pipline.is_read_valid() && _return_network.is_write_valid(port))
			{
				log.bytes_read += bank.return_pipline.peek().size;
				_return_network.write(bank.return_pipline.read(), port);
			}
		}
	}

	_return_network.clock();
}

bool UnitCache::request_port_write_valid(uint port_index)
{
	return _request_network.is_write_valid(port_index);
}

void UnitCache::write_request(const MemoryRequest& request)
{
	_request_network.write(request, request.port);
}

bool UnitCache::return_port_read_valid(uint port_index)
{
	return _return_network.is_read_valid(port_index);
}

const MemoryReturn& UnitCache::peek_return(uint port_index)
{
	return _return_network.peek(port_index);
}

const MemoryReturn UnitCache::read_return(uint port_index)
{
	return _return_network.read(port_index);
}

}}